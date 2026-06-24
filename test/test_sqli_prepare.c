/*
 * Phase 4b — prepared statement coverage tests.
 *
 * Exercises bind functions, build_bind_msg, estimate_bind_msg_size,
 * date/decimal parsing helpers, stmt lifecycle, and result column accessors
 * by constructing mock sqli_stmt_t objects.
 */

#include "libsqli/sqli.h"
#include "sqli_internal.h"

#include "unity.h"

#include <string.h>
#include <stdlib.h>

/* ----------------------------------------------------------------
 * Helper: create a mock statement with N allocatable params
 * ---------------------------------------------------------------- */

static sqli_stmt_t *mock_stmt(int num_params)
{
    sqli_stmt_t *s = calloc(1, sizeof(*s));
    TEST_ASSERT_NOT_NULL(s);
    s->socket_fd = -1;
    s->stmt_id = 99;
    s->param_count = num_params;
    s->read_only = false;
    s->executed = false;
    s->result_valid = false;
    memset(&s->result, 0, sizeof(s->result));

    if (num_params > 0) {
        s->param_cap = num_params;
        s->params = calloc((size_t)num_params, sizeof(sqli_bound_param));
        TEST_ASSERT_NOT_NULL(s->params);
    }
    return s;
}

/* ----------------------------------------------------------------
 * sqli_bind_int / sqli_bind_int64 / sqli_bind_double bodies
 * ---------------------------------------------------------------- */

void test_bind_int_success(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_int(s, 1, 42));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_int(s, 2, -100));
    sqli_stmt_destroy(s);
}

void test_bind_int64_success(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_int64(s, 1, (int64_t)9223372036854775807ll));
    sqli_stmt_destroy(s);
}

void test_bind_double_success(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_double(s, 1, 3.14));
    sqli_stmt_destroy(s);
}

void test_bind_int_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_int(s, 0, 1));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_int(s, 3, 1));
    sqli_stmt_destroy(s);
}

void test_bind_int64_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_int64(s, 0, 1));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_int64(s, 3, 1));
    sqli_stmt_destroy(s);
}

void test_bind_double_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_double(s, 0, 1.0));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_double(s, 3, 1.0));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_bind_null_int / null_int64 / null_double
 * ---------------------------------------------------------------- */

void test_bind_null_int_success(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_null_int(s, 1));
    sqli_stmt_destroy(s);
}

void test_bind_null_int64_success(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_null_int64(s, 1));
    sqli_stmt_destroy(s);
}

void test_bind_null_double_success(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_null_double(s, 1));
    sqli_stmt_destroy(s);
}

void test_bind_null_int_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_null_int(s, 0));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_null_int(s, 3));
    sqli_stmt_destroy(s);
}

void test_bind_null_int64_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_null_int64(s, 0));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_null_int64(s, 3));
    sqli_stmt_destroy(s);
}

void test_bind_null_double_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_null_double(s, 0));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_null_double(s, 3));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_bind_null (generic) invalid index
 * ---------------------------------------------------------------- */

void test_bind_null_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_null(s, 0));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_null(s, 3));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_bind_string / date / datetime / interval / bool / decimal
 * invalid index paths
 * ---------------------------------------------------------------- */

void test_bind_string_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_string(s, 0, "x"));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_string(s, 3, "x"));
    sqli_stmt_destroy(s);
}

void test_bind_date_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_date(s, 0, "2026-01-01"));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_date(s, 3, "2026-01-01"));
    sqli_stmt_destroy(s);
}

void test_bind_datetime_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_datetime(s, 0, "2026-01-01"));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_datetime(s, 3, "2026-01-01"));
    sqli_stmt_destroy(s);
}

void test_bind_interval_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_interval(s, 0, "1-2"));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_interval(s, 3, "1-2"));
    sqli_stmt_destroy(s);
}

void test_bind_bool_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_bool(s, 0, true));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_bool(s, 3, false));
    sqli_stmt_destroy(s);
}

void test_bind_decimal_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_decimal(s, 0, "1.5"));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_decimal(s, 3, "1.5"));
    sqli_stmt_destroy(s);
}

void test_bind_bytes_invalid_index(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    uint8_t data[] = {0xDE, 0xAD};
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_bytes(s, 0, data, 2));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_bytes(s, 3, data, 2));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * set_param_bytes edge cases: null value, zero len, oversized
 * ---------------------------------------------------------------- */

