#ifndef SQLI_TEMPORAL_H
#define SQLI_TEMPORAL_H

/** @file sqli_temporal.h
 * @brief Calendar dates, qualified datetime/interval values and conversions.
 *
 * @par Standalone value contract
 * The standalone native value operations below are reentrant, with no allocation after creation of
 * opaque objects. Read-only access may run concurrently; mutation of a shared
 * object requires external synchronization. DATE values need no allocation.
 * Getters export copies, never borrowed field pointers. Failed operations leave
 * values and output arguments unchanged, except format's short-buffer metadata.
 * No implicit field completion, timezone conversion, rounding or normalization
 * across units is performed. Leap seconds and hour 24 are not represented.
 * Native fractional precision up to 9 is not a claim of server support.
 */

#include "sqli.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name Native calendar and interval values
 * @{ */

enum {
    SQLI_TEMPORAL_MAX_TEXT = 64 /**< Maximum accepted qualified-text length in bytes. */
};

/** Semantic fields, independent of wire qualifier codes. UNKNOWN is allowed
 * only as a completely unknown range on a SQL-NULL object. */
typedef enum {
    SQLI_FIELD_UNKNOWN = 0, /**< Unknown range, allowed only for SQL NULL. */
    SQLI_FIELD_YEAR, /**< Year field. */
    SQLI_FIELD_MONTH, /**< Month field. */
    SQLI_FIELD_DAY, /**< Day field. */
    SQLI_FIELD_HOUR, /**< Hour field. */
    SQLI_FIELD_MINUTE, /**< Minute field. */
    SQLI_FIELD_SECOND, /**< Second field. */
    SQLI_FIELD_FRACTION /**< Fractional second field. */
} sqli_temporal_field_t;

/** @brief Inclusive semantic field range and explicit fractional precision. */
typedef struct {
    sqli_temporal_field_t first; /**< First included semantic field. */
    sqli_temporal_field_t last; /**< Last included semantic field. */
    uint8_t fractional_digits; /**< 1..9 with FRACTION, otherwise 0 */
} sqli_temporal_range_t;

/** Proleptic Gregorian date, years 1..9999; no epoch or timezone. */
typedef struct {
    int32_t year; /**< Gregorian year; zero when absent. */
    uint8_t month; /**< Calendar month; zero when absent. */
    uint8_t day; /**< Calendar day; zero when absent. */
    bool is_null; /**< True denotes SQL NULL. */
} sqli_date_t;

/** @brief Owned opaque qualified calendar/time value. */
typedef struct sqli_datetime sqli_datetime_t;
/** @brief Owned opaque signed interval value. */
typedef struct sqli_interval sqli_interval_t;

/** Absent numeric fields must be zero for non-NULL values. A partial date is
 * validated using only known fields; MONTH TO DAY permits February 29 without
 * inventing a year. Numeric fields are ignored and cleared on NULL import.
 */
typedef struct {
    sqli_temporal_range_t range; /**< Inclusive field range and fractional precision. */
    int32_t year; /**< Gregorian year; zero when absent. */
    uint8_t month; /**< Calendar month; zero when absent. */
    uint8_t day; /**< Calendar day; zero when absent. */
    uint8_t hour; /**< Hour field; zero when absent. */
    uint8_t minute; /**< Minute field; zero when absent. */
    uint8_t second; /**< Second field; zero when absent. */
    uint32_t nanosecond; /**< Fractional second magnitude in nanoseconds. */
    bool is_null; /**< True denotes SQL NULL. */
} sqli_datetime_parts_t;

/** One sign covers the entire interval. The first integral field may span
 * uint64_t; subordinate month/hour/minute/second magnitudes are bounded by
 * their usual radices. YEAR/MONTH cannot be mixed with DAY/time fields.
 * Leading precision belongs to the target SQL type, not this value.
 */
typedef struct {
    sqli_temporal_range_t range; /**< Inclusive field range and fractional precision. */
    uint64_t years; /**< Year magnitude. */
    uint64_t months; /**< Month magnitude. */
    uint64_t days; /**< Day magnitude. */
    uint64_t hours; /**< Hour magnitude. */
    uint64_t minutes; /**< Minute magnitude. */
    uint64_t seconds; /**< Second magnitude. */
    uint32_t nanosecond; /**< Fractional second magnitude in nanoseconds. */
    bool negative; /**< True when the nonzero value is negative. */
    bool is_null; /**< True denotes SQL NULL. */
} sqli_interval_parts_t;


/** Validate a calendar DATE or SQL NULL without modifying it. */
sqli_status sqli_date_validate(const sqli_date_t *value);
/** @brief Set and validate a non-NULL Gregorian date; failure preserves value. */
sqli_status sqli_date_set(sqli_date_t *value, int32_t year, int32_t month, int32_t day);
/** @brief Set SQL NULL and clear the calendar fields. */
sqli_status sqli_date_set_null(sqli_date_t *value);
/** Exact YYYY-MM-DD input/output. Explicit NULL input ignores text/length.
 * Formatting uses the shared temporal buffer contract documented below. */
sqli_status sqli_date_parse(sqli_date_t *value, const char *text, size_t length, bool is_null);
/** @brief Format an exact ISO calendar date.
 * @see sqli_datetime_format for capacity, required-size and NULL rules. */
sqli_status sqli_date_format(const sqli_date_t *value, char *buffer, size_t capacity,
                             size_t *required, bool *is_null);

/** Create starts as SQL NULL with unknown range. destroy(NULL) is allowed.
 * set_null retains any known range and clears numeric fields. */
sqli_status sqli_datetime_create(sqli_datetime_t **out);
/** @brief Free an owned value; NULL is allowed. */
void sqli_datetime_destroy(sqli_datetime_t *value);
/** @brief Copy fields, range and NULL state; failure preserves destination. */
sqli_status sqli_datetime_copy(sqli_datetime_t *destination, const sqli_datetime_t *source);
/** @brief Inspect SQL NULL state; failure preserves out. */
sqli_status sqli_datetime_is_null(const sqli_datetime_t *value, bool *out);
/** @brief Set SQL NULL, retaining any known range and clearing numeric fields. */
sqli_status sqli_datetime_set_null(sqli_datetime_t *value);
/** @brief Validate and copy semantic fields; failure preserves the value. */
sqli_status sqli_datetime_set_parts(sqli_datetime_t *value, const sqli_datetime_parts_t *parts);
/** @brief Export copied semantic fields; failure preserves out. */
sqli_status sqli_datetime_get_parts(const sqli_datetime_t *value, sqli_datetime_parts_t *out);

/** @brief Allocate an owned SQL-NULL value with unknown range; failure preserves out. */
sqli_status sqli_interval_create(sqli_interval_t **out);
/** @brief Free an owned value; NULL is allowed. */
void sqli_interval_destroy(sqli_interval_t *value);
/** @brief Copy fields, range and NULL state; failure preserves destination. */
sqli_status sqli_interval_copy(sqli_interval_t *destination, const sqli_interval_t *source);
/** @brief Inspect SQL NULL state; failure preserves out. */
sqli_status sqli_interval_is_null(const sqli_interval_t *value, bool *out);
/** @brief Set SQL NULL, retaining any known range and clearing numeric fields. */
sqli_status sqli_interval_set_null(sqli_interval_t *value);
/** @brief Validate and copy semantic fields; failure preserves the value. */
sqli_status sqli_interval_set_parts(sqli_interval_t *value, const sqli_interval_parts_t *parts);
/** @brief Export copied semantic fields; failure preserves out. */
sqli_status sqli_interval_get_parts(const sqli_interval_t *value, sqli_interval_parts_t *out);

/** Qualified text uses an explicit range. Calendar years have four digits,
 * other calendar/subordinate integral fields two. Leading interval fields use
 * 1..20 digits and an optional overall sign. Separators are '-', space, ':'
 * and '.'. DATETIME also accepts 'T' between DAY and HOUR. Fractions contain
 * exactly fractional_digits digits; fraction-only text accepts .digits or
 * 0.digits. No surrounding whitespace, zone suffix or embedded NUL is accepted.
 * On explicit NULL input text/length are ignored. A supplied range is validated
 * and retained; a NULL range instead retains the object's existing range.
 * Non-NULL input requires a known range. Text length is capped above.
 */
sqli_status sqli_datetime_parse(sqli_datetime_t *value, const sqli_temporal_range_t *range,
                                const char *text, size_t length, bool is_null);
/** @brief Parse qualified interval text with an explicit range.
 * @copydetails sqli_datetime_parse */
sqli_status sqli_interval_parse(sqli_interval_t *value, const sqli_temporal_range_t *range,
                                const char *text, size_t length, bool is_null);
/** Strict YYYY-MM-DDThh:mm:ss[.fraction] input, inferring 1..9 fractional
 * digits. No offset, timezone or implicit completion. NULL retains prior range.
 */
sqli_status sqli_datetime_parse_iso(sqli_datetime_t *value, const char *text,
                                    size_t length, bool is_null);

/** Formatting buffer contract: required includes NUL;
 * buffer=NULL/capacity=0 queries size; short buffers are unchanged and report
 * required/is_null. NULL succeeds with required=0 and leaves buffer unchanged.
 * All output pointers are required except buffer for size queries. Outputs
 * must not overlap each other or the source object.
 * Complete calendar/time DATETIME values use 'T'; partial values use qualified
 * text. Interval leading fields have no leading padding, and negative zero is
 * normalized on import. No fractional digits are stripped.
 */
sqli_status sqli_datetime_format(const sqli_datetime_t *value, char *buffer, size_t capacity,
                                 size_t *required, bool *is_null);
/** @brief Format a qualified interval without rounding.
 * @copydetails sqli_datetime_format */
sqli_status sqli_interval_format(const sqli_interval_t *value, char *buffer, size_t capacity,
                                 size_t *required, bool *is_null);
/** Non-NULL values must start at YEAR and end at SECOND or FRACTION;
 * otherwise SQLI_INVALID_STATE is returned without changing outputs. */
sqli_status sqli_datetime_format_iso(const sqli_datetime_t *value, char *buffer,
                                     size_t capacity, size_t *required, bool *is_null);

/** @brief Legacy completed calendar/time convenience, with microsecond precision. */
typedef struct {
    bool is_null; /**< True denotes SQL NULL. */
    int year; /**< Gregorian year; zero when absent. */
    int month; /**< Calendar month; zero when absent. */
    int day; /**< Calendar day; zero when absent. */
    int hour; /**< Hour field; zero when absent. */
    int minute; /**< Minute field; zero when absent. */
    int second; /**< Second field; zero when absent. */
    int microsecond; /**< Fractional second in microseconds. */
} sqli_timestamp_t;

/**
 * Convert a legacy calendar timestamp to Unix seconds without timezone adjustment.
 * NULL pointer or SQL NULL returns zero. Input must be valid and representable;
 * no conversion status is available. Prefer native values for checked validation.
 */
int64_t sqli_timestamp_to_epoch_sec(const sqli_timestamp_t *ts);
/** @brief Convert calendar fields to Unix milliseconds without timezone adjustment.
 * NULL pointer or SQL NULL returns zero. Input must be valid and representable;
 * this legacy convenience does not report conversion failures. */
int64_t sqli_timestamp_to_epoch_ms(const sqli_timestamp_t *ts);
/** @brief Convert calendar fields to Unix days without timezone adjustment.
 * NULL pointer or SQL NULL returns zero. Input must be valid and representable;
 * this legacy convenience does not report conversion failures. */
int32_t sqli_timestamp_to_epoch_days(const sqli_timestamp_t *ts);

/** @brief Fill a timestamp from Unix seconds; NULL destination is a no-op.
 * Input must be representable in the calendar conversion; no status is returned. */
void sqli_timestamp_from_epoch_sec(sqli_timestamp_t *ts, int64_t sec);
/** @brief Fill a timestamp from Unix milliseconds; NULL destination is a no-op.
 * Input must be representable in the calendar conversion; no status is returned. */
void sqli_timestamp_from_epoch_ms(sqli_timestamp_t *ts, int64_t ms);
/** @brief Fill a timestamp from Unix days; NULL destination is a no-op.
 * Input must be representable in the calendar conversion; no status is returned. */
void sqli_timestamp_from_epoch_days(sqli_timestamp_t *ts, int32_t days);

/** Bind a copied native value to a 0-based parameter, without text conversion.
 * target is an explicit source SQL range sent to the server, not inferred from
 * PREPARE fields. It must match the value's integral range; exact fractional
 * rescaling is allowed. Wire fractions support 1..5 digits. INTERVAL leading
 * precision is 1..9 for an integral first field, or 0 for FRACTION-only values.
 * The server converts this declared source type to the SQL expression's target.
 * SQL NULL is taken from the object; target is still required and validated.
 * NULL C pointers are invalid arguments. Failed binds preserve the preceding
 * parameter. No allocation occurs; the object can be changed/destroyed after
 * binding. The copy is also retained by batch snapshots. Synchronize statement
 * mutation externally. Unsupported precision returns SQLI_OUT_OF_RANGE;
 * discarded nonzero digits return SQLI_INEXACT.
 */
sqli_status sqli_bind_datetime(sqli_stmt_t *stmt, size_t param_index, const sqli_datetime_t *value,
                               const sqli_temporal_range_t *target);
/** @brief Bind a copied native interval with an explicit range and leading precision.
 * @copydetails sqli_bind_datetime */
sqli_status sqli_bind_interval(sqli_stmt_t *stmt, size_t param_index, const sqli_interval_t *value,
                               const sqli_temporal_range_t *target, uint8_t leading_precision);

/** Bind a copied native calendar DATE, with no allocation or text conversion.
 * NULL is taken from value->is_null; a NULL pointer is invalid. Invalid values
 * leave the previous binding unchanged. The copy survives source mutation and
 * batch capture. Parameter indices are 0-based; synchronize statement mutation.
 */
sqli_status sqli_bind_date(sqli_stmt_t *stmt, size_t param_index, const sqli_date_t *value);

/**
 * Bind a DATE value in ISO format (YYYY-MM-DD).
 */
sqli_status sqli_bind_date_string(sqli_stmt_t *stmt, size_t param_index, const char *value);

/**
 * Bind a DATETIME/TIMESTAMP-like value as text.
 */
sqli_status sqli_bind_datetime_string(sqli_stmt_t *stmt, size_t param_index, const char *value);

/**
 * Bind a standard portable timestamp struct.
 */
sqli_status sqli_bind_timestamp(sqli_stmt_t *stmt, size_t param_index, const sqli_timestamp_t *value);

/**
 * Direct Unix epoch binding helpers (format as YYYY-MM-DD HH:MM:SS.ffffff or YYYY-MM-DD).
 */
sqli_status sqli_bind_epoch_sec(sqli_stmt_t *stmt, size_t param_index, int64_t sec);
/** @brief Bind a Unix millisecond value through timestamp text conversion.
 * Parameter indices are zero-based; no timezone conversion is performed. */
sqli_status sqli_bind_epoch_ms(sqli_stmt_t *stmt, size_t param_index, int64_t ms);
/** @brief Bind a Unix day count through timestamp text conversion.
 * Parameter indices are zero-based; no timezone conversion is performed. */
sqli_status sqli_bind_epoch_days(sqli_stmt_t *stmt, size_t param_index, int32_t days);

/**
 * Bind an INTERVAL value as text.
 */
sqli_status sqli_bind_interval_string(sqli_stmt_t *stmt, size_t param_index, const char *value);

/** DATETIME/INTERVAL field range, including fractional precision. */
sqli_status sqli_descriptor_field_get_temporal_range(const sqli_descriptor_field_t *field,
                                                     sqli_temporal_range_t *out);

/** Read a DATE column into a calendar value (years 1..9999).
 * Zero-based index; requires a successfully fetched, validated row. All
 * failures preserve out. SQL NULL succeeds with out->is_null=true. Other source types, even
 * NULL, return SQLI_TYPE_MISMATCH. Invalid dates/descriptors return
 * SQLI_PROTO_ERROR. No allocation, epoch exposure or implicit conversion.
 * The legacy was_null flag is unchanged; use out->is_null directly.
 */
sqli_status sqli_result_get_date(sqli_result_t *result, size_t index, sqli_date_t *out);

/**
 * Extract DATE/DATETIME/INTERVAL textual representations.
 * Returns thread-local SQL-style text; SQL NULL and failures return an empty
 * string. Successful reads update the legacy was_null flag. Use native getters
 * and formatters for explicit status and caller-owned buffers.
 */
const char *sqli_result_get_date_string(sqli_result_t *result, size_t col_index);
/** @brief Return borrowed thread-local datetime text.
 * @copydetails sqli_result_get_date_string */
const char *sqli_result_get_datetime_string(sqli_result_t *result, size_t col_index);
/** @brief Return borrowed thread-local interval text.
 * @copydetails sqli_result_get_date_string */
const char *sqli_result_get_interval_string(sqli_result_t *result, size_t col_index);

/** Read an exact native temporal value into an existing owned object.
 * Zero-based index; requires a successfully fetched and validated row.
 * SQL NULL returns SQLI_OK and sets the value's NULL state and field range.
 * Wrong column type returns SQLI_TYPE_MISMATCH, including for SQL NULL.
 * Malformed payloads/qualifiers return SQLI_PROTO_ERROR. All failures preserve
 * the destination. No allocation or text conversion occurs. The value survives
 * result destruction; the legacy was_null flag is unchanged. Synchronize result
 * and destination access externally. Destination must not alias result storage.
 */
sqli_status sqli_result_get_datetime(sqli_result_t *result, size_t index, sqli_datetime_t *out);
/** @brief Read an exact native interval into an existing owned object.
 * @copydetails sqli_result_get_datetime */
sqli_status sqli_result_get_interval(sqli_result_t *result, size_t index, sqli_interval_t *out);

/** @brief Read a legacy timestamp convenience from DATE, DATETIME or text.
 * Missing DATETIME fields are completed with 1970-01-01 and zero time fields.
 * Fractions are truncated to microseconds. SQL NULL sets out->is_null.
 * Failure can change out; this is not the native getter atomicity contract.
 * Uses legacy NULL state and permissive text parsing; prefer native getters. */
sqli_status sqli_result_get_timestamp(sqli_result_t *result, size_t col_index,
                                      sqli_timestamp_t *out);

/**
 * Legacy Unix-second retrieval through sqli_result_get_timestamp().
 * A non-NULL output is initialized to zero before reading; SQL NULL succeeds
 * with zero and failures can leave zero. Use native getters for explicit NULL
 * state and unchanged outputs on failure. The timestamp convenience fills absent
 * fields and does not perform a timezone conversion.
 */
sqli_status sqli_result_get_epoch_sec(sqli_result_t *result, size_t col_index, int64_t *out_sec);
/** @brief Read Unix milliseconds through the legacy timestamp convenience.
 * @copydetails sqli_result_get_epoch_sec */
sqli_status sqli_result_get_epoch_ms(sqli_result_t *result, size_t col_index, int64_t *out_ms);
/** @brief Read Unix days through the legacy timestamp convenience.
 * @copydetails sqli_result_get_epoch_sec */
sqli_status sqli_result_get_epoch_days(sqli_result_t *result, size_t col_index, int32_t *out_days);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* SQLI_TEMPORAL_H */
