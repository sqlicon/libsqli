/*
 * Phase 4 unit tests for type encoding and prepared statements.
 *
 * Tests DATE/DATETIME/DECIMAL encoding, row extraction, and column accessors.
 */

#include "unity.h"
#include "libsqli/sqli.h"
#include "sqli_internal.h"

/* Declare sqli_decode_decimal for tests */
int sqli_decode_decimal(const uint8_t *buf, size_t buf_size,
                        uint8_t precision, uint8_t *scale,
                        int *negative, uint8_t *digits);

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <sys/time.h>

static uint64_t tv_now_ms(void);

/* ----------------------------------------------------------------
 * DATE encoding/decoding tests
 * ---------------------------------------------------------------- */

void test_encode_decode_date_epoch(void)
{
    /* Day 0 = Informix epoch (1899-12-30) */
    int32_t encoded = sqli_encode_date(0);
    TEST_ASSERT_EQUAL_INT32(0, encoded);
    TEST_ASSERT_EQUAL_INT32(0, sqli_decode_date(encoded));
}

void test_encode_decode_date_positive(void)
{
    /* 2024-01-01 is approximately 18756 days after 1899-12-30 */
    int32_t days = 18756;
    int32_t encoded = sqli_encode_date(days);
    TEST_ASSERT_EQUAL_INT32(days, sqli_decode_date(encoded));
}

void test_encode_decode_date_negative(void)
{
    /* Before epoch: negative days */
    int32_t days = -10;
    int32_t encoded = sqli_encode_date(days);
    TEST_ASSERT_EQUAL_INT32(days, sqli_decode_date(encoded));
}

/* ----------------------------------------------------------------
 * DATETIME encoding/decoding tests (spec §7.5 — BCD decimal format)
 * ---------------------------------------------------------------- */

