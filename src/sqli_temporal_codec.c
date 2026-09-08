#include "libsqli/sqli_temporal.h"
#include "sqli_temporal_codec.h"
#include "sqli_base100.h"

#include <string.h>

enum {
    first_year = 1, last_year = 9999, epoch_following_year = 1900,
    days_per_year = 365, months_per_year = 12,
    pair_base = 100, pair_digits = 2, exponent_bias = 64,
    positive_flag = 128, zero_marker = 128,
    wire_second = 10, wire_fraction_start = 12,
    max_fraction_digits = 5, max_leading_digits = 9,
    year_digits = 4, ordinary_digits = 2,
    field_count = SQLI_FIELD_FRACTION + 1
};

struct wire_range {
    sqli_temporal_range_t range;
    unsigned leading_digits;
    size_t integral_pairs;
    size_t fraction_pairs;
    size_t width;
    int exponent;
};

struct wire_fields {
    uint64_t fields[field_count];
    uint32_t nanosecond;
    bool negative;
    bool is_null;
};

static uint32_t power10(unsigned digits)
{
    uint32_t value = 1;
    while (digits-- != 0)
        value *= 10;
    return value;
}

static int32_t days_before_year(int32_t year)
{
    int32_t previous = year - 1;
    return previous * days_per_year + previous / 4 - previous / 100 + previous / 400;
}

static unsigned days_in_month(int32_t year, unsigned month)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return month == 2 && leap ? 29u : days[month - 1];
}

