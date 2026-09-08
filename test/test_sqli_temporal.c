#include "libsqli/sqli_temporal.h"
#include "libsqli/sqli.h"
#include "unity.h"

#include <string.h>

enum { text_capacity = SQLI_TEMPORAL_MAX_TEXT + 1 };
static sqli_datetime_t *datetime;
static sqli_datetime_t *copy;
static sqli_interval_t *interval;

void setUp(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_create(&datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_create(&copy));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_create(&interval));
}

void tearDown(void)
{
    sqli_datetime_destroy(datetime);
    sqli_datetime_destroy(copy);
    sqli_interval_destroy(interval);
    datetime = NULL;
    copy = NULL;
    interval = NULL;
}

static void expect_datetime(const char *expected)
{
    char text[text_capacity];
    size_t required = 0;
    bool is_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_format(datetime, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_EQUAL_STRING(expected, text);
    TEST_ASSERT_EQUAL_UINT(strlen(expected) + 1, required);
}

static void expect_interval(const char *expected)
{
    char text[text_capacity];
    size_t required = 0;
    bool is_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_format(interval, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_EQUAL_STRING(expected, text);
    TEST_ASSERT_EQUAL_UINT(strlen(expected) + 1, required);
}

static void test_date_calendar(void)
{
    sqli_date_t date = {.is_null = true};
    static const struct { int32_t year, month, day; bool valid; } cases[] = {
        {1, 1, 1, true}, {9999, 12, 31, true}, {0, 1, 1, false},
        {10000, 1, 1, false}, {1900, 2, 29, false}, {2000, 2, 29, true},
        {2024, 2, 29, true}, {2023, 2, 29, false}, {2024, 4, 31, false},
        {2024, 0, 1, false}, {2024, 13, 1, false}, {2024, -1, 1, false},
        {2024, 1, 0, false}, {2024, 1, -1, false}, {INT32_MIN, 1, 1, false}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sqli_date_t previous = date;
        TEST_ASSERT_EQUAL_INT(cases[i].valid ? SQLI_OK : SQLI_OUT_OF_RANGE,
            sqli_date_set(&date, cases[i].year, cases[i].month, cases[i].day));
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_date_validate(&date));
        if (!cases[i].valid) {
            TEST_ASSERT_EQUAL_INT(previous.year, date.year);
            TEST_ASSERT_EQUAL_UINT(previous.month, date.month);
            TEST_ASSERT_EQUAL_UINT(previous.day, date.day);
            TEST_ASSERT_EQUAL_INT(previous.is_null, date.is_null);
        }
    }
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_date_parse(&date, "0001-01-01", 10, false));
    char text[text_capacity];
    size_t required = 0;
    bool is_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_date_format(&date, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_EQUAL_STRING("0001-01-01", text);
    TEST_ASSERT_EQUAL_UINT(11, required);
    TEST_ASSERT_FALSE(is_null);
    date.month = 255;
    required = 7;
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE,
        sqli_date_format(&date, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_EQUAL_UINT(7, required);
    TEST_ASSERT_EQUAL_STRING("0001-01-01", text);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_date_parse(&date, NULL, SIZE_MAX, true));
    TEST_ASSERT_TRUE(date.is_null);
    TEST_ASSERT_EQUAL_INT(0, date.year);
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_date_format(&date, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_EQUAL_UINT(0, required);
    TEST_ASSERT_EQUAL_STRING("0001-01-01", text);
}

static void test_date_text_errors(void)
{
    static const char *const invalid[] = {
        "2024-2-29", " 2024-02-29", "2024-02-29 ", "2024/02/29",
        "2024-02-29T00:00:00", "+2024-02-29", "2024-00-01", "2024-04-31"
    };
    sqli_date_t date;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_date_set(&date, 2024, 2, 29));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        TEST_ASSERT_NOT_EQUAL(SQLI_OK,
            sqli_date_parse(&date, invalid[i], strlen(invalid[i]), false));
        TEST_ASSERT_EQUAL_INT(2024, date.year);
        TEST_ASSERT_EQUAL_UINT(2, date.month);
        TEST_ASSERT_EQUAL_UINT(29, date.day);
    }
    const char unterminated[] = {'2','0','2','4','-','0','2','-','2','9'};
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_date_parse(&date, unterminated, sizeof(unterminated), false));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_date_parse(&date, "2024-\0\x32-29", 10, false));
}