void test_encode_decode_datetime_full(void)
{
    uint8_t buf[16];
    size_t n = sqli_encode_datetime(2024, 6, 15, 14, 30, 45, 0, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    int year, month, day, hour, minute, second;
    unsigned int frac;
    sqli_decode_datetime(buf, n, &year, &month, &day,
                         &hour, &minute, &second, &frac);

    TEST_ASSERT_EQUAL_INT(2024, year);
    TEST_ASSERT_EQUAL_INT(6, month);
    TEST_ASSERT_EQUAL_INT(15, day);
    TEST_ASSERT_EQUAL_INT(14, hour);
    TEST_ASSERT_EQUAL_INT(30, minute);
    TEST_ASSERT_EQUAL_INT(45, second);
}

void test_encode_decode_datetime_exact_year(void)
{
    uint8_t buf[16];
    size_t n = sqli_encode_datetime(1970, 1, 1, 0, 0, 0, 0, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    int year, month, day, hour, minute, second;
    unsigned int frac;
    sqli_decode_datetime(buf, n, &year, &month, &day,
                         &hour, &minute, &second, &frac);

    TEST_ASSERT_EQUAL_INT(1970, year);
    TEST_ASSERT_EQUAL_INT(1, month);
    TEST_ASSERT_EQUAL_INT(1, day);
    TEST_ASSERT_EQUAL_INT(0, hour);
    TEST_ASSERT_EQUAL_INT(0, minute);
    TEST_ASSERT_EQUAL_INT(0, second);
}

void test_encode_decode_datetime_time_only(void)
{
    /* hour=12, others 0 — check hour is preserved */
    uint8_t buf[16];
    size_t n = sqli_encode_datetime(1970, 1, 1, 12, 0, 0, 0, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    int year, month, day, hour, minute, second;
    unsigned int frac;
    sqli_decode_datetime(buf, n, &year, &month, &day,
                         &hour, &minute, &second, &frac);

    TEST_ASSERT_EQUAL_INT(12, hour);
    TEST_ASSERT_EQUAL_INT(0, minute);
    TEST_ASSERT_EQUAL_INT(0, second);
}

void test_encode_decode_datetime_null_ptrs(void)
{
    int year, month, day, hour, minute, second;
    unsigned int frac;

    /* NULL buf should return defaults */
    sqli_decode_datetime(NULL, 0, &year, &month, &day,
                         &hour, &minute, &second, &frac);
    TEST_ASSERT_EQUAL_INT(1970, year);
    TEST_ASSERT_EQUAL_INT(1, month);
    TEST_ASSERT_EQUAL_INT(1, day);
    TEST_ASSERT_EQUAL_INT(0, hour);
    TEST_ASSERT_EQUAL_INT(0, minute);
    TEST_ASSERT_EQUAL_INT(0, second);
}

void test_encode_decode_datetime_boundaries(void)
{
    uint8_t buf[16];
    size_t n = sqli_encode_datetime(9999, 12, 31, 23, 59, 59, 0, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    int year, month, day, hour, minute, second;
    unsigned int frac;
    sqli_decode_datetime(buf, n, &year, &month, &day,
                         &hour, &minute, &second, &frac);
    TEST_ASSERT_EQUAL_INT(9999, year);
    TEST_ASSERT_EQUAL_INT(12, month);
    TEST_ASSERT_EQUAL_INT(31, day);
    TEST_ASSERT_EQUAL_INT(23, hour);
    TEST_ASSERT_EQUAL_INT(59, minute);
    TEST_ASSERT_EQUAL_INT(59, second);
}

/* ----------------------------------------------------------------
 * DECIMAL encoding/decoding tests
 * ---------------------------------------------------------------- */

void test_encode_decimal_positive(void)
{
    uint8_t buf[32];
    uint8_t digits[5] = {1, 2, 3, 4, 5};

    /* 123.45 -> precision=5, scale=2, negative=0 */
    /* exp=(5-2)-1=2; exp_byte=((2+64)&0x7F)|0x80=0xC2; bcd_len=3; total=6 */
    size_t n = sqli_encode_decimal(buf, sizeof(buf), 5, 2, 0, digits);
    TEST_ASSERT_EQUAL_INT(6, n);
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);    /* high byte of 2-byte length */
    TEST_ASSERT_EQUAL_UINT8(0xC2, buf[2]); /* exponent byte: exp=2, positive */
    TEST_ASSERT_EQUAL_UINT8(0x12, buf[3]); /* BCD: digits 1,2 */
    TEST_ASSERT_EQUAL_UINT8(0x34, buf[4]); /* BCD: digits 3,4 */
    TEST_ASSERT_EQUAL_UINT8(0x50, buf[5]); /* BCD: digit 5, padded */
}

void test_encode_decimal_negative(void)
{
    uint8_t buf[32];
    uint8_t digits[4] = {9, 8, 7, 6};

    /* -98.76 -> precision=4, scale=2, negative=1 */
    /* exp=(4-2)-1=1; exp_byte=((1+64)&0x7F)|0x00=0x41; bcd_len=2; total=5 */
    size_t n = sqli_encode_decimal(buf, sizeof(buf), 4, 2, 1, digits);
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_UINT8(0x41, buf[2]); /* exponent byte: exp=1, negative */
}

void test_encode_decimal_null_buf(void)
{
    uint8_t digits[4] = {1, 2, 3, 4};
    size_t n = sqli_encode_decimal(NULL, 0, 4, 2, 0, digits);
    TEST_ASSERT_EQUAL_INT(0, n);
}

void test_encode_decimal_too_small_buf(void)
{
    uint8_t buf[1];
    uint8_t digits[4] = {1, 2, 3, 4};
    size_t n = sqli_encode_decimal(buf, 1, 4, 2, 0, digits);
    TEST_ASSERT_EQUAL_INT(0, n);
}

void test_encode_decimal_invalid_precision(void)
{
    uint8_t buf[32];
    uint8_t digits[4] = {1, 2, 3, 4};

    TEST_ASSERT_EQUAL_INT(0, sqli_encode_decimal(buf, sizeof(buf), 0, 0, 0, digits));
    TEST_ASSERT_EQUAL_INT(0, sqli_encode_decimal(buf, sizeof(buf), 33, 0, 0, digits));
    TEST_ASSERT_EQUAL_INT(0, sqli_encode_decimal(buf, sizeof(buf), 4, 5, 0, digits));
}

void test_encode_decode_decimal_roundtrip(void)
{
    uint8_t buf[32];
    uint8_t digits[3] = {3, 1, 4};

    /* 3.14 -> precision=3, scale=2 */
    size_t n = sqli_encode_decimal(buf, sizeof(buf), 3, 2, 0, digits);
    TEST_ASSERT_GREATER_THAN(0, n);

    uint8_t scale;
    int negative;
    uint8_t out_digits[15];

    /* Pass precision=3 as input */
    int decoded = sqli_decode_decimal(buf, n, 3, &scale, &negative, out_digits);
    TEST_ASSERT_EQUAL_INT(3, decoded);
    TEST_ASSERT_EQUAL_UINT8(2, scale);
    TEST_ASSERT_EQUAL_INT(0, negative);

    /* Digits should match */
    TEST_ASSERT_EQUAL_UINT8(3, out_digits[0]);
    TEST_ASSERT_EQUAL_UINT8(1, out_digits[1]);
    TEST_ASSERT_EQUAL_UINT8(4, out_digits[2]);
}

void test_decode_decimal_with_precision(void)
{
    uint8_t buf[32];
    uint8_t digits[3] = {3, 1, 4};

    /* 3.14 -> precision=3, scale=2 */
    size_t n = sqli_encode_decimal(buf, sizeof(buf), 3, 2, 0, digits);
    TEST_ASSERT_GREATER_THAN(0, n);

    uint8_t scale;
    int negative;
    uint8_t out_digits[15];

    /* Pass precision=3 as input */
    int decoded = sqli_decode_decimal(buf, n, 3, &scale, &negative, out_digits);
    TEST_ASSERT_EQUAL_INT(3, decoded);
    TEST_ASSERT_EQUAL_UINT8(2, scale);
    TEST_ASSERT_EQUAL_INT(0, negative);
    TEST_ASSERT_EQUAL_UINT8(3, out_digits[0]);
    TEST_ASSERT_EQUAL_UINT8(1, out_digits[1]);
    TEST_ASSERT_EQUAL_UINT8(4, out_digits[2]);
}

/* ----------------------------------------------------------------
 * Row extraction tests
 * ---------------------------------------------------------------- */

void test_extract_int_from_tuple(void)
{
    /* Build a mock tuple: 2 INT columns, values 42 and -7 */
    uint8_t tuple[8];
    /* Column 0: 4 bytes, big-endian */
    tuple[0] = 0; tuple[1] = 0; tuple[2] = 0; tuple[3] = 42;
    /* Column 1: 4 bytes, big-endian (negative: two's complement) */
    tuple[4] = (uint8_t)(-7 >> 24); tuple[5] = (uint8_t)(-7 >> 16);
    tuple[6] = (uint8_t)(-7 >> 8); tuple[7] = (uint8_t)(-7);

    sqli_column_info col;
    memset(&col, 0, sizeof(col));
    col.type = SQLI_TYPE_INT;
    col.col_start_pos = 0;

    uint8_t buf[4];
    size_t len = sizeof(buf);
    sqli_status rc = extract_value_from_tuple(&col, tuple, sizeof(tuple), buf, &len);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_INT(4, len);

    int32_t val = (int32_t)((uint32_t)(buf[0] << 24) | (uint32_t)(buf[1] << 16) |
                            (uint32_t)(buf[2] << 8) | (uint32_t)buf[3]);
    TEST_ASSERT_EQUAL_INT32(42, val);
}

void test_extract_bigint_from_tuple(void)
{
    uint8_t tuple[8];
    memset(tuple, 0, sizeof(tuple));
    tuple[0] = 0x01; tuple[1] = 0x23; tuple[2] = 0x45; tuple[3] = 0x67;
    tuple[4] = 0x89; tuple[5] = 0xAB; tuple[6] = 0xCD; tuple[7] = 0xEF;

    sqli_column_info col;
    memset(&col, 0, sizeof(col));
    col.type = SQLI_TYPE_BIGINT;
    col.col_start_pos = 0;

    uint8_t buf[8];
    size_t len = sizeof(buf);
    sqli_status rc = extract_value_from_tuple(&col, tuple, sizeof(tuple), buf, &len);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_INT(8, len);
    TEST_ASSERT_EQUAL_UINT8(0xEF, buf[7]);
}

void test_extract_float_from_tuple(void)
{
    /* 1.0 in IEEE 754 big-endian */
    uint8_t tuple[8] = {0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    sqli_column_info col;
    memset(&col, 0, sizeof(col));
    col.type = SQLI_TYPE_FLOAT;
    col.col_start_pos = 0;

    uint8_t buf[8];
    size_t len = sizeof(buf);
    sqli_status rc = extract_value_from_tuple(&col, tuple, sizeof(tuple), buf, &len);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_INT(8, len);

    /* Verify the value is 1.0 */
    uint8_t native[8];
    for (int i = 0; i < 8; i++)
        native[i] = buf[7 - i];
    double val;
    memcpy(&val, native, 8);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, (float)val);
}

void test_extract_varchar_from_tuple(void)
{
    /* Length-prefixed string: [1B len=4][data="ABCD"] */
    uint8_t tuple[5];
    tuple[0] = 4; /* len = 4 */
    tuple[1] = 'A'; tuple[2] = 'B'; tuple[3] = 'C'; tuple[4] = 'D';

    sqli_column_info col;
    memset(&col, 0, sizeof(col));
    col.type = SQLI_TYPE_VARCHAR;
    col.col_start_pos = 0;

    uint8_t buf[8];
    size_t len = sizeof(buf);
    sqli_status rc = extract_value_from_tuple(&col, tuple, 5, buf, &len);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_INT(4, len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(buf, "ABCD", 4));
}

void test_extract_varchar_null_result(void)
{
    uint8_t buf[4];
    size_t len = sizeof(buf);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE,
                          extract_value_from_tuple(NULL, NULL, 0, buf, &len));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE,
                          extract_value_from_tuple(NULL, NULL, 0, NULL, &len));
}

/* ----------------------------------------------------------------
 * Column accessor tests
 * ---------------------------------------------------------------- */

void test_column_name_out_of_range(void)
{
    sqli_result_t result;
    memset(&result, 0, sizeof(result));
    result.column_count = 2;

    TEST_ASSERT_NULL(sqli_result_column_name(&result, -1));
    TEST_ASSERT_NULL(sqli_result_column_name(&result, 2));
    TEST_ASSERT_NULL(sqli_result_column_name(NULL, 0));
}

void test_column_type_out_of_range(void)
{
    sqli_result_t result;
    memset(&result, 0, sizeof(result));
    result.column_count = 2;

    TEST_ASSERT_EQUAL_INT(-1, sqli_result_column_type(&result, -1));
    TEST_ASSERT_EQUAL_INT(-1, sqli_result_column_type(&result, 2));
    TEST_ASSERT_EQUAL_INT(-1, sqli_result_column_type(NULL, 0));
}

/* ----------------------------------------------------------------
 * Prepared statement null/safety tests
 * ---------------------------------------------------------------- */

void test_prepare_null_params(void)
{
    sqli_status rc = sqli_prepare(NULL, "SELECT 1", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, rc);

    sqli_stmt_t *stmt = NULL;
    rc = sqli_prepare(NULL, "SELECT 1", NULL, &stmt);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, rc);
}

