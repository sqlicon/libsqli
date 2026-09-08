#define _POSIX_C_SOURCE 200809L
#include "libsqli/sqli.h"
#include "sqli_internal.h"
#include "sqli_tcp.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Deliberately unsafe reuse is a negative protocol experiment, not an API example. */
enum { sql_capacity = 256, table_capacity = 64, interrupt_byte = 0x42,
       interruption_code = -213, lock_timeout_code = -244, release_delay_ms = 100,
       milliseconds_per_second = 1000,
       nanoseconds_per_millisecond = 1000000, next_lock_wait_seconds = 2 };

struct delayed_action {
    sqli_conn_t *connection;
    int socket_fd;
    unsigned delay_ms;
    bool interrupt;
    sqli_status status;
};

static sqli_status test_params(sqli_connect_params *params)
{
    const char *database = getenv("SQLI_TEST_LOGGING_DB");
    *params = (sqli_connect_params){
        .hostname = getenv("SQLI_TEST_HOST"), .service = getenv("SQLI_TEST_PORT"),
        .server = getenv("SQLI_TEST_SERVER"), .username = getenv("SQLI_TEST_USER"),
        .password = getenv("SQLI_TEST_PASS"),
        .database = database != NULL && database[0] != '\0' ? database : "sqli_log_test",
        .client_locale = getenv("SQLI_TEST_CLIENT_LOCALE"),
        .db_locale = getenv("SQLI_TEST_DB_LOCALE")
    };
    return params->hostname != NULL && params->service != NULL && params->server != NULL &&
        params->username != NULL && params->password != NULL ? SQLI_OK : SQLI_INVALID_ARGUMENT;
}

static sqli_status execute_sql(sqli_conn_t *connection, const char *sql, int *sqlcode)
{
    sqli_result_t *result = NULL;
    sqli_status status = sqli_query(connection, sql, &result);
    sqli_error_info error = {0};
    sqli_status diagnostic_status = sqli_error_get_info(connection, &error);
    if (sqlcode != NULL)
        *sqlcode = error.sqlcode;
    sqli_result_destroy(result);
    return diagnostic_status == SQLI_OK ? status : diagnostic_status;
}

static sqli_status session_id(sqli_conn_t *connection, int64_t *out)
{
    sqli_result_t *result = NULL;
    sqli_status status = sqli_query(connection,
        "SELECT DBINFO('sessionid') FROM systables WHERE tabid = 1", &result);
    if (status == SQLI_OK)
        status = sqli_result_fetch(result);
    if (status == SQLI_OK) {
        const size_t session_column = 0;
        bool is_null;
        status = sqli_result_get_int64(result, session_column, out, &is_null);
        if (status == SQLI_OK && is_null)
            status = SQLI_NULL_VALUE;
    }
    sqli_result_destroy(result);
    return status;
}

static void *run_delayed(void *context)
{
    struct delayed_action *action = context;
    struct timespec delay = {
        .tv_sec = action->delay_ms / milliseconds_per_second,
        .tv_nsec = (long)(action->delay_ms % milliseconds_per_second) * nanoseconds_per_millisecond
    };
    while (nanosleep(&delay, &delay) != 0) {
        if (errno != EINTR) {
            action->status = SQLI_IO_ERROR;
            return NULL;
        }
    }
    if (action->interrupt) {
        const unsigned char request = interrupt_byte;
        ssize_t sent = send(action->socket_fd, &request, sizeof(request), MSG_OOB);
        action->status = sent == (ssize_t)sizeof(request) ? SQLI_OK : SQLI_IO_ERROR;
    } else {
        action->status = sqli_rollback(action->connection);
    }
    return NULL;
}

static void join_action(pthread_t thread)
{
    if (pthread_join(thread, NULL) != 0) {
        fputs("Cannot join probe thread; refusing concurrent cleanup\n", stderr);
        _exit(EXIT_FAILURE);
    }
}

