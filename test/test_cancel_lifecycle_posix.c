#define _POSIX_C_SOURCE 200809L
#include "unity.h"
#include "sqli_cancel.h"
#include "sqli_internal.h"
#include "sqli_tcp.h"
#include "sqli_tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

/* Link wrappers give deterministic sender/finisher ordering without timing bets. */
enum { peer_capacity = 16, stream_end = 0, peer_end = 1 };
static int peers[peer_capacity];
static size_t peer_count;
static sqli_conn_t *connection;
static sqli_pool_t *pool;
static sqli_cancel_operation *operation;
sqli_status __real_sqli_tcp_interrupt(int fd);
sqli_status __real_sqli_connect(sqli_conn_t *conn, const sqli_connect_params *params);

static unsigned shutdown_calls;
static unsigned send_calls;
static bool fail_send, pause_send, sender_entered, release_sender;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;

static void check_thread(int status)
{
    /* Continuing cleanup after synchronization failure could race test threads. */
    if (status != 0)
        _exit(EXIT_FAILURE);
}

int __wrap_SSL_shutdown(SSL *ssl)
{
    (void)ssl;
    shutdown_calls++;
    return 1;
}

sqli_status __wrap_sqli_tcp_interrupt(int fd)
{
    check_thread(pthread_mutex_lock(&gate));
    send_calls++;
    sender_entered = true;
    check_thread(pthread_cond_broadcast(&changed));
    while (pause_send && !release_sender)
        check_thread(pthread_cond_wait(&changed, &gate));
    bool failed = fail_send;
    check_thread(pthread_mutex_unlock(&gate));
    return fd >= 0 && !failed ? SQLI_OK : SQLI_IO_ERROR;
}

/* A local connected TCP pair stands in for an authenticated SQLI session. */
sqli_status __wrap_sqli_connect(sqli_conn_t *conn, const sqli_connect_params *params)
{
    (void)params;
    if (peer_count == peer_capacity)
        return SQLI_LIMIT_EXCEEDED;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        return SQLI_IO_ERROR;
    struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(address);
    int client = -1, peer = -1;
    sqli_status status = SQLI_IO_ERROR;
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&address, &length) != 0)
        goto cleanup;
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 || connect(client, (struct sockaddr *)&address, length) != 0)
        goto cleanup;
    peer = accept(listener, NULL, NULL);
    if (peer < 0)
        goto cleanup;
    conn->socket_fd = client;
    conn->state = SQLI_CONN_READY;
    peers[peer_count++] = peer;
    client = -1;
    peer = -1;
    status = SQLI_OK;
cleanup:
    if (close(listener) != 0)
        status = SQLI_IO_ERROR;
    if (client >= 0 && close(client) != 0)
        status = SQLI_IO_ERROR;
    if (peer >= 0 && close(peer) != 0)
        status = SQLI_IO_ERROR;
    return status;
}

void setUp(void)
{
    peer_count = 0;
    connection = NULL;
    pool = NULL;
    operation = NULL;
    shutdown_calls = 0;
    send_calls = 0;
    fail_send = pause_send = sender_entered = release_sender = false;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_create(&connection));
    TEST_ASSERT_EQUAL(SQLI_OK, __wrap_sqli_connect(connection, NULL));
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_create(&operation));
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_destroy(operation));
    if (connection != NULL) {
        TEST_ASSERT_EQUAL(SQLI_OK, sqli_conn_discard(connection));
        if (pool != NULL)
            TEST_ASSERT_EQUAL(SQLI_OK, sqli_pool_release(pool, connection));
        else
            sqli_destroy(connection);
    }
    sqli_pool_destroy(pool);
    for (size_t i = 0; i < peer_count; i++)
        TEST_ASSERT_EQUAL(0, close(peers[i]));
    TEST_ASSERT_EQUAL_UINT(0, shutdown_calls);
}

static void begin_operation(void)
{
    bool execute = false;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_begin(operation, connection, &execute));
    TEST_ASSERT_TRUE(execute);
}

static void test_prestart_and_single_use(void)
{
    sqli_cancel_disposition disposition;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_request(operation, &disposition));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_LATCHED, disposition);
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_request(operation, &disposition));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_ALREADY_REQUESTED, disposition);
    bool execute = true;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_begin(operation, connection, &execute));
    TEST_ASSERT_FALSE(execute);
    TEST_ASSERT_EQUAL_UINT(0, atomic_load(&connection->lifecycle));
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE,
        sqli_cancel_operation_begin(operation, connection, &execute));
    sqli_cancel_snapshot snapshot;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_snapshot(operation, &snapshot));
    TEST_ASSERT_FALSE(snapshot.started);
    TEST_ASSERT_TRUE(snapshot.requested);
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_request(operation, &disposition));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_COMPLETED, disposition);
    TEST_ASSERT_EQUAL_UINT(0, send_calls);
}

