/* Deterministic temporal matrix, adapted from docs/test_temporal_matrix.c.
 * CTest runs generator checks offline; --live requires SQLI_TEST_*.
 * Expected semantic fields are generated independently of libsqli's decoder.
 */
#include "scalar_expectations.h"
#include "libsqli/sqli_temporal.h"
#include "temporal_result_test.h"
#include "libsqli/sqli.h"
#include "sqli_internal.h"
#include "sqli_result_internal.h"
#include "sqli_temporal_codec.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MATRIX_CAPACITY 2048
#define MATRIX_CASES 2036
#define DEFAULT_SEED UINT64_C(0xc0ffee)

enum field { YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, FRACTION };
static const char *const field_names[] = {
    "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND", "FRACTION"
};
struct temporal_case {
    char type[64];
    char value[64];
    int fields[7];
    int start, end, scale, precision, variant;
    bool interval, negative, is_null;
};
struct matrix {
    struct temporal_case cases[MATRIX_CAPACITY];
    size_t count;
    uint64_t rng;
};

/* Independent test model: pack generated fields into decimal pairs, then
 * normalize the coefficient. It does not invoke a production wire encoder. */
enum {
    temporal_wire_capacity = 20,
    decimal_pair_base = 100,
    decimal_pair_digits = 2,
    temporal_exponent_bias = 64,
    temporal_positive_flag = 128,
    temporal_zero_marker = 128,
    temporal_second_code = 10,
    temporal_fraction_start = 12,
    matrix_id_column = 0,
    matrix_value_column = 1,
    matrix_sentinel_column = 2,
    matrix_interval_column = 3,
    matrix_tail_column = 4,
    matrix_equal_column = 5,
    matrix_text_column = 6,
    matrix_integer_width = 4,
    matrix_projection_columns = 7,
    offline_value_column = 0
};

struct temporal_wire {
    uint8_t bytes[temporal_wire_capacity];
    size_t length;
    uint16_t qualifier;
};

static bool make_wire(const struct temporal_case *c, struct temporal_wire *out)
{
    uint8_t pairs[temporal_wire_capacity] = {0};
    size_t count = 0;
    unsigned leading = c->interval ? (unsigned)c->precision : c->start == YEAR ? 4u : 2u;
    unsigned start = c->start == FRACTION ? temporal_fraction_start : (unsigned)c->start * 2u;
    unsigned end = c->end == FRACTION ? temporal_second_code + (unsigned)c->scale : (unsigned)c->end * 2u;
    unsigned digits = c->start == FRACTION ? (unsigned)c->scale : leading + end - start;
    unsigned padding = c->interval && c->start != FRACTION ? leading & 1u : 0;
    struct temporal_wire wire = {0};
    wire.length = 1u + (digits + padding + 1u) / decimal_pair_digits;
    if (wire.length > sizeof(wire.bytes) || digits > UINT8_MAX)
        return false;
    wire.qualifier = (uint16_t)((digits << 8) | (start << 4) | end);
    if (c->is_null) {
        *out = wire;
        return true;
    }
    int exponent = 0;
    for (int field = c->start; field <= c->end && field <= SECOND; field++) {
        unsigned width = field == c->start ? (leading + 1u) / decimal_pair_digits : 1u;
        if (width > sizeof(pairs) - count || c->fields[field] < 0)
            return false;
        unsigned value = (unsigned)c->fields[field];
        for (unsigned i = width; i > 0; i--) {
            pairs[count + i - 1] = (uint8_t)(value % decimal_pair_base);
            value /= decimal_pair_base;
        }
        if (value != 0)
            return false;
        count += width;
    }
    if (c->start != FRACTION)
        exponent = (int)count + SECOND - (c->end > SECOND ? SECOND : c->end);
    if (c->end == FRACTION) {
        unsigned width = ((unsigned)c->scale + 1u) / decimal_pair_digits;
        if (width > sizeof(pairs) - count || c->fields[FRACTION] < 0)
            return false;
        unsigned value = (unsigned)c->fields[FRACTION];
        if ((c->scale & 1) != 0)
            value *= 10; /* Right-pad the last fractional decimal pair. */
        for (unsigned i = width; i > 0; i--) {
            pairs[count + i - 1] = (uint8_t)(value % decimal_pair_base);
            value /= decimal_pair_base;
        }
        if (value != 0)
            return false;
        count += width;
    }
    size_t first = 0;
    while (first < count && pairs[first] == 0) {
        first++;
        exponent--;
    }
    while (count > first && pairs[count - 1] == 0)
        count--;
    if (first == count) {
        wire.bytes[0] = temporal_zero_marker;
    } else {
        if (count - first > wire.length - 1 ||
            exponent < -temporal_exponent_bias || exponent >= temporal_exponent_bias)
            return false;
        unsigned encoded = (unsigned)(exponent + temporal_exponent_bias);
        wire.bytes[0] = (uint8_t)(encoded | temporal_positive_flag);
        memcpy(wire.bytes + 1, pairs + first, count - first);
        if (c->negative) {
            wire.bytes[0] = (uint8_t)(encoded ^ 0x7f);
            /* Base-100 radix complement, retaining trailing zero padding. */
            unsigned carry = 1;
            for (size_t i = wire.length; i > 1; i--) {
                unsigned digit = decimal_pair_base - 1u - wire.bytes[i - 1] + carry;
                wire.bytes[i - 1] = (uint8_t)(digit % decimal_pair_base);
                carry = digit / decimal_pair_base;
            }
        }
    }
    *out = wire;
    return true;
}