static sqli_status lock_row(sqli_conn_t *holder, const char *table, int row)
{
    sqli_status status = sqli_begin(holder);
    if (status == SQLI_OK) {
        char sql[sql_capacity];
        snprintf(sql, sizeof(sql), "UPDATE %s SET amount = amount + 1 WHERE id = %d", table, row);
        status = execute_sql(holder, sql, NULL);
    }
    return status;
}

static sqli_status run_case(sqli_conn_t *holder, sqli_pool_t *pool, const char *table,
                            bool late, bool discard, unsigned cancel_delay_ms)
{
    sqli_conn_t *worker = NULL;
    sqli_status status = sqli_pool_acquire(pool, &worker);
    if (status != SQLI_OK)
        return status;
    int64_t initial_session = 0, next_session = 0;
    status = session_id(worker, &initial_session);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_set_lock_wait(worker, next_lock_wait_seconds);
    if (status != SQLI_OK)
        goto cleanup;
    status = lock_row(holder, table, 1);
    if (status != SQLI_OK)
        goto cleanup;

    /* Capture the old operation's socket before it finishes. */
    struct delayed_action cancel = {.socket_fd = worker->socket_fd,
        .delay_ms = cancel_delay_ms, .interrupt = true};
    struct delayed_action release = {.connection = holder, .delay_ms = release_delay_ms};
    pthread_t release_thread, cancel_thread;
    if (pthread_create(&release_thread, NULL, run_delayed, &release) != 0) {
        status = SQLI_IO_ERROR;
        goto cleanup;
    }
    bool cancel_running = false;
    if (!late) {
        if (pthread_create(&cancel_thread, NULL, run_delayed, &cancel) != 0) {
            join_action(release_thread);
            status = SQLI_IO_ERROR;
            goto cleanup;
        }
        cancel_running = true;
    }
    char sql[sql_capacity];
    snprintf(sql, sizeof(sql), "UPDATE %s SET amount = amount + 10 WHERE id = 1", table);
    int first_code = 0;
    sqli_status first_status = execute_sql(worker, sql, &first_code);
    join_action(release_thread);
    if (cancel_running)
        join_action(cancel_thread);
    if (release.status != SQLI_OK || (!late && cancel.status != SQLI_OK) ||
        (late && first_status != SQLI_OK) ||
        (first_status != SQLI_OK && first_code != interruption_code)) {
        status = SQLI_ERR;
        goto cleanup;
    }

    if (discard) {
        /* No ordinary SQL or TLS I/O remains active; discard this physical session. */
        int fd = worker->socket_fd;
        if (shutdown(fd, SHUT_RDWR) != 0) {
            status = SQLI_IO_ERROR;
            goto cleanup;
        }
        sqli_tcp_close(fd);
        worker->socket_fd = -1;
        worker->state = SQLI_CONN_CLOSED;
    }
    status = sqli_pool_release(pool, worker);
    if (status != SQLI_OK)
        goto cleanup;
    worker = NULL;
    status = sqli_pool_acquire(pool, &worker);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_set_lock_wait(worker, next_lock_wait_seconds);
    if (status != SQLI_OK)
        goto cleanup;
    status = session_id(worker, &next_session);
    if (status != SQLI_OK)
        goto cleanup;
    status = lock_row(holder, table, 2);
    if (status != SQLI_OK)
        goto cleanup;

    /* Late mode gates the stale send until the next pool lease is executing. */
    if (late && pthread_create(&cancel_thread, NULL, run_delayed, &cancel) != 0) {
        status = SQLI_IO_ERROR;
        goto cleanup;
    }
    snprintf(sql, sizeof(sql), "UPDATE %s SET amount = amount + 10 WHERE id = 2", table);
    int next_code = 0;
    sqli_status next_status = execute_sql(worker, sql, &next_code);
    if (late)
        join_action(cancel_thread);
    printf("mode=%s delay_ms=%u first=%s first_code=%d next=%s next_code=%d same_session=%d\n",
        late ? "late-reuse" : discard ? "race-discard" : "race-reuse", cancel_delay_ms,
        sqli_status_name(first_status), first_code, sqli_status_name(next_status), next_code,
        initial_session == next_session);
    if (next_status == SQLI_OK || (discard && initial_session == next_session) ||
        (!discard && initial_session != next_session) ||
        (late && (cancel.status != SQLI_OK || next_code != interruption_code)) ||
        (!late && next_code != lock_timeout_code && next_code != interruption_code) ||
        (discard && next_code == interruption_code))
        status = SQLI_ERR;
cleanup:
    if (holder->in_transaction && sqli_rollback(holder) != SQLI_OK)
        status = SQLI_ERR;
    if (worker != NULL && sqli_pool_release(pool, worker) != SQLI_OK)
        status = SQLI_ERR;
    return status;
}

