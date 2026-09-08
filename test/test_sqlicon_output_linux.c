#include "sqlicon.h"
#include "sqli_internal.h"
#include "allocation_test.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

enum { test_column = 0, long_text_length = 6000, opaque_prefix = 5 };
static sqli_result_t *result;
static FILE *output;
static sqlicon_runtime runtime;

void setUp(void)
{
    sqli_test_allow_allocations();
    runtime = (sqlicon_runtime){.mode = SQLICON_OUTPUT_CSV, .null_repr = "NULL"};
    output = tmpfile();
    TEST_ASSERT_NOT_NULL(output);
}

void tearDown(void)
{
    sqli_test_allow_allocations();
    sqli_result_destroy(result);
    result = NULL;
    fclose(output);
}

static void make_result(sqli_column_type type, const uint8_t *wire, size_t length)
{
    sqli_result_destroy(result);
    result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);
    result->columns = calloc(1, sizeof(*result->columns));
    result->rows = calloc(1, sizeof(*result->rows));
    result->row_lens = calloc(1, sizeof(*result->row_lens));
    TEST_ASSERT_NOT_NULL(result->columns);
    TEST_ASSERT_NOT_NULL(result->rows);
    TEST_ASSERT_NOT_NULL(result->row_lens);
    result->rows[0] = malloc(length);
    TEST_ASSERT_NOT_NULL(result->rows[0]);
    memcpy(result->rows[0], wire, length);
    result->row_lens[0] = length;
    result->column_count = result->row_count = result->row_capacity = 1;
    result->columns[test_column].type = type;
    result->columns[test_column].encoded_length = (uint32_t)length;
    result->cursor = result->current_row = result->cur_cache_row = -1;
    result->at_before_first = true;
    result->eof = result->saw_done = true;
}

static void expect_output(const char *expected)
{
    enum { output_capacity = 128 };
    char text[output_capacity] = {0};
    rewind(output);
    size_t count = fread(text, 1, sizeof(text) - 1, output);
    TEST_ASSERT_EQUAL_UINT(strlen(expected), count);
    TEST_ASSERT_EQUAL_STRING(expected, text);
}

static void test_long_text_all_modes(void)
{
    uint8_t wire[opaque_prefix + long_text_length] = {0};
    wire[3] = (uint8_t)(long_text_length >> 8);
    wire[4] = (uint8_t)(long_text_length & 0xff);
    memset(wire + opaque_prefix, 'x', long_text_length);
    for (int mode = SQLICON_OUTPUT_ALIGNED; mode <= SQLICON_OUTPUT_MARKDOWN; mode++) {
        make_result(SQLI_TYPE_LVARCHAR, wire, sizeof(wire));
        runtime.mode = (sqlicon_output_mode)mode;
        long start = ftell(output);
        TEST_ASSERT_EQUAL_INT(SQLI_OK, print_result_rows(output, &runtime, result));
        long end = ftell(output);
        TEST_ASSERT_TRUE(end - start >= long_text_length);
        TEST_ASSERT_EQUAL_INT(0, fseek(output, start, SEEK_SET));
        size_t count = 0;
        for (long pos = start; pos < end; pos++)
            if (fgetc(output) == 'x') count++;
        TEST_ASSERT_EQUAL_UINT(long_text_length, count);
        TEST_ASSERT_EQUAL_INT(0, fseek(output, end, SEEK_SET));
    }
}

static void test_lob_placeholders_and_null(void)
{
    const uint8_t locator[] = {0, 0, 0, 0, 4, 0xde, 0xad, 0xbe, 0xef};
    make_result(SQLI_TYPE_BLOB, locator, sizeof(locator));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, print_result_rows(output, &runtime, result));
    make_result(SQLI_TYPE_CLOB, locator, sizeof(locator));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, print_result_rows(output, &runtime, result));
    const uint8_t null_lob[] = {1, 0, 0, 0, 0};
    make_result(SQLI_TYPE_BLOB, null_lob, sizeof(null_lob));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, print_result_rows(output, &runtime, result));
    expect_output("<BLOB>\n<CLOB>\nNULL\n");
}

static void test_native_date_and_conversion_failure(void)
{
    const uint8_t date[] = {0, 0, 0x8e, 0xe8};
    make_result(SQLI_TYPE_DATE, date, sizeof(date));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, print_result_rows(output, &runtime, result));
    const uint8_t invalid_date[] = {0x7f, 0xff, 0xff, 0xff};
    make_result(SQLI_TYPE_DATE, invalid_date, sizeof(invalid_date));
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, print_result_rows(output, &runtime, result));
    expect_output("2000-02-29\n");
}

static void test_fetch_allocation_and_embedded_null_failures(void)
{
    const uint8_t text[] = {3, 'a', 0, 'b'};
    make_result(SQLI_TYPE_VARCHAR, text, sizeof(text));
    result->fetch_status = SQLI_IO_ERROR;
    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR, print_result_rows(output, &runtime, result));
    make_result(SQLI_TYPE_VARCHAR, text, sizeof(text));
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, print_result_rows(output, &runtime, result));
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, print_result_rows(output, &runtime, result));
    TEST_ASSERT_EQUAL_INT(0, ftell(output));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_long_text_all_modes);
    RUN_TEST(test_lob_placeholders_and_null);
    RUN_TEST(test_native_date_and_conversion_failure);
    RUN_TEST(test_fetch_allocation_and_embedded_null_failures);
    return UNITY_END();
}