void test_bind_bytes_null_value(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_bytes(s, 1, NULL, 4));
    sqli_stmt_destroy(s);
}

void test_bind_bytes_zero_len(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    uint8_t data[] = {1};
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_bytes(s, 1, data, 0));
    sqli_stmt_destroy(s);
}

void test_bind_bytes_success(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_bytes(s, 1, data, 4));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * set_param_string null value edge case
 * ---------------------------------------------------------------- */

void test_bind_string_null_value(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_string(s, 1, NULL));
    sqli_stmt_destroy(s);
}

void test_bind_date_null_value(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_date(s, 1, NULL));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * Bool true/false both branches
 * ---------------------------------------------------------------- */

void test_bind_bool_true_false(void)
{
    sqli_stmt_t *s = mock_stmt(2);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_bool(s, 1, true));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_bool(s, 2, false));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * Rebinding: overwriting an existing string param with a new value
 * (exercises free(p->sval) path)
 * ---------------------------------------------------------------- */

void test_bind_string_rebind(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_string(s, 1, "first"));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_string(s, 1, "second-longer"));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_null(s, 1));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_string(s, 1, "third"));
    sqli_stmt_destroy(s);
}

void test_bind_bytes_rebind(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    uint8_t a[] = {1, 2, 3};
    uint8_t b[] = {4, 5};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_bytes(s, 1, a, 3));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_bytes(s, 1, b, 2));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_string(s, 1, "overwrite"));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * Stmt with 0 params — close/destroy without params array
 * ---------------------------------------------------------------- */

