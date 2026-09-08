#ifndef SQLI_DECIMAL_H
#define SQLI_DECIMAL_H

/** @file sqli_decimal.h
 * @brief Exact decimal values and native decimal read/bind operations.
 *
 * @par Standalone value contract
 * The standalone decimal value operations below are reentrant. Distinct objects may be used
 * concurrently; a shared object requires external synchronization when any
 * thread modifies it. Read-only operations may run concurrently.
 *
 * All fallible operations leave existing values unchanged on failure.
 * Caller-owned outputs also remain unchanged except for format's documented
 * required-size and NULL outputs on SQLI_BUFFER_TOO_SMALL.
 * A null C object pointer is SQLI_INVALID_ARGUMENT, never SQL NULL.
 * destroy(NULL) is allowed. No function uses a connection or server conversion.
 */

#include "sqli.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @name Exact decimal values (DECIMAL / NUMERIC / MONEY)
 * @{ */

/** Resource ceilings, independent of server precision/scale limits. */
enum {
    SQLI_DECIMAL_LIMB_BASE = 1000000000, /**< Radix of each coefficient limb (10^9). */
    SQLI_DECIMAL_MAX_DIGITS = 1000000, /**< Maximum application coefficient length in decimal digits. */
    SQLI_DECIMAL_MAX_TEXT = SQLI_DECIMAL_MAX_DIGITS + 32 /**< Maximum accepted decimal text length in bytes. */
};

/** @brief Owned opaque exact decimal value with scale and SQL NULL state. */
typedef struct sqli_decimal sqli_decimal_t;

/** Borrowed coefficient, least significant base-10^9 limb first.
 * value = sign * coefficient * 10^(-scale). Each limb must be below
 * SQLI_DECIMAL_LIMB_BASE. Zero may have no limbs; its sign is nonnegative
 * and its scale is retained. Numeric fields are ignored for SQL NULL.
 */
typedef struct {
    const uint32_t *limbs; /**< Borrowed base-10^9 coefficient limbs, least significant first. */
    size_t limb_count; /**< Number of coefficient limbs. */
    int32_t scale; /**< Decimal scale. */
    bool negative; /**< True when the nonzero value is negative. */
    bool is_null; /**< True denotes SQL NULL. */
} sqli_decimal_parts_t;


/** Allocate an owned SQL-NULL value. On failure, out is unchanged. */
sqli_status sqli_decimal_create(sqli_decimal_t **out);
/** @brief Free an owned decimal value; NULL is allowed. */
void sqli_decimal_destroy(sqli_decimal_t *value);
/** @brief Copy a decimal including scale and NULL state; failure preserves destination. */
sqli_status sqli_decimal_copy(sqli_decimal_t *destination,
                              const sqli_decimal_t *source);
/** @brief Inspect SQL NULL state without modifying the value; failure preserves out. */
sqli_status sqli_decimal_is_null(const sqli_decimal_t *value, bool *out);
/** @brief Replace the value with SQL NULL. */
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
/** @brief Set a signed coefficient and decimal scale, preserving the value on failure.
 * The numeric value is coefficient multiplied by 10 to the power of -scale. */
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

/** Explicit source SQL decimal type. Floating scale ignores scale. */
typedef struct {
    uint8_t precision; /**< Source decimal precision, 1..32. */
    uint8_t scale; /**< Decimal scale. */
    bool floating_scale; /**< True selects floating scale and ignores scale. */
} sqli_decimal_target_t;

/** Bind a copied native DECIMAL/NUMERIC/MONEY value using explicit source
 * precision (1..32) and fixed (0..precision) or floating scale. No text conversion or
 * allocation. NULL is taken from the object; a NULL C pointer is invalid.
 * Exact representability is required: SQLI_INEXACT or SQLI_OUT_OF_RANGE leaves
 * the preceding binding unchanged. Source ownership may end after binding;
 * batch snapshots retain copies. Parameter indices are 0-based. Synchronize
 * statement and source mutation externally. The server converts to the SQL
 * expression's target type; no PREPARE ordinal inference is performed.
 */
sqli_status sqli_bind_decimal(sqli_stmt_t *stmt, size_t param_index, const sqli_decimal_t *value,
                              const sqli_decimal_target_t *target);

/**
 * Bind textual DECIMAL/NUMERIC representation (e.g. "123.45").
 */
sqli_status sqli_bind_decimal_string(sqli_stmt_t *stmt, size_t param_index, const char *value);

/** DECIMAL/MONEY precision and fixed scale. Floating scale is unavailable. */
sqli_status sqli_descriptor_field_get_precision(const sqli_descriptor_field_t *field, uint8_t *out);
/** @brief Get fixed DECIMAL/MONEY scale; floating scale returns SQLI_METADATA_UNAVAILABLE.
 * Failure preserves out; the caller must hold the owning descriptor reference. */
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

/**
 * Return thread-local decimal text; SQL NULL and failures return an empty
 * string. Successful reads update the legacy was_null flag. Use the native
 * getter and formatter for explicit status and caller-owned buffers.
 */
const char *sqli_result_get_decimal_string(sqli_result_t *result, size_t col_index);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* SQLI_DECIMAL_H */
