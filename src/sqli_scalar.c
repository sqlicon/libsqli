#include "libsqli/sqli.h"
#include "libsqli/sqli_decimal.h"
#include "sqli_result_internal.h"

#include <float.h>
#include <math.h>
#include <string.h>

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "SQLI SMFLOAT requires binary32 float storage");
_Static_assert(sizeof(double) == 8 && DBL_MANT_DIG == 53,
               "SQLI FLOAT requires binary64 double storage");

static bool integer_type(sqli_column_type type)
{
    return type == SQLI_TYPE_SMALLINT || type == SQLI_TYPE_INT || type == SQLI_TYPE_SERIAL ||
        type == SQLI_TYPE_BIGINT || type == SQLI_TYPE_BIGSERIAL || type == SQLI_TYPE_INT8 ||
        type == SQLI_TYPE_SERIAL8 || type == SQLI_TYPE_BOOL || type == SQLI_TYPE_DBOOLEAN;
}

static bool decimal_type(sqli_column_type type)
{
    return type == SQLI_TYPE_DECIMAL || type == SQLI_TYPE_MONEY;
}

static sqli_status decode_boolean(const uint8_t *bytes, size_t length, bool *out)
{
    if (length != 1)
        return SQLI_PROTO_ERROR;
    switch (bytes[0]) {
    case 0: case '0': case 'f': case 'F': case 'n': case 'N':
        *out = false;
        return SQLI_OK;
    case 1: case 0xff: case '1': case 't': case 'T': case 'y': case 'Y':
        *out = true;
        return SQLI_OK;
    default:
        return SQLI_PROTO_ERROR;
    }
}

static uint64_t unsigned_be(const uint8_t *bytes, size_t length)
{
    uint64_t value = 0;
    for (size_t i = 0; i < length; i++)
        value = (value << 8) | bytes[i];
    return value;
}

static sqli_status decode_integer(sqli_column_type type, const uint8_t *bytes,
                                  size_t length, int64_t *out)
{
    if (type == SQLI_TYPE_BOOL || type == SQLI_TYPE_DBOOLEAN) {
        bool value;
        sqli_status status = decode_boolean(bytes, length, &value);
        if (status == SQLI_OK)
            *out = value ? 1 : 0;
        return status;
    }
    if (type == SQLI_TYPE_INT8 || type == SQLI_TYPE_SERIAL8) {
        enum { sign_width = 2, word_width = 4, int8_width = 10 };
        if (length != int8_width)
            return SQLI_PROTO_ERROR;
        uint64_t sign = unsigned_be(bytes, sign_width);
        if (sign != 1 && sign != UINT16_MAX)
            return SQLI_PROTO_ERROR;
        uint64_t magnitude = unsigned_be(bytes + sign_width, word_width) |
            (unsigned_be(bytes + sign_width + word_width, word_width) << 32);
        uint64_t limit = (uint64_t)INT64_MAX + (sign == UINT16_MAX ? 1u : 0u);
        if (magnitude > limit)
            return SQLI_OUT_OF_RANGE;
        if (sign == 1)
            *out = (int64_t)magnitude;
        else if (magnitude == (uint64_t)INT64_MAX + 1)
            *out = INT64_MIN;
        else
            *out = -(int64_t)magnitude;
        return SQLI_OK;
    }
    size_t width = type == SQLI_TYPE_SMALLINT ? 2 :
        type == SQLI_TYPE_BIGINT || type == SQLI_TYPE_BIGSERIAL ? 8 : 4;
    if (length != width)
        return SQLI_PROTO_ERROR;
    uint64_t value = unsigned_be(bytes, length);
    if (length < 8 && (bytes[0] & 0x80) != 0)
        value |= UINT64_MAX << (length * 8);
    *out = value <= INT64_MAX ? (int64_t)value : -1 - (int64_t)(UINT64_MAX - value);
    return SQLI_OK;
}

static sqli_status decimal_integer(sqli_result_t *result, size_t column, int64_t *out)
{
    sqli_decimal_t *value = NULL;
    sqli_status status = sqli_decimal_create(&value);
    if (status == SQLI_OK)
        status = sqli_result_get_decimal(result, column, value);
    if (status == SQLI_OK)
        status = sqli_decimal_to_i64(value, out);
    sqli_decimal_destroy(value);
    return status;
}

sqli_status sqli_result_get_int64(sqli_result_t *result, size_t column, int64_t *out, bool *is_null)
{
    if (out == NULL || is_null == NULL)
        return SQLI_INVALID_ARGUMENT;
    const sqli_column_info *info;
    const uint8_t *bytes;
    size_t length;
    sqli_status status = sqli_result_current_span(result, column, &info, &bytes, &length);
    if (status != SQLI_OK)
        return status;
    if (!integer_type(info->type) && !decimal_type(info->type))
        return SQLI_TYPE_MISMATCH;
    if (result->cur_col_is_null == NULL)
        return SQLI_INVALID_STATE;
    if (result->cur_col_is_null[column]) {
        *is_null = true;
        return SQLI_OK;
    }
    int64_t value;
    status = decimal_type(info->type) ? decimal_integer(result, column, &value) :
        decode_integer(info->type, bytes, length, &value);
    if (status == SQLI_OK) {
        *out = value;
        *is_null = false;
    }
    return status;
}