static void test_null_and_lifecycle(void)
{
    sqli_datetime_parts_t parts;
    sqli_interval_parts_t duration;
    bool is_null = false;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_is_null(datetime, &is_null));
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_get_parts(datetime, &parts));
    TEST_ASSERT_EQUAL_INT(SQLI_FIELD_UNKNOWN, parts.range.first);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_get_parts(interval, &duration));
    TEST_ASSERT_TRUE(duration.is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_FIELD_UNKNOWN, duration.range.last);
    const sqli_temporal_range_t range = {SQLI_FIELD_HOUR, SQLI_FIELD_MINUTE, 0};
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_parse(datetime, &range, "23:59", 5, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_copy(copy, datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_copy(datetime, datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_get_parts(datetime, &parts));
    parts.hour = 0;
    expect_datetime("23:59");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_parse(datetime, NULL, NULL, SIZE_MAX, true));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_get_parts(datetime, &parts));
    TEST_ASSERT_TRUE(parts.is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_FIELD_HOUR, parts.range.first);
    TEST_ASSERT_EQUAL_UINT(0, parts.hour);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_copy(datetime, copy));
    expect_datetime("23:59");
    parts = (sqli_datetime_parts_t){.range = range, .year = -1, .hour = 255, .is_null = true};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_set_parts(datetime, &parts));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_get_parts(datetime, &parts));
    TEST_ASSERT_EQUAL_INT(0, parts.year);
    TEST_ASSERT_EQUAL_UINT(0, parts.hour);
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_parse(interval, &range, "-25:59", 6, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_copy(interval, interval));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_parse(interval, NULL, NULL, SIZE_MAX, true));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_get_parts(interval, &duration));
    TEST_ASSERT_TRUE(duration.is_null);
    TEST_ASSERT_FALSE(duration.negative);
    TEST_ASSERT_EQUAL_UINT64(0, duration.hours);
    TEST_ASSERT_EQUAL_INT(SQLI_FIELD_HOUR, duration.range.first);
    sqli_datetime_destroy(NULL);
    sqli_interval_destroy(NULL);
}

static void test_datetime_calendar_and_partial(void)
{
    sqli_datetime_parts_t parts = {
        .range = {SQLI_FIELD_MONTH, SQLI_FIELD_DAY, 0}, .month = 2, .day = 29
    };
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_set_parts(datetime, &parts));
    expect_datetime("02-29");
    parts.day = 30;
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_datetime_set_parts(datetime, &parts));
    parts.day = 29;
    parts.year = 1900;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_datetime_set_parts(datetime, &parts));
    parts.range.first = SQLI_FIELD_YEAR;
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_datetime_set_parts(datetime, &parts));
    expect_datetime("02-29");
    parts.year = 2000;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_set_parts(datetime, &parts));
    expect_datetime("2000-02-29");
    parts.hour = 1;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_datetime_set_parts(datetime, &parts));
    parts.hour = 0;
    parts.nanosecond = 1;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_datetime_set_parts(datetime, &parts));
    const sqli_temporal_range_t range = {SQLI_FIELD_DAY, SQLI_FIELD_MINUTE, 0};
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_parse(datetime, &range, "31 23:59", 8, false));
    expect_datetime("31 23:59");
    size_t required = 7;
    bool is_null = true;
    char text[] = "untouched";
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE,
        sqli_datetime_format_iso(datetime, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_EQUAL_STRING("untouched", text);
    TEST_ASSERT_EQUAL_UINT(7, required);
    TEST_ASSERT_TRUE(is_null);
}

