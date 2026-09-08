#include "libsqli/sqli.h"
#include "libsqli/sqli_decimal.h"
#include "libsqli/sqli_temporal.h"
#include "libsqli/sqli_sblob.h"
#include "sqli_internal.h"
#include "sqli_sblob_internal.h"
#include "allocation_test.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

enum { value_column = 0, columns = 1, guard = 0xa5 };
static sqli_result_t *result;
static sqli_decimal_t *decimal;
static sqli_bound_param parameter;
static sqli_stmt_t statement;
static sqli_sblob_t *lob;

void setUp(void)
{
    sqli_test_allow_allocations();
    parameter = (sqli_bound_param){0};
    statement = (sqli_stmt_t){.params = &parameter, .param_count = 1};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_create(&decimal));
}

void tearDown(void)
{
    sqli_test_allow_allocations();
    sqli_result_destroy(result);
    result = NULL;
    sqli_decimal_destroy(decimal);
    decimal = NULL;
    sqli_stmt_batch_clear(&statement);
    free(parameter.sval);
    free(parameter.bval);
    sqli_sblob_destroy(lob);
    lob = NULL;
}

static void make_result(sqli_column_type type, uint32_t qualifier, const uint8_t *wire, size_t length)
{
    sqli_result_destroy(result);
    result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);
    result->columns = calloc(columns, sizeof(*result->columns));
    result->rows = calloc(1, sizeof(*result->rows));
    result->row_lens = calloc(1, sizeof(*result->row_lens));
    TEST_ASSERT_NOT_NULL(result->columns);
    TEST_ASSERT_NOT_NULL(result->rows);
    TEST_ASSERT_NOT_NULL(result->row_lens);
    result->row_count = result->row_capacity = 1;
    result->rows[0] = malloc(length);
    TEST_ASSERT_NOT_NULL(result->rows[0]);
    memcpy(result->rows[0], wire, length);
    result->row_lens[0] = length;
    result->column_count = columns;
    result->columns[0].type = type;
    result->columns[0].encoded_length = qualifier;
    result->cursor = result->current_row = result->cur_cache_row = -1;
    result->at_before_first = true;
    result->eof = result->saw_done = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
}

static void test_scalar_narrowing_null_and_overflow(void)
{
    const uint8_t large[] = {0, 0, 0, 0, 0x80, 0, 0, 0};
    make_result(SQLI_TYPE_BIGINT, sizeof(large), large, sizeof(large));
    int64_t wide = 0;
    int32_t narrow = 42;
    double floating = 0;
    bool is_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_int64(result, value_column, &wide, &is_null));
    TEST_ASSERT_EQUAL_INT64(INT64_C(2147483648), wide);
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_result_get_int(result, value_column, &narrow, &is_null));
    TEST_ASSERT_EQUAL_INT(42, narrow);
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_double(result, value_column, &floating, &is_null));
    TEST_ASSERT_TRUE(floating == 2147483648.0);
    const uint8_t int8_null[] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    make_result(SQLI_TYPE_INT8, sizeof(int8_null), int8_null, sizeof(int8_null));
    result->last_was_null = false;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_int(result, value_column, &narrow, &is_null));
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_EQUAL_INT(42, narrow);
    TEST_ASSERT_FALSE(sqli_result_was_null(result));
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_result_get_int64(result, SIZE_MAX, &wide, &is_null));
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_EQUAL_INT64(INT64_C(2147483648), wide);
    const uint8_t int8_overflow[] = {0, 1, 0, 0, 0, 0, 0x80, 0, 0, 0};
    make_result(SQLI_TYPE_INT8, sizeof(int8_overflow), int8_overflow, sizeof(int8_overflow));
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_result_get_int64(result, value_column, &wide, &is_null));
    const uint8_t int8_minimum[] = {0xff, 0xff, 0, 0, 0, 0, 0x80, 0, 0, 0};
    make_result(SQLI_TYPE_INT8, sizeof(int8_minimum), int8_minimum, sizeof(int8_minimum));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_int64(result, value_column, &wide, &is_null));
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, wide);
    const uint8_t infinity[] = {0x7f, 0xf0, 0, 0, 0, 0, 0, 0};
    make_result(SQLI_TYPE_FLOAT, sizeof(infinity), infinity, sizeof(infinity));
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_result_get_double(result, value_column, &floating, &is_null));
    TEST_ASSERT_TRUE(floating == 2147483648.0);
    const uint8_t text[] = {1, '1'};
    make_result(SQLI_TYPE_VARCHAR, 8, text, sizeof(text));
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_MISMATCH, sqli_result_get_int(result, value_column, &narrow, &is_null));
    TEST_ASSERT_EQUAL_STRING("SQLI_INEXACT", sqli_status_name(SQLI_INEXACT));
    TEST_ASSERT_EQUAL_STRING("SQLI_UNKNOWN_STATUS", sqli_status_name((sqli_status)-1));
    TEST_ASSERT_NOT_NULL(sqli_status_description(SQLI_OUT_OF_RANGE));
}

