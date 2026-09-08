#include "libsqli/sqli_temporal.h"
#include "sqli_internal.h"
#include "allocation_test.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

enum { temporal_column = 0, sentinel_column = 1, column_count = 2, integer_width = 4 };
static sqli_datetime_t *datetime;
static sqli_interval_t *interval;
static sqli_result_t *result;
static sqli_bound_param parameter;
static sqli_stmt_t statement;
static const sqli_temporal_range_t full_range = {SQLI_FIELD_YEAR, SQLI_FIELD_SECOND, 0};
static const sqli_temporal_range_t day_range = {SQLI_FIELD_DAY, SQLI_FIELD_SECOND, 0};
static const uint8_t datetime_wire[] = {0xc7, 20, 26, 6, 20, 12, 34, 56};
static const uint8_t interval_wire[] = {0xc4, 12, 3, 4, 5};

void setUp(void)
{
    sqli_test_allow_allocations();
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_create(&datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_create(&interval));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_parse_iso(datetime, "2000-02-29T01:02:03", 19, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_parse(interval, &day_range, "1 02:03:04", 10, false));
    parameter = (sqli_bound_param){0};
    statement = (sqli_stmt_t){.params = &parameter, .param_count = 1};
}

void tearDown(void)
{
    sqli_test_allow_allocations();
    sqli_stmt_batch_clear(&statement);
    free(parameter.sval);
    free(parameter.bval);
    sqli_result_destroy(result);
    result = NULL;
    sqli_datetime_destroy(datetime);
    sqli_interval_destroy(interval);
    datetime = NULL;
    interval = NULL;
}

static void make_result(bool is_interval, bool is_null)
{
    const uint8_t *wire = is_interval ? interval_wire : datetime_wire;
    size_t width = is_interval ? sizeof(interval_wire) : sizeof(datetime_wire);
    result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);
    result->columns = calloc(column_count, sizeof(*result->columns));
    result->rows = calloc(1, sizeof(*result->rows));
    result->row_lens = calloc(1, sizeof(*result->row_lens));
    TEST_ASSERT_NOT_NULL(result->columns);
    TEST_ASSERT_NOT_NULL(result->rows);
    TEST_ASSERT_NOT_NULL(result->row_lens);
    result->row_count = result->row_capacity = 1;
    result->rows[0] = calloc(width + integer_width, 1);
    TEST_ASSERT_NOT_NULL(result->rows[0]);
    if (!is_null)
        memcpy(result->rows[0], wire, width);
    result->rows[0][width + integer_width - 1] = 42;
    result->row_lens[0] = width + integer_width;
    result->column_count = column_count;
    result->columns[temporal_column].type = is_interval ? SQLI_TYPE_INTERVAL : SQLI_TYPE_DATETIME;
    result->columns[temporal_column].encoded_length = is_interval ? 0x084a : 0x0e0a;
    result->columns[sentinel_column].type = SQLI_TYPE_INT;
    result->columns[sentinel_column].encoded_length = integer_width;
    result->cursor = result->current_row = result->cur_cache_row = -1;
    result->at_before_first = true;
    result->eof = result->saw_done = true;
}

static void expect_datetime(const char *expected)
{
    char text[SQLI_TEMPORAL_MAX_TEXT];
    size_t required;
    bool is_null;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_format_iso(datetime, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_EQUAL_STRING(expected, text);
}

static void test_datetime_getter_ownership_and_errors(void)
{
    make_result(false, false);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_result_get_datetime(result, temporal_column, datetime));
    expect_datetime("2000-02-29T01:02:03");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    result->last_was_null = true;
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_datetime(result, temporal_column, datetime));
    TEST_ASSERT_TRUE(result->last_was_null);
    expect_datetime("2026-06-20T12:34:56");
    sqli_datetime_t *allocation = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_datetime_create(&allocation));
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_MISMATCH, sqli_result_get_datetime(result, sentinel_column, datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_MISMATCH, sqli_result_get_interval(result, temporal_column, interval));
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_result_get_datetime(result, SIZE_MAX, datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_result_get_datetime(NULL, 0, datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_result_get_datetime(result, 0, NULL));
    result->tuple_buffer[3] = 13; /* Invalid calendar month. */
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_datetime(result, temporal_column, datetime));
    expect_datetime("2026-06-20T12:34:56");
    result->columns[0].encoded_length = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_datetime(result, temporal_column, datetime));
    result->fetch_status = SQLI_IO_ERROR;
    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR, sqli_result_get_datetime(result, temporal_column, datetime));
    sqli_result_destroy(result);
    result = NULL;
    expect_datetime("2026-06-20T12:34:56");
}