void test_prepare_with_retry_null_params(void)
{
    sqli_stmt_t *stmt = NULL;
    sqli_status rc = sqli_prepare_with_retry(NULL, "SELECT 1", 1, NULL, &stmt);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, rc);
}

void test_prepare_with_retry_retries_on_retryable_error(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.state = SQLI_CONN_READY;
    conn.socket_fd = -1; /* force send failure path in sqli_prepare */

    sqli_stmt_t *stmt = (sqli_stmt_t *)0x1;
    uint64_t t0 = tv_now_ms();
    sqli_status rc = sqli_prepare_with_retry(&conn, "SELECT 1", 1, NULL, &stmt);
    uint64_t t1 = tv_now_ms();

    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR, rc);
    TEST_ASSERT_NULL(stmt);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(200, t1 - t0);
}

void test_bind_null_stmt(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_int(NULL, 1, 42));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_int64(NULL, 1, 42));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_double(NULL, 1, 1.0));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_null(NULL, 1));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_null_int(NULL, 1));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_null_int64(NULL, 1));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_bind_null_double(NULL, 1));
}

void test_execute_null_stmt(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_execute(NULL));
}

void test_execute_rejects_unimplemented_prepared_lob_streaming(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.state = SQLI_CONN_READY;

    sqli_bound_param param;
    memset(&param, 0, sizeof(param));
    uint8_t b[1] = {0x41};
    param.type = SQLI_BIND_BYTES;
    param.bval = b;
    param.blen = sizeof(b);
    param.is_null = false;

    uint8_t param_types[1] = { SQLI_TYPE_BYTE };

    sqli_stmt_t stmt;
    memset(&stmt, 0, sizeof(stmt));
    stmt.conn = &conn;
    stmt.socket_fd = -1;
    stmt.param_count = 1;
    stmt.params = &param;
    stmt.param_server_types = param_types;
    stmt.param_server_type_count = 1;

    sqli_status rc = sqli_execute(&stmt);
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, rc);
    TEST_ASSERT_NOT_NULL(sqli_error(&conn));
}

void test_execute_with_retry_null_stmt(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_execute_with_retry(NULL, 1));
}

static uint64_t tv_now_ms(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
        return 0;
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
}

void test_execute_with_retry_retries_on_retryable_error(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    sqli_stmt_t stmt;
    memset(&stmt, 0, sizeof(stmt));
    stmt.conn = &conn;
    stmt.socket_fd = -1; /* force failed send path */
    stmt.read_only = 1;  /* retry gate for safety */

    uint64_t t0 = tv_now_ms();
    sqli_status rc = sqli_execute_with_retry(&stmt, 1);
    uint64_t t1 = tv_now_ms();

    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR, rc);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(200, t1 - t0);
}

void test_execute_with_retry_nonreadonly_does_not_retry(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));

    sqli_stmt_t stmt;
    memset(&stmt, 0, sizeof(stmt));
    stmt.conn = &conn;
    stmt.socket_fd = -1;
    stmt.read_only = 0;

    uint64_t t0 = tv_now_ms();
    sqli_status rc = sqli_execute_with_retry(&stmt, 3);
    uint64_t t1 = tv_now_ms();

    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR, rc);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(150, t1 - t0);
}

