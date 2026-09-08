#include "sqli_temporal_internal.h"
#include "libsqli/sqli_temporal.h"
#include "libsqli/sqli.h"

#include <stdlib.h>
#include <string.h>

enum {
    calendar_first_year = 1,
    calendar_last_year = 9999,
    calendar_year_digits = 4,
    calendar_field_digits = 2,
    calendar_date_length = 10,
    timestamp_second_length = 19,
    timestamp_fraction_offset = 20,
    temporal_field_count = SQLI_FIELD_FRACTION + 1,
    nanosecond_digits = 9,
    nanoseconds_per_second = 1000000000,
    uint64_decimal_digits = 20,
    months_per_year = 12,
    hours_per_day = 24,
    minutes_per_hour = 60,
    seconds_per_minute = 60
};


struct temporal_fields {
    sqli_temporal_range_t range;
    uint64_t integral[temporal_field_count];
    uint32_t nanosecond;
    bool negative;
    bool is_null;
};

struct text_reader {
    const char *text;
    size_t length;
    size_t position;
};

struct text_writer {
    char bytes[SQLI_TEMPORAL_MAX_TEXT + 1];
    size_t used;
};

static const uint32_t fraction_factors[] = {
    1000000000, 100000000, 10000000, 1000000, 100000, 10000,
    1000, 100, 10, 1
};

static bool leap_year(int32_t year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static unsigned month_days(unsigned month, int32_t year)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && (year == 0 || leap_year(year)))
        return 29;
    return days[month - 1];
}

static sqli_status validate_range(const sqli_temporal_range_t *range, bool interval,
                                  bool allow_unknown)
{
    if (range == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (range->first == SQLI_FIELD_UNKNOWN && range->last == SQLI_FIELD_UNKNOWN &&
        range->fractional_digits == 0)
        return allow_unknown ? SQLI_OK : SQLI_INVALID_ARGUMENT;
    if (range->first < SQLI_FIELD_YEAR || range->last > SQLI_FIELD_FRACTION ||
        range->first > range->last)
        return SQLI_INVALID_ARGUMENT;
    if (interval && range->first <= SQLI_FIELD_MONTH && range->last > SQLI_FIELD_MONTH)
        return SQLI_INVALID_ARGUMENT;
    if (range->last == SQLI_FIELD_FRACTION) {
        if (range->fractional_digits == 0 || range->fractional_digits > nanosecond_digits)
            return SQLI_INVALID_ARGUMENT;
    } else if (range->fractional_digits != 0) {
        return SQLI_INVALID_ARGUMENT;
    }
    return SQLI_OK;
}

static sqli_status validate_fields(const struct temporal_fields *fields, bool interval)
{
    sqli_status status = validate_range(&fields->range, interval, fields->is_null);
    if (status != SQLI_OK || fields->is_null)
        return status;
    for (int field = SQLI_FIELD_YEAR; field <= SQLI_FIELD_SECOND; field++) {
        uint64_t magnitude = fields->integral[field];
        if (field < (int)fields->range.first || field > (int)fields->range.last) {
            if (magnitude != 0)
                return SQLI_INVALID_ARGUMENT;
            continue;
        }
        if (interval && field == (int)fields->range.first)
            continue;
        uint64_t minimum = !interval && field <= SQLI_FIELD_DAY ? 1u : 0u;
        uint64_t maximum;
        switch (field) {
        case SQLI_FIELD_YEAR: maximum = calendar_last_year; break;
        case SQLI_FIELD_MONTH: maximum = interval ? months_per_year - 1 : months_per_year; break;
        case SQLI_FIELD_DAY: maximum = 31; break;
        case SQLI_FIELD_HOUR: maximum = hours_per_day - 1; break;
        case SQLI_FIELD_MINUTE: maximum = minutes_per_hour - 1; break;
        default: maximum = seconds_per_minute - 1; break;
        }
        if (magnitude < minimum || magnitude > maximum)
            return SQLI_OUT_OF_RANGE;
    }
    if (!interval && fields->integral[SQLI_FIELD_MONTH] != 0 &&
        fields->integral[SQLI_FIELD_DAY] != 0) {
        /* Year and month have already been range-checked above. Year zero
         * means absent: validate February against its possible maximum. */
        unsigned month = (unsigned)fields->integral[SQLI_FIELD_MONTH];
        int32_t year = (int32_t)fields->integral[SQLI_FIELD_YEAR];
        if (fields->integral[SQLI_FIELD_DAY] > month_days(month, year))
            return SQLI_OUT_OF_RANGE;
    }
    if (fields->range.last != SQLI_FIELD_FRACTION)
        return fields->nanosecond == 0 ? SQLI_OK : SQLI_INVALID_ARGUMENT;
    if (fields->nanosecond >= nanoseconds_per_second)
        return SQLI_OUT_OF_RANGE;
    if (fields->nanosecond % fraction_factors[fields->range.fractional_digits] != 0)
        return SQLI_INEXACT;
    return SQLI_OK;
}

sqli_status sqli_date_validate(const sqli_date_t *value)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (value->is_null)
        return SQLI_OK;
    if (value->year < calendar_first_year || value->year > calendar_last_year ||
        value->month < 1 || value->month > months_per_year || value->day < 1)
        return SQLI_OUT_OF_RANGE;
    return value->day <= month_days(value->month, value->year) ? SQLI_OK : SQLI_OUT_OF_RANGE;
}

