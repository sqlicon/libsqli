#include "libsqli/sqli_decimal.h"
#include "sqli_decimal_codec.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

enum { floating32 = 0x20ff, fixed8_4 = 0x0804, text_capacity = 96 };
static sqli_decimal_t *source;
static sqli_decimal_t *destination;

void setUp(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_create(&source));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_create(&destination));
}
void tearDown(void)
{
    sqli_decimal_destroy(source);
    sqli_decimal_destroy(destination);
    source = NULL;
    destination = NULL;
}
static void parse(const char *text)
{
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_parse(source, text, strlen(text), false));
}
static void expect_destination(const char *expected)
{
    char text[text_capacity];
    size_t required;
    bool is_null;
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_decimal_format(destination, text, sizeof(text), &required, &is_null));
    TEST_ASSERT_FALSE(is_null);
    TEST_ASSERT_EQUAL_STRING(expected, text);
}
static void test_fixed_fixtures(void)
{
    static const struct { uint16_t descriptor; uint8_t bytes[18]; size_t length; const char *text; } cases[] = {
        {fixed8_4,{0xc2,1,23,45,0},5,"123.4500"},
        {fixed8_4,{0x3d,98,76,55,0},5,"-123.4500"},
        {fixed8_4,{0x80,0,0,0,0},5,"0.0000"},
        {0x0302,{0xc1,1,23},3,"1.23"},
        {0x0303,{0x40,90,0},3,"-0.001"},
        {floating32,{0x80,1},18,"1E-130"},
        {floating32,{0x7f,99},18,"-1E-130"},
        {floating32,{0xff,10},18,"1E+125"},
        {floating32,{0,90},18,"-1E+125"},
        {floating32,{0xc2,1,23,45},18,"123.45"},
        {floating32,{0xc2,10},18,"1E+3"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_ASSERT_EQUAL_INT(SQLI_OK,
            sqli_decimal_decode_wire(cases[i].bytes, cases[i].length, cases[i].descriptor, destination));
        expect_destination(cases[i].text);
        parse(cases[i].text);
        uint8_t bytes[SQLI_DECIMAL_WIRE_CAPACITY];
        size_t length = 0;
        TEST_ASSERT_EQUAL_INT(SQLI_OK,
            sqli_decimal_encode_wire(source, cases[i].descriptor, bytes, sizeof(bytes), &length));
        TEST_ASSERT_EQUAL_UINT(cases[i].length, length);
        TEST_ASSERT_EQUAL_MEMORY(cases[i].bytes, bytes, length);
    }
}
static void test_all_descriptors(void)
{
    unsigned accepted = 0;
    for (unsigned descriptor = 0; descriptor <= UINT16_MAX; descriptor++) {
        unsigned precision = descriptor >> 8, scale = descriptor & 0xff;
        bool valid = precision >= 1 && precision <= 32 && (scale <= precision || scale == 255);
        size_t width = SIZE_MAX;
        TEST_ASSERT_EQUAL_INT(valid ? SQLI_OK : SQLI_PROTO_ERROR,
            sqli_decimal_wire_size((uint16_t)descriptor, &width));
        if (valid) {
            TEST_ASSERT_EQUAL_UINT((precision + (scale & 1u) + 3) / 2, width);
            accepted++;
        } else {
            TEST_ASSERT_EQUAL_UINT(SIZE_MAX, width);
        }
    }
    TEST_ASSERT_EQUAL_UINT(592, accepted);
}
static void test_all_fixed_precisions_and_scales(void)
{
    char coefficient[33];
    for (unsigned precision = 1; precision <= 32; precision++) {
        for (unsigned scale = 0; scale <= precision; scale++) {
            for (unsigned variant = 0; variant < 4; variant++) {
                /* Maximum, alternating digits, minimum magnitude, and zero. */
                for (unsigned digit = 0; digit < precision; digit++)
                    coefficient[digit] = variant == 0 ? '9' : variant == 1 ? (digit % 2 == 0 ? '1' : '0') : '0';
                if (variant == 2)
                    coefficient[precision - 1] = '1';
                coefficient[precision] = '\0';
                char text[text_capacity];
                int n = snprintf(text, sizeof(text), "-%se-%u", coefficient, scale);
                TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(text));
                parse(text);
                uint16_t descriptor = (uint16_t)((precision << 8) | scale);
                uint8_t bytes[SQLI_DECIMAL_WIRE_CAPACITY];
                size_t length = 0;
                if (precision == 32 && (scale & 1u) != 0 && variant == 0) {
                    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE,
                        sqli_decimal_encode_wire(source, descriptor, bytes, sizeof(bytes), &length));
                    TEST_ASSERT_EQUAL_UINT(0, length);
                    TEST_ASSERT_EQUAL_INT(SQLI_OUT_OF_RANGE,
                        sqli_decimal_encode_wire(source, floating32, bytes, sizeof(bytes), &length));
                    continue;
                }
                TEST_ASSERT_EQUAL_INT(SQLI_OK,
                    sqli_decimal_encode_wire(source, descriptor, bytes, sizeof(bytes), &length));
                TEST_ASSERT_EQUAL_INT(SQLI_OK,
                    sqli_decimal_decode_wire(bytes, length, descriptor, destination));
                int ordering = 7;
                TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_compare(source, destination, &ordering));
                TEST_ASSERT_EQUAL_INT(0, ordering);
                sqli_decimal_parts_t parts;
                TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_get_parts(destination, &parts));
                TEST_ASSERT_EQUAL_INT(scale, parts.scale);
                descriptor = (uint16_t)((precision << 8) | 255u);
                TEST_ASSERT_EQUAL_INT(SQLI_OK,
                    sqli_decimal_encode_wire(source, descriptor, bytes, sizeof(bytes), &length));
                TEST_ASSERT_EQUAL_INT(SQLI_OK,
                    sqli_decimal_decode_wire(bytes, length, descriptor, destination));
                TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_compare(source, destination, &ordering));
                TEST_ASSERT_EQUAL_INT(0, ordering);
            }
        }
    }
}
static void test_every_exponent_and_single_group(void)
{
    /* Independent single-group oracle: group * 100^(exponent-1).
     * A one-group negative coefficient is 100-group, then zero padding. */
    for (int exponent = -64; exponent <= 63; exponent++) {
        for (unsigned group = 1; group < 100; group++) {
            for (unsigned negative = 0; negative < 2; negative++) {
                uint8_t bytes[SQLI_DECIMAL_WIRE_CAPACITY] = {0};
                bytes[0] = negative ? (uint8_t)(127 - (exponent + 64)) : (uint8_t)(128 + exponent + 64);
                bytes[1] = (uint8_t)(negative ? 100 - group : group);
                TEST_ASSERT_EQUAL_INT(SQLI_OK,
                    sqli_decimal_decode_wire(bytes, sizeof(bytes), floating32, destination));
                TEST_ASSERT_EQUAL_INT(SQLI_OK,
                    sqli_decimal_set_i64(source, negative ? -(int64_t)group : group, 2 * (1 - exponent)));
                int ordering = 7;
                TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_compare(source, destination, &ordering));
                TEST_ASSERT_EQUAL_INT(0, ordering);
                uint8_t encoded[SQLI_DECIMAL_WIRE_CAPACITY];
                size_t length = 0;
                TEST_ASSERT_EQUAL_INT(SQLI_OK,
                    sqli_decimal_encode_wire(source, floating32, encoded, sizeof(encoded), &length));
                TEST_ASSERT_EQUAL_UINT(sizeof(bytes), length);
                TEST_ASSERT_EQUAL_MEMORY(bytes, encoded, length);
            }
        }
    }
}
static void test_target_failures_and_source_preservation(void)
{
    static const struct { const char *text; uint16_t target; sqli_status status; } cases[] = {
        {"123.45001",fixed8_4,SQLI_INEXACT}, {"10000",fixed8_4,SQLI_OUT_OF_RANGE},
        {"1e-131",floating32,SQLI_OUT_OF_RANGE}, {"1e126",floating32,SQLI_OUT_OF_RANGE},
        {"1e2147483648",floating32,SQLI_OUT_OF_RANGE}, {"1e-2147483647",floating32,SQLI_OUT_OF_RANGE},
        {"1.23",0x02ff,SQLI_OUT_OF_RANGE}, {"1",0x0101,SQLI_OUT_OF_RANGE},
        {"0.1",0x0100,SQLI_INEXACT},
        {"9999999999999999999999999999999.9",0x2001,SQLI_OUT_OF_RANGE},
        {"-9999999999999999999999999999999.9",floating32,SQLI_OUT_OF_RANGE}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        parse(cases[i].text);
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_copy(destination, source));
        uint8_t bytes[SQLI_DECIMAL_WIRE_CAPACITY];
        memset(bytes, 0xa5, sizeof(bytes));
        size_t length = 7;
        TEST_ASSERT_EQUAL_INT(cases[i].status,
            sqli_decimal_encode_wire(source, cases[i].target, bytes, sizeof(bytes), &length));
        TEST_ASSERT_EQUAL_UINT(7, length);
        TEST_ASSERT_EACH_EQUAL_UINT8(0xa5, bytes, sizeof(bytes));
        int ordering;
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_compare(source, destination, &ordering));
        TEST_ASSERT_EQUAL_INT(0, ordering);
    }
    parse("123.450000");
    uint8_t bytes[SQLI_DECIMAL_WIRE_CAPACITY];
    size_t length;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_encode_wire(source, fixed8_4, bytes, sizeof(bytes), &length));
    sqli_decimal_parts_t parts;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_get_parts(source, &parts));
    TEST_ASSERT_EQUAL_INT(6, parts.scale);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_decode_wire(bytes, length, fixed8_4, destination));
    expect_destination("123.4500");
    /* Large application coefficients with harmless trailing zeros need no
     * large intermediate allocation to fit a small native target. */
    char large[1002];
    large[0] = '1';
    memset(large + 1, '0', 1000);
    large[1001] = '\0';
    parse(large);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_get_parts(source, &parts));
    parts.scale = 1000;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_parts(source, &parts));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_encode_wire(source, 0x0100, bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_decode_wire(bytes, length, 0x0100, destination));
    expect_destination("1");
}
static void test_malformed_payloads_and_null(void)
{
    static const uint8_t bad[][5] = {
        {0xc2,100,0,0,0}, {0xc2,0,1,23,45}, {0xc2,0,0,0,0},
        {0x3d,0,0,0,0}, {0x80,1,0,0,0}, {0xff,1,0,0,0},
        {0xc1,1,23,45,1}, {0xc3,1,0,0,0}
    };
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_i64(destination, 42, 2));
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR,
            sqli_decimal_decode_wire(bad[i], sizeof(bad[i]), fixed8_4, destination));
        expect_destination("0.42");
    }
    uint8_t too_many_groups[SQLI_DECIMAL_WIRE_CAPACITY];
    memset(too_many_groups, 99, sizeof(too_many_groups));
    too_many_groups[0] = 0xd0;
    too_many_groups[1] = 9;
    too_many_groups[SQLI_DECIMAL_WIRE_CAPACITY - 1] = 90;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR,
        sqli_decimal_decode_wire(too_many_groups, sizeof(too_many_groups), 0x2001, destination));
    expect_destination("0.42");
    const uint8_t zeros[SQLI_DECIMAL_WIRE_CAPACITY] = {0};
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_decimal_decode_wire(zeros, 4, fixed8_4, destination));
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_decimal_decode_wire(zeros, SIZE_MAX, fixed8_4, destination));
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_decimal_decode_wire(zeros, 5, 0, destination));
    expect_destination("0.42");
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_decode_wire(zeros, 5, fixed8_4, destination));
    bool is_null = false;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_is_null(destination, &is_null));
    TEST_ASSERT_TRUE(is_null);
    uint8_t bytes[SQLI_DECIMAL_WIRE_CAPACITY];
    size_t length = 7;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_encode_wire(destination, fixed8_4, bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL_UINT(5, length);
    TEST_ASSERT_EQUAL_MEMORY(zeros, bytes, length);
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_wire_size(fixed8_4, NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_decode_wire(NULL, 5, fixed8_4, destination));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_encode_wire(NULL, fixed8_4, bytes, sizeof(bytes), &length));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_encode_wire(source, fixed8_4, NULL, 0, &length));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_decimal_encode_wire(source, fixed8_4, bytes, sizeof(bytes), NULL));
    memset(bytes, 0xa5, sizeof(bytes));
    length = 7;
    TEST_ASSERT_EQUAL_INT(SQLI_BUFFER_TOO_SMALL, sqli_decimal_encode_wire(source, fixed8_4, bytes, 4, &length));
    TEST_ASSERT_EQUAL_UINT(7, length);
    TEST_ASSERT_EACH_EQUAL_UINT8(0xa5, bytes, sizeof(bytes));
}
static void test_mutations_are_atomic_or_canonical(void)
{
    static const struct { uint16_t descriptor; size_t length; uint8_t bytes[18]; } cases[] = {
        {fixed8_4,5,{0xc2,1,23,45,0}},
        {floating32,18,{0x80,1}}, {floating32,18,{0,90}}
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        for (size_t position = 0; position < cases[c].length; position++) {
            for (unsigned byte = 0; byte <= UINT8_MAX; byte++) {
                uint8_t changed[SQLI_DECIMAL_WIRE_CAPACITY];
                memcpy(changed, cases[c].bytes, sizeof(changed));
                changed[position] = (uint8_t)byte;
                TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_set_null(destination));
                sqli_status status = sqli_decimal_decode_wire(changed, cases[c].length, cases[c].descriptor, destination);
                if (status == SQLI_OK) {
                    uint8_t encoded[SQLI_DECIMAL_WIRE_CAPACITY];
                    size_t length = 0;
                    TEST_ASSERT_EQUAL_INT(SQLI_OK,
                        sqli_decimal_encode_wire(destination, cases[c].descriptor, encoded, sizeof(encoded), &length));
                    TEST_ASSERT_EQUAL_UINT(cases[c].length, length);
                    TEST_ASSERT_EQUAL_MEMORY(changed, encoded, length);
                } else {
                    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, status);
                    bool is_null = false;
                    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_decimal_is_null(destination, &is_null));
                    TEST_ASSERT_TRUE(is_null);
                }
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fixed_fixtures);
    RUN_TEST(test_all_descriptors);
    RUN_TEST(test_all_fixed_precisions_and_scales);
    RUN_TEST(test_every_exponent_and_single_group);
    RUN_TEST(test_target_failures_and_source_preservation);
    RUN_TEST(test_malformed_payloads_and_null);
    RUN_TEST(test_mutations_are_atomic_or_canonical);
    return UNITY_END();
}