void test_stmt_zero_params_close_destroy(void)
{
    sqli_stmt_t *s = mock_stmt(0);
    TEST_ASSERT_NULL(s->params);
    sqli_stmt_close(s);  /* should not crash when params is NULL */
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_stmt_next / sqli_stmt_result with valid but non-executed stmt
 * ---------------------------------------------------------------- */

void test_stmt_next_not_executed(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->result_valid = false;
    TEST_ASSERT_EQUAL_INT8(0, sqli_stmt_next(s));
    sqli_stmt_destroy(s);
}

void test_stmt_result_not_valid(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->result_valid = false;
    TEST_ASSERT_NULL(sqli_stmt_result(s));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_result_column_name / sqli_result_column_type
 * ---------------------------------------------------------------- */

void test_result_column_name_valid(void)
{
    sqli_result_t r;
    memset(&r, 0, sizeof(r));
    r.column_count = 2;
    r.columns = calloc(2, sizeof(sqli_column_info));
    strcpy(r.columns[0].name, "id");
    strcpy(r.columns[1].name, "name");

    TEST_ASSERT_EQUAL_STRING("id", sqli_result_column_name(&r, 0));
    TEST_ASSERT_EQUAL_STRING("name", sqli_result_column_name(&r, 1));

    free(r.columns);
}

void test_result_column_name_null_result(void)
{
    TEST_ASSERT_NULL(sqli_result_column_name(NULL, 0));
}

void test_result_column_name_negative_index(void)
{
    sqli_result_t r;
    memset(&r, 0, sizeof(r));
    r.column_count = 1;
    TEST_ASSERT_NULL(sqli_result_column_name(&r, -1));
}

void test_result_column_name_out_of_bounds(void)
{
    sqli_result_t r;
    memset(&r, 0, sizeof(r));
    r.column_count = 1;
    TEST_ASSERT_NULL(sqli_result_column_name(&r, 1));
}

void test_result_column_type_valid(void)
{
    sqli_result_t r;
    memset(&r, 0, sizeof(r));
    r.column_count = 1;
    r.columns = calloc(1, sizeof(sqli_column_info));
    r.columns[0].type = SQLI_TYPE_INT;

    TEST_ASSERT_EQUAL_INT((int)SQLI_TYPE_INT, sqli_result_column_type(&r, 0));

    free(r.columns);
}

void test_result_column_type_null_result(void)
{
    TEST_ASSERT_EQUAL_INT(-1, sqli_result_column_type(NULL, 0));
}

void test_result_column_type_negative_index(void)
{
    sqli_result_t r;
    memset(&r, 0, sizeof(r));
    r.column_count = 1;
    TEST_ASSERT_EQUAL_INT(-1, sqli_result_column_type(&r, -1));
}

void test_result_column_type_out_of_bounds(void)
{
    sqli_result_t r;
    memset(&r, 0, sizeof(r));
    r.column_count = 1;
    TEST_ASSERT_EQUAL_INT(-1, sqli_result_column_type(&r, 1));
}

/* ----------------------------------------------------------------
 * sqli_stmt_best_effort_control — exercises the SQ_CLOSE/SQ_RELEASE
 * message construction (socket_fd=-1 avoids actual send)
 * ---------------------------------------------------------------- */

void test_stmt_close_sends_control_msgs(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->socket_fd = -1;
    s->stmt_id = 42;
    /* close with valid stmt_id and fd=-1 should not crash */
    sqli_stmt_close(s);
    TEST_ASSERT_EQUAL_INT(-1, s->stmt_id);
    sqli_stmt_destroy(s);
}

void test_stmt_close_invalid_stmt_id(void)
{
    sqli_stmt_t *s = mock_stmt(0);
    s->socket_fd = -1;
    s->stmt_id = -1;
    sqli_stmt_close(s);  /* should be no-op for invalid stmt_id */
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_stmt_param_needs_lob_streaming — exercises return false paths
 * ---------------------------------------------------------------- */

void test_lob_streaming_null_stmt(void)
{
    /* Cannot call directly (static), but we can verify via sqli_execute
     * null-stmt path which was already tested. Instead, test that
     * a stmt with no server types does not crash. */
    sqli_stmt_t *s = mock_stmt(1);
    s->param_server_types = NULL;
    s->param_server_type_count = 0;
    /* bind a string to param 1 — should succeed without lob check */
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_string(s, 1, "test"));
    sqli_stmt_destroy(s);
}

void test_lob_streaming_null_param_server_types(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->param_server_types = NULL;
    s->param_server_type_count = 0;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_bytes(s, 1, (uint8_t[]){1}, 1));
    sqli_stmt_destroy(s);
}

void test_lob_streaming_null_param(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->param_server_types = calloc(1, sizeof(uint8_t));
    TEST_ASSERT_NOT_NULL(s->param_server_types);
    s->param_server_types[0] = SQLI_TYPE_BLOB;
    s->param_server_type_count = 1;
    /* Bind NULL — lob streaming should return false for null params */
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_null(s, 1));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_execute — no-params path (exercises SQ_ID + SQ_EXECUTE send)
 * With socket_fd=-1, send will fail — exercises error path
 * ---------------------------------------------------------------- */

void test_execute_no_params_sends_id_and_exec(void)
{
    sqli_stmt_t *s = mock_stmt(0);
    s->socket_fd = -1;
    s->stmt_id = 1;
    s->read_only = true;
    s->conn = NULL;
    /* Will fail on send because fd=-1, but exercises the no-param code path.
     * executed stays false because sqli_execute returns early on IO error. */
    sqli_status rc = sqli_execute(s);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_INT8(0, s->executed);
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_execute_with_retry — non-read-only skip path
 * ---------------------------------------------------------------- */

void test_execute_with_retry_nonreadonly_skip(void)
{
    sqli_stmt_t *s = mock_stmt(0);
    s->socket_fd = -1;
    s->stmt_id = 1;
    s->read_only = false;
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    s->conn = &conn;
    /* Non-read-only: should fail immediately without retrying */
    sqli_status rc = sqli_execute_with_retry(s, 2);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_OK, rc);
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_execute with params — exercises estimate_bind_msg_size +
 * build_bind_msg (fd=-1 causes send to fail, covering error path)
 * ---------------------------------------------------------------- */

void test_execute_with_params_int(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->socket_fd = -1;
    s->stmt_id = 1;
    s->read_only = true;
    s->conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_int(s, 1, 42));
    sqli_status rc = sqli_execute(s);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_OK, rc); /* fails on send */
    sqli_stmt_destroy(s);
}

void test_execute_with_params_int64(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->socket_fd = -1;
    s->stmt_id = 1;
    s->read_only = true;
    s->conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_int64(s, 1, (int64_t)123456789012ll));
    sqli_status rc = sqli_execute(s);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_OK, rc);
    sqli_stmt_destroy(s);
}