sqli_status sqli_date_encode_wire(const sqli_date_t *value, uint8_t *bytes,
                                   size_t capacity, size_t *length)
{
    if (value == NULL || bytes == NULL || length == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_status status = sqli_date_validate(value);
    if (status != SQLI_OK)
        return status;
    if (capacity < SQLI_DATE_WIRE_SIZE)
        return SQLI_BUFFER_TOO_SMALL;
    uint32_t encoded = UINT32_C(0x80000000);
    if (!value->is_null) {
        int32_t ordinal = days_before_year(value->year) + value->day - 1;
        for (unsigned month = 1; month < value->month; month++)
            ordinal += (int32_t)days_in_month(value->year, month);
        int32_t days = ordinal - (days_before_year(epoch_following_year) - 1);
        /* Conversion to unsigned is defined modulo 2^32, including pre-epoch offsets. */
        encoded = (uint32_t)days;
    }
    for (size_t i = 0; i < SQLI_DATE_WIRE_SIZE; i++)
        bytes[i] = (uint8_t)(encoded >> (8 * (SQLI_DATE_WIRE_SIZE - 1 - i)));
    *length = SQLI_DATE_WIRE_SIZE;
    return SQLI_OK;
}

sqli_status sqli_date_decode_wire(const uint8_t *bytes, size_t length, sqli_date_t *out)
{
    if (bytes == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (length != SQLI_DATE_WIRE_SIZE)
        return SQLI_PROTO_ERROR;
    uint32_t encoded = 0;
    for (size_t i = 0; i < length; i++)
        encoded = (encoded << 8) | bytes[i];
    if (encoded == UINT32_C(0x80000000))
        return sqli_date_set_null(out);
    int64_t days = encoded;
    if (encoded > INT32_MAX)
        days -= INT64_C(4294967296);
    int64_t ordinal = days + days_before_year(epoch_following_year) - 1;
    if (ordinal < 0 || ordinal >= days_before_year(last_year + 1))
        return SQLI_PROTO_ERROR;
    int32_t low = first_year, high = last_year;
    while (low < high) {
        int32_t middle = low + (high - low + 1) / 2;
        if (days_before_year(middle) <= ordinal)
            low = middle;
        else
            high = middle - 1;
    }
    unsigned day = (unsigned)(ordinal - days_before_year(low));
    unsigned month = 1;
    while (month < months_per_year && day >= days_in_month(low, month)) {
        day -= days_in_month(low, month);
        month++;
    }
    return sqli_date_set(out, low, (int32_t)month, (int32_t)day + 1);
}

static sqli_status decode_range(uint16_t qualifier, bool interval, struct wire_range *out)
{
    unsigned digits = qualifier >> 8;
    unsigned start = (qualifier >> 4) & 0xf;
    unsigned end = qualifier & 0xf;
    if (start > wire_fraction_start || (start & 1) != 0 ||
        (end <= wire_second && ((end & 1) != 0 || end < start)) ||
        (start == wire_fraction_start && end <= wire_second))
        return SQLI_PROTO_ERROR;
    struct wire_range result = {0};
    result.range.first = (sqli_temporal_field_t)(start / 2 + SQLI_FIELD_YEAR);
    result.range.last = end > wire_second ? SQLI_FIELD_FRACTION :
        (sqli_temporal_field_t)(end / 2 + SQLI_FIELD_YEAR);
    result.range.fractional_digits = end > wire_second ? (uint8_t)(end - wire_second) : 0;
    if (interval && result.range.first <= SQLI_FIELD_MONTH && result.range.last > SQLI_FIELD_MONTH)
        return SQLI_PROTO_ERROR;
    if (start == wire_fraction_start) {
        if (digits != result.range.fractional_digits)
            return SQLI_PROTO_ERROR;
    } else {
        unsigned remaining = end - start;
        if (digits <= remaining)
            return SQLI_PROTO_ERROR;
        result.leading_digits = digits - remaining;
        if (interval) {
            if (result.leading_digits > max_leading_digits)
                return SQLI_PROTO_ERROR;
        } else if (result.leading_digits != (start == 0 ? year_digits : ordinary_digits)) {
            return SQLI_PROTO_ERROR;
        }
        unsigned integral_end = end > wire_second ? wire_second : end;
        result.integral_pairs = (result.leading_digits + 1) / pair_digits + (integral_end - start) / pair_digits;
        result.exponent = (int)result.integral_pairs + (int)((wire_second - integral_end) / pair_digits);
    }
    result.fraction_pairs = (result.range.fractional_digits + 1u) / pair_digits;
    result.width = 1 + result.integral_pairs + result.fraction_pairs;
    if (result.width > SQLI_TEMPORAL_WIRE_CAPACITY)
        return SQLI_PROTO_ERROR;
    *out = result;
    return SQLI_OK;
}

sqli_status sqli_temporal_decode_range(uint16_t qualifier, bool interval, sqli_temporal_range_t *out)
{
    if (out == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct wire_range range;
    sqli_status status = decode_range(qualifier, interval, &range);
    if (status == SQLI_OK)
        *out = range.range;
    return status;
}

sqli_status sqli_temporal_wire_size(uint16_t qualifier, bool interval, size_t *out)
{
    if (out == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct wire_range range;
    sqli_status status = decode_range(qualifier, interval, &range);
    if (status == SQLI_OK)
        *out = range.width;
    return status;
}

static sqli_status unpack(const uint8_t *bytes, size_t length, const struct wire_range *range,
                           struct wire_fields *out)
{
    if (length != range->width)
        return SQLI_PROTO_ERROR;
    bool all_zero = true;
    for (size_t i = 0; i < length; i++)
        all_zero = all_zero && bytes[i] == 0;
    out->is_null = all_zero;
    if (all_zero)
        return SQLI_OK;
    uint8_t coefficient[SQLI_TEMPORAL_WIRE_CAPACITY] = {0};
    size_t count = length - 1;
    for (size_t i = 0; i < count; i++) {
        if (bytes[i + 1] >= pair_base)
            return SQLI_PROTO_ERROR;
        coefficient[i] = bytes[i + 1];
    }
    out->negative = (bytes[0] & positive_flag) == 0;
    if (out->negative)
        sqli_base100_radix_complement(coefficient, count);
    bool zero = true;
    for (size_t i = 0; i < count; i++)
        zero = zero && coefficient[i] == 0;
    if (zero)
        return bytes[0] == zero_marker ? SQLI_OK : SQLI_PROTO_ERROR;
    if (coefficient[0] == 0)
        return SQLI_PROTO_ERROR; /* Nonzero coefficients are normalized. */
    int exponent = out->negative ? (int)(bytes[0] ^ 0x7f) - exponent_bias :
        (int)(bytes[0] & 0x7f) - exponent_bias;
    int offset = range->exponent - exponent;
    uint8_t aligned[SQLI_TEMPORAL_WIRE_CAPACITY] = {0};
    for (size_t i = 0; i < count; i++) {
        int slot = offset + (int)i;
        if (slot < 0 || slot >= (int)count) {
            if (coefficient[i] != 0)
                return SQLI_PROTO_ERROR;
        } else {
            aligned[slot] = coefficient[i];
        }
    }
    size_t position = 0;
    for (int field = range->range.first; field <= (int)range->range.last && field <= SQLI_FIELD_SECOND; field++) {
        size_t width = field == (int)range->range.first ? (range->leading_digits + 1) / pair_digits : 1;
        uint64_t magnitude = 0;
        for (size_t i = 0; i < width; i++)
            magnitude = magnitude * pair_base + aligned[position++];
        out->fields[field] = magnitude;
    }
    uint32_t fraction = 0;
    for (size_t i = 0; i < range->fraction_pairs; i++)
        fraction = fraction * pair_base + aligned[position++];
    if ((range->range.fractional_digits & 1) != 0) {
        if (fraction % 10 != 0)
            return SQLI_PROTO_ERROR;
        fraction /= 10;
    }
    out->nanosecond = fraction * power10(9u - range->range.fractional_digits);
    if (range->leading_digits != 0 &&
        out->fields[range->range.first] >= power10(range->leading_digits))
        return SQLI_PROTO_ERROR;
    return SQLI_OK;
}

static bool matching_range(sqli_temporal_range_t value, const struct wire_range *target, bool is_null)
{
    if (is_null && value.first == SQLI_FIELD_UNKNOWN && value.last == SQLI_FIELD_UNKNOWN)
        return true;
    return value.first == target->range.first && value.last == target->range.last;
}

static sqli_status pack(const struct wire_fields *fields, const struct wire_range *range,
                         uint8_t *bytes, size_t capacity, size_t *length)
{
    uint8_t temporary[SQLI_TEMPORAL_WIRE_CAPACITY] = {0};
    if (!fields->is_null) {
        if (range->leading_digits != 0 &&
            fields->fields[range->range.first] >= power10(range->leading_digits))
            return SQLI_OUT_OF_RANGE;
        uint32_t factor = power10(9u - range->range.fractional_digits);
        if (fields->nanosecond % factor != 0)
            return SQLI_INEXACT;
        uint8_t groups[SQLI_TEMPORAL_WIRE_CAPACITY] = {0};
        size_t count = 0;
        for (int field = range->range.first; field <= (int)range->range.last && field <= SQLI_FIELD_SECOND; field++) {
            size_t width = field == (int)range->range.first ? (range->leading_digits + 1) / pair_digits : 1;
            uint64_t magnitude = fields->fields[field];
            for (size_t i = width; i > 0; i--) {
                groups[count + i - 1] = (uint8_t)(magnitude % pair_base);
                magnitude /= pair_base;
            }
            count += width;
        }
        uint32_t fraction = fields->nanosecond / factor;
        if ((range->range.fractional_digits & 1) != 0)
            fraction *= 10;
        for (size_t i = range->fraction_pairs; i > 0; i--) {
            groups[count + i - 1] = (uint8_t)(fraction % pair_base);
            fraction /= pair_base;
        }
        count += range->fraction_pairs;
        size_t first = 0;
        while (first < count && groups[first] == 0)
            first++;
        if (first == count) {
            temporary[0] = zero_marker;
        } else {
            int exponent = range->exponent - (int)first;
            temporary[0] = (uint8_t)(positive_flag | (unsigned)(exponent + exponent_bias));
            memcpy(temporary + 1, groups + first, count - first);
            if (fields->negative) {
                temporary[0] ^= 0xff;
                sqli_base100_radix_complement(temporary + 1, range->width - 1);
            }
        }
    }
    if (capacity < range->width)
        return SQLI_BUFFER_TOO_SMALL;
    memcpy(bytes, temporary, range->width);
    *length = range->width;
    return SQLI_OK;
}

sqli_status sqli_datetime_decode_wire(const uint8_t *bytes, size_t length,
                                       uint16_t qualifier, sqli_datetime_t *out)
{
    if (bytes == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct wire_range range;
    sqli_status status = decode_range(qualifier, false, &range);
    if (status != SQLI_OK)
        return status;
    struct wire_fields fields = {0};
    status = unpack(bytes, length, &range, &fields);
    if (status != SQLI_OK || fields.negative)
        return SQLI_PROTO_ERROR;
    /* Reject narrowing before importing the validated calendar value. */
    if (fields.fields[SQLI_FIELD_YEAR] > last_year)
        return SQLI_PROTO_ERROR;
    for (int field = SQLI_FIELD_MONTH; field <= SQLI_FIELD_SECOND; field++) {
        if (fields.fields[field] > UINT8_MAX)
            return SQLI_PROTO_ERROR;
    }
    sqli_datetime_parts_t parts = {
        .range = range.range, .year = (int32_t)fields.fields[SQLI_FIELD_YEAR],
        .month = (uint8_t)fields.fields[SQLI_FIELD_MONTH], .day = (uint8_t)fields.fields[SQLI_FIELD_DAY],
        .hour = (uint8_t)fields.fields[SQLI_FIELD_HOUR], .minute = (uint8_t)fields.fields[SQLI_FIELD_MINUTE],
        .second = (uint8_t)fields.fields[SQLI_FIELD_SECOND],
        .nanosecond = fields.nanosecond, .is_null = fields.is_null
    };
    return sqli_datetime_set_parts(out, &parts) == SQLI_OK ? SQLI_OK : SQLI_PROTO_ERROR;
}

sqli_status sqli_interval_decode_wire(const uint8_t *bytes, size_t length,
                                       uint16_t qualifier, sqli_interval_t *out)
{
    if (bytes == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct wire_range range;
    sqli_status status = decode_range(qualifier, true, &range);
    if (status != SQLI_OK)
        return status;
    struct wire_fields fields = {0};
    status = unpack(bytes, length, &range, &fields);
    if (status != SQLI_OK)
        return status;
    sqli_interval_parts_t parts = {
        .range = range.range, .years = fields.fields[SQLI_FIELD_YEAR], .months = fields.fields[SQLI_FIELD_MONTH],
        .days = fields.fields[SQLI_FIELD_DAY], .hours = fields.fields[SQLI_FIELD_HOUR],
        .minutes = fields.fields[SQLI_FIELD_MINUTE], .seconds = fields.fields[SQLI_FIELD_SECOND],
        .nanosecond = fields.nanosecond, .negative = fields.negative, .is_null = fields.is_null
    };
    return sqli_interval_set_parts(out, &parts) == SQLI_OK ? SQLI_OK : SQLI_PROTO_ERROR;
}

sqli_status sqli_datetime_encode_wire(const sqli_datetime_t *value, uint16_t qualifier,
                                       uint8_t *bytes, size_t capacity, size_t *length)
{
    if (value == NULL || bytes == NULL || length == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct wire_range range;
    sqli_status status = decode_range(qualifier, false, &range);
    if (status != SQLI_OK)
        return status;
    sqli_datetime_parts_t parts;
    status = sqli_datetime_get_parts(value, &parts);
    if (status != SQLI_OK)
        return status;
    if (!matching_range(parts.range, &range, parts.is_null))
        return SQLI_INVALID_ARGUMENT;
    struct wire_fields fields = {.nanosecond = parts.nanosecond, .is_null = parts.is_null};
    fields.fields[SQLI_FIELD_YEAR] = (uint64_t)parts.year;
    fields.fields[SQLI_FIELD_MONTH] = parts.month;
    fields.fields[SQLI_FIELD_DAY] = parts.day;
    fields.fields[SQLI_FIELD_HOUR] = parts.hour;
    fields.fields[SQLI_FIELD_MINUTE] = parts.minute;
    fields.fields[SQLI_FIELD_SECOND] = parts.second;
    return pack(&fields, &range, bytes, capacity, length);
}

sqli_status sqli_interval_encode_wire(const sqli_interval_t *value, uint16_t qualifier,
                                       uint8_t *bytes, size_t capacity, size_t *length)
{
    if (value == NULL || bytes == NULL || length == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct wire_range range;
    sqli_status status = decode_range(qualifier, true, &range);
    if (status != SQLI_OK)
        return status;
    sqli_interval_parts_t parts;
    status = sqli_interval_get_parts(value, &parts);
    if (status != SQLI_OK)
        return status;
    if (!matching_range(parts.range, &range, parts.is_null))
        return SQLI_INVALID_ARGUMENT;
    struct wire_fields fields = {
        .nanosecond = parts.nanosecond, .negative = parts.negative, .is_null = parts.is_null
    };
    fields.fields[SQLI_FIELD_YEAR] = parts.years;
    fields.fields[SQLI_FIELD_MONTH] = parts.months;
    fields.fields[SQLI_FIELD_DAY] = parts.days;
    fields.fields[SQLI_FIELD_HOUR] = parts.hours;
    fields.fields[SQLI_FIELD_MINUTE] = parts.minutes;
    fields.fields[SQLI_FIELD_SECOND] = parts.seconds;
    return pack(&fields, &range, bytes, capacity, length);
}