static void test_interval_getter_and_typed_null(void)
{
    make_result(true, false);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_interval(result, temporal_column, interval));
    sqli_interval_parts_t parts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_get_parts(interval, &parts));
    TEST_ASSERT_EQUAL_UINT64(12, parts.days);
    TEST_ASSERT_EQUAL_UINT64(3, parts.hours);
    sqli_interval_t *allocation = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_interval_create(&allocation));
    result->tuple_buffer[2] = 24; /* Subordinate hour must be below 24. */
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_interval(result, temporal_column, interval));
    result->cur_col_data_len[0]--;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_interval(result, temporal_column, interval));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_get_parts(interval, &parts));
    TEST_ASSERT_EQUAL_UINT64(3, parts.hours);
    sqli_result_destroy(result);
    result = NULL;
    make_result(true, true);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_MISMATCH, sqli_result_get_datetime(result, temporal_column, datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_interval(result, temporal_column, interval));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_get_parts(interval, &parts));
    TEST_ASSERT_TRUE(parts.is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_FIELD_DAY, parts.range.first);
    TEST_ASSERT_EQUAL_INT(SQLI_EOF, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_result_get_interval(result, temporal_column, interval));
    sqli_result_destroy(result);
    result = NULL;
    make_result(false, true);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_datetime(result, temporal_column, datetime));
    bool is_null;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_is_null(datetime, &is_null));
    TEST_ASSERT_TRUE(is_null);
}

static void test_native_bind_copy_batch_and_atomic_failure(void)
{
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_datetime(&statement, 0, datetime, &full_range));
    TEST_ASSERT_EQUAL_INT(SQLI_BIND_DATETIME, parameter.type);
    TEST_ASSERT_EQUAL_UINT16(0x0e0a, parameter.native_qualifier);
    sqli_datetime_t *allocation = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_datetime_create(&allocation));
    sqli_bound_param original = parameter;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_stmt_batch_add(&statement));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_set_null(datetime));
    TEST_ASSERT_FALSE(parameter.is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_datetime(&statement, 0, datetime, &full_range));
    TEST_ASSERT_TRUE(parameter.is_null);
    TEST_ASSERT_FALSE(statement.batch_rows[0].params[0].is_null);
    TEST_ASSERT_EQUAL_MEMORY(original.native_bytes, statement.batch_rows[0].params[0].native_bytes,
                             original.native_length);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_interval(&statement, 0, interval, &day_range, 3));
    original = parameter;
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_bind_interval(&statement, 0, interval, &day_range, 0));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_bind_datetime(&statement, 0, NULL, &full_range));
    TEST_ASSERT_EQUAL_MEMORY(original.native_bytes, parameter.native_bytes, original.native_length);
    TEST_ASSERT_EQUAL_UINT16(original.native_qualifier, parameter.native_qualifier);
    TEST_ASSERT_EQUAL_INT(SQLI_BIND_INTERVAL, parameter.type);
}

static void test_bind_precision_and_range_failures(void)
{
    const sqli_temporal_range_t fraction = {SQLI_FIELD_SECOND, SQLI_FIELD_FRACTION, 5};
    const sqli_temporal_range_t wider = {SQLI_FIELD_SECOND, SQLI_FIELD_FRACTION, 9};
    const sqli_interval_parts_t parts = {.range = wider, .seconds = 12, .nanosecond = 123456789};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_set_parts(interval, &parts));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_int(&statement, 0, 42));
    TEST_ASSERT_EQUAL_INT(SQLI_INEXACT, sqli_bind_interval(&statement, 0, interval, &fraction, 2));
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_bind_interval(&statement, 0, interval, &wider, 2));
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_bind_interval(&statement, 0, interval, &fraction, 1));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_bind_interval(&statement, 0, interval, &day_range, 3));
    TEST_ASSERT_EQUAL_INT(SQLI_BIND_INT, parameter.type);
    TEST_ASSERT_EQUAL_INT(42, parameter.value.ival);
    const sqli_temporal_range_t coarse = {SQLI_FIELD_YEAR, SQLI_FIELD_FRACTION, 3};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_parse_iso(datetime, "2000-02-29T01:02:03.12345", 25, false));
    TEST_ASSERT_EQUAL_INT(SQLI_INEXACT, sqli_bind_datetime(&statement, 0, datetime, &coarse));
    TEST_ASSERT_EQUAL_INT(SQLI_BIND_INT, parameter.type);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_datetime_getter_ownership_and_errors);
    RUN_TEST(test_interval_getter_and_typed_null);
    RUN_TEST(test_native_bind_copy_batch_and_atomic_failure);
    RUN_TEST(test_bind_precision_and_range_failures);
    return UNITY_END();
}