static void test_iso_fraction_precision(void)
{
    const char timestamp[] = "2024-02-29T23:59:58.123456789";
    const uint32_t nanoseconds[] = {
        100000000, 120000000, 123000000, 123400000, 123450000,
        123456000, 123456700, 123456780, 123456789
    };
    enum { fraction_offset = 20 };
    for (size_t digits = 1; digits <= sizeof(nanoseconds) / sizeof(nanoseconds[0]); digits++) {
        size_t length = fraction_offset + digits;
        TEST_ASSERT_EQUAL_INT(SQLI_OK,
            sqli_datetime_parse_iso(datetime, timestamp, length, false));
        sqli_datetime_parts_t parts;
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_get_parts(datetime, &parts));
        TEST_ASSERT_EQUAL_UINT(digits, parts.range.fractional_digits);
        TEST_ASSERT_EQUAL_UINT32(nanoseconds[digits - 1], parts.nanosecond);
        char text[text_capacity];
        size_t required = 0;
        bool is_null = true;
        TEST_ASSERT_EQUAL_INT(SQLI_OK,
            sqli_datetime_format_iso(datetime, text, sizeof(text), &required, &is_null));
        TEST_ASSERT_EQUAL_MEMORY(timestamp, text, length);
        TEST_ASSERT_EQUAL_UINT(length + 1, required);
        TEST_ASSERT_EQUAL_INT('\0', text[length]);
        TEST_ASSERT_FALSE(is_null);
    }
    const char trailing[] = "2024-02-29T00:00:00.1200";
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_parse_iso(datetime, trailing, strlen(trailing), false));
    expect_datetime(trailing);
    sqli_datetime_parts_t parts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_get_parts(datetime, &parts));
    parts.nanosecond++;
    TEST_ASSERT_EQUAL_INT(SQLI_INEXACT, sqli_datetime_set_parts(datetime, &parts));
    parts.nanosecond = 1000000000;
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_datetime_set_parts(datetime, &parts));
    expect_datetime(trailing);
}

static void test_iso_errors(void)
{
    static const char *const invalid[] = {
        "2024-02-29 23:59:58", "2024-02-29T23:59:58Z", "2024-02-29T23:59:58+02:00",
        "2024-02-29T23:59", "2024-02-29T23:59:58.", "2024-02-29T23:59:58.1234567890",
        "2024-02-29T24:00:00", "2024-02-29T23:60:00", "2024-02-29T23:59:60",
        "0000-01-01T00:00:00", "2023-02-29T00:00:00", "2024-02-29t00:00:00",
        "2024-02-29T23:59:58\n", "2024-02-29T23:59:58.12x"
    };
    const char valid[] = "9999-12-31T23:59:59";
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_parse_iso(datetime, valid, strlen(valid), false));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        TEST_ASSERT_NOT_EQUAL(SQLI_OK,
            sqli_datetime_parse_iso(datetime, invalid[i], strlen(invalid[i]), false));
        expect_datetime(valid);
    }
    const sqli_temporal_range_t range = {SQLI_FIELD_YEAR, SQLI_FIELD_SECOND, 0};
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_parse(datetime, &range, "9999-12-31 23:59:59", 19, false));
    expect_datetime(valid);
    TEST_ASSERT_EQUAL_INT(SQLI_LIMIT_EXCEEDED,
        sqli_datetime_parse_iso(datetime, valid, SIZE_MAX, false));
    expect_datetime(valid);
}

