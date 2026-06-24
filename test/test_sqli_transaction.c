/*
 * Phase 5 unit tests for transaction control.
 *
 * Tests sqli_begin, sqli_commit, sqli_rollback, autocommit, isolation level,
 * and sqli_in_transaction via the internal sqli_conn struct.
 */

#include "unity.h"
#include "libsqli/sqli.h"
#include "sqli_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ----------------------------------------------------------------
 * Helper: create a minimal mock connection for state testing
 * ---------------------------------------------------------------- */

static sqli_conn_t *make_mock_conn(void)
{
    sqli_conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;

    c->state = SQLI_CONN_READY;
    c->socket_fd = -1; /* no real socket, but state is ready */
    c->database_open = 1;
    c->in_transaction = 0;
    c->autocommit = 1;
    c->isolation = SQLI_TXN_ISOLATION_COMMITTED;

    return c;
}

/* ----------------------------------------------------------------
 * sqli_begin tests
 * ---------------------------------------------------------------- */

void test_begin_null_conn(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_begin(NULL));
}

void test_begin_not_ready_conn(void)
{
    sqli_conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }

    c->state = SQLI_CONN_CLOSED;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_begin(c));

    free(c);
}

void test_begin_already_in_transaction(void)
{
    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }

    /* Mark as already in transaction */
    c->in_transaction = 1;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_begin(c));

    /* State should be unchanged */
    TEST_ASSERT_TRUE(c->in_transaction);

    free(c);
}

void test_begin_socket_not_connected(void)
{
    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }

    /* socket_fd is -1, verify_txn_ready should catch this */
    c->socket_fd = -1;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_begin(c));

    free(c);
}

/* ----------------------------------------------------------------
 * sqli_commit tests
 * ---------------------------------------------------------------- */

void test_commit_null_conn(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_commit(NULL));
}

void test_commit_no_active_transaction(void)
{
    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }

    c->in_transaction = 0;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_commit(c));

    free(c);
}

/* ----------------------------------------------------------------
 * sqli_rollback tests
 * ---------------------------------------------------------------- */

void test_rollback_null_conn(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_rollback(NULL));
}

void test_rollback_no_active_transaction(void)
{
    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }

    c->in_transaction = 0;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_rollback(c));

    free(c);
}

/* ----------------------------------------------------------------
 * Autocommit tests
 * ---------------------------------------------------------------- */

void test_autocommit_null_conn(void)
{
    TEST_ASSERT_EQUAL_INT(0, sqli_get_autocommit(NULL));
}

void test_autocommit_default(void)
{
    sqli_conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }

    c->state = SQLI_CONN_READY;
    c->autocommit = 1;

    TEST_ASSERT_EQUAL_INT(1, sqli_get_autocommit(c));

    sqli_status rc = sqli_set_autocommit(c, 0);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_INT(0, sqli_get_autocommit(c));

    rc = sqli_set_autocommit(c, 1);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, sqli_get_autocommit(c));

    free(c);
}

void test_autocommit_off_with_active_txn(void)
{
    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }

    c->in_transaction = 1;
    sqli_set_autocommit(c, 0);

    /* Transaction should be ended when autocommit is turned off */
    TEST_ASSERT_EQUAL_INT(0, c->in_transaction);
    TEST_ASSERT_EQUAL_INT(0, sqli_get_autocommit(c));

    free(c);
}

/* ----------------------------------------------------------------
 * Isolation level tests
 * ---------------------------------------------------------------- */

void test_isolation_null_conn(void)
{
    sqli_isolation_level level = sqli_get_isolation_level(NULL);
    TEST_ASSERT_EQUAL_INT(SQLI_TXN_ISOLATION_COMMITTED, (int)level);
}

void test_isolation_default(void)
{
    sqli_conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }

    c->state = SQLI_CONN_READY;
    c->isolation = SQLI_TXN_ISOLATION_COMMITTED;

    TEST_ASSERT_EQUAL_INT(SQLI_TXN_ISOLATION_COMMITTED, (int)sqli_get_isolation_level(c));

    free(c);
}

