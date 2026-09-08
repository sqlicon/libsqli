#include "libsqli/sqli_decimal.h"
#include "libsqli/sqli.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

enum {
    text_capacity = 256,
    large_coefficient_digits = 90,
    test_exit_usage = 2
};

/* Unity owns the test lifecycle; tearDown also cleans up after assertions. */
static sqli_decimal_t *left;
static sqli_decimal_t *right;
static char *scratch;

void setUp(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_create(&left));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_create(&right));
}

void tearDown(void)
{
    sqli_decimal_destroy(left);
    sqli_decimal_destroy(right);
    free(scratch);
    left = NULL;
    right = NULL;
    scratch = NULL;
}

static void expect_text(const sqli_decimal_t *value, const char *expected)
{
    char text[text_capacity];
    size_t required = 0;
    bool is_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_format(value, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_EQUAL_STRING(expected, text);
    TEST_ASSERT_EQUAL_UINT(strlen(expected) + 1, required);
}

static void parse(sqli_decimal_t *value, const char *text)
{
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(value, text, strlen(text), false));
}

static void test_null_and_invalid_arguments(void)
{
    bool is_null = false;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_is_null(left, &is_null));
    TEST_ASSERT_TRUE(is_null);
    int ordering = 7;
    int64_t integer = 7;
    TEST_ASSERT_EQUAL_INT(SQLI_NULL_VALUE, sqli_decimal_compare(left, right, &ordering));
    TEST_ASSERT_EQUAL_INT(7, ordering);
    TEST_ASSERT_EQUAL_INT(SQLI_NULL_VALUE, sqli_decimal_to_i64(left, &integer));
    TEST_ASSERT_EQUAL_INT64(7, integer);
    TEST_ASSERT_EQUAL_INT(SQLI_NULL_VALUE, sqli_decimal_rescale_exact(left, 0));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_create(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_set_null(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_is_null(NULL, &is_null));
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_get_parts(left, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_set_parts(left, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_copy(left, NULL));
    sqli_decimal_destroy(NULL);
    parse(left, "12.00");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(left, NULL, SIZE_MAX, true));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_is_null(left, &is_null));
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_parse(left, NULL, 0, false));
}

static void test_parts_and_ownership(void)
{
    uint32_t limbs[] = {678901234, 12345, 0};
    sqli_decimal_parts_t parts = {
        .limbs = limbs, .limb_count = sizeof(limbs) / sizeof(limbs[0]), .scale = 4
    };
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_parts(left, &parts));
    limbs[0] = 0;
    expect_text(left, "1234567890.1234");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_get_parts(left, &parts));
    TEST_ASSERT_EQUAL_UINT(2, parts.limb_count);
    TEST_ASSERT_EQUAL_UINT32(678901234, parts.limbs[0]);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_parts(left, &parts));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_copy(right, left));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_copy(right, right));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_i64(left, 0, 0));
    expect_text(right, "1234567890.1234");
    uint32_t invalid = SQLI_DECIMAL_LIMB_BASE;
    parts.limbs = &invalid;
    parts.limb_count = 1;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_set_parts(right, &parts));
    expect_text(right, "1234567890.1234");
    parts.limbs = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_set_parts(right, &parts));
    parts.limb_count = SIZE_MAX;
    TEST_ASSERT_EQUAL_INT(SQLI_LIMIT_EXCEEDED, sqli_decimal_set_parts(right, &parts));
}

static void test_scale_and_canonical_format(void)
{
    static const struct { const char *input; const char *output; int32_t scale; } cases[] = {
        {"123.4500", "123.4500", 4},
        {"+00123.4500", "123.4500", 4},
        {".5", "0.5", 1},
        {"1.", "1", 0},
        {"-0.00", "0.00", 2},
        {"1e3", "1E+3", -3},
        {"1.2300E+2", "123.00", 2},
        {"0.000001", "0.000001", 6},
        {"0.0000001", "1E-7", 7},
        {"-0.00000012300", "-1.2300E-7", 11},
        {"0E-7", "0E-7", 7},
        {"1E+2147483648", "1E+2147483648", INT32_MIN},
        {"1E-2147483647", "1E-2147483647", INT32_MAX},
        {"0E+2147483648", "0E+2147483648", INT32_MIN},
        {"9.9E+2147483649", "9.9E+2147483649", INT32_MIN}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        parse(left, cases[i].input);
        expect_text(left, cases[i].output);
        sqli_decimal_parts_t parts;
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_get_parts(left, &parts));
        TEST_ASSERT_EQUAL_INT32(cases[i].scale, parts.scale);
        parse(right, cases[i].output);
        int ordering = 1;
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_compare(left, right, &ordering));
        TEST_ASSERT_EQUAL_INT(0, ordering);
    }
}

