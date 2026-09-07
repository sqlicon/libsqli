#include "sqli_temporal_codec.h"
#include "unity.h"

#include <string.h>

enum { full_datetime = 0x0e0a, day_interval = 0x0e4f, year_datetime = 0x0400 };
static sqli_datetime_t *datetime;
static sqli_interval_t *interval;

void setUp(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_create(&datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_create(&interval));
}

void tearDown(void)
{
    sqli_datetime_destroy(datetime);
    sqli_interval_destroy(interval);
    datetime = NULL;
    interval = NULL;
}

static void test_date_fixtures_and_invalid_payloads(void)
{
    static const struct { uint8_t bytes[4]; int year, month, day; bool is_null; } cases[] = {
        {{0,0,0,0}, 1899,12,31,false}, {{255,255,255,255},1899,12,30,false},
        {{0,0,0x63,0xe0},1970,1,1,false}, {{0x80,0,0,0},0,0,0,true},
        {{0xff,0xf5,0x6a,0xa6},1,1,1,false}, {{0,0x2d,0x24,0x80},9999,12,31,false}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sqli_date_t date;
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_date_decode_wire(cases[i].bytes, 4, &date));
        TEST_ASSERT_EQUAL_INT(cases[i].year, date.year);
        TEST_ASSERT_EQUAL_UINT(cases[i].month, date.month);
        TEST_ASSERT_EQUAL_UINT(cases[i].day, date.day);
        TEST_ASSERT_EQUAL_INT(cases[i].is_null, date.is_null);
        uint8_t encoded[4];
        size_t length = 0;
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_date_encode_wire(&date, encoded, sizeof(encoded), &length));
        TEST_ASSERT_EQUAL_UINT(4, length);
        TEST_ASSERT_EQUAL_MEMORY(cases[i].bytes, encoded, length);
    }
    const uint8_t outside[] = {0x7f,0xff,0xff,0xff};
    sqli_date_t date = {.year=2024,.month=2,.day=29};
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_date_decode_wire(outside, 4, &date));
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_date_decode_wire(outside, 3, &date));
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_date_decode_wire(outside, SIZE_MAX, &date));
    TEST_ASSERT_EQUAL_INT(2024, date.year);
    TEST_ASSERT_EQUAL_UINT(29, date.day);
    uint8_t buffer[] = {1,2,3,4};
    size_t length = 7;
    TEST_ASSERT_EQUAL_INT(SQLI_BUFFER_TOO_SMALL, sqli_date_encode_wire(&date, buffer, 3, &length));
    TEST_ASSERT_EQUAL_UINT(7, length);
    const uint8_t original[] = {1,2,3,4};
    TEST_ASSERT_EQUAL_MEMORY(original, buffer, 4);
    date.month = 13;
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_date_encode_wire(&date, buffer, 4, &length));
    TEST_ASSERT_EQUAL_UINT(7, length);
}