void test_execute_with_params_double(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->socket_fd = -1;
    s->stmt_id = 1;
    s->read_only = true;
    s->conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_double(s, 1, 2.718));
    sqli_status rc = sqli_execute(s);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_OK, rc);
    sqli_stmt_destroy(s);
}

void test_execute_with_params_string(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->socket_fd = -1;
    s->stmt_id = 1;
    s->read_only = true;
    s->conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_string(s, 1, "hello"));
    sqli_status rc = sqli_execute(s);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_OK, rc);
    sqli_stmt_destroy(s);
}

void test_execute_with_params_null(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->socket_fd = -1;
    s->stmt_id = 1;
    s->read_only = true;
    s->conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_null(s, 1));
    sqli_status rc = sqli_execute(s);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_OK, rc);
    sqli_stmt_destroy(s);
}

void test_execute_with_params_bytes(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->socket_fd = -1;
    s->stmt_id = 1;
    s->read_only = true;
    s->conn = NULL;
    uint8_t data[] = {0xAA, 0xBB};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_bytes(s, 1, data, 2));
    sqli_status rc = sqli_execute(s);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_OK, rc);
    sqli_stmt_destroy(s);
}

void test_execute_with_params_date(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->socket_fd = -1;
    s->stmt_id = 1;
    s->read_only = true;
    s->conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_date(s, 1, "2026-01-15"));
    sqli_status rc = sqli_execute(s);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_OK, rc);
    sqli_stmt_destroy(s);
}

void test_execute_with_params_decimal(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    s->socket_fd = -1;
    s->stmt_id = 1;
    s->read_only = true;
    s->conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_decimal(s, 1, "123.45"));
    sqli_status rc = sqli_execute(s);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_OK, rc);
    sqli_stmt_destroy(s);
}

void test_execute_with_params_multiple(void)
{
    sqli_stmt_t *s = mock_stmt(3);
    s->socket_fd = -1;
    s->stmt_id = 1;
    s->read_only = true;
    s->conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_int(s, 1, 10));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_string(s, 2, "test"));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_null(s, 3));
    sqli_status rc = sqli_execute(s);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_OK, rc);
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_bind_decimal success path
 * ---------------------------------------------------------------- */

void test_bind_decimal_success(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_decimal(s, 1, "99.99"));
    sqli_stmt_destroy(s);
}

void test_bind_decimal_negative(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_decimal(s, 1, "-42.5"));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_bind_datetime / sqli_bind_interval success paths
 * ---------------------------------------------------------------- */

void test_bind_datetime_success(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_datetime(s, 1, "2026-01-01 12:00:00"));
    sqli_stmt_destroy(s);
}

void test_bind_interval_success(void)
{
    sqli_stmt_t *s = mock_stmt(1);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_interval(s, 1, "1-2"));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_execute null stmt
 * ---------------------------------------------------------------- */

void test_prepare_execute_null_stmt(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_execute(NULL));
}

/* ----------------------------------------------------------------
 * sqli_execute_with_retry null stmt / null conn
 * ---------------------------------------------------------------- */

void test_prepare_execute_with_retry_null_stmt(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_execute_with_retry(NULL, 0));
}

void test_prepare_execute_with_retry_null_conn(void)
{
    sqli_stmt_t *s = mock_stmt(0);
    s->conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_execute_with_retry(s, 2));
    sqli_stmt_destroy(s);
}

/* ----------------------------------------------------------------
 * sqli_stmt_next / sqli_stmt_result null stmt
 * ---------------------------------------------------------------- */

void test_prepare_stmt_next_null_stmt(void)
{
    TEST_ASSERT_EQUAL_INT8(0, sqli_stmt_next(NULL));
}

void test_prepare_stmt_result_null_stmt(void)
{
    TEST_ASSERT_NULL(sqli_stmt_result(NULL));
}

/* ----------------------------------------------------------------
 * sqli_result_column_name/type with valid result and negative index
 * ---------------------------------------------------------------- */

void test_result_column_name_zero_count(void)
{
    sqli_result_t r;
    memset(&r, 0, sizeof(r));
    r.column_count = 0;
    TEST_ASSERT_NULL(sqli_result_column_name(&r, 0));
}

void test_result_column_type_zero_count(void)
{
    sqli_result_t r;
    memset(&r, 0, sizeof(r));
    r.column_count = 0;
    TEST_ASSERT_EQUAL_INT(-1, sqli_result_column_type(&r, 0));
}
