#include "libsqli/sqli_temporal.h"
#include "libsqli/sqli.h"
#include "allocation_test.h"
#include "sqli_temporal_codec.h"
#include "unity.h"

#include <string.h>

static sqli_datetime_t *datetime;
static sqli_interval_t *interval;
static sqli_interval_t *copy;

void setUp(void)
{
    sqli_test_allow_allocations();
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_create(&datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_create(&interval));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_create(&copy));
}

void tearDown(void)
{
    sqli_test_allow_allocations();
    sqli_datetime_destroy(datetime);
    sqli_interval_destroy(interval);
    sqli_interval_destroy(copy);
    datetime = NULL;
    interval = NULL;
    copy = NULL;
}

static void test_create_failure_preserves_output(void)
{
    sqli_datetime_t *out = datetime;
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_datetime_create(&out));
    TEST_ASSERT_EQUAL_PTR(datetime, out);
    sqli_interval_t *interval_out = interval;
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_interval_create(&interval_out));
    TEST_ASSERT_EQUAL_PTR(interval, interval_out);
}

static void test_value_operations_do_not_allocate(void)
{
    const sqli_temporal_range_t range = {SQLI_FIELD_DAY, SQLI_FIELD_FRACTION, 9};
    const char text[] = "-18446744073709551615 23:59:59.123456789";
    char buffer[SQLI_TEMPORAL_MAX_TEXT + 1];
    size_t required = 0;
    bool is_null = true;
    sqli_interval_parts_t parts;
    sqli_datetime_parts_t calendar;
    sqli_date_t date;
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_parse(interval, &range, text, strlen(text), false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_get_parts(interval, &parts));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_set_parts(interval, &parts));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_copy(copy, interval));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_set_null(interval));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_format(copy, buffer, sizeof(buffer), &required, &is_null));
    TEST_ASSERT_EQUAL_STRING(text, buffer);
    const char timestamp[] = "2024-02-29T23:59:59.123456789";
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_parse_iso(datetime, timestamp, strlen(timestamp), false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_get_parts(datetime, &calendar));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_set_parts(datetime, &calendar));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_format_iso(datetime, buffer, sizeof(buffer), &required, &is_null));
    TEST_ASSERT_EQUAL_STRING(timestamp, buffer);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_set_null(datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_date_parse(&date, "2024-02-29", 10, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_date_format(&date, buffer, sizeof(buffer), &required, &is_null));
    TEST_ASSERT_EQUAL_STRING("2024-02-29", buffer);
    const char exact_timestamp[] = "2024-02-29T23:59:59.123450000";
    const char exact_interval[] = "-999999999 23:59:59.123450000";
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_parse_iso(datetime, exact_timestamp, strlen(exact_timestamp), false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_parse(interval, &range, exact_interval, strlen(exact_interval), false));
    uint8_t wire[SQLI_TEMPORAL_WIRE_CAPACITY];
    size_t wire_length = 0;
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_date_encode_wire(&date, wire, sizeof(wire), &wire_length));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_date_decode_wire(wire, wire_length, &date));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_encode_wire(datetime, 0x130f, wire, sizeof(wire), &wire_length));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_decode_wire(wire, wire_length, 0x130f, datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_encode_wire(interval, 0x144f, wire, sizeof(wire), &wire_length));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_decode_wire(wire, wire_length, 0x144f, interval));
    /* The pending fault proves that none of the value operations allocated. */
    sqli_datetime_t *out = datetime;
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_datetime_create(&out));
    TEST_ASSERT_EQUAL_PTR(datetime, out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_failure_preserves_output);
    RUN_TEST(test_value_operations_do_not_allocate);
    return UNITY_END();
}