void test_stmt_close_null(void)
{
    sqli_stmt_close(NULL); /* should not crash */
}

void test_stmt_destroy_null(void)
{
    sqli_stmt_destroy(NULL); /* should not crash */
}

void test_stmt_next_null_result(void)
{
    sqli_stmt_t stmt;
    memset(&stmt, 0, sizeof(stmt));
    stmt.result_valid = 0;
    TEST_ASSERT_EQUAL_INT(0, sqli_stmt_next(&stmt));
}

void test_stmt_result_null(void)
{
    sqli_stmt_t stmt;
    memset(&stmt, 0, sizeof(stmt));
    stmt.result_valid = 0;
    TEST_ASSERT_NULL(sqli_stmt_result(&stmt));
}

void test_result_get_int_null(void)
{
    TEST_ASSERT_EQUAL_INT32(0, sqli_result_get_int(NULL, 0));
}

void test_result_get_int64_null(void)
{
    TEST_ASSERT_EQUAL_INT64(0, sqli_result_get_int64(NULL, 0));
}

void test_result_get_double_null(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sqli_result_get_double(NULL, 0));
}

void test_result_get_string_null(void)
{
    const char *s = sqli_result_get_string(NULL, 0);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("", s);
}

void test_result_get_bytes_null(void)
{
    uint8_t buf[4];
    size_t len = 4;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE,
                          sqli_result_get_bytes(NULL, 0, buf, &len));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE,
                          sqli_result_get_bytes(NULL, 0, NULL, &len));
}

/* ----------------------------------------------------------------
 * Helpers for building mock results for full extraction tests
 * ---------------------------------------------------------------- */

static void setup_mock_result_heap(sqli_result_t **result, int ncols,
                                   const uint8_t *types, const uint32_t *offsets)
{
    *result = malloc(sizeof(sqli_result_t));
    memset(*result, 0, sizeof(sqli_result_t));

    (*result)->column_count = ncols;
    (*result)->columns = calloc((size_t)ncols, sizeof(sqli_column_info));

    const char *names[] = {"col0", "col1", "col2", "col3", "col4"};
    for (int i = 0; i < ncols; i++) {
        (*result)->columns[(size_t)i].type = (sqli_column_type)types[i];
        (*result)->columns[(size_t)i].col_start_pos = offsets[i];
        snprintf((*result)->columns[(size_t)i].name,
                 sizeof((*result)->columns[0].name), "%s",
                 names[i < 4 ? i : 3]);
    }
}

void test_result_get_int_from_mock_result(void)
{
    sqli_result_t *result;
    uint8_t types[] = {SQLI_TYPE_INT, SQLI_TYPE_SMALLINT};
    uint32_t offsets[] = {0, 4};
    setup_mock_result_heap(&result, 2, types, offsets);

    /* Build tuple: INT=100, SMALLINT=200 */
    uint8_t *tuple = malloc(6);
    tuple[0] = 0; tuple[1] = 0; tuple[2] = 0; tuple[3] = 100;
    tuple[4] = 0; tuple[5] = 200;

    result->rows = malloc(sizeof(uint8_t *));
    result->rows[0] = tuple;
    result->row_lens = malloc(sizeof(size_t));
    result->row_lens[0] = 6;
    result->row_count = 1;
    result->row_capacity = 1;
    result->cursor = 0;
    result->tuple_buffer = tuple;
    result->tuple_len = 6;
    result->current_row = 0;

    TEST_ASSERT_EQUAL_INT32(100, sqli_result_get_int(result, 0));
    TEST_ASSERT_EQUAL_INT32(200, sqli_result_get_int(result, 1));

    sqli_result_destroy(result);
}

void test_result_get_string_from_mock_result(void)
{
    sqli_result_t *result;
    uint8_t types[] = {SQLI_TYPE_VARCHAR};
    uint32_t offsets[] = {0};
    setup_mock_result_heap(&result, 1, types, offsets);

    /* Build tuple: VARCHAR with "Hello" (1-byte len=5) */
    uint8_t *tuple = malloc(6);
    tuple[0] = 5; /* len */
    tuple[1] = 'H'; tuple[2] = 'e'; tuple[3] = 'l';
    tuple[4] = 'l'; tuple[5] = 'o';

    result->rows = malloc(sizeof(uint8_t *));
    result->rows[0] = tuple;
    result->row_lens = malloc(sizeof(size_t));
    result->row_lens[0] = 6;
    result->row_count = 1;
    result->row_capacity = 1;
    result->cursor = 0;
    result->tuple_buffer = tuple;
    result->tuple_len = 6;
    result->current_row = 0;

    const char *s = sqli_result_get_string(result, 0);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("Hello", s);

    sqli_result_destroy(result);
}

void test_result_next_recomputes_packed_offsets(void)
{
    sqli_result_t *result;
    uint8_t types[] = {SQLI_TYPE_CHAR, SQLI_TYPE_INT};
    uint32_t offsets[] = {13, 33292550}; /* mimic unusable DESCRIBE startPos values */
    setup_mock_result_heap(&result, 2, types, offsets);

    result->columns[0].encoded_length = 128; /* char-like field with 1-byte length prefix */
    result->columns[1].encoded_length = 4;   /* int */

    /* Tuple layout: [len=9]["systables"][int=1] */
    uint8_t *tuple = malloc(14);
    TEST_ASSERT_NOT_NULL(tuple);
    tuple[0] = 9;
    memcpy(tuple + 1, "systables", 9);
    tuple[10] = 0; tuple[11] = 0; tuple[12] = 0; tuple[13] = 1;

    result->rows = malloc(sizeof(uint8_t *));
    result->row_lens = malloc(sizeof(size_t));
    TEST_ASSERT_NOT_NULL(result->rows);
    TEST_ASSERT_NOT_NULL(result->row_lens);
    result->rows[0] = tuple;
    result->row_lens[0] = 14;
    result->row_count = 1;
    result->row_capacity = 1;
    result->cursor = -1;
    result->current_row = -1;
    result->tuple_buffer = NULL;
    result->tuple_len = 0;

    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(0, (int)result->columns[0].col_start_pos);
    TEST_ASSERT_EQUAL_INT(10, (int)result->columns[1].col_start_pos);
    TEST_ASSERT_EQUAL_STRING("systables", sqli_result_get_string(result, 0));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_get_int(result, 1));

    sqli_result_destroy(result);
}

