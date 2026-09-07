#include "libsqli/sqli.h"
#include "allocation_test.h"
#include "sqli_decimal_codec.h"
#include "unity.h"

#include <string.h>

enum { large_digits = 90, small_text_capacity = 32, coefficient_limb_digits = 9 };
static sqli_decimal_t *source;
static sqli_decimal_t *destination;

void setUp(void)
{
    sqli_test_allow_allocations();
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_create(&source));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_create(&destination));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(destination, "42.00", 5, false));
}

void tearDown(void)
{
    sqli_test_allow_allocations();
    sqli_decimal_destroy(source);
    sqli_decimal_destroy(destination);
    source = NULL;
    destination = NULL;
}

static void expect_destination(void)
{
    char buffer[small_text_capacity];
    size_t required = 0;
    bool is_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_format(destination, buffer, sizeof(buffer), &required, &is_null));
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_EQUAL_STRING("42.00", buffer);
}

static void test_create_failure_preserves_output(void)
{
    sqli_decimal_t *out = destination;
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_decimal_create(&out));
    TEST_ASSERT_EQUAL_PTR(destination, out);
    expect_destination();
}

static void test_parse_copy_parts_and_growth_fail_atomically(void)
{
    char text[large_digits];
    memset(text, '9', sizeof(text));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(source, text, sizeof(text), false));
    sqli_decimal_parts_t parts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_get_parts(source, &parts));
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL,
        sqli_decimal_parse(destination, text, sizeof(text), false));
    expect_destination();
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_decimal_copy(destination, source));
    expect_destination();
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_decimal_set_parts(destination, &parts));
    expect_destination();
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_decimal_rescale_exact(destination, large_digits));
    expect_destination();
}

static void test_shrink_failure_preserves_heap_value(void)
{
    char text[large_digits];
    memset(text, '9', sizeof(text));
    /* Removing one base-10^9 limb still leaves a heap-sized coefficient. */
    memset(text + sizeof(text) - coefficient_limb_digits, '0', coefficient_limb_digits);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(source, text, sizeof(text), false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_copy(destination, source));
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_decimal_rescale_exact(destination, -coefficient_limb_digits));
    int ordering = 1;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_compare(source, destination, &ordering));
    TEST_ASSERT_EQUAL_INT(0, ordering);
    sqli_decimal_parts_t parts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_get_parts(destination, &parts));
    TEST_ASSERT_EQUAL_INT32(0, parts.scale);
}

static void test_codecs_do_not_allocate(void)
{
    char text[large_digits];
    memset(text, '0', sizeof(text));
    text[0] = '1';
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(source, text, sizeof(text), false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_copy(destination, source));
    sqli_decimal_parts_t parts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_get_parts(source, &parts));
    parts.scale = large_digits - 1;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_parts(source, &parts));
    uint8_t bytes[SQLI_DECIMAL_WIRE_CAPACITY];
    size_t length = 0;
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_encode_wire(source, 0x0100, bytes, sizeof(bytes), &length));
    /* Replacement of a heap-backed destination also needs no new allocation. */
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_decode_wire(bytes, length, 0x0100, destination));
    int64_t value = 0;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_to_i64(destination, &value));
    TEST_ASSERT_EQUAL_INT64(1, value);
    const uint8_t maximum[SQLI_DECIMAL_WIRE_CAPACITY] = {
        0xd0,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,0
    };
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_decode_wire(maximum, sizeof(maximum), 0x20ff, destination));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_encode_wire(destination, 0x20ff, bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL_MEMORY(maximum, bytes, length);
    sqli_decimal_t *out = source;
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_decimal_create(&out));
    TEST_ASSERT_EQUAL_PTR(source, out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_failure_preserves_output);
    RUN_TEST(test_parse_copy_parts_and_growth_fail_atomically);
    RUN_TEST(test_shrink_failure_preserves_heap_value);
    RUN_TEST(test_codecs_do_not_allocate);
    return UNITY_END();
}
