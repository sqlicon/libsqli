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

ssize_t __real_sqli_tcp_send(int fd, const unsigned char *bytes, size_t length);
sqli_status __real_sqli_receive_dispatch(int fd, sqli_result_t *result, sqli_conn_t *conn);
sqli_status __real_sqli_tcp_tls_discard(int fd);
static bool public_execution, request_during_write, fail_write, fail_disposal;
static int server_sqlcode;
static unsigned connect_calls, fail_connect_call;
static bool omit_done;
static sqli_status response_status;
static sqli_cancel_disposition write_disposition;

ssize_t __wrap_sqli_tcp_send(int fd, const unsigned char *bytes, size_t length)
{
    if (!public_execution)
        return __real_sqli_tcp_send(fd, bytes, length);
    if (request_during_write) {
        request_during_write = false;
        if (sqli_cancel_operation_request(operation, &write_disposition) != SQLI_OK)
            return -1;
    }
    return fail_write ? -1 : (ssize_t)length;
}

sqli_status __wrap_sqli_receive_dispatch(int fd, sqli_result_t *result, sqli_conn_t *conn)
{
    if (!public_execution)
        return __real_sqli_receive_dispatch(fd, result, conn);
    if (response_status != SQLI_OK)
        return response_status;
    result->saw_done = !omit_done;
    if (server_sqlcode != 0) {
        conn->error_info.sqlcode = server_sqlcode;
        conn->error_info.has_error = true;
        conn->error_info.status = SQLI_PROTO_ERROR;
        return SQLI_PROTO_ERROR;
    }
    return SQLI_OK;
}

sqli_status __wrap_sqli_tcp_tls_discard(int fd)
{
    return fail_disposal ? SQLI_ERR : __real_sqli_tcp_tls_discard(fd);
}

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
    connect_calls++;
    if (connect_calls == fail_connect_call)
        return SQLI_IO_ERROR;
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
    public_execution = request_during_write = fail_write = fail_disposal = false;
    server_sqlcode = 0;
    connect_calls = fail_connect_call = 0;
    omit_done = false;
    response_status = SQLI_OK;
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
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_pool_destroy(pool));
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
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE, sqli_pool_destroy(pool));
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

static sqli_stmt_t public_statement(void)
{
    sqli_stmt_t stmt = {0};
    stmt.conn = connection;
    stmt.socket_fd = connection->socket_fd;
    stmt.stmt_id = 17;
    stmt.result.statement_type = 4; /* UPDATE */
    return stmt;
}

static void test_public_prestart_and_unsupported(void)
{
    sqli_stmt_t stmt = public_statement();
    stmt.result.statement_type = 2; /* SELECT is not supported by this version. */
    TEST_ASSERT_EQUAL(SQLI_UNSUPPORTED, sqli_execute_cancelable(&stmt, operation));
    sqli_cancel_disposition disposition;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_request(operation, &disposition));
    stmt.result.statement_type = 4;
    connection->autocommit = false;
    TEST_ASSERT_EQUAL(SQLI_UNSUPPORTED, sqli_execute_cancelable(&stmt, operation));
    connection->in_transaction = true;
    TEST_ASSERT_EQUAL(SQLI_CANCELED, sqli_execute_cancelable(&stmt, operation));
    sqli_cancel_snapshot snapshot;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_snapshot(operation, &snapshot));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_NOT_EXECUTED, snapshot.outcome);
    TEST_ASSERT_FALSE(snapshot.started);
    TEST_ASSERT_FALSE(snapshot.connection_discarded);
    TEST_ASSERT_EQUAL_UINT(0, send_calls);
}

static void test_public_latches_until_execute_written(void)
{
    sqli_stmt_t stmt = public_statement();
    public_execution = request_during_write = true;
    server_sqlcode = -213;
    TEST_ASSERT_EQUAL(SQLI_CANCELED, sqli_execute_cancelable(&stmt, operation));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_LATCHED, write_disposition);
    TEST_ASSERT_EQUAL_UINT(1, send_calls);
    sqli_cancel_snapshot snapshot;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_snapshot(operation, &snapshot));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_INTERRUPTED, snapshot.outcome);
    TEST_ASSERT_EQUAL(SQLI_CANCELED, snapshot.operation_status);
    TEST_ASSERT_TRUE(snapshot.connection_discarded);
    sqli_stmt_close(&stmt);
}

static void test_public_partial_write_is_unknown(void)
{
    sqli_stmt_t stmt = public_statement();
    public_execution = request_during_write = fail_write = true;
    TEST_ASSERT_EQUAL(SQLI_IO_ERROR, sqli_execute_cancelable(&stmt, operation));
    sqli_cancel_snapshot snapshot;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_snapshot(operation, &snapshot));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_UNKNOWN, snapshot.outcome);
    TEST_ASSERT_FALSE(snapshot.send_attempted);
    TEST_ASSERT_TRUE(snapshot.connection_discarded);
    TEST_ASSERT_EQUAL_UINT(0, send_calls);
    sqli_stmt_close(&stmt);
}