/* ----------------------------------------------------------------
 * MVP datatype coverage tests (DT-001..DT-504)
 * ---------------------------------------------------------------- */

static sqli_result_t *make_single_row_result(uint8_t type, uint32_t encoded_len,
                                             const uint8_t *tuple, size_t tuple_len)
{
    sqli_result_t *result = NULL;
    uint8_t types[] = {type};
    uint32_t offsets[] = {0};
    setup_mock_result_heap(&result, 1, types, offsets);
    TEST_ASSERT_NOT_NULL(result);

    result->columns[0].encoded_length = encoded_len;

    result->rows = malloc(sizeof(uint8_t *));
    result->row_lens = malloc(sizeof(size_t));
    TEST_ASSERT_NOT_NULL(result->rows);
    TEST_ASSERT_NOT_NULL(result->row_lens);

    result->rows[0] = malloc(tuple_len);
    TEST_ASSERT_NOT_NULL(result->rows[0]);
    memcpy(result->rows[0], tuple, tuple_len);
    result->row_lens[0] = tuple_len;
    result->row_count = 1;
    result->row_capacity = 1;
    result->cursor = -1;
    result->current_row = -1;
    result->tuple_buffer = NULL;
    result->tuple_len = 0;
    return result;
}

void test_dt_001_varchar_roundtrip_ascii(void)
{
    /* [len=5]hello */
    const uint8_t tuple[] = {5, 'h', 'e', 'l', 'l', 'o'};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_VARCHAR, 0, tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_STRING("hello", sqli_result_get_string(result, 0));
    sqli_result_destroy(result);
}

void test_dt_002_varchar_roundtrip_utf8(void)
{
    /* [len=7]Gr\xc3\xbc\xc3\x9fe */
    const uint8_t tuple[] = {7, 'G', 'r', 0xC3, 0xBC, 0xC3, 0x9F, 'e'};
    const char *expected = "Gr\xC3\xBC\xC3\x9F" "e";
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_VARCHAR, 0, tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_STRING(expected, sqli_result_get_string(result, 0));
    sqli_result_destroy(result);
}

void test_dt_003_char_padding_trim(void)
{
    /* CHAR fixed-width 4, payload \"AB  \" */
    const uint8_t tuple[] = {'A', 'B', ' ', ' '};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_CHAR, 4, tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_STRING("AB", sqli_result_get_string(result, 0));
    sqli_result_destroy(result);
}

void test_dt_004_char_padding_preserve_when_trim_disabled(void)
{
    const uint8_t tuple[] = {'A', 'B', ' ', ' '};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_CHAR, 4, tuple, sizeof(tuple));
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_set_trim_trailing_spaces(conn, 0));
    result->owner_conn = conn;

    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_STRING("AB  ", sqli_result_get_string(result, 0));

    sqli_result_destroy(result);
    sqli_destroy(conn);
}

void test_dt_005_empty_string_vs_no_row(void)
{
    const uint8_t tuple[] = {0};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_VARCHAR, 0, tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_STRING("", sqli_result_get_string(result, 0));
    TEST_ASSERT_EQUAL_STRING("", sqli_result_get_string(result, 99)); /* out-of-range behaves as empty */
    sqli_result_destroy(result);
}

void test_dt_009_lvarchar_roundtrip(void)
{
    /* [len16=5]hello */
    const uint8_t tuple[] = {0x00, 0x05, 'h', 'e', 'l', 'l', 'o'};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_LVARCHAR, 128, tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_STRING("hello", sqli_result_get_string(result, 0));
    sqli_result_destroy(result);
}

void test_dt_010_lvarchar_marker_len8_roundtrip(void)
{
    /* [marker=0][len8=5]hello */
    const uint8_t tuple[] = {0x00, 0x00, 0x00, 0x00, 0x05, 'h', 'e', 'l', 'l', 'o'};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_LVARCHAR, 128, tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_STRING("hello", sqli_result_get_string(result, 0));
    sqli_result_destroy(result);
}

void test_dt_101_smallint_bounds(void)
{
    const uint8_t tuple_min[] = {0x80, 0x00}; /* -32768 */
    sqli_result_t *min_result = make_single_row_result(SQLI_TYPE_SMALLINT, 2, tuple_min, sizeof(tuple_min));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(min_result));
    TEST_ASSERT_EQUAL_INT(-32768, sqli_result_get_int(min_result, 0));
    sqli_result_destroy(min_result);

    const uint8_t tuple_max[] = {0x7F, 0xFF}; /* 32767 */
    sqli_result_t *max_result = make_single_row_result(SQLI_TYPE_SMALLINT, 2, tuple_max, sizeof(tuple_max));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(max_result));
    TEST_ASSERT_EQUAL_INT(32767, sqli_result_get_int(max_result, 0));
    sqli_result_destroy(max_result);
}

void test_dt_102_integer_bounds(void)
{
    const uint8_t tuple_min[] = {0x80, 0x00, 0x00, 0x00}; /* INT32_MIN */
    sqli_result_t *min_result = make_single_row_result(SQLI_TYPE_INT, 4, tuple_min, sizeof(tuple_min));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(min_result));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, sqli_result_get_int(min_result, 0));
    sqli_result_destroy(min_result);

    const uint8_t tuple_max[] = {0x7F, 0xFF, 0xFF, 0xFF}; /* INT32_MAX */
    sqli_result_t *max_result = make_single_row_result(SQLI_TYPE_INT, 4, tuple_max, sizeof(tuple_max));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(max_result));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, sqli_result_get_int(max_result, 0));
    sqli_result_destroy(max_result);
}

void test_dt_103_bigint_bounds(void)
{
    const uint8_t tuple_min[] = {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; /* INT64_MIN */
    sqli_result_t *min_result = make_single_row_result(SQLI_TYPE_BIGINT, 8, tuple_min, sizeof(tuple_min));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(min_result));
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, sqli_result_get_int64(min_result, 0));
    sqli_result_destroy(min_result);

    const uint8_t tuple_max[] = {0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; /* INT64_MAX */
    sqli_result_t *max_result = make_single_row_result(SQLI_TYPE_BIGINT, 8, tuple_max, sizeof(tuple_max));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(max_result));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, sqli_result_get_int64(max_result, 0));
    sqli_result_destroy(max_result);
}

