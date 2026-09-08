#ifndef SQLI_DECIMAL_H
#define SQLI_DECIMAL_H

#include "sqli.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 * Exact decimal values (DECIMAL / NUMERIC / MONEY)
 * ---------------------------------------------------------------- */

/** Resource ceilings, independent of server precision/scale limits. */
enum {
    SQLI_DECIMAL_LIMB_BASE = 1000000000,
    SQLI_DECIMAL_MAX_DIGITS = 1000000,
    SQLI_DECIMAL_MAX_TEXT = SQLI_DECIMAL_MAX_DIGITS + 32
};

typedef struct sqli_decimal sqli_decimal_t;

/** Borrowed coefficient, least significant base-10^9 limb first.
 * value = sign * coefficient * 10^(-scale). Each limb must be below
 * SQLI_DECIMAL_LIMB_BASE. Zero may have no limbs; its sign is nonnegative
 * and its scale is retained. Numeric fields are ignored for SQL NULL.
 */
typedef struct {
    const uint32_t *limbs;
    size_t limb_count;
    int32_t scale;
    bool negative;
    bool is_null;
} sqli_decimal_parts_t;

/** The standalone decimal value operations below are reentrant. Distinct objects may be used
 * concurrently; a shared object requires external synchronization when any
 * thread modifies it. Read-only operations may run concurrently.
 *
 * All fallible operations leave existing values unchanged on failure.
 * Caller-owned outputs also remain unchanged except for format's documented
 * required-size and NULL outputs on SQLI_BUFFER_TOO_SMALL.
 * A null C object pointer is SQLI_INVALID_ARGUMENT, never SQL NULL.
 * destroy(NULL) is allowed. No function uses a connection or server conversion.
 */
/** Allocate an owned SQL-NULL value. On failure, out is unchanged. */
sqli_status sqli_decimal_create(sqli_decimal_t **out);
void sqli_decimal_destroy(sqli_decimal_t *value);
sqli_status sqli_decimal_copy(sqli_decimal_t *destination,
                              const sqli_decimal_t *source);
sqli_status sqli_decimal_is_null(const sqli_decimal_t *value, bool *out);
sqli_status sqli_decimal_set_null(sqli_decimal_t *value);

/** Import validates and copies. Leading zero limbs are removed; decimal
 * trailing zeros and scale are retained. Aliasing the destination's borrowed
 * coefficient is allowed. NULL input ignores the numeric parts.
 */
sqli_status sqli_decimal_set_parts(sqli_decimal_t *value,
                                   const sqli_decimal_parts_t *parts);
/** Returned limbs borrow from value until its next successful mutation or
 * destruction. The view must not outlive the owner or be modified by the caller.
 */
sqli_status sqli_decimal_get_parts(const sqli_decimal_t *value,
                                   sqli_decimal_parts_t *out);
sqli_status sqli_decimal_set_i64(sqli_decimal_t *value, int64_t coefficient,
                                 int32_t scale);
/** Convert the numeric value exactly, not merely its coefficient.
 * Fractional values return SQLI_INEXACT; integral overflow is SQLI_OUT_OF_RANGE.
 * SQL NULL returns SQLI_NULL_VALUE and leaves out unchanged.
 */
sqli_status sqli_decimal_to_i64(const sqli_decimal_t *value, int64_t *out);
/** Numeric ordering (-1, 0, +1), ignoring differences in scale.
 * Either operand being SQL NULL returns SQLI_NULL_VALUE, leaving out unchanged.
 */
sqli_status sqli_decimal_compare(const sqli_decimal_t *left,
                                 const sqli_decimal_t *right, int *out);
/** Preserve the numeric value while changing scale, without rounding.
 * SQLI_INEXACT means discarded digits would be nonzero. SQL NULL returns
 * SQLI_NULL_VALUE. Excessive coefficient growth returns SQLI_LIMIT_EXCEEDED.
 */