static void test_date_gregorian_cycle(void)
{
    /* Independent month iteration across a complete 400-year leap cycle.
     * Wire day 1 is 1900-01-01; each calendar day must advance exactly once. */
    uint32_t day_number = 1;
    static const unsigned month_lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (int year = 1900; year < 2300; year++) {
        for (unsigned month = 1; month <= 12; month++) {
            unsigned days = month_lengths[month - 1];
            if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
                days++;
            for (unsigned day = 1; day <= days; day++, day_number++) {
                uint8_t expected[] = {
                    (uint8_t)(day_number >> 24), (uint8_t)(day_number >> 16),
                    (uint8_t)(day_number >> 8), (uint8_t)day_number
                };
                sqli_date_t value;
                TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_date_decode_wire(expected, sizeof(expected), &value));
                TEST_ASSERT_EQUAL_INT(year, value.year);
                TEST_ASSERT_EQUAL_UINT(month, value.month);
                TEST_ASSERT_EQUAL_UINT(day, value.day);
                uint8_t encoded[4];
                size_t length = 0;
                TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_date_encode_wire(&value, encoded, sizeof(encoded), &length));
                TEST_ASSERT_EQUAL_MEMORY(expected, encoded, sizeof(expected));
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32(146098, day_number);
}

static void test_all_qualifiers(void)
{
    enum { qualifier_count = 65536 };
    static bool valid_datetime[qualifier_count], valid_interval[qualifier_count];
    memset(valid_datetime, 0, sizeof(valid_datetime));
    memset(valid_interval, 0, sizeof(valid_interval));
    unsigned datetime_count = 0, interval_count = 0;
    for (unsigned start = 0; start <= 6; start++) {
        for (unsigned end = start; end <= 6; end++) {
            for (unsigned fraction = end == 6 ? 1u : 0u; fraction <= (end == 6 ? 5u : 0u); fraction++) {
                unsigned last = end == 6 ? 10 + fraction : end * 2;
                unsigned digits = start == 6 ? fraction : (start == 0 ? 4u : 2u) + last - start * 2;
                unsigned qualifier = (digits << 8) | (start * 2 << 4) | last;
                valid_datetime[qualifier] = true;
                datetime_count++;
                if (start < 2 && end > 1)
                    continue;
                for (unsigned precision = 1; precision <= (start == 6 ? 1u : 9u); precision++) {
                    digits = start == 6 ? fraction : precision + last - start * 2;
                    qualifier = (digits << 8) | (start * 2 << 4) | last;
                    valid_interval[qualifier] = true;
                    interval_count++;
                }
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT(56, datetime_count);
    TEST_ASSERT_EQUAL_UINT(302, interval_count);
    for (unsigned qualifier = 0; qualifier < qualifier_count; qualifier++) {
        size_t width = SIZE_MAX;
        TEST_ASSERT_EQUAL_INT(valid_datetime[qualifier] ? SQLI_OK : SQLI_PROTO_ERROR,
            sqli_temporal_wire_size((uint16_t)qualifier, false, &width));
        if (!valid_datetime[qualifier])
            TEST_ASSERT_EQUAL_UINT(SIZE_MAX, width);
        width = SIZE_MAX;
        TEST_ASSERT_EQUAL_INT(valid_interval[qualifier] ? SQLI_OK : SQLI_PROTO_ERROR,
            sqli_temporal_wire_size((uint16_t)qualifier, true, &width));
        if (!valid_interval[qualifier])
            TEST_ASSERT_EQUAL_UINT(SIZE_MAX, width);
    }
}

static void test_malformed_datetime_is_atomic(void)
{
    static const uint8_t bad[][8] = {
        {0xc7,20,23,2,29,0,0,0}, /* Invalid leap day. */
        {0xc7,20,24,13,1,0,0,0}, {0xc7,20,24,1,1,24,0,0},
        {0xc7,20,24,1,1,0,60,0}, {0xc7,20,24,1,1,0,0,60},
        {0xc7,100,0,0,0,0,0,0}, /* Invalid radix digit. */
        {0xc7,0,20,24,1,1,0,0}, /* Unnormalized nonzero coefficient. */
        {0xff,1,0,0,0,0,0,0}, {0x81,1,0,0,0,0,0,0}, /* Exponent outside range. */
        {0xc7,0,0,0,0,0,0,0}, /* Noncanonical zero. */
        {0x38,79,75,98,98,0,0,0} /* Negative calendar value. */
    };
    const char original[] = "2024-02-29T12:34:56";
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_parse_iso(datetime, original, strlen(original), false));
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR,
            sqli_datetime_decode_wire(bad[i], sizeof(bad[i]), full_datetime, datetime));
        char text[SQLI_TEMPORAL_MAX_TEXT + 1];
        size_t required;
        bool is_null;
        TEST_ASSERT_EQUAL_INT(SQLI_OK,
            sqli_datetime_format_iso(datetime, text, sizeof(text), &required, &is_null));
        TEST_ASSERT_EQUAL_STRING(original, text);
    }
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR,
        sqli_datetime_decode_wire(bad[0], 7, full_datetime, datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR,
        sqli_datetime_decode_wire(bad[0], SIZE_MAX, full_datetime, datetime));
    const uint8_t odd_fraction[] = {0xbe,11,0,0};
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR,
        sqli_datetime_decode_wire(odd_fraction, sizeof(odd_fraction), 0x05cf, datetime));
}