static bool check_wire(sqli_result_t *result, const struct temporal_case *c)
{
    struct temporal_wire wire;
    if (!make_wire(c, &wire) || result == NULL || result->tuple_buffer == NULL ||
        result->column_count != matrix_projection_columns ||
        result->columns[matrix_value_column].encoded_length != wire.qualifier ||
        result->tuple_len < matrix_integer_width ||
        wire.length > result->tuple_len - matrix_integer_width)
        return false;
    if (memcmp(result->tuple_buffer + matrix_integer_width, wire.bytes, wire.length) != 0) {
        fprintf(stderr, "  received temporal payload differs from field-derived fixture\n");
        return false;
    }
    return true;
}

static unsigned power10(unsigned n)
{
    unsigned value = 1;
    while (n-- > 0)
        value *= 10;
    return value;
}

static unsigned random_bounded(struct matrix *m, unsigned limit)
{
    uint64_t x = m->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    m->rng = x;
    return (unsigned)(x % limit);
}

static bool build_case(struct matrix *m, bool interval, int start, int end,
                       int scale, int precision, int variant)
{
    if (m->count == MATRIX_CAPACITY)
        return false;
    struct temporal_case *c = &m->cases[m->count++];
    c->interval = interval;
    c->start = start;
    c->end = end;
    c->scale = scale;
    c->precision = precision;
    c->variant = variant;
    c->is_null = variant == (interval ? 5 : 3);
    c->negative = interval && (variant == 2 || variant == 4);
    bool maximum = variant == 1 || (interval && variant == 2);
    bool random = interval ? (variant == 3 || variant == 4) : variant == 2;
    char first[24], last[24];
    if (interval && start != FRACTION)
        snprintf(first, sizeof(first), "%s(%d)", field_names[start], precision);
    else
        snprintf(first, sizeof(first), "%s", field_names[start]);
    if (end == FRACTION)
        snprintf(last, sizeof(last), "FRACTION(%d)", scale);
    else
        snprintf(last, sizeof(last), "%s", field_names[end]);
    int n = snprintf(c->type, sizeof(c->type), "%s %s TO %s",
                     interval ? "INTERVAL" : "DATETIME", first, last);
    if (n < 0 || (size_t)n >= sizeof(c->type))
        return false;

    bool nonzero = false;
    size_t used = 0;
    if (c->negative)
        c->value[used++] = '-';
    for (int f = start; f <= end; f++) {
        static const int datetime_min[] = {1, 1, 1, 0, 0, 0, 0};
        static const int datetime_max[] = {9999, 12, 31, 23, 59, 59, 0};
        static const int interval_max[] = {0, 11, 0, 23, 59, 59, 0};
        int low = interval ? 0 : datetime_min[f];
        int high = interval ? interval_max[f] : datetime_max[f];
        if (f == FRACTION)
            high = (int)power10((unsigned)scale) - 1;
        else if (interval && f == start)
            high = (int)power10((unsigned)precision) - 1;
        /* Random partial dates must be valid regardless of omitted fields. */
        if (!interval && f == DAY && random)
            high = 28;
        int value = maximum ? high : low;
        if (random)
            value = low + (int)random_bounded(m, (unsigned)(high - low + 1));
        c->fields[f] = value;
        nonzero = nonzero || value != 0;
        const char *separator = "";
        if (f == FRACTION)
            separator = ".";
        else if (f > start)
            separator = f <= DAY ? "-" : f == HOUR ? " " : ":";
        int width = f == FRACTION ? scale : (!interval && f == YEAR ? 4 : 2);
        if (interval && f == start && f != FRACTION)
            width = 1;
        n = snprintf(c->value + used, sizeof(c->value) - used,
                     "%s%0*d", separator, width, value);
        if (n < 0 || (size_t)n >= sizeof(c->value) - used)
            return false;
        used += (size_t)n;
    }
    if (!nonzero && c->negative) {
        memmove(c->value, c->value + 1, strlen(c->value));
        c->negative = false;
    }
    return true;
}