static void test_terminal_snapshot_outlives_connection(void)
{
    begin_operation();
    connection->error_info.sqlcode = -213;
    connection->in_transaction = true;
    sqli_cancel_disposition disposition;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_request(operation, &disposition));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_SENT, disposition);
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_request(operation, &disposition));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_ALREADY_REQUESTED, disposition);
    /* Cleanup after a send attempt must not issue more statement-control I/O,
     * even before finish transitions the connection to its terminal state.
     */
    sqli_stmt_t *stmt = calloc(1, sizeof(*stmt));
    TEST_ASSERT_NOT_NULL(stmt);
    stmt->conn = connection;
    stmt->socket_fd = connection->socket_fd;
    stmt->stmt_id = 17;
    sqli_stmt_destroy(stmt);
    char byte;
    ssize_t received = recv(peers[stream_end], &byte, sizeof(byte), MSG_DONTWAIT);
    int saved_errno = errno;
    TEST_ASSERT_EQUAL_INT(-1, received);
    TEST_ASSERT_TRUE(saved_errno == EAGAIN || saved_errno == EWOULDBLOCK);
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_finish(operation, SQLI_PROTO_ERROR));
    TEST_ASSERT_EQUAL_INT(-1, connection->socket_fd);
    TEST_ASSERT_TRUE(connection->in_transaction);
    TEST_ASSERT_EQUAL_UINT64(0, connection->rollback_epoch);
    sqli_destroy(connection);
    connection = NULL;
    sqli_cancel_snapshot snapshot;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_snapshot(operation, &snapshot));
    TEST_ASSERT_EQUAL_INT(-213, snapshot.diagnostic.sqlcode);
    TEST_ASSERT_EQUAL(SQLI_PROTO_ERROR, snapshot.operation_status);
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_request(operation, &disposition));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_COMPLETED, disposition);
    TEST_ASSERT_EQUAL_UINT(1, send_calls);
}

static void test_failed_send_still_discards(void)
{
    begin_operation();
    fail_send = true;
    sqli_cancel_disposition disposition;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_request(operation, &disposition));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_SEND_FAILED, disposition);
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_finish(operation, SQLI_OK));
    sqli_cancel_snapshot snapshot;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_snapshot(operation, &snapshot));
    TEST_ASSERT_TRUE(snapshot.send_attempted);
    TEST_ASSERT_EQUAL(SQLI_IO_ERROR, snapshot.send_status);
    TEST_ASSERT_EQUAL(SQLI_OK, snapshot.operation_status);
    TEST_ASSERT_EQUAL_INT(-1, connection->socket_fd);
}

static void test_completion_wins_without_send(void)
{
    begin_operation();
    int fd = connection->socket_fd;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_finish(operation, SQLI_OK));
    sqli_cancel_disposition disposition;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_request(operation, &disposition));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_COMPLETED, disposition);
    TEST_ASSERT_EQUAL_UINT(0, send_calls);
    TEST_ASSERT_EQUAL_INT(fd, connection->socket_fd);
    TEST_ASSERT_EQUAL_UINT(0, atomic_load(&connection->lifecycle));
}

struct thread_result { sqli_status status; sqli_cancel_disposition disposition; };

static void *request_thread(void *context)
{
    struct thread_result *result = context;
    result->status = sqli_cancel_operation_request(operation, &result->disposition);
    return NULL;
}

static void *finish_thread(void *context)
{
    struct thread_result *result = context;
    result->status = sqli_cancel_operation_finish(operation, SQLI_OK);
    return NULL;
}

static void test_sender_pin_blocks_pool_close_and_finish(void)
{
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_conn_discard(connection));
    sqli_destroy(connection);
    connection = NULL;
    const sqli_connect_params params = {0};
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_pool_create(&pool, &params, 1));
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_pool_acquire(pool, &connection));
    begin_operation();
    pause_send = true;
    pthread_t requester, finisher;
    struct thread_result request = {0}, finish = {0};
    check_thread(pthread_create(&requester, NULL, request_thread, &request));
    check_thread(pthread_mutex_lock(&gate));
    while (!sender_entered)
        check_thread(pthread_cond_wait(&changed, &gate));
    check_thread(pthread_mutex_unlock(&gate));
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE, sqli_pool_release(pool, connection));
    sqli_conn_t *other = NULL;
    TEST_ASSERT_EQUAL(SQLI_TIMEOUT, sqli_pool_try_acquire(pool, &other));
    int fd = connection->socket_fd;
    sqli_close(connection);
    sqli_destroy(connection);
    sqli_pool_destroy(pool);
    TEST_ASSERT_EQUAL_INT(fd, connection->socket_fd);
    check_thread(pthread_create(&finisher, NULL, finish_thread, &finish));
    TEST_ASSERT_TRUE(atomic_load(&connection->lifecycle) & SQLI_CONN_PINNED);
    check_thread(pthread_mutex_lock(&gate));
    release_sender = true;
    check_thread(pthread_cond_broadcast(&changed));
    check_thread(pthread_mutex_unlock(&gate));
    check_thread(pthread_join(requester, NULL));
    check_thread(pthread_join(finisher, NULL));
    TEST_ASSERT_EQUAL(SQLI_OK, request.status);
    TEST_ASSERT_EQUAL(SQLI_OK, finish.status);
    TEST_ASSERT_EQUAL_INT(-1, connection->socket_fd);
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_pool_release(pool, connection));
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE, sqli_pool_release(pool, connection));
    bool execute = false;
    sqli_cancel_operation *fresh = NULL;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_create(&fresh));
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE,
        sqli_cancel_operation_begin(fresh, connection, &execute));
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_destroy(fresh));
    connection = NULL;
    size_t before = peer_count;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_pool_acquire(pool, &connection));
    TEST_ASSERT_EQUAL_UINT(before + 1, peer_count);
}