static void test_scalar_exact_decimal_and_boolean_contracts(void)
{
    enum { decimal_qualifier = 0x0802 };
    uint8_t wire[SQLI_DECIMAL_WIRE_CAPACITY];
    size_t length;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(decimal, "12.34", sizeof("12.34") - 1, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_encode_wire(decimal, decimal_qualifier, wire, sizeof(wire), &length));
    make_result(SQLI_TYPE_DECIMAL, decimal_qualifier, wire, length);
    int64_t integer = 42;
    double floating = 42;
    bool is_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_INEXACT, sqli_result_get_int64(result, value_column, &integer, &is_null));
    TEST_ASSERT_EQUAL_INT64(42, integer);
    TEST_ASSERT_TRUE(is_null);
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_result_get_double(result, value_column, &floating, &is_null));
    TEST_ASSERT_TRUE(floating == 42);
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_double(result, value_column, &floating, &is_null));
    TEST_ASSERT_TRUE(floating > 12.339 && floating < 12.341);
    TEST_ASSERT_FALSE(is_null);
    const uint8_t boolean_true[] = {0, 0, 0, 0, 1, 0xff};
    make_result(SQLI_TYPE_BOOL, sizeof(boolean_true), boolean_true, sizeof(boolean_true));
    bool boolean = false;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_bool(result, value_column, &boolean, &is_null));
    TEST_ASSERT_TRUE(boolean);
    const uint8_t boolean_bad[] = {0, 0, 0, 0, 1, 0x42};
    make_result(SQLI_TYPE_BOOL, sizeof(boolean_bad), boolean_bad, sizeof(boolean_bad));
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_bool(result, value_column, &boolean, &is_null));
    TEST_ASSERT_TRUE(boolean);
    TEST_ASSERT_FALSE(is_null);
    const uint8_t boolean_null[] = {1, 0, 0, 0, 0};
    make_result(SQLI_TYPE_BOOL, sizeof(boolean_null), boolean_null, sizeof(boolean_null));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_bool(result, value_column, &boolean, &is_null));
    TEST_ASSERT_TRUE(boolean);
    TEST_ASSERT_TRUE(is_null);
    const uint8_t date[] = {0, 0, 0, 0};
    make_result(SQLI_TYPE_DATE, sizeof(date), date, sizeof(date));
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_MISMATCH, sqli_result_get_int64(result, value_column, &integer, &is_null));
    TEST_ASSERT_EQUAL_INT64(42, integer);
    TEST_ASSERT_TRUE(is_null);
}