static bool add_qualifier(struct matrix *m, bool interval, int start, int end,
                          int scale, int precision)
{
    for (int variant = 0; variant < (interval ? 6 : 4); variant++) {
        if (!build_case(m, interval, start, end, scale, precision, variant))
            return false;
    }
    return true;
}

static bool generate(struct matrix *m, uint64_t seed)
{
    memset(m, 0, sizeof(*m));
    m->rng = seed ? seed : UINT64_C(0x9e3779b97f4a7c15);
    for (int start = YEAR; start <= FRACTION; start++) {
        for (int end = start; end <= FRACTION; end++) {
            for (int scale = end == FRACTION ? 1 : 0;
                 scale <= (end == FRACTION ? 5 : 0); scale++) {
                if (!add_qualifier(m, false, start, end, scale, 0))
                    return false;
            }
        }
    }
    for (int start = YEAR; start <= FRACTION; start++) {
        int last = start <= MONTH ? MONTH : FRACTION;
        for (int end = start; end <= last; end++) {
            for (int scale = end == FRACTION ? 1 : 0;
                 scale <= (end == FRACTION ? 5 : 0); scale++) {
                for (int precision = start == FRACTION ? 0 : 1;
                     precision <= (start == FRACTION ? 0 : 9); precision++) {
                    if (!add_qualifier(m, true, start, end, scale, precision))
                        return false;
                }
            }
        }
    }
    return m->count == MATRIX_CASES;
}

static bool parse_number(const char *text, uint64_t *value)
{
    if (text == NULL || *text == '\0' || !isdigit((unsigned char)*text))
        return false;
    char *end;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || *end != '\0' || end == text)
        return false;
    *value = (uint64_t)parsed;
    return true;
}

/* Compare native imports with independently generated semantic fields, then
 * verify canonical text, precision and NULL range retention for every case. */