static void test_interval_malformed_and_target_limits(void)
{
    const sqli_temporal_range_t range = {SQLI_FIELD_SECOND, SQLI_FIELD_FRACTION, 9};
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_parse(interval, &range, "999.123456789", 13, false));
    uint8_t buffer[SQLI_TEMPORAL_WIRE_CAPACITY];
    memset(buffer, 0xa5, sizeof(buffer));
    size_t length = 7;
    TEST_ASSERT_EQUAL_INT(SQLI_INEXACT,
        sqli_interval_encode_wire(interval, 0x08af, buffer, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_UINT(7, length);
    TEST_ASSERT_EACH_EQUAL_UINT8(0xa5, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_parse(interval, &range, "999.123450000", 13, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_encode_wire(interval, 0x08af, buffer, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_decode_wire(buffer, length, 0x08af, interval));
    sqli_interval_parts_t parts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_get_parts(interval, &parts));
    TEST_ASSERT_EQUAL_UINT(5, parts.range.fractional_digits);
    TEST_ASSERT_EQUAL_UINT32(123450000, parts.nanosecond);
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE,
        sqli_interval_encode_wire(interval, 0x07af, buffer, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_interval_encode_wire(interval, day_interval, buffer, sizeof(buffer), &length));
    const uint8_t invalid_month[] = {0xc6,1,12};
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR,
        sqli_interval_decode_wire(invalid_month, sizeof(invalid_month), 0x0302, interval));
    const uint8_t leading_overflow[] = {0xc1,10};
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR,
        sqli_interval_decode_wire(leading_overflow, sizeof(leading_overflow), 0x01aa, interval));
    const uint8_t negative_zero[] = {0x3f,0};
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR,
        sqli_interval_decode_wire(negative_zero, sizeof(negative_zero), 0x02aa, interval));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_get_parts(interval, &parts));
    TEST_ASSERT_EQUAL_UINT64(999, parts.seconds);
    parts.seconds = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_set_parts(interval, &parts));
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE,
        sqli_interval_encode_wire(interval, 0x0eaf, buffer, sizeof(buffer), &length));
}

