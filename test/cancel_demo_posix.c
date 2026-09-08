#define _POSIX_C_SOURCE 200809L
/* Public-API-only cancellation demo; uses its own table in a logged test database. */
#include "libsqli/sqli_cancel.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { sql_capacity = 256, table_capacity = 64, mode_argument = 1,
       lock_wait_seconds = 5, cancel_delay_seconds = 1, amount_parameter = 0,
       id_parameter = 1, value_column = 0 };

struct request_context {
    sqli_cancel_operation *operation;
    sqli_cancel_disposition disposition;
    sqli_status status;
};

static void *request_cancel(void *context)
{
    struct request_context *request = context;
    struct timespec delay = {.tv_sec = cancel_delay_seconds};
    while (nanosleep(&delay, &delay) != 0) {
        int saved_errno = errno;
        if (saved_errno != EINTR) {
            request->status = SQLI_IO_ERROR;
            return NULL;
        }
    }
    request->status = sqli_cancel_operation_request(request->operation, &request->disposition);
    return NULL;
}

static sqli_status run_sql(sqli_conn_t *conn, const char *sql)
{
    sqli_result_t *result = NULL;
    sqli_status status = sqli_query(conn, sql, &result);
    sqli_result_destroy(result);
    return status;
}

static sqli_status read_value(sqli_conn_t *conn, const char *sql, int64_t *value)
{
    sqli_result_t *result = NULL;
    sqli_status status = sqli_query(conn, sql, &result);
    if (status == SQLI_OK)
        status = sqli_result_fetch(result);
    if (status == SQLI_OK) {
        bool is_null = false;
        status = sqli_result_get_int64(result, value_column, value, &is_null);
        if (status == SQLI_OK && is_null)
            status = SQLI_NULL_VALUE;
    }
    sqli_result_destroy(result);
    return status;
}

static sqli_status run_cancelable_sql(sqli_conn_t *conn, const char *sql)
{
    sqli_stmt_t *stmt = NULL;
    sqli_cancel_operation *operation = NULL;
    sqli_status status = sqli_prepare(conn, sql, NULL, &stmt);
    if (status == SQLI_OK)
        status = sqli_cancel_operation_create(&operation);
    if (status == SQLI_OK)
        status = sqli_execute_cancelable(stmt, operation);
    if (status == SQLI_OK) {
        sqli_cancel_snapshot snapshot;
        status = sqli_cancel_operation_snapshot(operation, &snapshot);
        if (status == SQLI_OK && (snapshot.outcome != SQLI_CANCEL_EXECUTED ||
                                 snapshot.connection_discarded))
            status = SQLI_ERR;
    }
    sqli_stmt_destroy(stmt);
    if (sqli_cancel_operation_destroy(operation) != SQLI_OK)
        status = SQLI_ERR;
    return status;
}

static const char *outcome_name(sqli_cancel_outcome outcome)
{
    switch (outcome) {
    case SQLI_CANCEL_NOT_EXECUTED: return "not-executed";
    case SQLI_CANCEL_EXECUTED: return "executed";
    case SQLI_CANCEL_INTERRUPTED: return "server-interrupted";
    case SQLI_CANCEL_FAILED: return "failed";
    case SQLI_CANCEL_UNKNOWN: return "unknown";
    }
    return "invalid";
}

