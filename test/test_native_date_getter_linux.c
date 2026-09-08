#include "libsqli/sqli_temporal.h"
#include "libsqli/sqli_decimal.h"
#include "libsqli/sqli.h"
#include "unity.h"
#include "allocation_test.h"
#include "sqli_internal.h"

#include <stdlib.h>
#include <string.h>

enum { date_column = 1, sentinel_column = 2, column_count = 3,
       date_offset = 2, date_width = 4, tuple_size = 10 };
static sqli_result_t *result;
static sqli_conn_t *connection;
static sqli_date_t value;
static const uint8_t leap_day[] = {0, 0, 0x8e, 0xe8};

void setUp(void)
{
    sqli_test_allow_allocations();
    value = (sqli_date_t){.year = 2026, .month = 9, .day = 8};
}
void tearDown(void)
{
    sqli_test_allow_allocations();
    sqli_result_destroy(result);
    result = NULL;
    sqli_destroy(connection);
    connection = NULL;
}
static void make_result(const uint8_t bytes[date_width])
{
    result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);
    result->columns = calloc(column_count, sizeof(*result->columns));
    result->rows = calloc(1, sizeof(*result->rows));
    result->row_lens = calloc(1, sizeof(*result->row_lens));
    TEST_ASSERT_NOT_NULL(result->columns);
    TEST_ASSERT_NOT_NULL(result->rows);
    TEST_ASSERT_NOT_NULL(result->row_lens);
    result->row_count = result->row_capacity = 1;
    result->rows[0] = calloc(tuple_size, 1);
    TEST_ASSERT_NOT_NULL(result->rows[0]);
    result->rows[0][0] = 1;
    result->rows[0][1] = 'x';
    memcpy(result->rows[0] + date_offset, bytes, date_width);
    result->rows[0][tuple_size - 1] = 42;
    result->row_lens[0] = tuple_size;
    result->column_count = column_count;
    result->columns[0].type = SQLI_TYPE_VARCHAR;
    result->columns[0].encoded_length = 8;
    result->columns[date_column].type = SQLI_TYPE_DATE;
    result->columns[date_column].encoded_length = date_width;
    result->columns[sentinel_column].type = SQLI_TYPE_INT;
    result->columns[sentinel_column].encoded_length = 4;
    result->cursor = result->current_row = result->cur_cache_row = -1;
    result->at_before_first = true;
    result->eof = result->saw_done = true;
}
static void expect_original(void)
{
    TEST_ASSERT_EQUAL_INT(2026, value.year);
    TEST_ASSERT_EQUAL_UINT(9, value.month);
    TEST_ASSERT_EQUAL_UINT(8, value.day);
    TEST_ASSERT_FALSE(value.is_null);
}
static void test_calendar_ownership_and_conveniences(void)
{
    make_result(leap_day);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    result->last_was_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_date(result, date_column, &value));
    TEST_ASSERT_TRUE(result->last_was_null);
    TEST_ASSERT_EQUAL_INT(2000, value.year);
    TEST_ASSERT_EQUAL_UINT(2, value.month);
    TEST_ASSERT_EQUAL_UINT(29, value.day);
    TEST_ASSERT_FALSE(value.is_null);
    TEST_ASSERT_EQUAL_STRING("2000-02-29", sqli_result_get_date_string(result, date_column));
    TEST_ASSERT_FALSE(result->last_was_null);
    TEST_ASSERT_EQUAL_INT(42, sqli_result_get_int(result, sentinel_column));
    sqli_timestamp_t timestamp;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_timestamp(result, date_column, &timestamp));
    TEST_ASSERT_EQUAL_INT(2000, timestamp.year);
    TEST_ASSERT_EQUAL_INT(29, timestamp.day);
    TEST_ASSERT_EQUAL_INT(0, timestamp.hour);
    TEST_ASSERT_EQUAL_INT(SQLI_EOF, sqli_result_fetch(result));
    sqli_result_destroy(result);
    result = NULL;
    char text[SQLI_TEMPORAL_MAX_TEXT];
    size_t required;
    bool is_null;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_date_format(&value, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_EQUAL_STRING("2000-02-29", text);
}
static void test_null_and_wrong_type_null(void)
{
    const uint8_t null_bytes[] = {0x80, 0, 0, 0};
    make_result(null_bytes);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_date(result, date_column, &value));
    TEST_ASSERT_TRUE(value.is_null);
    TEST_ASSERT_EQUAL_INT(0, value.year);
    TEST_ASSERT_EQUAL_STRING("", sqli_result_get_date_string(result, date_column));
    TEST_ASSERT_TRUE(result->last_was_null);
    result->columns[date_column].type = SQLI_TYPE_INT;
    value = (sqli_date_t){.year = 2026, .month = 9, .day = 8};
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_MISMATCH, sqli_result_get_date(result, date_column, &value));
    expect_original();
}
static void test_invalid_days_and_descriptor_preserve_output(void)
{
    static const uint8_t invalid[][date_width] = {
        {0xff, 0xf5, 0x6a, 0xa5}, /* Before 0001-01-01. */
        {0, 0x2d, 0x24, 0x81},   /* After 9999-12-31. */
        {0x7f, 0xff, 0xff, 0xff}, {0x80, 0, 0, 1}
    };
    make_result(leap_day);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        memcpy(result->rows[0] + date_offset, invalid[i], date_width);
        TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_date(result, date_column, &value));
        TEST_ASSERT_EQUAL_STRING("", sqli_result_get_date_string(result, date_column));
        expect_original();
    }
    memcpy(result->rows[0] + date_offset, leap_day, date_width);
    result->columns[date_column].encoded_length = 0x10004;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_date(result, date_column, &value));
    result->columns[date_column].encoded_length = date_width;
    result->cur_col_data_len[date_column]--;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_date(result, date_column, &value));
    result->cur_col_data_start[date_column] = SIZE_MAX;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_date(result, date_column, &value));
    expect_original();
}
static void test_row_state_and_arguments_preserve_output(void)
{
    make_result(leap_day);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_result_get_date(NULL, date_column, &value));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_result_get_date(result, date_column, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_result_get_date(result, SIZE_MAX, &value));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_result_get_date(result, date_column, &value));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_MISMATCH, sqli_result_get_date(result, 0, &value));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&connection));
    result->owner_conn = connection;
    connection->rollback_epoch++;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_result_get_date(result, date_column, &value));
    TEST_ASSERT_FALSE(connection->error_info.has_error);
    expect_original();
    result->owner_conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_EOF, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_result_get_date(result, date_column, &value));
    expect_original();
}
static void test_fetch_error_propagates_and_reads_do_not_allocate(void)
{
    make_result(leap_day);
    result->row_lens[0]--;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_date(result, date_column, &value));
    expect_original();
    sqli_result_destroy(result);
    result = NULL;
    make_result(leap_day);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_date(result, date_column, &value));
    TEST_ASSERT_EQUAL_STRING("2000-02-29", sqli_result_get_date_string(result, date_column));
    sqli_decimal_t *unexpected = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_decimal_create(&unexpected));
    TEST_ASSERT_NULL(unexpected);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_calendar_ownership_and_conveniences);
    RUN_TEST(test_null_and_wrong_type_null);
    RUN_TEST(test_invalid_days_and_descriptor_preserve_output);
    RUN_TEST(test_row_state_and_arguments_preserve_output);
    RUN_TEST(test_fetch_error_propagates_and_reads_do_not_allocate);
    return UNITY_END();
}