static void test_parse_rejects_malformed_and_preserves_value(void)
{
    static const char *const invalid[] = {
        "", "+", "-", ".", "1..0", " 1", "1 ", "1,2", "NaN", "Infinity",
        "NULL", "1e", "1e+", "1e-", "1e2x", "1e2.0", "1_000", "0x10"
    };
    parse(left, "42.00");
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
            sqli_decimal_parse(left, invalid[i], strlen(invalid[i]), false));
        expect_text(left, "42.00");
    }
    const char embedded_null[] = {'1', '\0', '2'};
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_decimal_parse(left, embedded_null, sizeof(embedded_null), false));
    const char not_terminated[] = {'1', '2', '.', '3', '0'};
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_parse(left, not_terminated, sizeof(not_terminated), false));
    expect_text(left, "12.30");
    static const char *const outside[] = {
        "1e2147483649", "1e-2147483648", "0.1e-2147483647",
        "1e9223372036854775808", "1e-9223372036854775808"
    };
    for (size_t i = 0; i < sizeof(outside) / sizeof(outside[0]); i++) {
        TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE,
            sqli_decimal_parse(left, outside[i], strlen(outside[i]), false));
        expect_text(left, "12.30");
    }
}

static void test_buffer_contract(void)
{
    parse(left, "123.4500");
    size_t required = 0;
    bool is_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_format(left, NULL, 0, &required, &is_null));
    TEST_ASSERT_EQUAL_UINT(sizeof("123.4500"), required);
    TEST_ASSERT_FALSE(is_null);
    char buffer[sizeof("123.4500")];
    memset(buffer, '#', sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(SQLI_BUFFER_TOO_SMALL,
        sqli_decimal_format(left, buffer, sizeof(buffer) - 1, &required, &is_null));
    for (size_t i = 0; i < sizeof(buffer); i++)
        TEST_ASSERT_EQUAL_CHAR('#', buffer[i]);
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_format(left, buffer, sizeof(buffer), &required, &is_null));
    TEST_ASSERT_EQUAL_STRING("123.4500", buffer);
    required = 123;
    is_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_decimal_format(left, NULL, 1, &required, &is_null));
    TEST_ASSERT_EQUAL_UINT(123, required);
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_null(left));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_format(left, buffer, sizeof(buffer), &required, &is_null));
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_EQUAL_UINT(0, required);
    TEST_ASSERT_EQUAL_STRING("123.4500", buffer);
}

static void test_int64_boundaries(void)
{
    const int64_t integers[] = {INT64_MIN, INT64_MAX, 0, -1, 1};
    for (size_t i = 0; i < sizeof(integers) / sizeof(integers[0]); i++) {
        int64_t output = 0;
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_i64(left, integers[i], 0));
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_to_i64(left, &output));
        TEST_ASSERT_EQUAL_INT64(integers[i], output);
    }
    parse(left, "9223372036854775808");
    int64_t output = 42;
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_decimal_to_i64(left, &output));
    TEST_ASSERT_EQUAL_INT64(42, output);
    parse(left, "-9223372036854775809");
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_decimal_to_i64(left, &output));
    parse(left, "-9223372036854775808.000");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_to_i64(left, &output));
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, output);
    parse(left, "12.001");
    TEST_ASSERT_EQUAL_INT(SQLI_INEXACT, sqli_decimal_to_i64(left, &output));
    parse(left, "1E-2147483647");
    TEST_ASSERT_EQUAL_INT(SQLI_INEXACT, sqli_decimal_to_i64(left, &output));
    parse(left, "1E+2147483648");
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_decimal_to_i64(left, &output));
    parse(left, "12E+2");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_to_i64(left, &output));
    TEST_ASSERT_EQUAL_INT64(1200, output);
}

static void test_compare_without_rescaling(void)
{
    static const struct { const char *left; const char *right; int ordering; } cases[] = {
        {"1.0", "1.00", 0}, {"-1.0", "-1.00", 0},
        {"0E+2147483648", "0E-2147483647", 0},
        {"1E+2147483648", "1E-2147483647", 1},
        {"-2", "-1", -1}, {"0", "-1", 1}, {"0", "1", -1},
        {"1", "0", 1}, {"-1", "0", -1}, {"-1", "1", -1},
        {"1234567890123456789012345678901234567890.01",
         "1234567890123456789012345678901234567890.02", -1},
        {"999999999.999999999", "1000000000", -1}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        parse(left, cases[i].left);
        parse(right, cases[i].right);
        int ordering = 7;
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_compare(left, right, &ordering));
        TEST_ASSERT_EQUAL_INT(cases[i].ordering, ordering);
    }
}