static void test_zero_based_parameter_boundaries_and_status_text(void)
{
    enum { first_parameter = 0, last_parameter = 2, parameter_count = 3 };
    sqli_bound_param parameters[parameter_count] = {0};
    sqli_stmt_t stmt = {.params = parameters, .param_count = parameter_count};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_int(&stmt, first_parameter, 11));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_int(&stmt, last_parameter, 22));
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_bind_int(&stmt, parameter_count, 99));
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_bind_int(&stmt, SIZE_MAX, 99));
    TEST_ASSERT_EQUAL_INT(11, parameters[first_parameter].value.ival);
    TEST_ASSERT_EQUAL_INT(22, parameters[last_parameter].value.ival);
    for (int status = SQLI_OK; status <= SQLI_TYPE_MISMATCH; status++) {
        TEST_ASSERT_NOT_NULL(sqli_status_name((sqli_status)status));
        TEST_ASSERT_NOT_NULL(sqli_status_description((sqli_status)status));
        TEST_ASSERT_TRUE(sqli_status_description((sqli_status)status)[0] != '\0');
    }
    TEST_ASSERT_EQUAL_STRING("SQLI_UNKNOWN_STATUS", sqli_status_name((sqli_status)INT32_MAX));
    sqli_sblob_read_cursor_t *cursor = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_sblob_reader_open(NULL, NULL, &cursor));
    TEST_ASSERT_NULL(cursor);
    size_t count = 42;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_sblob_reader_read(NULL, NULL, 0, &count));
    TEST_ASSERT_EQUAL_UINT(42, count);
    sqli_sblob_reader_destroy(NULL);
}

static void test_whole_value_buffer_contract(void)
{
    const uint8_t text[] = {3, 'a', 'b', 'c'};
    make_result(SQLI_TYPE_VARCHAR, 16, text, sizeof(text));
    size_t required = 99;
    bool is_null = true;
    result->last_was_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_string_len(result, value_column, NULL, 0, &required, &is_null));
    TEST_ASSERT_EQUAL_UINT(4, required);
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_TRUE(result->last_was_null);
    uint8_t buffer[8];
    memset(buffer, guard, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(SQLI_BUFFER_TOO_SMALL, sqli_result_get_string_len(result, value_column,
        (char *)buffer, 3, &required, &is_null));
    TEST_ASSERT_EACH_EQUAL_UINT8(guard, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_string_len(result, value_column,
        (char *)buffer, 4, &required, &is_null));
    TEST_ASSERT_EQUAL_STRING("abc", buffer);
    TEST_ASSERT_EQUAL_UINT8(guard, buffer[4]);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_bytes(result, value_column, NULL, 0, &required, &is_null));
    TEST_ASSERT_EQUAL_UINT(3, required);
    memset(buffer, guard, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(SQLI_BUFFER_TOO_SMALL, sqli_result_get_bytes(result, value_column, buffer, 2, &required, &is_null));
    TEST_ASSERT_EACH_EQUAL_UINT8(guard, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_bytes(result, value_column, buffer, 3, &required, &is_null));
    TEST_ASSERT_EQUAL_MEMORY("abc", buffer, 3);
    TEST_ASSERT_EQUAL_UINT8(guard, buffer[3]);
    required = 99;
    is_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_result_get_bytes(result, SIZE_MAX, buffer, sizeof(buffer), &required, &is_null));
    TEST_ASSERT_EQUAL_UINT(99, required);
    TEST_ASSERT_TRUE(is_null);
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_result_get_string_len(result, value_column,
        (char *)buffer, sizeof(buffer), &required, &is_null));
    TEST_ASSERT_EQUAL_UINT(99, required);
    TEST_ASSERT_TRUE(is_null);
    const uint8_t null_integer[] = {0x80, 0, 0, 0};
    make_result(SQLI_TYPE_INT, sizeof(null_integer), null_integer, sizeof(null_integer));
    memset(buffer, guard, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_bytes(result, value_column, buffer, sizeof(buffer), &required, &is_null));
    TEST_ASSERT_EQUAL_UINT(0, required);
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_EACH_EQUAL_UINT8(guard, buffer, sizeof(buffer));
}