int main(int argc, char **argv)
{
    const char *mode = argc == 2 ? argv[mode_argument] : getenv("SQLI_CANCEL_DEMO_MODE");
    if (mode == NULL)
        mode = "--interrupt";
    bool interrupt = strcmp(mode, "--interrupt") == 0;
    bool prestart = strcmp(mode, "--prestart") == 0;
    bool complete = strcmp(mode, "--complete") == 0;
    if (argc > 2 || (!interrupt && !prestart && !complete && strcmp(mode, "--baseline") != 0)) {
        fputs("Usage: sqli_cancel_demo --interrupt|--prestart|--complete|--baseline\n", stderr);
        return EXIT_FAILURE;
    }
    const char *database = getenv("SQLI_TEST_LOGGING_DB");
    sqli_connect_params params = {
        .hostname = getenv("SQLI_TEST_HOST"), .service = getenv("SQLI_TEST_PORT"),
        .server = getenv("SQLI_TEST_SERVER"), .username = getenv("SQLI_TEST_USER"),
        .password = getenv("SQLI_TEST_PASS"),
        .database = database != NULL && database[0] != '\0' ? database : "sqli_log_test",
        .client_locale = getenv("SQLI_TEST_CLIENT_LOCALE"),
        .db_locale = getenv("SQLI_TEST_DB_LOCALE")
    };
    sqli_conn_t *holder = NULL, *worker = NULL;
    sqli_pool_t *pool = NULL;
    sqli_stmt_t *stmt = NULL;
    sqli_cancel_operation *operation = NULL;
    bool created = false, holder_transaction = false;
    char table[table_capacity], sql[sql_capacity];
    int table_length = snprintf(table, sizeof(table), "sqli_cancel_demo_%ld", (long)getpid());
    if (table_length < 0 || (size_t)table_length >= sizeof(table))
        return EXIT_FAILURE;
    sqli_status status = sqli_create(&holder);
    if (status == SQLI_OK)
        status = sqli_connect(holder, &params);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_pool_create(&pool, &params, 1);
    if (status == SQLI_OK)
        status = sqli_pool_acquire(pool, &worker);
    if (status != SQLI_OK)
        goto cleanup;
    snprintf(sql, sizeof(sql), "CREATE TABLE %s (id INTEGER, amount INTEGER) LOCK MODE ROW", table);
    status = run_sql(holder, sql);
    if (status != SQLI_OK)
        goto cleanup;
    created = true;
    snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (1, 0)", table);
    status = run_sql(holder, sql);
    if (status != SQLI_OK)
        goto cleanup;
    if (!complete) {
        status = sqli_begin(holder);
        if (status != SQLI_OK)
            goto cleanup;
        holder_transaction = true;
        snprintf(sql, sizeof(sql), "UPDATE %s SET amount = 1 WHERE id = 1", table);
        status = run_sql(holder, sql);
        if (status != SQLI_OK)
            goto cleanup;
    }
    status = sqli_set_lock_wait(worker, lock_wait_seconds);
    if (status != SQLI_OK)
        goto cleanup;
    const char *session_sql = "SELECT DBINFO('sessionid') FROM systables WHERE tabid = 1";
    int64_t old_session = 0, new_session = 0;
    status = read_value(worker, session_sql, &old_session);
    if (status != SQLI_OK)
        goto cleanup;
    snprintf(sql, sizeof(sql), "UPDATE %s SET amount = ? WHERE id = ?", table);
    status = sqli_prepare(worker, sql, NULL, &stmt);
    if (status == SQLI_OK)
        status = sqli_bind_int(stmt, amount_parameter, 2);
    if (status == SQLI_OK)
        status = sqli_bind_int(stmt, id_parameter, 1);
    if (status == SQLI_OK)
        status = sqli_cancel_operation_create(&operation);
    if (status != SQLI_OK)
        goto cleanup;
    struct request_context request = {.operation = operation};
    if (prestart) {
        status = sqli_cancel_operation_request(operation, &request.disposition);
        if (status != SQLI_OK)
            goto cleanup;
    }
    pthread_t requester;
    if (interrupt && pthread_create(&requester, NULL, request_cancel, &request) != 0) {
        status = SQLI_IO_ERROR;
        goto cleanup;
    }
    sqli_status execution_status = sqli_execute_cancelable(stmt, operation);
    if (interrupt && pthread_join(requester, NULL) != 0) {
        fputs("Cannot join requester; refusing concurrent cleanup\n", stderr);
        _exit(EXIT_FAILURE);
    }
    sqli_cancel_snapshot snapshot;
    status = sqli_cancel_operation_snapshot(operation, &snapshot);
    if (status != SQLI_OK)
        goto cleanup;
    printf("mode=%s status=%s outcome=%s sqlcode=%d send_attempted=%d discarded=%d disposal=%s\n",
        mode, sqli_status_name(execution_status), outcome_name(snapshot.outcome),
        snapshot.diagnostic.sqlcode, snapshot.send_attempted, snapshot.connection_discarded,
        sqli_status_name(snapshot.disposal_status));
    if (request.status != SQLI_OK || snapshot.disposal_status != SQLI_OK ||
        ((interrupt || prestart) && execution_status != SQLI_CANCELED) ||
        (interrupt && (snapshot.outcome != SQLI_CANCEL_INTERRUPTED || !snapshot.connection_discarded)) ||
        (prestart && (snapshot.outcome != SQLI_CANCEL_NOT_EXECUTED || snapshot.send_attempted)) ||
        (complete && execution_status != SQLI_OK) ||
        (!interrupt && !prestart && !complete && snapshot.diagnostic.sqlcode != -244)) {
        status = SQLI_ERR;
        goto cleanup;
    }
    sqli_stmt_destroy(stmt);
    stmt = NULL;
    status = sqli_pool_release(pool, worker);
    if (status != SQLI_OK)
        goto cleanup; /* Failed release retains the borrowed pointer. */
    worker = NULL;
    status = sqli_pool_acquire(pool, &worker);
    if (status == SQLI_OK)
        status = read_value(worker, session_sql, &new_session);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_cancel_operation_request(operation, &request.disposition);
    if (status != SQLI_OK || request.disposition != SQLI_CANCEL_COMPLETED ||
        ((old_session != new_session) != snapshot.connection_discarded)) {
        status = SQLI_ERR;
        goto cleanup;
    }
    if (holder_transaction) {
        status = sqli_rollback(holder);
        if (status != SQLI_OK)
            goto cleanup;
        holder_transaction = false;
    }
    int64_t amount = -1;
    snprintf(sql, sizeof(sql), "SELECT amount FROM %s WHERE id = 1", table);
    status = read_value(holder, sql, &amount);
    if (status == SQLI_OK && amount != (complete ? 2 : 0))
        status = SQLI_ERR;
    printf("same_session=%d independent_amount=%lld verification=%s\n",
        old_session == new_session, (long long)amount, sqli_status_name(status));
    if (status == SQLI_OK && complete) {
        snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (2, 3)", table);
        status = run_cancelable_sql(worker, sql);
        if (status == SQLI_OK) {
            snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE id = 2", table);
            status = run_cancelable_sql(worker, sql);
        }
        printf("public_insert_delete=%s\n", sqli_status_name(status));
    }
cleanup:
    sqli_stmt_destroy(stmt);
    if (sqli_cancel_operation_destroy(operation) != SQLI_OK)
        status = SQLI_ERR;
    if (worker != NULL && sqli_pool_release(pool, worker) != SQLI_OK)
        status = SQLI_ERR;
    if (sqli_pool_destroy(pool) != SQLI_OK)
        status = SQLI_ERR;
    if (holder_transaction && sqli_rollback(holder) != SQLI_OK)
        status = SQLI_ERR;
    if (created) {
        snprintf(sql, sizeof(sql), "DROP TABLE %s", table);
        if (run_sql(holder, sql) != SQLI_OK) {
            fprintf(stderr, "Test table cleanup failed: %s\n", table);
            status = SQLI_ERR;
        }
    }
    sqli_destroy(holder);
    if (status != SQLI_OK)
        fprintf(stderr, "Cancel demo failed: %s\n", sqli_status_name(status));
    return status == SQLI_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