static void test_public_response_allocation_failure_is_unknown(void)
{
    sqli_stmt_t stmt = public_statement();
    public_execution = true;
    response_status = SQLI_ALLOC_FAIL;
    TEST_ASSERT_EQUAL(SQLI_ALLOC_FAIL, sqli_execute_cancelable(&stmt, operation));
    sqli_cancel_snapshot snapshot;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_snapshot(operation, &snapshot));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_UNKNOWN, snapshot.outcome);
    TEST_ASSERT_TRUE(snapshot.connection_discarded);
    TEST_ASSERT_FALSE(snapshot.send_attempted);
    sqli_stmt_close(&stmt);
}

static void test_public_missing_terminal_is_unknown(void)
{
    sqli_stmt_t stmt = public_statement();
    public_execution = omit_done = true;
    TEST_ASSERT_EQUAL(SQLI_PROTO_ERROR, sqli_execute_cancelable(&stmt, operation));
    sqli_cancel_snapshot snapshot;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_snapshot(operation, &snapshot));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_UNKNOWN, snapshot.outcome);
    TEST_ASSERT_TRUE(snapshot.connection_discarded);
    sqli_stmt_close(&stmt);
}

static void test_public_disposal_error_preserves_execution_and_lease(void)
{
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_conn_discard(connection));
    sqli_destroy(connection);
    connection = NULL;
    const sqli_connect_params params = {0};
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_pool_create(&pool, &params, 1));
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_pool_acquire(pool, &connection));
    sqli_stmt_t stmt = public_statement();
    public_execution = request_during_write = fail_disposal = true;
    TEST_ASSERT_EQUAL(SQLI_ERR, sqli_execute_cancelable(&stmt, operation));
    sqli_cancel_snapshot snapshot;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_cancel_operation_snapshot(operation, &snapshot));
    TEST_ASSERT_EQUAL(SQLI_CANCEL_EXECUTED, snapshot.outcome);
    TEST_ASSERT_EQUAL(SQLI_OK, snapshot.operation_status);
    TEST_ASSERT_EQUAL(SQLI_ERR, snapshot.disposal_status);
    TEST_ASSERT_EQUAL(SQLI_CONN_ERROR, connection->state);
    TEST_ASSERT_EQUAL(SQLI_ERR, sqli_pool_release(pool, connection));
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE, sqli_pool_destroy(pool));
    sqli_conn_t *other = NULL;
    TEST_ASSERT_EQUAL(SQLI_TIMEOUT, sqli_pool_try_acquire(pool, &other));
    sqli_stmt_close(&stmt);
    fail_disposal = false;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_pool_release(pool, connection));
    connection = NULL;
}

static void test_partial_pool_creation_retains_failed_cleanup(void)
{
    const sqli_connect_params params = {0};
    /* The setup connection is the first connect call; fail the pool's second. */
    fail_connect_call = connect_calls + 2;
    fail_disposal = true;
    sqli_pool_t *partial = NULL;
    TEST_ASSERT_EQUAL(SQLI_ERR, sqli_pool_create(&partial, &params, 2));
    TEST_ASSERT_NOT_NULL(partial);
    sqli_conn_t *borrowed = NULL;
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE, sqli_pool_try_acquire(partial, &borrowed));
    fail_disposal = false;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_pool_destroy(partial));
}

static void test_pool_destroy_error_is_retryable(void)
{
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_conn_discard(connection));
    sqli_destroy(connection);
    connection = NULL;
    const sqli_connect_params params = {0};
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_pool_create(&pool, &params, 1));
    fail_disposal = true;
    TEST_ASSERT_EQUAL(SQLI_ERR, sqli_pool_destroy(pool));
    TEST_ASSERT_EQUAL(SQLI_INVALID_STATE, sqli_pool_try_acquire(pool, &connection));
    TEST_ASSERT_NULL(connection);
    fail_disposal = false;
    TEST_ASSERT_EQUAL(SQLI_OK, sqli_pool_destroy(pool));
    pool = NULL;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_public_prestart_and_unsupported);
    RUN_TEST(test_public_latches_until_execute_written);
    RUN_TEST(test_public_partial_write_is_unknown);
    RUN_TEST(test_public_missing_terminal_is_unknown);
    RUN_TEST(test_public_response_allocation_failure_is_unknown);
    RUN_TEST(test_public_disposal_error_preserves_execution_and_lease);
    RUN_TEST(test_pool_destroy_error_is_retryable);
    RUN_TEST(test_partial_pool_creation_retains_failed_cleanup);
    RUN_TEST(test_reject_invalid_and_active_handle_use);
    RUN_TEST(test_prestart_and_single_use);
    RUN_TEST(test_terminal_snapshot_outlives_connection);
    RUN_TEST(test_failed_send_still_discards);
    RUN_TEST(test_completion_wins_without_send);
    RUN_TEST(test_sender_pin_blocks_pool_close_and_finish);
    RUN_TEST(test_tls_discard_and_stale_statement_descriptor);
    return UNITY_END();
}