static bool check_native_value(const struct temporal_case *c)
{
    static const sqli_temporal_field_t native_fields[] = {
        SQLI_FIELD_YEAR, SQLI_FIELD_MONTH, SQLI_FIELD_DAY, SQLI_FIELD_HOUR,
        SQLI_FIELD_MINUTE, SQLI_FIELD_SECOND, SQLI_FIELD_FRACTION
    };
    struct temporal_wire expected_wire;
    if (!make_wire(c, &expected_wire))
        return false;
    uint8_t encoded[SQLI_TEMPORAL_WIRE_CAPACITY];
    size_t encoded_length = 0;
    sqli_temporal_range_t range = {
        native_fields[c->start], native_fields[c->end], (uint8_t)c->scale
    };
    uint32_t nanosecond = c->end == FRACTION ?
        (uint32_t)c->fields[FRACTION] * power10(9u - (unsigned)c->scale) : 0;
    uint64_t actual[7] = {0};
    sqli_temporal_range_t actual_range = {0};
    bool actual_null = false, actual_negative = false;
    char text[SQLI_TEMPORAL_MAX_TEXT + 1] = "unchanged";
    size_t required = SIZE_MAX;
    bool format_null = false;
    bool ok;
    if (c->interval) {
        sqli_interval_t *value = NULL;
        sqli_interval_parts_t parts = {0};
        ok = sqli_interval_create(&value) == SQLI_OK &&
             sqli_interval_parse(value, &range, c->value, strlen(c->value), c->is_null) == SQLI_OK &&
             sqli_interval_encode_wire(value, expected_wire.qualifier, encoded, sizeof(encoded), &encoded_length) == SQLI_OK &&
             encoded_length == expected_wire.length &&
             memcmp(encoded, expected_wire.bytes, encoded_length) == 0 &&
             sqli_interval_decode_wire(expected_wire.bytes, expected_wire.length, expected_wire.qualifier, value) == SQLI_OK &&
             sqli_interval_get_parts(value, &parts) == SQLI_OK &&
             sqli_interval_set_parts(value, &parts) == SQLI_OK &&
             sqli_interval_format(value, text, sizeof(text), &required, &format_null) == SQLI_OK;
        actual[YEAR] = parts.years;
        actual[MONTH] = parts.months;
        actual[DAY] = parts.days;
        actual[HOUR] = parts.hours;
        actual[MINUTE] = parts.minutes;
        actual[SECOND] = parts.seconds;
        actual[FRACTION] = parts.nanosecond;
        actual_range = parts.range;
        actual_null = parts.is_null;
        actual_negative = parts.negative;
        sqli_interval_destroy(value);
    } else {
        sqli_datetime_t *value = NULL;
        sqli_datetime_parts_t parts = {0};
        ok = sqli_datetime_create(&value) == SQLI_OK &&
             sqli_datetime_parse(value, &range, c->value, strlen(c->value), c->is_null) == SQLI_OK &&
             sqli_datetime_encode_wire(value, expected_wire.qualifier, encoded, sizeof(encoded), &encoded_length) == SQLI_OK &&
             encoded_length == expected_wire.length &&
             memcmp(encoded, expected_wire.bytes, encoded_length) == 0 &&
             sqli_datetime_decode_wire(expected_wire.bytes, expected_wire.length, expected_wire.qualifier, value) == SQLI_OK &&
             sqli_datetime_get_parts(value, &parts) == SQLI_OK &&
             sqli_datetime_set_parts(value, &parts) == SQLI_OK &&
             sqli_datetime_format(value, text, sizeof(text), &required, &format_null) == SQLI_OK;
        if (parts.year < 0) {
            ok = false;
        } else {
            actual[YEAR] = (uint64_t)parts.year;
        }
        actual[MONTH] = parts.month;
        actual[DAY] = parts.day;
        actual[HOUR] = parts.hour;
        actual[MINUTE] = parts.minute;
        actual[SECOND] = parts.second;
        actual[FRACTION] = parts.nanosecond;
        actual_range = parts.range;
        actual_null = parts.is_null;
        sqli_datetime_destroy(value);
    }
    ok = ok && actual_null == c->is_null && format_null == c->is_null &&
         actual_negative == (!c->is_null && c->negative) &&
         actual_range.first == range.first && actual_range.last == range.last &&
         actual_range.fractional_digits == range.fractional_digits;
    for (int field = YEAR; field <= FRACTION; field++) {
        uint64_t expected = c->is_null ? 0 :
            field == FRACTION ? nanosecond : (uint64_t)c->fields[field];
        ok = ok && actual[field] == expected;
    }
    char expected[sizeof(c->value)];
    memcpy(expected, c->value, sizeof(expected));
    if (!c->interval && c->start == YEAR && c->end >= SECOND) {
        enum { timestamp_date_length = 10 };
        expected[timestamp_date_length] = 'T';
    }
    ok = ok && (c->is_null ? required == 0 && strcmp(text, "unchanged") == 0 :
        required == strlen(expected) + 1 && strcmp(text, expected) == 0);
    if (!ok)
        fprintf(stderr, "native temporal value: %s value=%s\n", c->type, c->value);
    return ok;
}

static bool check_offline_wire(const struct temporal_case *c);