void test_dt_104_serial8_bounds(void)
{
    /* sign + low32 + high32 */
    const uint8_t tuple_min[] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00};
    sqli_result_t *min_result = make_single_row_result(SQLI_TYPE_SERIAL8, 10,
                                                        tuple_min, sizeof(tuple_min));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(min_result));
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, sqli_result_get_int64(min_result, 0));
    sqli_result_destroy(min_result);

    const uint8_t tuple_max[] = {0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF};
    sqli_result_t *max_result = make_single_row_result(SQLI_TYPE_SERIAL8, 10,
                                                        tuple_max, sizeof(tuple_max));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(max_result));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, sqli_result_get_int64(max_result, 0));
    sqli_result_destroy(max_result);
}

void test_dt_201_decimal_roundtrip_precision_scale(void)
{
    uint8_t digits[9] = {1,2,3,4,5,6,7,8,9};
    uint8_t buf[32];
    size_t n = sqli_encode_decimal(buf, sizeof(buf), 9, 4, 0, digits);
    TEST_ASSERT_GREATER_THAN(0, n);

    uint8_t scale = 0;
    int negative = 0;
    uint8_t out_digits[15] = {0};
    int decoded = sqli_decode_decimal(buf, n, 9, &scale, &negative, out_digits);
    TEST_ASSERT_EQUAL_INT(9, decoded);
    TEST_ASSERT_EQUAL_UINT8(4, scale);
    TEST_ASSERT_EQUAL_INT(0, negative);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(digits, out_digits, 9);
}

void test_dt_201_decimal_get_double_packed(void)
{
    /* Observed live wire for DECIMAL(12,4) value 12345.6789 */
    const uint8_t tuple[] = {0xC3, 0x01, 0x17, 0x2D, 0x43, 0x59, 0x00};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_DECIMAL, 0x00000C04u,
                                                    tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_FLOAT_WITHIN(0.000001f, 12345.6789f,
                             (float)sqli_result_get_double(result, 0));
    sqli_result_destroy(result);
}

void test_dt_202_numeric_get_double_packed(void)
{
    /* Observed live wire for NUMERIC(10,3) value -42.125 */
    const uint8_t tuple[] = {0x3E, 0x39, 0x57, 0x32, 0x00, 0x00, 0x00};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_DECIMAL, 0x00000A03u,
                                                    tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_FLOAT_WITHIN(0.000001f, -42.125f,
                             (float)sqli_result_get_double(result, 0));
    sqli_result_destroy(result);
}

void test_dt_203_money_get_double_packed(void)
{
    /* Observed live wire for MONEY(12,2) value -98765.43 */
    const uint8_t tuple[] = {0x3C, 0x5A, 0x0C, 0x22, 0x39, 0x00, 0x00};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_MONEY, 0x00000C02u,
                                                    tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_FLOAT_WITHIN(0.000001f, -98765.43f,
                             (float)sqli_result_get_double(result, 0));
    sqli_result_destroy(result);
}

void test_dt_301_float_double_roundtrip(void)
{
    /* big-endian IEEE754 for 3.5 */
    const uint8_t tuple[] = {0x40, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_FLOAT, 8, tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_FLOAT_WITHIN(0.000001f, 3.5f, (float)sqli_result_get_double(result, 0));
    sqli_result_destroy(result);
}

void test_dt_401_date_roundtrip(void)
{
    int32_t days = 45205; /* arbitrary positive day count */
    TEST_ASSERT_EQUAL_INT32(days, sqli_decode_date(sqli_encode_date(days)));
}

void test_dt_402_date_string_epoch_mapping(void)
{
    const uint8_t tuple_epoch[] = {0x00, 0x00, 0x00, 0x00}; /* day 0 */
    sqli_result_t *r0 = make_single_row_result(SQLI_TYPE_DATE, 4, tuple_epoch, sizeof(tuple_epoch));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(r0));
    TEST_ASSERT_EQUAL_STRING("1899-12-31", sqli_result_get_date_string(r0, 0));
    sqli_result_destroy(r0);

    const uint8_t tuple_unix[] = {0x00, 0x00, 0x63, 0xE0}; /* 25568 */
    sqli_result_t *r1 = make_single_row_result(SQLI_TYPE_DATE, 4, tuple_unix, sizeof(tuple_unix));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(r1));
    TEST_ASSERT_EQUAL_STRING("1970-01-01", sqli_result_get_date_string(r1, 0));
    sqli_result_destroy(r1);
}

void test_dt_501_boolean_roundtrip(void)
{
    const uint8_t tuple_true[] = {1};
    sqli_result_t *true_result = make_single_row_result(SQLI_TYPE_BOOL, 1, tuple_true, sizeof(tuple_true));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(true_result));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_get_int(true_result, 0));
    sqli_result_destroy(true_result);

    const uint8_t tuple_false[] = {0};
    sqli_result_t *false_result = make_single_row_result(SQLI_TYPE_BOOL, 1, tuple_false, sizeof(tuple_false));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(false_result));
    TEST_ASSERT_EQUAL_INT(0, sqli_result_get_int(false_result, 0));
    sqli_result_destroy(false_result);

    /* Informix BOOLEAN may arrive as [00000000][len][ascii 't'/'f'] */
    const uint8_t tuple_true_ascii[] = {0, 0, 0, 0, 1, 't'};
    sqli_result_t *true_ascii = make_single_row_result(SQLI_TYPE_BOOL, 1,
                                                       tuple_true_ascii,
                                                       sizeof(tuple_true_ascii));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(true_ascii));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_get_int(true_ascii, 0));
    sqli_result_destroy(true_ascii);

    const uint8_t tuple_false_ascii[] = {0, 0, 0, 0, 1, 'f'};
    sqli_result_t *false_ascii = make_single_row_result(SQLI_TYPE_BOOL, 1,
                                                        tuple_false_ascii,
                                                        sizeof(tuple_false_ascii));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(false_ascii));
    TEST_ASSERT_EQUAL_INT(0, sqli_result_get_int(false_ascii, 0));
    sqli_result_destroy(false_ascii);
}