static void test_tls_discard_and_stale_statement_descriptor(void)
{
    struct sigaction before, after;
    TEST_ASSERT_EQUAL(0, sigaction(SIGPIPE, NULL, &before));
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    TEST_ASSERT_NOT_NULL(ctx);
    SSL *ssl = SSL_new(ctx);
    TEST_ASSERT_NOT_NULL(ssl);
    int old_fd = connection->socket_fd;
    TEST_ASSERT_EQUAL(1, SSL_set_fd(ssl, old_fd));
    TEST_ASSERT_EQUAL(0, sqli_tcp_tls_attach(old_fd, ctx, ssl));
    TEST_ASSERT_EQUAL(0, shutdown(old_fd, SHUT_RDWR));
    TEST_ASSERT_EQUAL(SQLI_IO_ERROR, __real_sqli_tcp_interrupt(old_fd));
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_conn_discard(connection));
    TEST_ASSERT_NULL(find_tls_entry(old_fd));
    const sqli_connect_params params = {0};
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE, __real_sqli_connect(connection, &params));
    sqli_result_t *rows = calloc(1, sizeof(*rows));
    TEST_ASSERT_NOT_NULL(rows);
    rows->owner_conn = connection;
    rows->statement_type = 2;
    rows->stmt_id = 17;
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE, sqli_result_fetch(rows));
    sqli_result_destroy(rows);
    TEST_ASSERT_EQUAL_UINT(0, shutdown_calls);
    TEST_ASSERT_EQUAL(0, sigaction(SIGPIPE, NULL, &after));
    TEST_ASSERT_TRUE(before.sa_handler == after.sa_handler);
    int pair[2];
    TEST_ASSERT_EQUAL(0, socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    if (pair[peer_end] == old_fd) {
        int temp = pair[stream_end];
        pair[stream_end] = pair[peer_end];
        pair[peer_end] = temp;
    }
    if (pair[stream_end] != old_fd) {
        TEST_ASSERT_EQUAL_INT(old_fd, dup2(pair[stream_end], old_fd));
        TEST_ASSERT_EQUAL(0, close(pair[stream_end]));
        pair[stream_end] = old_fd;
    }
    sqli_stmt_t *stmt = calloc(1, sizeof(*stmt));
    TEST_ASSERT_NOT_NULL(stmt);
    stmt->conn = connection;
    stmt->socket_fd = old_fd;
    stmt->stmt_id = 17;
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE, sqli_execute(stmt));
    sqli_stmt_destroy(stmt);
    char byte;
    ssize_t received = recv(pair[peer_end], &byte, sizeof(byte), MSG_DONTWAIT);
    int saved_errno = errno;
    TEST_ASSERT_EQUAL_INT(-1, received);
    TEST_ASSERT_TRUE(saved_errno == EAGAIN || saved_errno == EWOULDBLOCK);
    TEST_ASSERT_EQUAL(0, close(pair[stream_end]));
    TEST_ASSERT_EQUAL(0, close(pair[peer_end]));
}

static void test_reject_invalid_and_active_handle_use(void)
{
    bool execute = true;
    sqli_cancel_snapshot snapshot;
    TEST_ASSERT_EQUAL(SQLI_INVALID_ARGUMENT, sqli_cancel_operation_create(NULL));
    TEST_ASSERT_EQUAL(SQLI_INVALID_ARGUMENT,
        sqli_cancel_operation_begin(operation, NULL, &execute));
    TEST_ASSERT_EQUAL(SQLI_INVALID_ARGUMENT, sqli_cancel_operation_request(operation, NULL));
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE, sqli_cancel_operation_snapshot(operation, &snapshot));
    connection->state = SQLI_CONN_CLOSED;
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE,
        sqli_cancel_operation_begin(operation, connection, &execute));
    TEST_ASSERT_TRUE(execute);
    TEST_ASSERT_EQUAL_UINT(0, atomic_load(&connection->lifecycle));
    connection->state = SQLI_CONN_READY;
    begin_operation();
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE, sqli_cancel_operation_destroy(operation));
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE,
        sqli_cancel_operation_begin(operation, connection, &execute));
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_finish(operation, SQLI_OK));
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE, sqli_cancel_operation_finish(operation, SQLI_OK));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reject_invalid_and_active_handle_use);
    RUN_TEST(test_prestart_and_single_use);
    RUN_TEST(test_terminal_snapshot_outlives_connection);
    RUN_TEST(test_failed_send_still_discards);
    RUN_TEST(test_completion_wins_without_send);
    RUN_TEST(test_sender_pin_blocks_pool_close_and_finish);
    RUN_TEST(test_tls_discard_and_stale_statement_descriptor);
    return UNITY_END();
}