void test_isolation_set_get(void)
{
    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_set_isolation_level(c, SQLI_TXN_ISOLATION_REPEATABLE_READ));
    TEST_ASSERT_EQUAL_INT(SQLI_TXN_ISOLATION_REPEATABLE_READ, (int)sqli_get_isolation_level(c));

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_set_isolation_level(c, SQLI_TXN_ISOLATION_SERIALIZABLE));
    TEST_ASSERT_EQUAL_INT(SQLI_TXN_ISOLATION_SERIALIZABLE, (int)sqli_get_isolation_level(c));

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_set_isolation_level(c, SQLI_TXN_ISOLATION_COMMITTED));
    TEST_ASSERT_EQUAL_INT(SQLI_TXN_ISOLATION_COMMITTED, (int)sqli_get_isolation_level(c));

    free(c);
}

void test_isolation_invalid(void)
{
    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }

    /* Out of range */
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_set_isolation_level(c, 5));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_set_isolation_level(c, -1));

    free(c);
}

void test_isolation_in_transaction(void)
{
    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }

    c->in_transaction = 1;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_set_isolation_level(c, SQLI_TXN_ISOLATION_SERIALIZABLE));

    free(c);
}

/* ----------------------------------------------------------------
 * sqli_in_transaction tests
 * ---------------------------------------------------------------- */

void test_in_transaction_null(void)
{
    TEST_ASSERT_EQUAL_INT(0, sqli_in_transaction(NULL));
}

void test_in_transaction_state(void)
{
    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }

    TEST_ASSERT_EQUAL_INT(0, sqli_in_transaction(c));

    c->in_transaction = 1;
    TEST_ASSERT_EQUAL_INT(1, sqli_in_transaction(c));

    c->in_transaction = 0;
    TEST_ASSERT_EQUAL_INT(0, sqli_in_transaction(c));

    free(c);
}

/* ----------------------------------------------------------------
 * Batch protocol tests
 * ---------------------------------------------------------------- */

void test_batch_begin_null_conn(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_batch_begin(NULL));
}

void test_batch_end_null_conn(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_batch_end(NULL));
}

void test_batch_in_state_null(void)
{
    TEST_ASSERT_EQUAL_INT(0, sqli_in_batch(NULL));
}

void test_batch_begin_end_sends_opcodes(void)
{
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        TEST_IGNORE_MESSAGE("socketpair unavailable");
        return;
    }

    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        close(sv[0]);
        close(sv[1]);
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }
    c->socket_fd = sv[0];

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_batch_begin(c));
    TEST_ASSERT_EQUAL_INT(1, sqli_in_batch(c));

    uint8_t b[2] = {0};
    ssize_t n = read(sv[1], b, sizeof(b));
    TEST_ASSERT_EQUAL_INT((int)sizeof(b), (int)n);
    TEST_ASSERT_EQUAL_UINT8(0x00, b[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_BATCHSTART, b[1]);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_batch_end(c));
    TEST_ASSERT_EQUAL_INT(0, sqli_in_batch(c));

    n = read(sv[1], b, sizeof(b));
    TEST_ASSERT_EQUAL_INT((int)sizeof(b), (int)n);
    TEST_ASSERT_EQUAL_UINT8(0x00, b[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_BATCHEND, b[1]);

    close(sv[0]);
    close(sv[1]);
    free(c);
}

void test_batch_begin_twice_invalid(void)
{
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        TEST_IGNORE_MESSAGE("socketpair unavailable");
        return;
    }

    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        close(sv[0]);
        close(sv[1]);
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }
    c->socket_fd = sv[0];

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_batch_begin(c));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_batch_begin(c));
    TEST_ASSERT_EQUAL_INT(1, sqli_in_batch(c));

    close(sv[0]);
    close(sv[1]);
    free(c);
}

void test_batch_end_without_begin_invalid(void)
{
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        TEST_IGNORE_MESSAGE("socketpair unavailable");
        return;
    }

    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        close(sv[0]);
        close(sv[1]);
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }
    c->socket_fd = sv[0];

    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_batch_end(c));
    TEST_ASSERT_EQUAL_INT(0, sqli_in_batch(c));

    close(sv[0]);
    close(sv[1]);
    free(c);
}

void test_batch_execute_null_params(void)
{
    const char *sqls[1] = {"SELECT 1"};
    sqli_batch_result_t *batch = NULL;

    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_batch_execute(NULL, sqls, 1, &batch));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_batch_execute((sqli_conn_t *)1, NULL, 1, &batch));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_batch_execute((sqli_conn_t *)1, sqls, 1, NULL));
}