static void test_exact_rescale(void)
{
    parse(left, "123.4500");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_rescale_exact(left, 2));
    expect_text(left, "123.45");
    TEST_ASSERT_EQUAL_INT(SQLI_INEXACT, sqli_decimal_rescale_exact(left, 1));
    expect_text(left, "123.45");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_rescale_exact(left, 13));
    expect_text(left, "123.4500000000000");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_rescale_exact(left, 2));
    parse(left, "999999999999999999999999999999999999");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_rescale_exact(left, 10));
    expect_text(left, "999999999999999999999999999999999999.0000000000");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_rescale_exact(left, 0));
    expect_text(left, "999999999999999999999999999999999999");
    parse(left, "-120000000000000000000000000000000000000000000000000");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_rescale_exact(left, -49));
    expect_text(left, "-1.2E+50");
    sqli_decimal_parts_t parts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_get_parts(left, &parts));
    TEST_ASSERT_EQUAL_INT32(-49, parts.scale);
    parse(left, "0.00");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_rescale_exact(left, INT32_MIN));
    expect_text(left, "0E+2147483648");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_rescale_exact(left, INT32_MAX));
    expect_text(left, "0E-2147483647");
    parse(left, "1E+2147483648");
    TEST_ASSERT_EQUAL_INT(SQLI_LIMIT_EXCEEDED, sqli_decimal_rescale_exact(left, INT32_MAX));
    expect_text(left, "1E+2147483648");
    parse(left, "1E-2147483647");
    TEST_ASSERT_EQUAL_INT(SQLI_INEXACT, sqli_decimal_rescale_exact(left, INT32_MIN));
}

static void test_heap_copy_and_alias(void)
{
    scratch = malloc(large_coefficient_digits + 1);
    TEST_ASSERT_NOT_NULL(scratch);
    memset(scratch, '9', large_coefficient_digits);
    scratch[large_coefficient_digits] = '\0';
    parse(left, scratch);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_copy(right, left));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_copy(left, left));
    sqli_decimal_parts_t parts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_get_parts(left, &parts));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_parts(left, &parts));
    expect_text(left, scratch);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_rescale_exact(left, 1));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_rescale_exact(left, 0));
    expect_text(left, scratch);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_i64(left, 0, 0));
    expect_text(right, scratch);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_copy(right, left));
    expect_text(right, "0");
}

static void test_resource_limit(void)
{
    parse(left, "1");
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_rescale_exact(left, SQLI_DECIMAL_MAX_DIGITS - 1));
    TEST_ASSERT_EQUAL_INT(SQLI_LIMIT_EXCEEDED,
        sqli_decimal_rescale_exact(left, SQLI_DECIMAL_MAX_DIGITS));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_rescale_exact(left, 0));
    expect_text(left, "1");
    TEST_ASSERT_EQUAL_INT(SQLI_LIMIT_EXCEEDED,
        sqli_decimal_parse(left, "1", (size_t)SQLI_DECIMAL_MAX_TEXT + 1, false));
    scratch = malloc((size_t)SQLI_DECIMAL_MAX_DIGITS + 1);
    TEST_ASSERT_NOT_NULL(scratch);
    memset(scratch, '1', (size_t)SQLI_DECIMAL_MAX_DIGITS + 1);
    TEST_ASSERT_EQUAL_INT(SQLI_LIMIT_EXCEEDED,
        sqli_decimal_parse(left, scratch, (size_t)SQLI_DECIMAL_MAX_DIGITS + 1, false));
    expect_text(left, "1");
}

static void test_maximum_coefficient_format_roundtrip(void)
{
    parse(left, "1");
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_rescale_exact(left, SQLI_DECIMAL_MAX_DIGITS - 1));
    sqli_decimal_parts_t parts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_get_parts(left, &parts));
    parts.scale = SQLI_DECIMAL_MAX_DIGITS + 5;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_parts(left, &parts));
    size_t required = 0;
    bool is_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_format(left, NULL, 0, &required, &is_null));
    TEST_ASSERT_LESS_OR_EQUAL_UINT(SQLI_DECIMAL_MAX_TEXT, required);
    scratch = malloc(required);
    TEST_ASSERT_NOT_NULL(scratch);
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_format(left, scratch, required, &required, &is_null));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(right, scratch, required - 1, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_get_parts(right, &parts));
    TEST_ASSERT_EQUAL_INT32(SQLI_DECIMAL_MAX_DIGITS + 5, parts.scale);
    int ordering = 1;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_compare(left, right, &ordering));
    TEST_ASSERT_EQUAL_INT(0, ordering);
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
        return test_exit_usage;
    UNITY_BEGIN();
    RUN_TEST(test_null_and_invalid_arguments);
    RUN_TEST(test_parts_and_ownership);
    RUN_TEST(test_scale_and_canonical_format);
    RUN_TEST(test_parse_rejects_malformed_and_preserves_value);
    RUN_TEST(test_buffer_contract);
    RUN_TEST(test_int64_boundaries);
    RUN_TEST(test_compare_without_rescaling);
    RUN_TEST(test_exact_rescale);
    RUN_TEST(test_heap_copy_and_alias);
    RUN_TEST(test_resource_limit);
    RUN_TEST(test_maximum_coefficient_format_roundtrip);
    return UNITY_END();
}