void test_dt_204_decimal_get_string_and_null(void)
{
    const uint8_t tuple[] = {0xC3, 0x01, 0x17, 0x2D, 0x43, 0x59, 0x00};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_DECIMAL, 0x00000C04u,
                                                    tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_STRING("12345.6789", sqli_result_get_decimal_string(result, 0));
    TEST_ASSERT_EQUAL_INT(0, sqli_result_was_null(result));
    sqli_result_destroy(result);
}

void test_dt_206_result_is_null_and_was_null(void)
{
    const uint8_t tuple[] = {0, 0, 0, 0};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_INT, 4, tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(0, sqli_result_is_null(result, 0));
    (void)sqli_result_get_int(result, 0);
    TEST_ASSERT_EQUAL_INT(0, sqli_result_was_null(result));
    sqli_result_destroy(result);
}

typedef struct {
    size_t seen;
} stream_ctx_t;

static int count_stream_chunk(const uint8_t *chunk, size_t len, void *ctx)
{
    (void)chunk;
    stream_ctx_t *s = (stream_ctx_t *)ctx;
    s->seen += len;
    return 0;
}

void test_dt_611_stream_bytes_chunked(void)
{
    const uint8_t tuple[] = {5, 'h', 'e', 'l', 'l', 'o'};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_VARCHAR, 0, tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));

    stream_ctx_t s = {0};
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
                          sqli_result_stream_bytes(result, 0, 2, count_stream_chunk, &s));
    TEST_ASSERT_EQUAL_UINT(5, (unsigned)s.seen);
    sqli_result_destroy(result);
}

void test_dt_412_datetime_semantic_object(void)
{
    /* DATETIME YEAR TO SECOND: 2026-06-20 12:34:56 */
    const uint8_t tuple[] = {0xC7, 20, 26, 6, 20, 12, 34, 56};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_DATETIME, 0x00000E0Au,
                                                    tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));

    sqli_datetime_value dt;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_datetime(result, 0, &dt));
    TEST_ASSERT_EQUAL_INT(0, dt.is_null);
    TEST_ASSERT_EQUAL_INT(2026, dt.year);
    TEST_ASSERT_EQUAL_INT(6, dt.month);
    TEST_ASSERT_EQUAL_INT(20, dt.day);
    TEST_ASSERT_EQUAL_INT(12, dt.hour);
    TEST_ASSERT_EQUAL_INT(34, dt.minute);
    TEST_ASSERT_EQUAL_INT(56, dt.second);
    TEST_ASSERT_EQUAL_INT(0, dt.fraction_scale);
    sqli_result_destroy(result);
}

void test_dt_413_interval_semantic_object(void)
{
    /* INTERVAL DAY TO SECOND: 12 03:04:05 */
    const uint8_t tuple[] = {0xC4, 12, 3, 4, 5};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_INTERVAL, 0x0000084Au,
                                                    tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));

    sqli_interval_value iv;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_interval(result, 0, &iv));
    TEST_ASSERT_EQUAL_INT(0, iv.is_null);
    TEST_ASSERT_EQUAL_INT(0, iv.negative);
    TEST_ASSERT_EQUAL_INT(12, iv.day);
    TEST_ASSERT_EQUAL_INT(3, iv.hour);
    TEST_ASSERT_EQUAL_INT(4, iv.minute);
    TEST_ASSERT_EQUAL_INT(5, iv.second);
    sqli_result_destroy(result);
}

void test_dt_414_datetime_interval_two_column_layout(void)
{
    /* Live row layout is fixed-width decimal payload without len16 prefixes. */
    const uint8_t tuple[] = {
        0xC7, 20, 26, 6, 20, 12, 34, 56,
        0xC4, 12, 3, 4, 5
    };

    sqli_result_t *result = NULL;
    uint8_t types[] = {SQLI_TYPE_DATETIME, SQLI_TYPE_INTERVAL};
    uint32_t offsets[] = {0, 0};
    setup_mock_result_heap(&result, 2, types, offsets);
    TEST_ASSERT_NOT_NULL(result);
    result->columns[0].encoded_length = 0x00000E0Au;
    result->columns[1].encoded_length = 0x0000084Au;

    result->rows = malloc(sizeof(uint8_t *));
    result->row_lens = malloc(sizeof(size_t));
    TEST_ASSERT_NOT_NULL(result->rows);
    TEST_ASSERT_NOT_NULL(result->row_lens);
    result->rows[0] = malloc(sizeof(tuple));
    TEST_ASSERT_NOT_NULL(result->rows[0]);
    memcpy(result->rows[0], tuple, sizeof(tuple));
    result->row_lens[0] = sizeof(tuple);
    result->row_count = 1;
    result->row_capacity = 1;
    result->cursor = -1;
    result->current_row = -1;
    result->tuple_buffer = NULL;
    result->tuple_len = 0;

    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(0, (int)result->columns[0].col_start_pos);
    TEST_ASSERT_EQUAL_INT(8, (int)result->columns[1].col_start_pos);

    sqli_datetime_value dt;
    sqli_interval_value iv;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_datetime(result, 0, &dt));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_interval(result, 1, &iv));
    TEST_ASSERT_EQUAL_INT(0, dt.is_null);
    TEST_ASSERT_EQUAL_INT(0, iv.is_null);
    TEST_ASSERT_EQUAL_INT(2026, dt.year);
    TEST_ASSERT_EQUAL_INT(12, iv.day);
    TEST_ASSERT_EQUAL_INT(5, iv.second);
    sqli_result_destroy(result);
}