static void test_native_date_decimal_bindings(void)
{
    const sqli_date_t date = {.year = 2000, .month = 2, .day = 29};
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_date(&statement, 0, &date));
    const uint8_t expected[] = {0, 0, 0x8e, 0xe8};
    TEST_ASSERT_EQUAL_MEMORY(expected, parameter.native_bytes, sizeof(expected));
    TEST_ASSERT_EQUAL_INT(SQLI_BIND_DATE, parameter.type);
    sqli_decimal_t *allocation = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_decimal_create(&allocation));
    const sqli_date_t invalid = {.year = 1900, .month = 2, .day = 29};
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_bind_date(&statement, 0, &invalid));
    TEST_ASSERT_EQUAL_MEMORY(expected, parameter.native_bytes, sizeof(expected));
    TEST_ASSERT_NOT_EQUAL(SQLI_OK, sqli_bind_date_string(&statement, 0, "2"));
    const sqli_decimal_target_t target = {.precision = 8, .scale = 2};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(decimal, "12.34", 5, false));
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_decimal(&statement, 0, decimal, &target));
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_decimal_create(&allocation));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_stmt_batch_add(&statement));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_null(decimal));
    TEST_ASSERT_FALSE(parameter.is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_decimal(&statement, 0, decimal, &target));
    TEST_ASSERT_TRUE(parameter.is_null);
    TEST_ASSERT_FALSE(statement.batch_rows[0].params[0].is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(decimal, "12.345", 6, false));
    TEST_ASSERT_EQUAL_INT(SQLI_INEXACT, sqli_bind_decimal(&statement, 0, decimal, &target));
    TEST_ASSERT_TRUE(parameter.is_null);
    const sqli_decimal_target_t floating = {.precision = 5, .floating_scale = true};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(decimal, "1E+100", 6, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_decimal(&statement, 0, decimal, &floating));
    TEST_ASSERT_EQUAL_UINT16(0xff, parameter.native_qualifier);
}

static void test_statement_fetch_status(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_stmt_fetch(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_stmt_fetch(&statement));
    statement.result_valid = true;
    statement.result.fetch_status = SQLI_IO_ERROR;
    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR, sqli_stmt_fetch(&statement));
    TEST_ASSERT_FALSE(sqli_stmt_next(&statement));
    statement.result.fetch_status = SQLI_OK;
    statement.result.eof = true;
    TEST_ASSERT_EQUAL_INT(SQLI_EOF, sqli_stmt_fetch(&statement));
    TEST_ASSERT_EQUAL_PTR(&statement.result, sqli_stmt_result(&statement));
}

static void test_opaque_sblob_allocation_and_views(void)
{
    sqli_conn_t connection = {.state = SQLI_CONN_READY, .socket_fd = -1};
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_sblob_create(&connection, SQLI_SBLOB_BLOB, NULL, &lob));
    TEST_ASSERT_NULL(lob);
    bool open = true;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_sblob_is_open(NULL, &open));
    TEST_ASSERT_TRUE(open);
    /* Private fixture, representing a closed, validated server handle. */
    lob = calloc(1, sizeof(*lob));
    TEST_ASSERT_NOT_NULL(lob);
    lob->lofd = -1;
    lob->type = SQLI_SBLOB_CLOB;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_sblob_is_open(lob, &open));
    TEST_ASSERT_FALSE(open);
    sqli_sblob_type type = SQLI_SBLOB_BLOB;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_sblob_get_type(lob, &type));
    TEST_ASSERT_EQUAL_INT(SQLI_SBLOB_CLOB, type);
    sqli_sblob_destroy(NULL);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_scalar_narrowing_null_and_overflow);
    RUN_TEST(test_scalar_exact_decimal_and_boolean_contracts);
    RUN_TEST(test_zero_based_parameter_boundaries_and_status_text);
    RUN_TEST(test_whole_value_buffer_contract);
    RUN_TEST(test_native_date_decimal_bindings);
    RUN_TEST(test_statement_fetch_status);
    RUN_TEST(test_opaque_sblob_allocation_and_views);
    return UNITY_END();
}
