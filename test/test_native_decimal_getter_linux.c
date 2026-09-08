#include "libsqli/sqli.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

#include "allocation_test.h"
#include "sqli_internal.h"

enum { native_column = 1, sentinel_column = 2, columns = 3, row_prefix = 2,
       integer_width = 4, text_capacity = 256, large_digits = 100 };
static sqli_result_t *result;
static sqli_decimal_t *value;
static sqli_conn_t *connection;
static const uint8_t fixed_value[] = {0xc2, 1, 23, 45, 0};

void setUp(void)
{
    sqli_test_allow_allocations();
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_create(&value));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(value, "99.00", 5, false));
}
void tearDown(void)
{
    sqli_test_allow_allocations();
    sqli_result_destroy(result);
    result = NULL;
    sqli_destroy(connection);
    connection = NULL;
    sqli_decimal_destroy(value);
    value = NULL;
}
static void make_result(sqli_column_type type, uint32_t descriptor,
                        const uint8_t *bytes, size_t length)
{
    enum { max_payload = 18 };
    TEST_ASSERT_TRUE(length <= max_payload);
    result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);
    result->columns = calloc(columns, sizeof(*result->columns));
    result->rows = calloc(1, sizeof(*result->rows));
    result->row_lens = calloc(1, sizeof(*result->row_lens));
    TEST_ASSERT_NOT_NULL(result->columns);
    TEST_ASSERT_NOT_NULL(result->rows);
    TEST_ASSERT_NOT_NULL(result->row_lens);
    result->row_count = result->row_capacity = 1;
    size_t tuple_length = row_prefix + length + integer_width;
    result->rows[0] = calloc(tuple_length, 1);
    TEST_ASSERT_NOT_NULL(result->rows[0]);
    result->row_lens[0] = tuple_length;
    result->rows[0][0] = 1;
    result->rows[0][1] = 'x';
    memcpy(result->rows[0] + row_prefix, bytes, length);
    result->rows[0][tuple_length - 1] = 42;
    result->column_count = columns;
    result->columns[0].type = SQLI_TYPE_VARCHAR;
    result->columns[0].encoded_length = 8;
    result->columns[native_column].type = type;
    result->columns[native_column].encoded_length = descriptor;
    result->columns[sentinel_column].type = SQLI_TYPE_INT;
    result->columns[sentinel_column].encoded_length = integer_width;
    result->cursor = result->current_row = result->cur_cache_row = -1;
    result->at_before_first = true;
    result->eof = result->saw_done = true;
}
static void expect_text(const char *expected)
{
    char text[text_capacity];
    size_t required;
    bool is_null;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_format(value, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_EQUAL_STRING(expected, text);
}
static void test_native_values_and_owned_lifetime(void)
{
    make_result(SQLI_TYPE_DECIMAL, 0x0804, fixed_value, sizeof(fixed_value));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    result->last_was_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_decimal(result, native_column, value));
    TEST_ASSERT_TRUE(result->last_was_null);
    expect_text("123.4500");
    TEST_ASSERT_EQUAL_INT(42, sqli_result_get_int(result, sentinel_column));
    TEST_ASSERT_EQUAL_INT(SQLI_EOF, sqli_result_fetch(result));
    expect_text("123.4500");
    sqli_result_destroy(result);
    result = NULL;
    expect_text("123.4500");
}
static void test_money_floating_and_null(void)
{
    const uint8_t negative[] = {0x3d, 98, 76, 55, 0};
    make_result(SQLI_TYPE_MONEY, 0x0802, negative, sizeof(negative));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_decimal(result, native_column, value));
    expect_text("-123.45");
    sqli_result_destroy(result);
    result = NULL;
    const uint8_t floating[] = {0xc2, 20, 0, 0, 0, 0, 0};
    make_result(SQLI_TYPE_DECIMAL, 0x0bff, floating, sizeof(floating));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_decimal(result, native_column, value));
    expect_text("2E+3");
    memset(result->rows[0] + row_prefix, 0, sizeof(floating));
    /* The getter validates raw bytes, independently of cached NULL markers. */
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_decimal(result, native_column, value));
    bool is_null = false;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_is_null(value, &is_null));
    TEST_ASSERT_TRUE(is_null);
}
static void test_failure_preserves_destination(void)
{
    make_result(SQLI_TYPE_DECIMAL, 0x0804, fixed_value, sizeof(fixed_value));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_result_get_decimal(NULL, native_column, value));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_result_get_decimal(result, native_column, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_result_get_decimal(result, SIZE_MAX, value));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_result_get_decimal(result, native_column, value));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_MISMATCH, sqli_result_get_decimal(result, 0, value));
    result->rows[0][row_prefix] = 0xc2;
    result->rows[0][row_prefix + 1] = 100;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_decimal(result, native_column, value));
    expect_text("99.00");
    memcpy(result->rows[0] + row_prefix, fixed_value, sizeof(fixed_value));
    result->columns[native_column].encoded_length = 0x1000804;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_decimal(result, native_column, value));
    result->columns[native_column].encoded_length = 0x0804;
    result->cur_col_data_start[native_column] = SIZE_MAX;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_decimal(result, native_column, value));
    result->cur_col_data_start[native_column] = row_prefix;
    result->cur_col_data_len[native_column]--;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_decimal(result, native_column, value));
    TEST_ASSERT_EQUAL_INT(SQLI_EOF, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_result_get_decimal(result, native_column, value));
    expect_text("99.00");
}
static void test_wrong_type_null_and_cursor_invalidation(void)
{
    const uint8_t null_int[] = {0x80, 0, 0, 0};
    make_result(SQLI_TYPE_INT, integer_width, null_int, sizeof(null_int));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_TYPE_MISMATCH, sqli_result_get_decimal(result, native_column, value));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&connection));
    result->owner_conn = connection;
    connection->rollback_epoch++;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_result_get_decimal(result, native_column, value));
    TEST_ASSERT_FALSE(connection->error_info.has_error);
    expect_text("99.00");
}
static void test_fetch_failure_is_preserved(void)
{
    make_result(SQLI_TYPE_DECIMAL, 0x0804, fixed_value, sizeof(fixed_value));
    result->row_lens[0]--;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_result_get_decimal(result, native_column, value));
    expect_text("99.00");
}
static void test_reused_destination_needs_no_allocation(void)
{
    char large[large_digits];
    memset(large, '9', sizeof(large));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(value, large, sizeof(large), false));
    make_result(SQLI_TYPE_DECIMAL, 0x0804, fixed_value, sizeof(fixed_value));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_decimal(result, native_column, value));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_decimal(result, native_column, value));
    expect_text("123.4500");
    sqli_decimal_t *unexpected = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_decimal_create(&unexpected));
    TEST_ASSERT_NULL(unexpected);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_native_values_and_owned_lifetime);
    RUN_TEST(test_money_floating_and_null);
    RUN_TEST(test_failure_preserves_destination);
    RUN_TEST(test_wrong_type_null_and_cursor_invalidation);
    RUN_TEST(test_fetch_failure_is_preserved);
    RUN_TEST(test_reused_destination_needs_no_allocation);
    return UNITY_END();
}