int main(int argc, char **argv)
{
    bool late = argc == 2 && strcmp(argv[1], "--late-reuse") == 0;
    bool discard = argc == 2 && strcmp(argv[1], "--race-discard") == 0;
    if (!late && !discard && !(argc == 2 && strcmp(argv[1], "--race-reuse") == 0)) {
        fprintf(stderr, "Usage: %s --late-reuse|--race-reuse|--race-discard\n", argv[0]);
        return EXIT_FAILURE;
    }
    /* Probe-only policy: TLS detach currently sends close_notify after shutdown.
     * Keep SIGPIPE from terminating this experiment; production discard needs
     * a separate no-I/O TLS teardown and must not change process signal policy.
     */
    struct sigaction pipe_action = {.sa_handler = SIG_IGN};
    if (sigemptyset(&pipe_action.sa_mask) != 0 ||
        sigaction(SIGPIPE, &pipe_action, NULL) != 0) {
        fputs("Cannot configure probe SIGPIPE policy\n", stderr);
        return EXIT_FAILURE;
    }
    sqli_conn_t *holder = NULL;
    sqli_pool_t *pool = NULL;
    sqli_connect_params params;
    bool created = false;
    char table[table_capacity], sql[sql_capacity];
    snprintf(table, sizeof(table), "sqli_cancel_race_%ld", (long)getpid());
    sqli_status status = test_params(&params);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_create(&holder);
    if (status == SQLI_OK)
        status = sqli_connect(holder, &params);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_pool_create(&pool, &params, 1);
    if (status != SQLI_OK)
        goto cleanup;
    snprintf(sql, sizeof(sql),
        "CREATE TABLE %s (id INTEGER, amount INTEGER) LOCK MODE ROW", table);
    status = execute_sql(holder, sql, NULL);
    if (status != SQLI_OK)
        goto cleanup;
    created = true;
    snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (1, 0)", table);
    status = execute_sql(holder, sql, NULL);
    if (status != SQLI_OK)
        goto cleanup;
    snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (2, 0)", table);
    status = execute_sql(holder, sql, NULL);
    if (status != SQLI_OK)
        goto cleanup;
    const unsigned delays[] = {90, 100, 110};
    for (size_t i = 0; i < (late ? 1 : sizeof(delays) / sizeof(delays[0])); i++) {
        status = run_case(holder, pool, table, late, discard, delays[i]);
        if (status != SQLI_OK)
            break;
    }
cleanup:
    sqli_pool_destroy(pool);
    if (created) {
        snprintf(sql, sizeof(sql), "DROP TABLE %s", table);
        sqli_status cleanup_status = execute_sql(holder, sql, NULL);
        if (cleanup_status != SQLI_OK) {
            fprintf(stderr, "Test table cleanup failed: %s\n", table);
            status = cleanup_status;
        }
    }
    sqli_destroy(holder);
    if (status != SQLI_OK)
        fprintf(stderr, "Cancel race probe failed: %s\n", sqli_status_name(status));
    return status == SQLI_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
