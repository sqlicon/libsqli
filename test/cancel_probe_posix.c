#define _POSIX_C_SOURCE 200809L
#include "libsqli/sqli.h"
#include "sqli_internal.h"

#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Manual protocol experiment, not a public cross-thread cancellation API. */
enum { sql_capacity = 256, table_capacity = 64, lock_wait_seconds = 10,
       interrupt_byte = 0x42, interrupted_sqlcode = -213,
       lock_timeout_sqlcode = -244, lock_timeout_isamcode = -154,
       nanoseconds_per_second = 1000000000 };

struct blocked_operation {
    sqli_conn_t *connection;
    char sql[sql_capacity];
    sqli_status status;
    sqli_error_info error;
    double elapsed_seconds;
};

static sqli_status connect_test(sqli_conn_t **out)
{
    const char *database = getenv("SQLI_TEST_LOGGING_DB");
    sqli_connect_params params = {
        .hostname = getenv("SQLI_TEST_HOST"), .service = getenv("SQLI_TEST_PORT"),
        .database = database != NULL && database[0] != '\0' ? database : "sqli_log_test",
        .username = getenv("SQLI_TEST_USER"), .password = getenv("SQLI_TEST_PASS"),
        .server = getenv("SQLI_TEST_SERVER"),
        .client_locale = getenv("SQLI_TEST_CLIENT_LOCALE"),
        .db_locale = getenv("SQLI_TEST_DB_LOCALE")
    };
    if (params.hostname == NULL || params.service == NULL || params.username == NULL ||
        params.password == NULL || params.server == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_status status = sqli_create(out);
    return status == SQLI_OK ? sqli_connect(*out, &params) : status;
}

static sqli_status execute_sql(sqli_conn_t *connection, const char *sql)
{
    sqli_result_t *result = NULL;
    sqli_status status = sqli_query(connection, sql, &result);
    sqli_result_destroy(result);
    return status;
}

static void *execute_blocked(void *context)
{
    struct blocked_operation *operation = context;
    struct timespec before, after;
    if (clock_gettime(CLOCK_MONOTONIC, &before) != 0) {
        operation->status = SQLI_IO_ERROR;
        return NULL;
    }
    operation->status = execute_sql(operation->connection, operation->sql);
    sqli_status diagnostic_status = sqli_error_get_info(operation->connection, &operation->error);
    if (diagnostic_status != SQLI_OK)
        operation->status = diagnostic_status;
    if (clock_gettime(CLOCK_MONOTONIC, &after) != 0) {
        operation->status = SQLI_IO_ERROR;
        return NULL;
    }
    operation->elapsed_seconds = (double)(after.tv_sec - before.tv_sec) +
        (double)(after.tv_nsec - before.tv_nsec) / nanoseconds_per_second;
    return NULL;
}

static sqli_status verify_rollback(const char *table)
{
    sqli_conn_t *observer = NULL;
    sqli_result_t *rows = NULL;
    sqli_status status = connect_test(&observer);
    if (status != SQLI_OK)
        goto cleanup;
    char sql[sql_capacity];
    snprintf(sql, sizeof(sql), "SELECT amount FROM %s WHERE id = 1", table);
    status = sqli_query(observer, sql, &rows);
    if (status == SQLI_OK)
        status = sqli_result_fetch(rows);
    if (status == SQLI_OK) {
        const size_t amount_column = 0;
        int32_t amount = -1;
        bool is_null;
        status = sqli_result_get_int(rows, amount_column, &amount, &is_null);
        if (status == SQLI_OK && (is_null || amount != 0))
            status = SQLI_ERR;
    }
cleanup:
    sqli_result_destroy(rows);
    sqli_destroy(observer);
    return status;
}

int main(int argc, char **argv)
{
    bool interrupt = argc == 2 && strcmp(argv[1], "--interrupt") == 0;
    if (!interrupt && !(argc == 2 && strcmp(argv[1], "--baseline") == 0)) {
        fprintf(stderr, "Usage: %s --baseline|--interrupt\n", argv[0]);
        return EXIT_FAILURE;
    }
    sqli_conn_t *holder = NULL, *worker = NULL;
    bool created = false, holder_transaction = false, worker_transaction = false;
    char sql[sql_capacity], table[table_capacity];
    snprintf(table, sizeof(table), "sqli_cancel_probe_%ld", (long)getpid());
    sqli_status status = connect_test(&holder);
    if (status != SQLI_OK)
        goto cleanup;
    status = connect_test(&worker);
    if (status != SQLI_OK)
        goto cleanup;
    snprintf(sql, sizeof(sql),
             "CREATE TABLE %s (id INTEGER, amount INTEGER) LOCK MODE ROW", table);
    status = execute_sql(holder, sql);
    if (status != SQLI_OK)
        goto cleanup;
    created = true;
    snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (1, 0)", table);
    status = execute_sql(holder, sql);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_begin(holder);
    if (status != SQLI_OK)
        goto cleanup;
    holder_transaction = true;
    snprintf(sql, sizeof(sql), "UPDATE %s SET amount = 1 WHERE id = 1", table);
    status = execute_sql(holder, sql);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_set_lock_wait(worker, lock_wait_seconds);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_begin(worker);
    if (status != SQLI_OK)
        goto cleanup;
    worker_transaction = true;

    struct blocked_operation operation = {.connection = worker};
    snprintf(operation.sql, sizeof(operation.sql),
             "UPDATE %s SET amount = 2 WHERE id = 1", table);
    pthread_t thread;
    if (pthread_create(&thread, NULL, execute_blocked, &operation) != 0) {
        status = SQLI_IO_ERROR;
        goto cleanup;
    }
    ssize_t sent = 0;
    if (interrupt) {
        struct timespec delay = {.tv_sec = 1};
        while (nanosleep(&delay, &delay) != 0) {
            if (errno != EINTR) {
                status = SQLI_IO_ERROR;
                break;
            }
        }
        if (status == SQLI_OK) {
            const unsigned char request = interrupt_byte;
            sent = send(worker->socket_fd, &request, sizeof(request), MSG_OOB);
            if (sent != (ssize_t)sizeof(request))
                status = SQLI_IO_ERROR;
        }
    }
    /* Join before any ordinary operation or cleanup touches the worker. */
    if (pthread_join(thread, NULL) != 0) {
        fputs("Unable to join protocol worker; refusing concurrent cleanup\n", stderr);
        _exit(EXIT_FAILURE);
    }
    printf("mode=%s sent=%ld status=%s sqlcode=%d isamcode=%d elapsed=%.3f\n",
           interrupt ? "interrupt" : "baseline", (long)sent,
           sqli_status_name(operation.status), operation.error.sqlcode,
           operation.error.isamcode, operation.elapsed_seconds);
    if (status == SQLI_OK && (operation.status == SQLI_OK ||
        !operation.error.has_error || operation.error.sqlcode == 0 ||
        (interrupt && operation.error.sqlcode != interrupted_sqlcode) ||
        (!interrupt && (operation.error.sqlcode != lock_timeout_sqlcode ||
                        operation.error.isamcode != lock_timeout_isamcode))))
        status = SQLI_ERR;
cleanup:
    if (worker_transaction && sqli_rollback(worker) != SQLI_OK)
        status = SQLI_ERR;
    if (holder_transaction && sqli_rollback(holder) != SQLI_OK)
        status = SQLI_ERR;
    if (status == SQLI_OK) {
        status = verify_rollback(table);
        printf("independent_visibility_after_explicit_rollback=%s\n", sqli_status_name(status));
    }
    if (created) {
        snprintf(sql, sizeof(sql), "DROP TABLE %s", table);
        sqli_status cleanup_status = execute_sql(holder, sql);
        if (cleanup_status != SQLI_OK) {
            fprintf(stderr, "Test table cleanup failed: %s (%s)\n", table,
                    sqli_status_name(cleanup_status));
            status = cleanup_status;
        }
    }
    sqli_destroy(worker);
    sqli_destroy(holder);
    if (status != SQLI_OK)
        fprintf(stderr, "Cancel probe failed: %s\n", sqli_status_name(status));
    return status == SQLI_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