static void test_interval_limits_and_sign(void)
{
    const sqli_temporal_range_t range = {SQLI_FIELD_DAY, SQLI_FIELD_FRACTION, 9};
    const char maximum[] = "-18446744073709551615 23:59:59.999999999";
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_parse(interval, &range, maximum, strlen(maximum), false));
    expect_interval(maximum);
    sqli_interval_parts_t parts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_get_parts(interval, &parts));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, parts.days);
    TEST_ASSERT_TRUE(parts.negative);
    parts.hours = 24;
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_interval_set_parts(interval, &parts));
    expect_interval(maximum);
    const sqli_temporal_range_t days = {SQLI_FIELD_DAY, SQLI_FIELD_DAY, 0};
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE,
        sqli_interval_parse(interval, &days, "18446744073709551616", 20, false));
    expect_interval(maximum);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_parse(interval, &days, "-000", 4, false));
    expect_interval("0");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_parse(interval, &days, "+025", 4, false));
    expect_interval("25");
    const sqli_temporal_range_t months = {SQLI_FIELD_YEAR, SQLI_FIELD_MONTH, 0};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_parse(interval, &months, "-2-11", 5, false));
    expect_interval("-2-11");
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE,
        sqli_interval_parse(interval, &months, "2-12", 4, false));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_interval_parse(interval, &months, "2--1", 4, false));
    expect_interval("-2-11");
    parts = (sqli_interval_parts_t){.range = days, .days = 1, .months = 1};
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_interval_set_parts(interval, &parts));
    parts = (sqli_interval_parts_t){.range = range, .nanosecond = 1000000000};
    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE, sqli_interval_set_parts(interval, &parts));
    parts.range.fractional_digits = 3;
    parts.nanosecond = 1;
    TEST_ASSERT_EQUAL_INT(SQLI_INEXACT, sqli_interval_set_parts(interval, &parts));
}

static void test_fraction_only(void)
{
    const sqli_temporal_range_t range = {SQLI_FIELD_FRACTION, SQLI_FIELD_FRACTION, 4};
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_parse(datetime, &range, "0.1200", 6, false));
    expect_datetime(".1200");
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_parse(interval, &range, "-0.1200", 7, false));
    expect_interval("-.1200");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_parse(interval, &range, "-.0000", 6, false));
    expect_interval(".0000");
    static const char *const invalid[] = {".12", ".12000", "1.1200", "00.1200", "1200", " .1200"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
            sqli_interval_parse(interval, &range, invalid[i], strlen(invalid[i]), false));
        expect_interval(".0000");
    }
}

static void test_invalid_ranges(void)
{
    static const sqli_temporal_range_t invalid[] = {
        {SQLI_FIELD_UNKNOWN, SQLI_FIELD_DAY, 0},
        {SQLI_FIELD_DAY, SQLI_FIELD_YEAR, 0},
        {SQLI_FIELD_YEAR, (sqli_temporal_field_t)99, 0},
        {(sqli_temporal_field_t)-1, SQLI_FIELD_SECOND, 0},
        {SQLI_FIELD_SECOND, SQLI_FIELD_SECOND, 1},
        {SQLI_FIELD_FRACTION, SQLI_FIELD_FRACTION, 0},
        {SQLI_FIELD_FRACTION, SQLI_FIELD_FRACTION, 10}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        sqli_datetime_parts_t parts = {.range = invalid[i], .is_null = true};
        sqli_interval_parts_t duration = {.range = invalid[i], .is_null = true};
        TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_datetime_set_parts(datetime, &parts));
        TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_interval_set_parts(interval, &duration));
        TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
            sqli_datetime_parse(datetime, &invalid[i], NULL, 0, true));
        TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
            sqli_interval_parse(interval, &invalid[i], NULL, 0, true));
    }
    sqli_interval_parts_t duration = {
        .range = {SQLI_FIELD_MONTH, SQLI_FIELD_DAY, 0}, .is_null = true
    };
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_interval_set_parts(interval, &duration));
    const sqli_temporal_range_t unknown = {0};
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_datetime_parse(datetime, &unknown, "00", 2, false));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_interval_parse(interval, &unknown, "0", 1, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_parse(datetime, &unknown, NULL, SIZE_MAX, true));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_parse(interval, &unknown, NULL, SIZE_MAX, true));
}