static int self_test(struct matrix *m)
{
    struct matrix *copy = malloc(sizeof(*copy));
    if (copy == NULL)
        return 1;
    bool ok = generate(m, DEFAULT_SEED) && generate(copy, DEFAULT_SEED) &&
              memcmp(m, copy, sizeof(*m)) == 0;
    size_t dt_types = 0, iv_types = 0, nulls = 0;
    for (size_t i = 0; ok && i < m->count; i++) {
        const struct temporal_case *c = &m->cases[i];
        if (c->variant == 0) {
            if (c->interval) iv_types++; else dt_types++;
            for (size_t j = 0; j < i; j++) {
                if (m->cases[j].variant == 0 && strcmp(c->type, m->cases[j].type) == 0)
                    ok = false;
            }
        }
        if (!check_offline_wire(c) || !check_native_value(c))
            ok = false;
        if (c->is_null) nulls++;
        if (c->interval && c->start != FRACTION &&
            (unsigned)c->fields[c->start] >= power10((unsigned)c->precision))
            ok = false;
        if (c->end == FRACTION &&
            (unsigned)c->fields[FRACTION] >= power10((unsigned)c->scale))
            ok = false;
    }
    ok = ok && dt_types == 56 && iv_types == 302 && nulls == 358;
    ok = ok && strcmp(m->cases[0].type, "DATETIME YEAR TO YEAR") == 0 &&
         strcmp(m->cases[0].value, "0001") == 0 &&
         strcmp(m->cases[1].value, "9999") == 0;
    ok = ok && generate(copy, DEFAULT_SEED + 1) &&
         strcmp(m->cases[2].value, copy->cases[2].value) != 0;
    uint64_t number;
    ok = ok && !parse_number("-1", &number) && !parse_number("1x", &number) &&
         !parse_number("18446744073709551616", &number);
    free(copy);
    printf("temporal generator: %zu cases, %zu DATETIME + %zu INTERVAL qualifiers: %s\n",
           m->count, dt_types, iv_types, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static bool execute_sql(sqli_conn_t *conn, const char *sql)
{
    sqli_result_t *result = NULL;
    sqli_status rc = sqli_query(conn, sql, &result);
    sqli_result_destroy(result);
    return rc == SQLI_OK;
}

static bool check_value(sqli_result_t *result, int column,
                        const struct temporal_case *c)
{
    int fields[7];
    bool is_null, negative = false;
    int scale, start, end;
    if (c->interval) {
        sqli_interval_parts_t v;
        if (test_interval_parts(result, column, &v) != SQLI_OK)
            return false;
        int decoded[] = {v.years, v.months, v.days, v.hours, v.minutes, v.seconds, test_fraction(v.nanosecond, v.range.fractional_digits)};
        memcpy(fields, decoded, sizeof(fields));
        is_null = v.is_null;
        negative = v.negative;
        scale = v.range.fractional_digits;
        start = ((v.range.first - SQLI_FIELD_YEAR) * 2);
        end = (v.range.last == SQLI_FIELD_FRACTION ? 10 + v.range.fractional_digits : ((int)v.range.last - SQLI_FIELD_YEAR) * 2);
    } else {
        sqli_datetime_parts_t v;
        if (test_datetime_parts(result, column, &v) != SQLI_OK)
            return false;
        int decoded[] = {v.year, v.month, v.day, v.hour, v.minute, v.second, test_fraction(v.nanosecond, v.range.fractional_digits)};
        memcpy(fields, decoded, sizeof(fields));
        is_null = v.is_null;
        scale = v.range.fractional_digits;
        start = ((v.range.first - SQLI_FIELD_YEAR) * 2);
        end = (v.range.last == SQLI_FIELD_FRACTION ? 10 + v.range.fractional_digits : ((int)v.range.last - SQLI_FIELD_YEAR) * 2);
    }
    if (is_null != c->is_null || sqli_result_is_null(result, column) != c->is_null) {
        fprintf(stderr, "  NULL expected=%d actual=%d\n", c->is_null, is_null);
        return false;
    }
    if (c->is_null)
        return true;
    if (negative != c->negative || start != (c->start == FRACTION ? 12 : c->start * 2) ||
        end != (c->end == FRACTION ? 10 + c->scale : c->end * 2) ||
        (c->end == FRACTION && scale != c->scale)) {
        fprintf(stderr, "  metadata start=%d end=%d scale=%d negative=%d\n",
                start, end, scale, negative);
        return false;
    }
    for (int field = c->start; field <= c->end; field++) {
        if (fields[field] != c->fields[field]) {
            fprintf(stderr, "  field=%s expected=%d actual=%d\n",
                    field_names[field], c->fields[field], fields[field]);
            return false;
        }
    }
    return true;
}

/* All buffers belong to this stack frame. Supplying the row-cache arrays
 * avoids allocation and makes the decoder exercise its ordinary cache path. */
static bool check_offline_wire(const struct temporal_case *c)
{
    struct temporal_wire wire;
    if (!make_wire(c, &wire))
        return false;
    sqli_column_info column = {0};
    column.type = c->interval ? SQLI_TYPE_INTERVAL : SQLI_TYPE_DATETIME;
    column.encoded_length = wire.qualifier;
    size_t data_start = 0;
    size_t data_length = 0;
    uint8_t is_null = 0;
    sqli_result_t result = {0};
    result.columns = &column;
    result.column_count = 1;
    result.current_row = 0;
    result.cur_cache_row = -1;
    result.tuple_buffer = wire.bytes;
    result.tuple_len = wire.length;
    result.cur_col_data_start = &data_start;
    result.cur_col_data_len = &data_length;
    result.cur_col_is_null = &is_null;
    bool ok = sqli_result_prepare_row_cache(&result) == SQLI_OK &&
              check_value(&result, offline_value_column, c);
    if (!ok)
        fprintf(stderr, "offline wire model: %s value=%s\n", c->type, c->value);
    return ok;
}

/* Informix pads INTERVAL leading fields with spaces. FRACTION-only text may
 * have an optional zero before the decimal point. Preserve all other digits
 * and separators when comparing the public string getter to the server. */
static void normalize_text(char *text)
{
    size_t length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1]))
        text[--length] = '\0';
    size_t start = 0;
    while (isspace((unsigned char)text[start]))
        start++;
    if (start > 0)
        memmove(text, text + start, strlen(text + start) + 1);
    size_t sign = text[0] == '-' ? 1 : 0;
    if (text[sign] == '0' && text[sign + 1] == '.')
        memmove(text + sign, text + sign + 1, strlen(text + sign + 1) + 1);
}