static void test_null_buffers_and_arguments(void)
{
    uint8_t bytes[SQLI_TEMPORAL_WIRE_CAPACITY];
    memset(bytes, 0xa5, sizeof(bytes));
    size_t length = 7;
    TEST_ASSERT_EQUAL_INT(SQLI_BUFFER_TOO_SMALL,
        sqli_datetime_encode_wire(datetime, full_datetime, bytes, 7, &length));
    TEST_ASSERT_EACH_EQUAL_UINT8(0xa5, bytes, sizeof(bytes));
    TEST_ASSERT_EQUAL_UINT(7, length);
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_encode_wire(datetime, full_datetime, bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL_UINT(8, length);
    TEST_ASSERT_EACH_EQUAL_UINT8(0, bytes, length);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_decode_wire(bytes, length, full_datetime, datetime));
    sqli_datetime_parts_t parts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_get_parts(datetime, &parts));
    TEST_ASSERT_TRUE(parts.is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_FIELD_SECOND, parts.range.last);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_datetime_encode_wire(datetime, year_datetime, bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_encode_wire(interval, day_interval, bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_decode_wire(bytes, length, day_interval, interval));
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR,
        sqli_interval_decode_wire(bytes, length, 0, interval));
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR,
        sqli_datetime_encode_wire(datetime, 0, bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_temporal_wire_size(full_datetime, false, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_datetime_decode_wire(NULL, 8, full_datetime, datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_interval_decode_wire(bytes, 9, day_interval, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_date_decode_wire(bytes, 4, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_date_encode_wire(NULL, bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_datetime_encode_wire(datetime, full_datetime, NULL, 0, &length));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_interval_encode_wire(interval, day_interval, bytes, sizeof(bytes), NULL));
}

static void test_payload_mutations_are_atomic_or_canonical(void)
{
    static const struct { uint16_t qualifier; size_t length; bool interval; uint8_t bytes[12]; } cases[] = {
        {0x0e0a,8,false,{0xc7,20,26,6,20,12,34,56}},
        {0x0400,3,false,{0xc6,1,0}},
        {0x05cf,4,false,{0xbe,10,0,0}},
        {0x05ac,4,true,{0x3d,90,0,1}},
        {0x0e4f,9,true,{0xc4,3,3,27,1,12,34,50,0}}
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        for (size_t position = 0; position < cases[c].length; position++) {
            for (unsigned byte = 0; byte <= UINT8_MAX; byte++) {
                uint8_t changed[SQLI_TEMPORAL_WIRE_CAPACITY];
                memcpy(changed, cases[c].bytes, sizeof(changed));
                changed[position] = (uint8_t)byte;
                uint8_t encoded[SQLI_TEMPORAL_WIRE_CAPACITY];
                size_t length = 0;
                sqli_status status;
                if (cases[c].interval) {
                    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_set_null(interval));
                    sqli_interval_parts_t before, after;
                    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_get_parts(interval, &before));
                    status = sqli_interval_decode_wire(changed, cases[c].length, cases[c].qualifier, interval);
                    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_get_parts(interval, &after));
                    if (status == SQLI_OK) {
                        TEST_ASSERT_EQUAL_INT(SQLI_OK,
                            sqli_interval_encode_wire(interval, cases[c].qualifier, encoded, sizeof(encoded), &length));
                    } else {
                        TEST_ASSERT_TRUE(after.is_null);
                        TEST_ASSERT_EQUAL_INT(before.range.first, after.range.first);
                        TEST_ASSERT_EQUAL_INT(before.range.last, after.range.last);
                        TEST_ASSERT_EQUAL_UINT(before.range.fractional_digits, after.range.fractional_digits);
                        TEST_ASSERT_EQUAL_UINT64(0, after.years | after.months | after.days | after.hours | after.minutes | after.seconds);
                        TEST_ASSERT_EQUAL_UINT32(0, after.nanosecond);
                        TEST_ASSERT_FALSE(after.negative);
                    }
                } else {
                    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_set_null(datetime));
                    sqli_datetime_parts_t before, after;
                    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_get_parts(datetime, &before));
                    status = sqli_datetime_decode_wire(changed, cases[c].length, cases[c].qualifier, datetime);
                    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_get_parts(datetime, &after));
                    if (status == SQLI_OK) {
                        TEST_ASSERT_EQUAL_INT(SQLI_OK,
                            sqli_datetime_encode_wire(datetime, cases[c].qualifier, encoded, sizeof(encoded), &length));
                    } else {
                        TEST_ASSERT_TRUE(after.is_null);
                        TEST_ASSERT_EQUAL_INT(before.range.first, after.range.first);
                        TEST_ASSERT_EQUAL_INT(before.range.last, after.range.last);
                        TEST_ASSERT_EQUAL_UINT(before.range.fractional_digits, after.range.fractional_digits);
                        TEST_ASSERT_EQUAL_INT(0, after.year | after.month | after.day | after.hour | after.minute | after.second);
                        TEST_ASSERT_EQUAL_UINT32(0, after.nanosecond);
                    }
                }
                if (status == SQLI_OK) {
                    TEST_ASSERT_EQUAL_UINT(cases[c].length, length);
                    TEST_ASSERT_EQUAL_MEMORY(changed, encoded, length);
                } else {
                    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, status);
                }
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_date_fixtures_and_invalid_payloads);
    RUN_TEST(test_date_gregorian_cycle);
    RUN_TEST(test_all_qualifiers);
    RUN_TEST(test_malformed_datetime_is_atomic);
    RUN_TEST(test_interval_malformed_and_target_limits);
    RUN_TEST(test_null_buffers_and_arguments);
    RUN_TEST(test_payload_mutations_are_atomic_or_canonical);
    return UNITY_END();
}