static void test_format_buffer_contract(void)
{
    const sqli_temporal_range_t range = {SQLI_FIELD_SECOND, SQLI_FIELD_SECOND, 0};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_parse(datetime, &range, "59", 2, false));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_parse(interval, &range, "-59", 3, false));
    char text[] = "untouched";
    size_t required = 0;
    bool is_null = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_format(datetime, NULL, 0, &required, &is_null));
    TEST_ASSERT_EQUAL_UINT(3, required);
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_BUFFER_TOO_SMALL,
        sqli_datetime_format(datetime, text, 2, &required, &is_null));
    TEST_ASSERT_EQUAL_STRING("untouched", text);
    TEST_ASSERT_EQUAL_UINT(3, required);
    TEST_ASSERT_EQUAL_INT(SQLI_BUFFER_TOO_SMALL,
        sqli_interval_format(interval, text, 3, &required, &is_null));
    TEST_ASSERT_EQUAL_STRING("untouched", text);
    TEST_ASSERT_EQUAL_UINT(4, required);
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_format(interval, text, 4, &required, &is_null));
    TEST_ASSERT_EQUAL_STRING("-59", text);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_interval_set_null(interval));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_interval_format(interval, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_EQUAL_STRING("-59", text);
    TEST_ASSERT_EQUAL_UINT(0, required);
    TEST_ASSERT_TRUE(is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_datetime_set_null(datetime));
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_datetime_format_iso(datetime, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_EQUAL_STRING("-59", text);
    TEST_ASSERT_TRUE(is_null);
    sqli_date_t date;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_date_set(&date, 2024, 1, 1));
    TEST_ASSERT_EQUAL_INT(SQLI_BUFFER_TOO_SMALL,
        sqli_date_format(&date, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_EQUAL_UINT(11, required);
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_EQUAL_STRING("-59", text);
}

static void test_invalid_arguments(void)
{
    bool is_null = false;
    size_t required = 7;
    sqli_date_t date = {.is_null = true};
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_date_validate(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_date_set(NULL, 2024, 1, 1));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_date_set_null(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_date_parse(&date, NULL, 10, false));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_date_format(&date, NULL, 1, &required, &is_null));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_datetime_create(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_interval_create(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_datetime_copy(datetime, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_interval_copy(interval, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_datetime_set_parts(datetime, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_interval_set_parts(interval, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_datetime_get_parts(datetime, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_interval_get_parts(interval, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_datetime_is_null(NULL, &is_null));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_interval_is_null(NULL, &is_null));
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_datetime_is_null(datetime, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_interval_is_null(interval, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_datetime_set_null(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_interval_set_null(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_datetime_parse(datetime, NULL, "00", 2, false));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_interval_parse(interval, NULL, "0", 1, false));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_datetime_format(datetime, NULL, 1, &required, &is_null));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_interval_format(interval, NULL, 1, &required, &is_null));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_datetime_format(datetime, NULL, 0, NULL, &is_null));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT,
        sqli_interval_format(interval, NULL, 0, &required, NULL));
    TEST_ASSERT_EQUAL_UINT(7, required);
    TEST_ASSERT_FALSE(is_null);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_date_calendar);
    RUN_TEST(test_date_text_errors);
    RUN_TEST(test_null_and_lifecycle);
    RUN_TEST(test_datetime_calendar_and_partial);
    RUN_TEST(test_iso_fraction_precision);
    RUN_TEST(test_iso_errors);
    RUN_TEST(test_interval_limits_and_sign);
    RUN_TEST(test_fraction_only);
    RUN_TEST(test_invalid_ranges);
    RUN_TEST(test_format_buffer_contract);
    RUN_TEST(test_invalid_arguments);
    return UNITY_END();
}