sqli_status sqli_result_get_int(sqli_result_t *result, size_t column, int32_t *out, bool *is_null)
{
    if (out == NULL || is_null == NULL)
        return SQLI_INVALID_ARGUMENT;
    int64_t value = 0;
    bool null_value;
    sqli_status status = sqli_result_get_int64(result, column, &value, &null_value);
    if (status != SQLI_OK)
        return status;
    if (!null_value && (value < INT32_MIN || value > INT32_MAX))
        return SQLI_OUT_OF_RANGE;
    if (!null_value)
        *out = (int32_t)value;
    *is_null = null_value;
    return SQLI_OK;
}

static sqli_status decimal_double(sqli_result_t *result, size_t column, double *out)
{
    sqli_decimal_t *value = NULL;
    sqli_decimal_parts_t parts;
    sqli_status status = sqli_decimal_create(&value);
    if (status == SQLI_OK)
        status = sqli_result_get_decimal(result, column, value);
    if (status == SQLI_OK)
        status = sqli_decimal_get_parts(value, &parts);
    if (status == SQLI_OK) {
        long double magnitude = 0;
        for (size_t i = parts.limb_count; i > 0; i--)
            magnitude = magnitude * SQLI_DECIMAL_LIMB_BASE + parts.limbs[i - 1];
        /* Native server decimals have bounded scale; no arbitrary application input. */
        for (int32_t scale = parts.scale; scale > 0; scale--)
            magnitude /= 10;
        for (int32_t scale = parts.scale; scale < 0; scale++)
            magnitude *= 10;
        double converted = (double)(parts.negative ? -magnitude : magnitude);
        if (!isfinite(converted))
            status = SQLI_OUT_OF_RANGE;
        else
            *out = converted;
    }
    sqli_decimal_destroy(value);
    return status;
}

sqli_status sqli_result_get_double(sqli_result_t *result, size_t column, double *out, bool *is_null)
{
    if (out == NULL || is_null == NULL)
        return SQLI_INVALID_ARGUMENT;
    const sqli_column_info *info;
    const uint8_t *bytes;
    size_t length;
    sqli_status status = sqli_result_current_span(result, column, &info, &bytes, &length);
    if (status != SQLI_OK)
        return status;
    if (!integer_type(info->type) && !decimal_type(info->type) &&
        info->type != SQLI_TYPE_FLOAT && info->type != SQLI_TYPE_SMFLOAT)
        return SQLI_TYPE_MISMATCH;
    if (result->cur_col_is_null == NULL)
        return SQLI_INVALID_STATE;
    if (result->cur_col_is_null[column]) {
        *is_null = true;
        return SQLI_OK;
    }
    double value = 0;
    if (integer_type(info->type)) {
        int64_t integer;
        status = decode_integer(info->type, bytes, length, &integer);
        if (status == SQLI_OK)
            value = (double)integer;
    } else if (decimal_type(info->type)) {
        status = decimal_double(result, column, &value);
    } else if (info->type == SQLI_TYPE_FLOAT && length == sizeof(double)) {
        uint64_t bits = unsigned_be(bytes, length);
        memcpy(&value, &bits, sizeof(value));
    } else if (info->type == SQLI_TYPE_SMFLOAT && length == sizeof(float)) {
        uint32_t bits = (uint32_t)unsigned_be(bytes, length);
        float single;
        memcpy(&single, &bits, sizeof(single));
        value = single;
    } else {
        return SQLI_PROTO_ERROR;
    }
    if (status != SQLI_OK)
        return status;
    if (!isfinite(value))
        return SQLI_OUT_OF_RANGE;
    *out = value;
    *is_null = false;
    return SQLI_OK;
}

sqli_status sqli_result_get_bool(sqli_result_t *result, size_t column, bool *out, bool *is_null)
{
    if (out == NULL || is_null == NULL)
        return SQLI_INVALID_ARGUMENT;
    const sqli_column_info *info;
    const uint8_t *bytes;
    size_t length;
    sqli_status status = sqli_result_current_span(result, column, &info, &bytes, &length);
    if (status != SQLI_OK)
        return status;
    if (info->type != SQLI_TYPE_BOOL && info->type != SQLI_TYPE_DBOOLEAN)
        return SQLI_TYPE_MISMATCH;
    if (result->cur_col_is_null == NULL)
        return SQLI_INVALID_STATE;
    if (result->cur_col_is_null[column]) {
        *is_null = true;
        return SQLI_OK;
    }
    bool value;
    status = decode_boolean(bytes, length, &value);
    if (status == SQLI_OK) {
        *out = value;
        *is_null = false;
    }
    return status;
}