void test_dt_timestamp_retrieval(void)
{
    /* 1. Test DATETIME YEAR TO SECOND: 2026-06-20 12:34:56 */
    const uint8_t tuple[] = {0xC7, 20, 26, 6, 20, 12, 34, 56};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_DATETIME, 0x00000E0Au,
                                                    tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));

    sqli_timestamp_t ts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_timestamp(result, 0, &ts));
    TEST_ASSERT_EQUAL_INT(0, ts.is_null);
    TEST_ASSERT_EQUAL_INT(2026, ts.year);
    TEST_ASSERT_EQUAL_INT(6, ts.month);
    TEST_ASSERT_EQUAL_INT(20, ts.day);
    TEST_ASSERT_EQUAL_INT(12, ts.hour);
    TEST_ASSERT_EQUAL_INT(34, ts.minute);
    TEST_ASSERT_EQUAL_INT(56, ts.second);
    TEST_ASSERT_EQUAL_INT(0, ts.microsecond);
    sqli_result_destroy(result);

    /* 2. Test DATE: 2024-06-15 (represented as days since 1899-12-31, 2024-06-15 is 45457 days) */
    const uint8_t date_tuple[] = {0x00, 0x00, 0xB1, 0x91};
    result = make_single_row_result(SQLI_TYPE_DATE, 4, date_tuple, sizeof(date_tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_timestamp(result, 0, &ts));
    TEST_ASSERT_EQUAL_INT(0, ts.is_null);
    TEST_ASSERT_EQUAL_INT(2024, ts.year);
    TEST_ASSERT_EQUAL_INT(6, ts.month);
    TEST_ASSERT_EQUAL_INT(15, ts.day);
    TEST_ASSERT_EQUAL_INT(0, ts.hour);
    TEST_ASSERT_EQUAL_INT(0, ts.minute);
    TEST_ASSERT_EQUAL_INT(0, ts.second);
    TEST_ASSERT_EQUAL_INT(0, ts.microsecond);
    sqli_result_destroy(result);

    /* 3. Test string fallback */
    const char *str_val = "2026-06-28 02:29:16.123456";
    size_t str_len = strlen(str_val);
    uint8_t *str_tuple = malloc(1 + str_len);
    str_tuple[0] = (uint8_t)str_len;
    memcpy(str_tuple + 1, str_val, str_len);

    result = make_single_row_result(SQLI_TYPE_VARCHAR, 0, str_tuple, 1 + str_len);
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_timestamp(result, 0, &ts));
    TEST_ASSERT_EQUAL_INT(0, ts.is_null);
    TEST_ASSERT_EQUAL_INT(2026, ts.year);
    TEST_ASSERT_EQUAL_INT(6, ts.month);
    TEST_ASSERT_EQUAL_INT(28, ts.day);
    TEST_ASSERT_EQUAL_INT(2, ts.hour);
    TEST_ASSERT_EQUAL_INT(29, ts.minute);
    TEST_ASSERT_EQUAL_INT(16, ts.second);
    TEST_ASSERT_EQUAL_INT(123456, ts.microsecond);

    sqli_result_destroy(result);
    free(str_tuple);
}

void test_sqli_epoch_helpers(void)
{
    /* 1. Test timestamp to epoch conversions */
    sqli_timestamp_t ts = {
        .is_null = false,
        .year = 2026,
        .month = 6,
        .day = 28,
        .hour = 2,
        .minute = 33,
        .second = 15,
        .microsecond = 123456
    };

    int32_t days = sqli_timestamp_to_epoch_days(&ts);
    TEST_ASSERT_EQUAL_INT32(20632, days);

    int64_t sec = sqli_timestamp_to_epoch_sec(&ts);
    TEST_ASSERT_EQUAL_INT64(1782613995LL, sec);

    int64_t ms = sqli_timestamp_to_epoch_ms(&ts);
    TEST_ASSERT_EQUAL_INT64(1782613995123LL, ms);

    /* 2. Test epoch to timestamp conversions */
    sqli_timestamp_t ts2;
    sqli_timestamp_from_epoch_sec(&ts2, 1782613995LL);
    TEST_ASSERT_EQUAL_INT(0, ts2.is_null);
    TEST_ASSERT_EQUAL_INT(2026, ts2.year);
    TEST_ASSERT_EQUAL_INT(6, ts2.month);
    TEST_ASSERT_EQUAL_INT(28, ts2.day);
    TEST_ASSERT_EQUAL_INT(2, ts2.hour);
    TEST_ASSERT_EQUAL_INT(33, ts2.minute);
    TEST_ASSERT_EQUAL_INT(15, ts2.second);
    TEST_ASSERT_EQUAL_INT(0, ts2.microsecond);

    sqli_timestamp_from_epoch_ms(&ts2, 1782613995123LL);
    TEST_ASSERT_EQUAL_INT(0, ts2.is_null);
    TEST_ASSERT_EQUAL_INT(2026, ts2.year);
    TEST_ASSERT_EQUAL_INT(6, ts2.month);
    TEST_ASSERT_EQUAL_INT(28, ts2.day);
    TEST_ASSERT_EQUAL_INT(2, ts2.hour);
    TEST_ASSERT_EQUAL_INT(33, ts2.minute);
    TEST_ASSERT_EQUAL_INT(15, ts2.second);
    TEST_ASSERT_EQUAL_INT(123000, ts2.microsecond);

    sqli_timestamp_from_epoch_days(&ts2, 20632);
    TEST_ASSERT_EQUAL_INT(0, ts2.is_null);
    TEST_ASSERT_EQUAL_INT(2026, ts2.year);
    TEST_ASSERT_EQUAL_INT(6, ts2.month);
    TEST_ASSERT_EQUAL_INT(28, ts2.day);
    TEST_ASSERT_EQUAL_INT(0, ts2.hour);
    TEST_ASSERT_EQUAL_INT(0, ts2.minute);
    TEST_ASSERT_EQUAL_INT(0, ts2.second);
    TEST_ASSERT_EQUAL_INT(0, ts2.microsecond);

    /* 3. Test direct result epoch functions */
    const uint8_t tuple[] = {0xC7, 20, 26, 6, 28, 2, 33, 15};
    sqli_result_t *result = make_single_row_result(SQLI_TYPE_DATETIME, 0x00000E0Au,
                                                    tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));

    int64_t out_sec = 0;
    int64_t out_ms = 0;
    int32_t out_days = 0;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_epoch_sec(result, 0, &out_sec));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_epoch_ms(result, 0, &out_ms));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_epoch_days(result, 0, &out_days));

    TEST_ASSERT_EQUAL_INT64(1782613995LL, out_sec);
    TEST_ASSERT_EQUAL_INT64(1782613995000LL, out_ms);
    TEST_ASSERT_EQUAL_INT32(20632, out_days);

    sqli_result_destroy(result);
}