static bool check_text(sqli_result_t *result, const struct temporal_case *c)
{
    if (c->is_null)
        return true;
    char server[128], client[128];
    const char *value = sqli_result_get_string(result, matrix_text_column);
    if (value == NULL || strlen(value) >= sizeof(server))
        return false;
    snprintf(server, sizeof(server), "%s", value);
    value = c->interval ? sqli_result_get_interval_string(result, matrix_value_column) :
                          sqli_result_get_datetime_string(result, matrix_value_column);
    if (value == NULL || strlen(value) >= sizeof(client))
        return false;
    snprintf(client, sizeof(client), "%s", value);
    normalize_text(server);
    normalize_text(client);
    if (strcmp(server, client) != 0) {
        fprintf(stderr, "  text server='%s' client='%s'\n", server, client);
        return false;
    }
    return true;
}

/* Session-local temporary table prevents name collisions and leaves no durable
 * fixture on failure. Every case verifies the server value, semantic fields,
 * NULLs, and following columns in a mixed projection for all three insertion paths. */
static bool bind_native_case(sqli_stmt_t *stmt, const struct temporal_case *c)
{
    /* The generator's fields are contiguous YEAR..FRACTION, unlike wire codes. */
    const sqli_temporal_range_t range = {
        (sqli_temporal_field_t)(SQLI_FIELD_YEAR + c->start),
        (sqli_temporal_field_t)(SQLI_FIELD_YEAR + c->end), (uint8_t)c->scale
    };
    bool ok;
    if (c->interval) {
        sqli_interval_t *value = NULL;
        ok = sqli_interval_create(&value) == SQLI_OK &&
             sqli_interval_parse(value, &range, c->value, strlen(c->value), c->is_null) == SQLI_OK &&
             sqli_bind_interval(stmt, 0, value, &range, c->start == FRACTION ? 0 : (uint8_t)c->precision) == SQLI_OK;
        sqli_interval_destroy(value);
    } else {
        sqli_datetime_t *value = NULL;
        ok = sqli_datetime_create(&value) == SQLI_OK &&
             sqli_datetime_parse(value, &range, c->value, strlen(c->value), c->is_null) == SQLI_OK &&
             sqli_bind_datetime(stmt, 0, value, &range) == SQLI_OK;
        sqli_datetime_destroy(value);
    }
    return ok;
}