sqli_status sqli_decimal_rescale_exact(sqli_decimal_t *value, int32_t scale);

/** Parse a length-delimited ASCII decimal, optionally signed, with optional
 * decimal point and E/e exponent. At least one mantissa digit is required.
 * No whitespace, grouping, embedded NUL, NaN or infinity is accepted.
 * Scale equals fractional digit count minus exponent, within int32_t range.
 * Input length and coefficient digits (excluding leading zeros) obey the
 * resource ceilings above.
 * Explicit is_null=true ignores text/length and sets SQL NULL.
 */
sqli_status sqli_decimal_parse(sqli_decimal_t *value, const char *text,
                               size_t length, bool is_null);
/** Canonical scale-preserving text. Use plain notation when scale >= 0 and
 * adjusted exponent >= -6; otherwise scientific notation with uppercase E.
 * required includes the terminating NUL for non-NULL values. SQL NULL succeeds
 * with is_null=true and required=0, leaving the buffer untouched.
 * buffer=NULL, capacity=0 queries size successfully. A short buffer returns
 * SQLI_BUFFER_TOO_SMALL, updates required/is_null and leaves buffer unchanged.
 * required and is_null are mandatory; buffer=NULL with capacity>0 is invalid.
 * Caller-provided output objects/buffers must not overlap one another or value.
 */
sqli_status sqli_decimal_format(const sqli_decimal_t *value, char *buffer,
                                size_t capacity, size_t *required, bool *is_null);

/*
 * Bind textual DECIMAL/NUMERIC representation (e.g. "123.45").
 */
sqli_status sqli_bind_decimal(sqli_stmt_t *stmt, int param_index, const char *value);

/** DECIMAL/MONEY precision and fixed scale. Floating scale is unavailable. */
sqli_status sqli_descriptor_field_get_precision(const sqli_descriptor_field_t *field, uint8_t *out);
sqli_status sqli_descriptor_field_get_scale(const sqli_descriptor_field_t *field, uint8_t *out);
/** Read a DECIMAL/NUMERIC/MONEY column directly into an existing native object.
 * The index is zero-based. No implicit conversion from other column types:
 * SQLI_TYPE_MISMATCH also applies to a NULL column of the wrong type.
 * Requires a successfully positioned, validated row. SQL NULL returns SQLI_OK
 * and sets the object's NULL state. Malformed bytes/descriptors return
 * SQLI_PROTO_ERROR. Every failure leaves the destination unchanged.
 * No allocation or text conversion is performed. The owned value survives row
 * advancement and result destruction. The legacy was_null flag is unchanged;
 * use sqli_decimal_is_null on the destination. Synchronize access to the result
 * and destination externally; destination must not overlap result storage.
 */
sqli_status sqli_result_get_decimal(sqli_result_t *result, size_t index, sqli_decimal_t *out);

/*
 * Extract DECIMAL/NUMERIC/MONEY as textual representation.
 */
const char *sqli_result_get_decimal_string(sqli_result_t *result, int col_index);

/*
 * Encode a DECIMAL value into a buffer using BCD encoding.
 *
 * precision: total number of digits (1-15)
 * scale: number of digits after decimal point (0 <= scale <= precision)
 * negative: non-zero for negative numbers
 * digits: array of 'precision' decimal digits (0-9)
 *
 * Wire format (spec §7.4): [2-byte length][exponent byte][BCD digit bytes]
 * exponent byte = ((exp+64) & 0x7F) | (positive ? 0x80 : 0x00)
 * where exp = (precision - scale) - 1.
 * Negative values are 10's-complemented in the BCD digit bytes.
 *
 * Returns bytes written, 0 on error (buffer too small).
 */
size_t sqli_encode_decimal(uint8_t *buf, size_t buf_size,
                           uint8_t precision, uint8_t scale,
                           int negative, const uint8_t *digits);

#ifdef __cplusplus
}
#endif

#endif /* SQLI_DECIMAL_H */