sqli_status sqli_date_set(sqli_date_t *value, int32_t year, int32_t month, int32_t day)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (month < 1 || month > months_per_year || day < 1 || day > 31)
        return SQLI_OUT_OF_RANGE;
    sqli_date_t temporary = {.year = year, .month = (uint8_t)month, .day = (uint8_t)day};
    sqli_status status = sqli_date_validate(&temporary);
    if (status == SQLI_OK)
        *value = temporary;
    return status;
}

sqli_status sqli_date_set_null(sqli_date_t *value)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    *value = (sqli_date_t){.is_null = true};
    return SQLI_OK;
}

sqli_status sqli_datetime_create(sqli_datetime_t **out)
{
    if (out == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_datetime_t *value = malloc(sizeof(*value));
    if (value == NULL)
        return SQLI_ALLOC_FAIL;
    value->parts = (sqli_datetime_parts_t){.is_null = true};
    *out = value;
    return SQLI_OK;
}

void sqli_datetime_destroy(sqli_datetime_t *value)
{
    free(value);
}

sqli_status sqli_datetime_copy(sqli_datetime_t *destination, const sqli_datetime_t *source)
{
    if (destination == NULL || source == NULL)
        return SQLI_INVALID_ARGUMENT;
    destination->parts = source->parts;
    return SQLI_OK;
}

sqli_status sqli_datetime_is_null(const sqli_datetime_t *value, bool *out)
{
    if (value == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    *out = value->parts.is_null;
    return SQLI_OK;
}

sqli_status sqli_datetime_set_null(sqli_datetime_t *value)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    value->parts = (sqli_datetime_parts_t){.range = value->parts.range, .is_null = true};
    return SQLI_OK;
}

static struct temporal_fields datetime_fields(const sqli_datetime_parts_t *parts)
{
    struct temporal_fields fields = {
        .range = parts->range, .nanosecond = parts->nanosecond, .is_null = parts->is_null
    };
    /* Non-NULL callers validate the sign before conversion. NULL callers
     * ignore numeric fields, including an otherwise invalid negative year. */
    fields.integral[SQLI_FIELD_YEAR] = parts->year >= 0 ? (uint64_t)parts->year : 0;
    fields.integral[SQLI_FIELD_MONTH] = parts->month;
    fields.integral[SQLI_FIELD_DAY] = parts->day;
    fields.integral[SQLI_FIELD_HOUR] = parts->hour;
    fields.integral[SQLI_FIELD_MINUTE] = parts->minute;
    fields.integral[SQLI_FIELD_SECOND] = parts->second;
    return fields;
}

sqli_status sqli_datetime_set_parts(sqli_datetime_t *value, const sqli_datetime_parts_t *parts)
{
    if (value == NULL || parts == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (!parts->is_null && parts->year < 0)
        return SQLI_OUT_OF_RANGE;
    struct temporal_fields fields = datetime_fields(parts);
    sqli_status status = validate_fields(&fields, false);
    if (status != SQLI_OK)
        return status;
    value->parts = parts->is_null ?
        (sqli_datetime_parts_t){.range = parts->range, .is_null = true} : *parts;
    return SQLI_OK;
}

sqli_status sqli_datetime_get_parts(const sqli_datetime_t *value, sqli_datetime_parts_t *out)
{
    if (value == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    *out = value->parts;
    return SQLI_OK;
}

sqli_status sqli_interval_create(sqli_interval_t **out)
{
    if (out == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_interval_t *value = malloc(sizeof(*value));
    if (value == NULL)
        return SQLI_ALLOC_FAIL;
    value->parts = (sqli_interval_parts_t){.is_null = true};
    *out = value;
    return SQLI_OK;
}

void sqli_interval_destroy(sqli_interval_t *value)
{
    free(value);
}

sqli_status sqli_interval_copy(sqli_interval_t *destination, const sqli_interval_t *source)
{
    if (destination == NULL || source == NULL)
        return SQLI_INVALID_ARGUMENT;
    destination->parts = source->parts;
    return SQLI_OK;
}

sqli_status sqli_interval_is_null(const sqli_interval_t *value, bool *out)
{
    if (value == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    *out = value->parts.is_null;
    return SQLI_OK;
}

sqli_status sqli_interval_set_null(sqli_interval_t *value)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    value->parts = (sqli_interval_parts_t){.range = value->parts.range, .is_null = true};
    return SQLI_OK;
}

static struct temporal_fields interval_fields(const sqli_interval_parts_t *parts)
{
    struct temporal_fields fields = {
        .range = parts->range, .nanosecond = parts->nanosecond,
        .negative = parts->negative, .is_null = parts->is_null
    };
    fields.integral[SQLI_FIELD_YEAR] = parts->years;
    fields.integral[SQLI_FIELD_MONTH] = parts->months;
    fields.integral[SQLI_FIELD_DAY] = parts->days;
    fields.integral[SQLI_FIELD_HOUR] = parts->hours;
    fields.integral[SQLI_FIELD_MINUTE] = parts->minutes;
    fields.integral[SQLI_FIELD_SECOND] = parts->seconds;
    return fields;
}

sqli_status sqli_interval_set_parts(sqli_interval_t *value, const sqli_interval_parts_t *parts)
{
    if (value == NULL || parts == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct temporal_fields fields = interval_fields(parts);
    sqli_status status = validate_fields(&fields, true);
    if (status != SQLI_OK)
        return status;
    sqli_interval_parts_t temporary = parts->is_null ?
        (sqli_interval_parts_t){.range = parts->range, .is_null = true} : *parts;
    bool zero = fields.nanosecond == 0;
    for (int field = SQLI_FIELD_YEAR; field <= SQLI_FIELD_SECOND; field++)
        zero = zero && fields.integral[field] == 0;
    if (zero)
        temporary.negative = false;
    value->parts = temporary;
    return SQLI_OK;
}

sqli_status sqli_interval_get_parts(const sqli_interval_t *value, sqli_interval_parts_t *out)
{
    if (value == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    *out = value->parts;
    return SQLI_OK;
}

static bool ascii_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

static sqli_status read_number(struct text_reader *reader, size_t minimum, size_t maximum,
                               uint64_t *out)
{
    size_t start = reader->position;
    uint64_t value = 0;
    while (reader->position < reader->length && ascii_digit(reader->text[reader->position])) {
        unsigned digit = (unsigned)(reader->text[reader->position] - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return SQLI_OUT_OF_RANGE;
        if (reader->position - start == maximum)
            return SQLI_INVALID_ARGUMENT;
        value = value * 10 + digit;
        reader->position++;
    }
    if (reader->position - start < minimum)
        return SQLI_INVALID_ARGUMENT;
    *out = value;
    return SQLI_OK;
}

static char field_separator(int field, bool iso)
{
    switch (field) {
    case SQLI_FIELD_MONTH:
    case SQLI_FIELD_DAY: return '-';
    case SQLI_FIELD_HOUR: return iso ? 'T' : ' ';
    case SQLI_FIELD_MINUTE:
    case SQLI_FIELD_SECOND: return ':';
    default: return '.';
    }
}

static sqli_status parse_fields(struct temporal_fields *fields, const char *text,
                                size_t length, bool interval)
{
    sqli_status status = validate_range(&fields->range, interval, false);
    if (status != SQLI_OK || text == NULL || length == 0)
        return SQLI_INVALID_ARGUMENT;
    if (length > SQLI_TEMPORAL_MAX_TEXT)
        return SQLI_LIMIT_EXCEEDED;
    struct text_reader reader = {.text = text, .length = length};
    if (interval && (text[0] == '+' || text[0] == '-')) {
        fields->negative = text[0] == '-';
        reader.position++;
    }
    for (int field = fields->range.first; field <= (int)fields->range.last; field++) {
        if (field == SQLI_FIELD_FRACTION && field == (int)fields->range.first &&
            reader.position < length && text[reader.position] == '0')
            reader.position++; /* Optional zero before a fraction-only point. */
        if (field != (int)fields->range.first || field == SQLI_FIELD_FRACTION) {
            if (reader.position == length)
                return SQLI_INVALID_ARGUMENT;
            char expected = field_separator(field, false);
            char actual = text[reader.position++];
            if (actual != expected && !(field == SQLI_FIELD_HOUR && !interval && actual == 'T'))
                return SQLI_INVALID_ARGUMENT;
        }
        size_t width = field == SQLI_FIELD_YEAR && !interval ? calendar_year_digits : calendar_field_digits;
        size_t maximum = width;
        if (field == SQLI_FIELD_FRACTION) {
            width = fields->range.fractional_digits;
            maximum = width;
        } else if (interval && field == (int)fields->range.first) {
            width = 1;
            maximum = uint64_decimal_digits;
        }
        uint64_t magnitude = 0;
        status = read_number(&reader, width, maximum, &magnitude);
        if (status != SQLI_OK)
            return status;
        if (field == SQLI_FIELD_FRACTION) {
            /* At most nine decimal digits were accepted above. */
            if (magnitude >= nanoseconds_per_second)
                return SQLI_OUT_OF_RANGE;
            fields->nanosecond = (uint32_t)magnitude * fraction_factors[fields->range.fractional_digits];
        } else {
            fields->integral[field] = magnitude;
        }
    }
    if (reader.position != length)
        return SQLI_INVALID_ARGUMENT;
    return validate_fields(fields, interval);
}

static sqli_datetime_parts_t datetime_parts(const struct temporal_fields *fields)
{
    /* Called only after calendar fields have been validated. */
    sqli_datetime_parts_t parts = {
        .range = fields->range,
        .year = (int32_t)fields->integral[SQLI_FIELD_YEAR],
        .month = (uint8_t)fields->integral[SQLI_FIELD_MONTH],
        .day = (uint8_t)fields->integral[SQLI_FIELD_DAY],
        .hour = (uint8_t)fields->integral[SQLI_FIELD_HOUR],
        .minute = (uint8_t)fields->integral[SQLI_FIELD_MINUTE],
        .second = (uint8_t)fields->integral[SQLI_FIELD_SECOND],
        .nanosecond = fields->nanosecond,
        .is_null = fields->is_null
    };
    return parts;
}

sqli_status sqli_datetime_parse(sqli_datetime_t *value, const sqli_temporal_range_t *range,
                                const char *text, size_t length, bool is_null)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (is_null && range == NULL)
        return sqli_datetime_set_null(value);
    sqli_status status = validate_range(range, false, is_null);
    if (status != SQLI_OK)
        return status;
    if (is_null) {
        value->parts = (sqli_datetime_parts_t){.range = *range, .is_null = true};
        return SQLI_OK;
    }
    struct temporal_fields fields = {.range = *range};
    status = parse_fields(&fields, text, length, false);
    if (status == SQLI_OK)
        value->parts = datetime_parts(&fields);
    return status;
}

sqli_status sqli_interval_parse(sqli_interval_t *value, const sqli_temporal_range_t *range,
                                const char *text, size_t length, bool is_null)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (is_null && range == NULL)
        return sqli_interval_set_null(value);
    sqli_status status = validate_range(range, true, is_null);
    if (status != SQLI_OK)
        return status;
    if (is_null) {
        value->parts = (sqli_interval_parts_t){.range = *range, .is_null = true};
        return SQLI_OK;
    }
    struct temporal_fields fields = {.range = *range};
    status = parse_fields(&fields, text, length, true);
    if (status != SQLI_OK)
        return status;
    sqli_interval_parts_t parts = {
        .range = fields.range,
        .years = fields.integral[SQLI_FIELD_YEAR], .months = fields.integral[SQLI_FIELD_MONTH],
        .days = fields.integral[SQLI_FIELD_DAY], .hours = fields.integral[SQLI_FIELD_HOUR],
        .minutes = fields.integral[SQLI_FIELD_MINUTE], .seconds = fields.integral[SQLI_FIELD_SECOND],
        .nanosecond = fields.nanosecond, .negative = fields.negative
    };
    return sqli_interval_set_parts(value, &parts);
}

sqli_status sqli_date_parse(sqli_date_t *value, const char *text, size_t length, bool is_null)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (is_null)
        return sqli_date_set_null(value);
    if (length != calendar_date_length)
        return SQLI_INVALID_ARGUMENT;
    struct temporal_fields fields = {.range = {SQLI_FIELD_YEAR, SQLI_FIELD_DAY, 0}};
    sqli_status status = parse_fields(&fields, text, length, false);
    if (status == SQLI_OK) {
        *value = (sqli_date_t){
            .year = (int32_t)fields.integral[SQLI_FIELD_YEAR],
            .month = (uint8_t)fields.integral[SQLI_FIELD_MONTH],
            .day = (uint8_t)fields.integral[SQLI_FIELD_DAY]
        };
    }
    return status;
}

sqli_status sqli_datetime_parse_iso(sqli_datetime_t *value, const char *text,
                                    size_t length, bool is_null)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (is_null)
        return sqli_datetime_set_null(value);
    if (text == NULL || length < timestamp_second_length)
        return SQLI_INVALID_ARGUMENT;
    if (length > SQLI_TEMPORAL_MAX_TEXT)
        return SQLI_LIMIT_EXCEEDED;
    if (text[calendar_date_length] != 'T')
        return SQLI_INVALID_ARGUMENT;
    sqli_temporal_range_t range = {SQLI_FIELD_YEAR, SQLI_FIELD_SECOND, 0};
    if (length > timestamp_second_length) {
        if (length <= timestamp_fraction_offset ||
            length - timestamp_fraction_offset > nanosecond_digits ||
            text[timestamp_second_length] != '.')
            return SQLI_INVALID_ARGUMENT;
        range.last = SQLI_FIELD_FRACTION;
        range.fractional_digits = (uint8_t)(length - timestamp_fraction_offset);
    }
    return sqli_datetime_parse(value, &range, text, length, false);
}

static sqli_status write_character(struct text_writer *writer, char ch)
{
    if (writer->used >= SQLI_TEMPORAL_MAX_TEXT)
        return SQLI_LIMIT_EXCEEDED;
    writer->bytes[writer->used++] = ch;
    return SQLI_OK;
}

static sqli_status write_number(struct text_writer *writer, uint64_t value, size_t width)
{
    char reversed[uint64_decimal_digits];
    size_t count = 0;
    do {
        reversed[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    size_t length = count > width ? count : width;
    if (length > SQLI_TEMPORAL_MAX_TEXT - writer->used)
        return SQLI_LIMIT_EXCEEDED;
    for (size_t i = count; i < width; i++)
        writer->bytes[writer->used++] = '0';
    while (count != 0)
        writer->bytes[writer->used++] = reversed[--count];
    return SQLI_OK;
}

static bool complete_timestamp(const sqli_temporal_range_t *range)
{
    return range->first == SQLI_FIELD_YEAR && range->last >= SQLI_FIELD_SECOND;
}

static sqli_status format_fields(const struct temporal_fields *fields, bool interval,
                                 struct text_writer *writer)
{
    if (fields->negative) {
        sqli_status status = write_character(writer, '-');
        if (status != SQLI_OK)
            return status;
    }
    for (int field = fields->range.first; field <= (int)fields->range.last; field++) {
        sqli_status status;
        if (field != (int)fields->range.first || field == SQLI_FIELD_FRACTION) {
            status = write_character(writer,
                field_separator(field, !interval && complete_timestamp(&fields->range)));
            if (status != SQLI_OK)
                return status;
        }
        uint64_t magnitude = fields->integral[field];
        size_t width = !interval && field == SQLI_FIELD_YEAR ? calendar_year_digits : calendar_field_digits;
        if (field == SQLI_FIELD_FRACTION) {
            width = fields->range.fractional_digits;
            magnitude = fields->nanosecond / fraction_factors[width];
        } else if (interval && field == (int)fields->range.first) {
            width = 1;
        }
        status = write_number(writer, magnitude, width);
        if (status != SQLI_OK)
            return status;
    }
    writer->bytes[writer->used] = '\0';
    return SQLI_OK;
}

static sqli_status format_value(const struct temporal_fields *fields, bool interval,
                                bool iso, char *buffer, size_t capacity,
                                size_t *required, bool *is_null)
{
    if (required == NULL || is_null == NULL || (buffer == NULL && capacity != 0))
        return SQLI_INVALID_ARGUMENT;
    sqli_status status = validate_fields(fields, interval);
    if (status != SQLI_OK)
        return status;
    if (fields->is_null) {
        *required = 0;
        *is_null = true;
        return SQLI_OK;
    }
    if (iso && !complete_timestamp(&fields->range))
        return SQLI_INVALID_STATE;
    struct text_writer writer = {0};
    status = format_fields(fields, interval, &writer);
    if (status != SQLI_OK)
        return status;
    *required = writer.used + 1;
    *is_null = false;
    if (buffer == NULL)
        return SQLI_OK;
    if (capacity < writer.used + 1)
        return SQLI_BUFFER_TOO_SMALL;
    memcpy(buffer, writer.bytes, writer.used + 1);
    return SQLI_OK;
}

sqli_status sqli_date_format(const sqli_date_t *value, char *buffer, size_t capacity,
                             size_t *required, bool *is_null)
{
    sqli_status status = sqli_date_validate(value);
    if (status != SQLI_OK)
        return status;
    struct temporal_fields fields = {
        .range = {SQLI_FIELD_YEAR, SQLI_FIELD_DAY, 0}, .is_null = value->is_null
    };
    if (!value->is_null) {
        fields.integral[SQLI_FIELD_YEAR] = (uint64_t)value->year;
        fields.integral[SQLI_FIELD_MONTH] = value->month;
        fields.integral[SQLI_FIELD_DAY] = value->day;
    }
    return format_value(&fields, false, false, buffer, capacity, required, is_null);
}

sqli_status sqli_datetime_format(const sqli_datetime_t *value, char *buffer, size_t capacity,
                                 size_t *required, bool *is_null)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct temporal_fields fields = datetime_fields(&value->parts);
    return format_value(&fields, false, false, buffer, capacity, required, is_null);
}

sqli_status sqli_datetime_format_iso(const sqli_datetime_t *value, char *buffer,
                                     size_t capacity, size_t *required, bool *is_null)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct temporal_fields fields = datetime_fields(&value->parts);
    return format_value(&fields, false, true, buffer, capacity, required, is_null);
}

sqli_status sqli_interval_format(const sqli_interval_t *value, char *buffer, size_t capacity,
                                 size_t *required, bool *is_null)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    struct temporal_fields fields = interval_fields(&value->parts);
    return format_value(&fields, true, false, buffer, capacity, required, is_null);
}