static bool run_case(sqli_conn_t *conn, const struct temporal_case *c,
                     const char **operation)
{
    char sql[768], literal[160];
    sqli_result_t *result = NULL;
    sqli_stmt_t *stmt = NULL;
    bool created = false, ok = false;
    const char *qualifier = strchr(c->type, ' ') + 1;
    if (c->is_null)
        snprintf(literal, sizeof(literal), "CAST(NULL AS %s)", c->type);
    else
        snprintf(literal, sizeof(literal), "%s%s(%s) %s",
                 c->negative ? "-" : "", c->interval ? "INTERVAL" : "DATETIME",
                 c->value + (c->negative ? 1 : 0), qualifier);
    *operation = "create";
    snprintf(sql, sizeof(sql), "CREATE TEMP TABLE sqli_temporal_matrix (id INT, v %s) WITH NO LOG", c->type);
    if (!execute_sql(conn, sql))
        goto cleanup;
    created = true;
    *operation = "literal insert";
    snprintf(sql, sizeof(sql), "INSERT INTO sqli_temporal_matrix VALUES (1, %s)", literal);
    if (!execute_sql(conn, sql))
        goto cleanup;
    *operation = "prepare insert";
    int parameters = 0;
    if (sqli_prepare(conn, "INSERT INTO sqli_temporal_matrix VALUES (2, ?)", &parameters, &stmt) != SQLI_OK || parameters != 1)
        goto cleanup;
    *operation = "bind insert";
    sqli_status rc = c->is_null ? sqli_bind_null(stmt, 0) : c->interval ?
        sqli_bind_interval_string(stmt, 0, c->value) : sqli_bind_datetime_string(stmt, 0, c->value);
    if (rc != SQLI_OK || sqli_execute(stmt) != SQLI_OK)
        goto cleanup;
    sqli_stmt_destroy(stmt);
    stmt = NULL;
    *operation = "prepare native insert";
    if (sqli_prepare(conn, "INSERT INTO sqli_temporal_matrix VALUES (3, ?)",
                     &parameters, &stmt) != SQLI_OK || parameters != 1)
        goto cleanup;
    *operation = "native wire insert";
    if (!bind_native_case(stmt, c) || sqli_execute(stmt) != SQLI_OK)
        goto cleanup;
    sqli_stmt_destroy(stmt);
    stmt = NULL;
    *operation = "mixed projection";
    snprintf(sql, sizeof(sql),
             "SELECT id,v,2468,-INTERVAL(3-02) YEAR(3) TO MONTH,1357,"
             "CASE WHEN %s THEN 1 ELSE 0 END,CAST(v AS LVARCHAR(100)) "
             "FROM sqli_temporal_matrix ORDER BY id",
             c->is_null ? "v IS NULL" : "v = v");
    /* Compare stored values to the independent typed SQL literal, not merely
     * two values decoded by the same client implementation. */
    if (!c->is_null)
        snprintf(sql, sizeof(sql),
                 "SELECT id,v,2468,-INTERVAL(3-02) YEAR(3) TO MONTH,1357,"
                 "CASE WHEN v = %s THEN 1 ELSE 0 END,CAST(v AS LVARCHAR(100)) "
                 "FROM sqli_temporal_matrix ORDER BY id", literal);
    if (sqli_query(conn, sql, &result) != SQLI_OK)
        goto cleanup;
    for (int id = 1; id <= 3; id++) {
        *operation = id == 1 ? "literal row decode" :
                     id == 2 ? "text-bound row decode" : "native-bound row decode";
        if (sqli_result_fetch(result) != SQLI_OK || !test_integer_equals(result, matrix_id_column, id) ||
            !test_integer_equals(result, matrix_sentinel_column, 2468) ||
            !test_integer_equals(result, matrix_tail_column, 1357) || !test_integer_equals(result, matrix_equal_column, 1) ||
            !check_wire(result, c) || !check_value(result, matrix_value_column, c) || !check_text(result, c))
            goto cleanup;
        sqli_interval_parts_t sentinel;
        if (test_interval_parts(result, matrix_interval_column, &sentinel) != SQLI_OK ||
            sentinel.is_null || !sentinel.negative || sentinel.years != 3 || sentinel.months != 2)
            goto cleanup;
    }
    *operation = "end of result";
    ok = sqli_result_fetch(result) == SQLI_EOF;
cleanup:
    if (!ok) {
        sqli_error_info error = {0};
        if (sqli_error_get_info(conn, &error) != SQLI_OK)
            fprintf(stderr, "  error details unavailable\n");
        fprintf(stderr, "  operation=%s sqlcode=%d isamcode=%d\n", *operation,
                error.sqlcode, error.isamcode);
    }
    sqli_result_destroy(result);
    sqli_stmt_destroy(stmt);
    if (created && !execute_sql(conn, "DROP TABLE sqli_temporal_matrix")) {
        *operation = "drop";
        return false;
    }
    return ok;
}