void test_batch_execute_not_ready_conn(void)
{
    sqli_conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }
    c->state = SQLI_CONN_CLOSED;
    c->socket_fd = -1;

    const char *sqls[1] = {"SELECT 1"};
    sqli_batch_result_t *batch = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_batch_execute(c, sqls, 1, &batch));
    TEST_ASSERT_NULL(batch);
    free(c);
}

void test_batch_result_accessors_null_safe(void)
{
    sqli_batch_item_result item;
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)sqli_batch_result_count(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)sqli_batch_result_success_count(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)sqli_batch_result_error_count(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_batch_result_item(NULL, 0, &item));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_batch_result_item(NULL, 0, NULL));
    sqli_batch_result_destroy(NULL);
}

static size_t build_done_eot(uint8_t *buf)
{
    size_t p = 0;
    buf[p++] = 0; buf[p++] = SQLI_SQ_DONE;
    buf[p++] = 0; buf[p++] = 0; /* warnings */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; /* rows_affected */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; /* row_id */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; /* errd1 */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; /* errd2 */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; /* errd3 */
    buf[p++] = 0; buf[p++] = SQLI_SQ_EOT;
    return p;
}

void test_savepoint_set_writes_opcode_payload_and_flag(void)
{
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        TEST_IGNORE_MESSAGE("socketpair unavailable");
        return;
    }

    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        close(sv[0]);
        close(sv[1]);
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }
    c->socket_fd = sv[0];
    c->autocommit = 0;
    c->in_transaction = 1;

    uint8_t reply[64];
    size_t rn = build_done_eot(reply);
    TEST_ASSERT_EQUAL_INT((int)rn, (int)write(sv[1], reply, rn));

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_savepoint_set(c, "sp1", true));

    uint8_t got[32] = {0};
    ssize_t n = read(sv[1], got, sizeof(got));
    TEST_ASSERT_TRUE(n >= 10);
    TEST_ASSERT_EQUAL_UINT8(0, got[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_SQLISETSVPT, got[1]);
    TEST_ASSERT_EQUAL_UINT8(0, got[2]);
    TEST_ASSERT_EQUAL_UINT8(3, got[3]); /* len(sp1) */
    TEST_ASSERT_EQUAL_UINT8('s', got[4]);
    TEST_ASSERT_EQUAL_UINT8('p', got[5]);
    TEST_ASSERT_EQUAL_UINT8('1', got[6]);
    TEST_ASSERT_EQUAL_UINT8(0, got[7]); /* odd-length pad */
    TEST_ASSERT_EQUAL_UINT8(0, got[8]);
    TEST_ASSERT_EQUAL_UINT8(1, got[9]); /* unique flag */

    close(sv[0]);
    close(sv[1]);
    free(c);
}

void test_savepoint_release_and_rollback_write_expected_opcodes(void)
{
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        TEST_IGNORE_MESSAGE("socketpair unavailable");
        return;
    }

    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        close(sv[0]);
        close(sv[1]);
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }
    c->socket_fd = sv[0];
    c->autocommit = 0;
    c->in_transaction = 1;

    uint8_t reply[64];
    size_t rn = build_done_eot(reply);
    TEST_ASSERT_EQUAL_INT((int)rn, (int)write(sv[1], reply, rn));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_savepoint_release(c, "abc"));

    uint8_t first[32] = {0};
    ssize_t n = read(sv[1], first, sizeof(first));
    TEST_ASSERT_TRUE(n >= 8);
    TEST_ASSERT_EQUAL_UINT8(0, first[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_SQLIRELSVPT, first[1]);

    TEST_ASSERT_EQUAL_INT((int)rn, (int)write(sv[1], reply, rn));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_savepoint_rollback(c, "abc"));

    uint8_t second[32] = {0};
    n = read(sv[1], second, sizeof(second));
    TEST_ASSERT_TRUE(n >= 8);
    TEST_ASSERT_EQUAL_UINT8(0, second[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_SQLIRBACKSVPT, second[1]);

    close(sv[0]);
    close(sv[1]);
    free(c);
}

void test_savepoint_rejected_in_autocommit(void)
{
    sqli_conn_t *c = make_mock_conn();
    if (c == NULL) {
        TEST_IGNORE_MESSAGE("allocation failed");
        return;
    }
    c->autocommit = 1;
    c->in_transaction = 0;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_savepoint_set(c, "sp1", false));
    free(c);
}