static int run_live(struct matrix *m, uint64_t seed, int only)
{
    sqli_connect_params p = {0};
    p.hostname = getenv("SQLI_TEST_HOST");
    p.service = getenv("SQLI_TEST_PORT");
    p.database = getenv("SQLI_TEST_DB");
    p.username = getenv("SQLI_TEST_USER");
    p.password = getenv("SQLI_TEST_PASS");
    p.server = getenv("SQLI_TEST_SERVER");
    p.client_locale = getenv("SQLI_CLIENT_LOCALE");
    p.db_locale = getenv("SQLI_DB_LOCALE");
    if (!p.hostname || !*p.hostname || !p.service || !*p.service ||
        !p.database || !*p.database || !p.username || !*p.username || !p.password) {
        fprintf(stderr, "SQLI_TEST_HOST/PORT/DB/USER/PASS not set; temporal live test skipped\n");
        return 77;
    }
    sqli_conn_t *conn = NULL;
    if (sqli_create(&conn) != SQLI_OK)
        return 1;
    if (sqli_connect(conn, &p) != SQLI_OK) {
        fprintf(stderr, "temporal matrix: connection failed: %s\n", sqli_error(conn));
        sqli_destroy(conn);
        return 1;
    }
    size_t tested = 0, failed = 0;
    fprintf(stderr, "temporal matrix: seed=0x%" PRIx64 " generated=%zu only=%d\n", seed, m->count, only);
    for (size_t i = 0; i < m->count; i++) {
        if (only >= 0 && (size_t)only != i)
            continue;
        const struct temporal_case *c = &m->cases[i];
        const char *operation = "";
        tested++;
        if (!run_case(conn, c, &operation)) {
            failed++;
            fprintf(stderr, "FAIL seed=0x%" PRIx64 " case=%zu type='%s' value='%s' variant=%d operation=%s\n",
                    seed, i, c->type, c->is_null ? "NULL" : c->value, c->variant, operation);
        }
        if (tested % 100 == 0)
            fprintf(stderr, "temporal matrix: tested=%zu failed=%zu\n", tested, failed);
    }
    sqli_close(conn);
    sqli_destroy(conn);
    printf("temporal matrix: seed=0x%" PRIx64 " tested=%zu passed=%zu failed=%zu\n",
           seed, tested, tested - failed, failed);
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "--self-test") != 0 &&
        strcmp(argv[1], "--live") != 0 && strcmp(argv[1], "--list") != 0)) {
        fprintf(stderr, "usage: %s --self-test|--list|--live\n", argv[0]);
        return 2;
    }
    uint64_t seed = DEFAULT_SEED, only_value = 0;
    const char *seed_text = getenv("SQLI_FUZZ_SEED");
    const char *only_text = getenv("SQLI_FUZZ_ONLY");
    if ((seed_text && !parse_number(seed_text, &seed)) ||
        (only_text && (!parse_number(only_text, &only_value) || only_value >= MATRIX_CASES))) {
        fprintf(stderr, "invalid SQLI_FUZZ_SEED or SQLI_FUZZ_ONLY (case range 0..%d)\n", MATRIX_CASES - 1);
        return 2;
    }
    struct matrix *m = malloc(sizeof(*m));
    if (m == NULL)
        return 1;
    int rc = 0;
    if (strcmp(argv[1], "--self-test") == 0) {
        rc = self_test(m);
    } else if (!generate(m, seed)) {
        fprintf(stderr, "temporal generation failed or case capacity exceeded\n");
        rc = 1;
    } else if (strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < m->count; i++) {
            const struct temporal_case *c = &m->cases[i];
            printf("%zu\t%s\t%s\tvariant=%d\n", i, c->type,
                   c->is_null ? "NULL" : c->value, c->variant);
        }
    } else {
        rc = run_live(m, seed, only_text ? (int)only_value : -1);
    }
    free(m);
    return rc;
}
