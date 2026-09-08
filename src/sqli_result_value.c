#define _GNU_SOURCE
#include "libsqli/sqli_temporal.h"
#include "libsqli/sqli_decimal.h"
#include "sqli_temporal_internal.h"
#include "sqli_decimal_codec.h"
#include "sqli_internal.h"
#include "sqli_charset.h"
#include "sqli_protocol_internal.h"
#include "sqli_result_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

sqli_status extract_value_from_tuple(const sqli_column_info *col_info,
                                     const uint8_t *tuple_buf, size_t tuple_len,
                                     uint8_t *out, size_t *out_len)
{
    if (col_info == NULL || tuple_buf == NULL || out == NULL || out_len == NULL)
        return SQLI_INVALID_STATE;

    size_t data_start = 0;
    size_t data_len = 0;
    size_t span = 0;
    sqli_status rc = sqli_tuple_locate_column(col_info, tuple_buf, tuple_len,
                                              &data_start, &data_len, &span);
    if (rc != SQLI_OK)
        return rc;

    size_t copy_len = *out_len < data_len ? *out_len : data_len;
    memcpy(out, tuple_buf + data_start, copy_len);
    *out_len = copy_len;
    return SQLI_OK;
}

static sqli_status sqli_extract_current_value(sqli_result_t *result, size_t col_index,
                                              uint8_t *out, size_t *out_len)
{
    if (result == NULL || out == NULL || out_len == NULL ||
        col_index >= (size_t)result->column_count ||
        result->tuple_buffer == NULL)
        return SQLI_INVALID_STATE;

    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL)
        return SQLI_PROTO_ERROR;

    size_t start = result->cur_col_data_start[col_index];
    size_t len = result->cur_col_data_len[col_index];
    if (start > result->tuple_len || len > result->tuple_len - start)
        return SQLI_PROTO_ERROR;

    size_t copy_len = *out_len < len ? *out_len : len;
    memcpy(out, result->tuple_buffer + start, copy_len);
    *out_len = copy_len;
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * Base-100 and typed extractors
 * ---------------------------------------------------------------- */

static int sqli_base100_decode_parts(const uint8_t *raw, size_t len,
                                     uint8_t *digits, size_t *ndgts,
                                     int *frac_digits, int *negative)
{
    if (raw == NULL || len < 2 || digits == NULL || ndgts == NULL ||
        frac_digits == NULL || negative == NULL)
        return 0;
    int expon = (int8_t)raw[0];
    *ndgts = len - 1;
    memcpy(digits, raw + 1, *ndgts);
    *negative = 0;
    if ((expon & 0x80) == 0) {
        sqli_base100_complement(digits, *ndgts);
        expon ^= 0x7F;
        *negative = 1;
    }
    expon = (expon & 0x7F) - 64;
    *frac_digits = (int)(*ndgts * 2) - (expon * 2);
    return 1;
}

static bool sqli_decimal_to_int64(const uint8_t *raw, size_t len, int64_t *out)
{
    if (raw == NULL || len < 1 || out == NULL)
        return false;

    int expon = (int8_t)raw[0];
    uint8_t digits[64];
    size_t ndgts = len - 1;
    if (ndgts > sizeof(digits))
        ndgts = sizeof(digits);
    if (ndgts > 0)
        memcpy(digits, raw + 1, ndgts);

    int negative = 0;
    if ((expon & 0x80) == 0) {
        sqli_base100_complement(digits, ndgts);
        expon ^= 0x7F;
        negative = 1;
    }
    expon = (expon & 0x7F) - 64;

    bool all_zero = true;
    for (size_t i = 0; i < ndgts; i++) {
        if (digits[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero || expon <= 0) {
        *out = 0;
        return true;
    }

    uint64_t v = 0;
    const uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    for (int i = 0; i < expon; i++) {
        uint8_t d = (i < (int)ndgts) ? digits[i] : 0;
        if (d > 99 || v > (limit - d) / 100)
            return false;
        v = (v * 100) + d;
    }
    *out = negative ? (v == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)v) : (int64_t)v;
    return true;
}

int32_t sqli_result_get_int(sqli_result_t *result, size_t col_index)
{
    if (result != NULL)
        result->last_was_null = false;
    if (result == NULL || col_index >= (size_t)result->column_count)
        return 0;
    if (result->current_row < 0 || result->tuple_len == 0)
        return 0;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null)
        return 0;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    uint8_t type = (uint8_t)col->type;

    if (type != SQLI_TYPE_BOOL && type != SQLI_TYPE_DBOOLEAN) {
        int64_t value = sqli_result_get_int64(result, col_index);
        if (value < INT32_MIN || value > INT32_MAX)
            return 0;
        return (int32_t)value;
    }

    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        return 0;

    size_t start = result->cur_col_data_start[col_index];
    size_t len = result->cur_col_data_len[col_index];
    if (len == 0 || start > result->tuple_len || len > result->tuple_len - start)
        return 0;
    const uint8_t *buf = result->tuple_buffer + start;

    if (type == SQLI_TYPE_BOOL || type == SQLI_TYPE_DBOOLEAN) {
        uint8_t b = buf[0];
        switch (b) {
        case 0:
        case '0':
        case 'f':
        case 'F':
        case 'n':
        case 'N':
            return 0;
        default:
            return 1;
        }
    }

    return 0;
}

int64_t sqli_result_get_int64(sqli_result_t *result, size_t col_index)
{
    if (result != NULL)
        result->last_was_null = false;
    if (result == NULL || col_index >= (size_t)result->column_count)
        return 0;
    if (result->current_row < 0 || result->tuple_len == 0)
        return 0;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null)
        return 0;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    uint8_t type = (uint8_t)col->type;

    if (type == SQLI_TYPE_DECIMAL || type == SQLI_TYPE_MONEY) {
        uint8_t raw[64];
        size_t raw_len = sizeof(raw);
        if (sqli_extract_current_value(result, col_index, raw, &raw_len) != SQLI_OK)
            return 0;
        int64_t v64 = 0;
        if (!sqli_decimal_to_int64(raw, raw_len, &v64))
            return 0;
        return v64;
    }

    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        return 0;

    size_t start = result->cur_col_data_start[col_index];
    size_t len = result->cur_col_data_len[col_index];
    if (len == 0 || start > result->tuple_len || len > result->tuple_len - start)
        return 0;
    const uint8_t *buf = result->tuple_buffer + start;

    if (type == SQLI_TYPE_INT8 || type == SQLI_TYPE_SERIAL8) {
        enum { int8_wire_width = 10 };
        if (len != int8_wire_width)
            return 0;
        unsigned sign = ((unsigned)buf[0] << 8) | buf[1];
        if (sign == 0) {
            result->last_was_null = true;
            return 0;
        }
        if (sign != 1 && sign != UINT16_MAX)
            return 0;
        uint32_t low = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
            ((uint32_t)buf[4] << 8) | buf[5];
        uint32_t high = ((uint32_t)buf[6] << 24) | ((uint32_t)buf[7] << 16) |
            ((uint32_t)buf[8] << 8) | buf[9];
        uint64_t magnitude = ((uint64_t)high << 32) | low;
        uint64_t limit = sign == 1 ? (uint64_t)INT64_MAX : (uint64_t)INT64_MAX + 1;
        if (magnitude > limit)
            return 0;
        return sign == 1 ? (int64_t)magnitude :
            magnitude == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)magnitude;
    }

    size_t width;
    switch (type) {
    case SQLI_TYPE_SMALLINT: width = 2; break;
    case SQLI_TYPE_INT: case SQLI_TYPE_SERIAL: case SQLI_TYPE_DATE: width = 4; break;
    case SQLI_TYPE_BIGINT: case SQLI_TYPE_BIGSERIAL: width = 8; break;
    case SQLI_TYPE_BOOL: case SQLI_TYPE_DBOOLEAN:
        return sqli_result_get_int(result, col_index);
    default: return 0;
    }
    if (len != width)
        return 0;
    uint64_t u = 0;
    for (size_t i = 0; i < len; i++)
        u = (u << 8) | buf[i];
    if (len < 8 && (buf[0] & 0x80) != 0)
        u |= UINT64_MAX << (len * 8);
    return u <= INT64_MAX ? (int64_t)u : -1 - (int64_t)(UINT64_MAX - u);
}

double sqli_result_get_double(sqli_result_t *result, size_t col_index)
{
    if (result != NULL)
        result->last_was_null = false;
    if (result == NULL || col_index >= (size_t)result->column_count)
        return 0.0;
    if (result->current_row < 0 || result->tuple_len == 0)
        return 0.0;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null)
        return 0.0;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    uint8_t type = (uint8_t)col->type;

    if (type == SQLI_TYPE_DECIMAL || type == SQLI_TYPE_MONEY) {
        uint8_t raw[64];
        size_t len = sizeof(raw);
        sqli_status rc = sqli_extract_current_value(result, col_index, raw, &len);
        if (rc != SQLI_OK || len < 2)
            return 0.0;

        int expon = (int8_t)raw[0];
        uint8_t digits[63];
        size_t ndgts = len - 1;
        memcpy(digits, raw + 1, ndgts);

        int negative = 0;
        if ((expon & 0x80) == 0) {
            sqli_base100_complement(digits, ndgts);
            expon ^= 0x7F;
            negative = 1;
        }
        expon = (expon & 0x7F) - 64;

        long double v = 0.0L;
        for (size_t i = 0; i < ndgts; i++) {
            if (digits[i] > 99)
                return 0.0;
            v = (v * 100.0L) + (long double)digits[i];
        }

        int frac_digits = (int)(ndgts * 2) - (expon * 2);
        while (frac_digits > 0) {
            v /= 10.0L;
            frac_digits--;
        }
        while (frac_digits < 0) {
            v *= 10.0L;
            frac_digits++;
        }
        if (negative)
            v = -v;
        double converted = (double)v;
        return isfinite(converted) ? converted : 0.0;
    }

    if (type != SQLI_TYPE_FLOAT && type != SQLI_TYPE_SMFLOAT)
        return (double)sqli_result_get_int64(result, col_index);

    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        return 0.0;

    size_t start = result->cur_col_data_start[col_index];
    size_t len = result->cur_col_data_len[col_index];
    if (len < 4 || start > result->tuple_len || len > result->tuple_len - start)
        return 0.0;
    const uint8_t *buf = result->tuple_buffer + start;

    /* Wire format is big-endian IEEE 754 (4-byte single or 8-byte double precision) */
    if (len == 4 && type == SQLI_TYPE_SMFLOAT) {
        uint32_t bits32 = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                          ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
        float fval;
        memcpy(&fval, &bits32, 4);
        return isfinite(fval) ? (double)fval : 0.0;
    }

    if (len == 8 && type == SQLI_TYPE_FLOAT) {
        uint64_t bits = ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48) |
                        ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32) |
                        ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16) |
                        ((uint64_t)buf[6] << 8)  | (uint64_t)buf[7];
        double val;
        memcpy(&val, &bits, 8);
        return isfinite(val) ? val : 0.0;
    }

    return 0.0;
}

/* String/UTF-8 decode buffer sizes per column, matching the previous
 * shared-buffer capacities (see sqli_result_get_string() below). */
#define SQLI_STR_BUF_SIZE  4096
#define SQLI_UTF8_BUF_SIZE 12288

/* Ensure per-column string decode buffers exist for the current
 * column_count. Returns false on allocation failure (caller falls back
 * to returning ""). Buffers are lazily allocated on first use and freed
 * in sqli_result_cleanup(). */
static bool sqli_result_ensure_col_bufs(sqli_result_t *result)
{
    if (result->col_str_bufs != NULL && result->col_bufs_count == result->column_count)
        return true;

    /* column_count changed (or first use) since bufs were sized: reset. */
    if (result->col_str_bufs != NULL) {
        for (int i = 0; i < result->col_bufs_count; i++)
            free(result->col_str_bufs[i]);
        free(result->col_str_bufs);
        result->col_str_bufs = NULL;
    }
    if (result->col_utf8_bufs != NULL) {
        for (int i = 0; i < result->col_bufs_count; i++)
            free(result->col_utf8_bufs[i]);
        free(result->col_utf8_bufs);
        result->col_utf8_bufs = NULL;
    }
    result->col_bufs_count = 0;

    if (result->column_count <= 0)
        return false;

    result->col_str_bufs = calloc((size_t)result->column_count, sizeof(char *));
    result->col_utf8_bufs = calloc((size_t)result->column_count, sizeof(char *));
    if (result->col_str_bufs == NULL || result->col_utf8_bufs == NULL)
        return false;

    for (int i = 0; i < result->column_count; i++) {
        result->col_str_bufs[i] = malloc(SQLI_STR_BUF_SIZE);
        result->col_utf8_bufs[i] = malloc(SQLI_UTF8_BUF_SIZE);
        if (result->col_str_bufs[i] == NULL || result->col_utf8_bufs[i] == NULL)
            return false;
    }
    result->col_bufs_count = result->column_count;
    return true;
}

const char *sqli_result_get_string(sqli_result_t *result, size_t col_index)
{
    if (result == NULL || col_index >= (size_t)result->column_count)
        return "";
    if (result->current_row < 0 || result->tuple_len == 0)
        return "";

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL)
        return "";

    result->last_was_null = result->cur_col_is_null[col_index] ? true : false;
    if (result->last_was_null)
        return "";

    size_t data_start = result->cur_col_data_start[col_index];
    size_t data_len = result->cur_col_data_len[col_index];
    if (data_len == 0 || data_start > result->tuple_len || data_start + data_len > result->tuple_len)
        return "";

    if (!sqli_result_ensure_col_bufs(result))
        return "";
    char *str_buf = result->col_str_bufs[col_index];
    char *utf8_buf = result->col_utf8_bufs[col_index];

    if (sqli_is_legacy_lob_type((uint8_t)col->type)) {
        uint8_t *lob = NULL;
        size_t lob_len = 0;
        sqli_status frc = sqli_fetchblob_materialize(result, col_index, &lob, &lob_len);
        if (frc == SQLI_OK) {
            if (lob_len == 0 || lob == NULL) {
                free(lob);
                str_buf[0] = '\0';
                return str_buf;
            }
            size_t copy = lob_len < SQLI_STR_BUF_SIZE - 1 ? lob_len : SQLI_STR_BUF_SIZE - 1;
            memcpy(str_buf, lob, copy);
            str_buf[copy] = '\0';
            free(lob);

            if (col->type == SQLI_TYPE_TEXT) {
                sqli_conn_t *conn = result->owner_conn;
                if (conn != NULL && !conn->decode_locale_checked) {
                    sqli_charset_decoder_close(&conn->decode_cs);
                    conn->decode_cs_ready = false;
                    conn->decode_locale_checked = true;
                    if (conn->client_locale != NULL && conn->db_locale != NULL &&
                        sqli_charset_decoder_open_locales(&conn->decode_cs,
                                                          conn->client_locale,
                                                          conn->db_locale))
                        conn->decode_cs_ready = true;
                }
                if (conn != NULL && conn->decode_cs_ready) {
                    size_t utf8_len = SQLI_UTF8_BUF_SIZE;
                    if (sqli_charset_decoder_convert(&conn->decode_cs, str_buf, copy,
                                                     utf8_buf, &utf8_len))
                        return utf8_buf;
                }
            }
            return str_buf;
        }
        free(lob);
        return "";
    }

    if (col->type == SQLI_TYPE_DECIMAL || col->type == SQLI_TYPE_MONEY) {
        const char *dec_str = sqli_result_get_decimal_string(result, col_index);
        size_t dlen = strlen(dec_str);
        size_t copy = dlen < SQLI_STR_BUF_SIZE - 1 ? dlen : SQLI_STR_BUF_SIZE - 1;
        memcpy(str_buf, dec_str, copy);
        str_buf[copy] = '\0';
        return str_buf;
    }

    if (col->type == SQLI_TYPE_DATETIME) {
        const char *dt_str = sqli_result_get_datetime_string(result, col_index);
        size_t dlen = strlen(dt_str);
        size_t copy = dlen < SQLI_STR_BUF_SIZE - 1 ? dlen : SQLI_STR_BUF_SIZE - 1;
        memcpy(str_buf, dt_str, copy);
        str_buf[copy] = '\0';
        return str_buf;
    }

    if (col->type == SQLI_TYPE_INTERVAL) {
        const char *iv_str = sqli_result_get_interval_string(result, col_index);
        size_t ilen = strlen(iv_str);
        size_t copy = ilen < SQLI_STR_BUF_SIZE - 1 ? ilen : SQLI_STR_BUF_SIZE - 1;
        memcpy(str_buf, iv_str, copy);
        str_buf[copy] = '\0';
        return str_buf;
    }

    size_t copy = data_len < SQLI_STR_BUF_SIZE - 1 ? data_len : SQLI_STR_BUF_SIZE - 1;
    memcpy(str_buf, result->tuple_buffer + data_start, copy);

    sqli_conn_t *conn = result->owner_conn;
    bool trim_trailing_spaces = (conn == NULL) ? true : conn->trim_trailing_spaces;
    if (trim_trailing_spaces && sqli_is_stringy_type((uint8_t)col->type)) {
        while (copy > 0 && str_buf[copy - 1] == ' ')
            copy--;
    }
    str_buf[copy] = '\0';

    if (conn != NULL && !conn->decode_locale_checked) {
        sqli_charset_decoder_close(&conn->decode_cs);
        conn->decode_cs_ready = false;
        conn->decode_locale_checked = true;
        if (conn->client_locale != NULL && conn->db_locale != NULL &&
            sqli_charset_decoder_open_locales(&conn->decode_cs,
                                              conn->client_locale,
                                              conn->db_locale))
            conn->decode_cs_ready = true;
    }

    if (conn != NULL && conn->decode_cs_ready) {
        size_t utf8_len = SQLI_UTF8_BUF_SIZE;
        if (sqli_charset_decoder_convert(&conn->decode_cs, str_buf, copy,
                                         utf8_buf, &utf8_len))
            return utf8_buf;
    }
    return str_buf;
}

static sqli_status copy_result_buffer(const void *data, size_t length, bool null_value,
                                       bool text, void *out, size_t capacity,
                                       size_t *required, bool *is_null)
{
    if (text && length == SIZE_MAX)
        return SQLI_LIMIT_EXCEEDED;
    size_t needed = null_value ? 0 : length + (text ? 1u : 0u);
    if (out != NULL && capacity < needed) {
        *required = needed;
        *is_null = null_value;
        return SQLI_BUFFER_TOO_SMALL;
    }
    if (out != NULL && !null_value) {
        if (length != 0)
            memcpy(out, data, length);
        if (text)
            ((char *)out)[length] = '\0';
    }
    *required = needed;
    *is_null = null_value;
    return SQLI_OK;
}

static sqli_status result_string_conversion(sqli_result_t *result, const uint8_t *raw,
                                             size_t length, uint8_t **owned, size_t *out_length)
{
    sqli_conn_t *conn = result->owner_conn;
    if (conn != NULL && !conn->decode_locale_checked) {
        sqli_charset_decoder_close(&conn->decode_cs);
        conn->decode_cs_ready = false;
        conn->decode_locale_checked = true;
        if (conn->client_locale != NULL && conn->db_locale != NULL)
            conn->decode_cs_ready = sqli_charset_decoder_open_locales(&conn->decode_cs,
                                                conn->client_locale, conn->db_locale);
    }
    if (length > (SIZE_MAX - 1) / 4)
        return SQLI_LIMIT_EXCEEDED;
    size_t capacity = length * 4 + 1;
    uint8_t *converted = malloc(capacity);
    if (converted == NULL)
        return SQLI_ALLOC_FAIL;
    size_t converted_length = length;
    if (length != 0 && conn != NULL && conn->decode_cs_ready) {
        converted_length = capacity;
        if (!sqli_charset_decoder_convert(&conn->decode_cs, (const char *)raw, length,
                                           (char *)converted, &converted_length)) {
            free(converted);
            return SQLI_PROTO_ERROR;
        }
    } else if (length != 0) {
        memcpy(converted, raw, length);
    }
    *owned = converted;
    *out_length = converted_length;
    return SQLI_OK;
}

sqli_status sqli_result_get_string_len(sqli_result_t *result, size_t col_index,
                                       char *out, size_t capacity, size_t *required, bool *is_null)
{
    if (required == NULL || is_null == NULL || (out == NULL && capacity != 0))
        return SQLI_INVALID_ARGUMENT;
    const sqli_column_info *column;
    const uint8_t *bytes;
    size_t length;
    sqli_status status = sqli_result_current_span(result, col_index, &column, &bytes, &length);
    if (status != SQLI_OK)
        return status;
    if (result->cur_col_is_null == NULL)
        return SQLI_INVALID_STATE;
    bool null_value = result->cur_col_is_null[col_index] != 0;
    if (null_value)
        return copy_result_buffer(NULL, 0, true, true, out, capacity, required, is_null);
    enum { scalar_text_capacity = 256 };
    char formatted[scalar_text_capacity] = {0};
    size_t needed;
    uint8_t *lob = NULL, *converted = NULL;
    if (column->type == SQLI_TYPE_DECIMAL || column->type == SQLI_TYPE_MONEY) {
        sqli_decimal_t *value = NULL;
        status = sqli_decimal_create(&value);
        if (status == SQLI_OK)
            status = sqli_result_get_decimal(result, col_index, value);
        if (status == SQLI_OK)
            status = sqli_decimal_format(value, formatted, sizeof(formatted), &needed, &null_value);
        sqli_decimal_destroy(value);
        if (status != SQLI_OK)
            return status;
        bytes = (const uint8_t *)formatted;
        length = null_value ? 0 : needed - 1;
    } else if (column->type == SQLI_TYPE_DATETIME || column->type == SQLI_TYPE_INTERVAL) {
        if (column->type == SQLI_TYPE_DATETIME) {
            struct sqli_datetime value = {0};
            status = sqli_result_get_datetime(result, col_index, &value);
            if (status == SQLI_OK)
                status = sqli_datetime_format(&value, formatted, sizeof(formatted), &needed, &null_value);
        } else {
            struct sqli_interval value = {0};
            status = sqli_result_get_interval(result, col_index, &value);
            if (status == SQLI_OK)
                status = sqli_interval_format(&value, formatted, sizeof(formatted), &needed, &null_value);
        }
        if (status != SQLI_OK)
            return status;
        bytes = (const uint8_t *)formatted;
        length = null_value ? 0 : needed - 1;
        char *separator = strchr(formatted, 'T');
        if (separator != NULL)
            *separator = ' ';
    } else if (column->type == SQLI_TYPE_DATE) {
        sqli_date_t value;
        status = sqli_result_get_date(result, col_index, &value);
        if (status == SQLI_OK)
            status = sqli_date_format(&value, formatted, sizeof(formatted), &needed, &null_value);
        if (status != SQLI_OK)
            return status;
        bytes = (const uint8_t *)formatted;
        length = null_value ? 0 : needed - 1;
    } else {
        if (sqli_is_legacy_lob_type((uint8_t)column->type)) {
            status = sqli_fetchblob_materialize(result, col_index, &lob, &length);
            if (status != SQLI_OK)
                goto cleanup;
            bytes = lob;
        } else if (!sqli_is_stringy_type((uint8_t)column->type)) {
            return SQLI_TYPE_MISMATCH;
        }
        if ((result->owner_conn == NULL || result->owner_conn->trim_trailing_spaces) &&
            sqli_is_stringy_type((uint8_t)column->type)) {
            while (length > 0 && bytes[length - 1] == ' ')
                length--;
        }
        status = result_string_conversion(result, bytes, length, &converted, &length);
        if (status != SQLI_OK)
            goto cleanup;
        bytes = converted;
    }
    status = copy_result_buffer(bytes, length, null_value, true, out, capacity, required, is_null);
cleanup:
    free(converted);
    free(lob);
    return status;
}

sqli_status sqli_result_get_bytes(sqli_result_t *result, size_t col_index,
                                  uint8_t *out, size_t capacity, size_t *required, bool *is_null)
{
    if (required == NULL || is_null == NULL || (out == NULL && capacity != 0))
        return SQLI_INVALID_ARGUMENT;
    const sqli_column_info *column;
    const uint8_t *bytes;
    size_t length;
    sqli_status status = sqli_result_current_span(result, col_index, &column, &bytes, &length);
    if (status != SQLI_OK)
        return status;
    if (result->cur_col_is_null == NULL)
        return SQLI_INVALID_STATE;
    bool null_value = result->cur_col_is_null[col_index] != 0;
    uint8_t *lob = NULL;
    if (!null_value && sqli_is_lob_like_type((uint8_t)column->type)) {
        status = sqli_fetchblob_materialize(result, col_index, &lob, &length);
        if (status != SQLI_OK) {
            free(lob);
            return status;
        }
        bytes = lob;
    }
    status = copy_result_buffer(bytes, length, null_value, false, out, capacity, required, is_null);
    free(lob);
    return status;
}

bool sqli_result_is_null(sqli_result_t *result, size_t col_index)
{
    bool is_null = sqli_result_is_null_internal(result, col_index);
    if (result != NULL)
        result->last_was_null = is_null;
    return is_null;
}

bool sqli_result_was_null(sqli_result_t *result)
{
    if (result == NULL)
        return true;
    return result->last_was_null;
}

bool sqli_result_get_bool(sqli_result_t *result, size_t col_index)
{
    int32_t v = sqli_result_get_int(result, col_index);
    return v != 0;
}

void sqli_days_to_ymd_ifx(int32_t ifx_days, int *y, int *m, int *d)
{
    int64_t z = (int64_t)ifx_days - 25568; /* IFX wire epoch (1899-12-31) -> Unix epoch days */
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp + (mp < 10 ? 3 : -9);
    yy += (mm <= 2);
    if (y) *y = (int)yy;
    if (m) *m = (int)mm;
    if (d) *d = (int)dd;
}

const char *sqli_result_get_decimal_string(sqli_result_t *result, size_t col_index)
{
    static _Thread_local char out[256];
    out[0] = '\0';

    if (result == NULL || col_index >= (size_t)result->column_count)
        return out;
    if (result->current_row < 0 || result->tuple_len == 0)
        return out;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null)
        return out;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    uint8_t t = (uint8_t)col->type;
    if (t != SQLI_TYPE_DECIMAL && t != SQLI_TYPE_MONEY)
        return sqli_result_get_string(result, col_index);

    uint8_t raw[64];
    size_t len = sizeof(raw);
    if (sqli_extract_current_value(result, col_index, raw, &len) != SQLI_OK)
        return out;

    uint8_t b100[63];
    size_t ndgts = 0;
    int frac_digits = 0;
    int negative = 0;
    if (!sqli_base100_decode_parts(raw, len, b100, &ndgts, &frac_digits, &negative))
        return out;
    int raw_scale = (int)(col->encoded_length & 0xFF);
    int scale;
    bool floating_scale = (raw_scale == 0xFF);
    if (floating_scale) {
        /* 0xFF marks an Informix "floating decimal" column (DECIMAL(p)
         * with no fixed scale) — there is no declared scale to render
         * with. Start from the fractional digit count implied by this
         * value's packed encoding, then strip trailing zero digits
         * below (the packed encoding can carry trailing zero digit
         * bytes for round values, e.g. 10 or 15000, which isn't
         * meaningful precision to display). Without this, such values
         * were rendered with a hardcoded 6 fractional digits
         * ("10.000000") even though the value itself is a whole
         * number; the underlying value was already decoded correctly,
         * only the display formatting was wrong. */
        scale = frac_digits;
        if (scale < 0)
            scale = 0;
        else if (scale > 30)
            scale = 30;
    } else if (raw_scale < 0 || raw_scale > 30) {
        scale = 6;
    } else {
        scale = raw_scale;
    }

    char digits[192] = {0};
    size_t digits_len = 0;
    int is_zero = 1;
    for (size_t i = 0; i < ndgts; i++) {
        if (b100[i] > 99 || digits_len + 2 >= sizeof(digits))
            return out;
        uint8_t hi = (uint8_t)(b100[i] / 10);
        uint8_t lo = (uint8_t)(b100[i] % 10);
        digits[digits_len++] = (char)('0' + hi);
        digits[digits_len++] = (char)('0' + lo);
        if (hi != 0 || lo != 0)
            is_zero = 0;
    }
    if (digits_len == 0) {
        digits[0] = '0';
        digits_len = 1;
        is_zero = 1;
    }

    if (frac_digits < 0)
        frac_digits = 0;
    ptrdiff_t dec_pos = (ptrdiff_t)digits_len - (ptrdiff_t)frac_digits;

    if (floating_scale) {
        /* Strip trailing zero fractional digits: the packed encoding
         * can carry trailing zero digit bytes for round values (e.g.
         * 10 or 15000 stored with extra zero digit pairs), which
         * inflates "scale" above without adding real precision. */
        while (scale > 0) {
            ptrdiff_t src = dec_pos + (ptrdiff_t)(scale - 1);
            char c = (src >= 0 && (size_t)src < digits_len) ? digits[src] : '0';
            if (c != '0')
                break;
            scale--;
        }
    }

    size_t pos = 0;
    if (negative && !is_zero && pos + 1 < sizeof(out))
        out[pos++] = '-';

    ptrdiff_t int_start = 0;
    ptrdiff_t int_end = dec_pos;
    if (int_end < 0)
        int_end = 0;
    while (int_start + 1 < int_end && digits[int_start] == '0')
        int_start++;
    if (int_end - int_start <= 0) {
        if (pos + 1 < sizeof(out))
            out[pos++] = '0';
    } else {
        for (ptrdiff_t i = int_start; i < int_end && pos + 1 < sizeof(out); i++)
            out[pos++] = digits[i];
    }

    if (scale > 0 && pos + 1 < sizeof(out))
        out[pos++] = '.';

    for (int j = 0; j < scale && pos + 1 < sizeof(out); j++) {
        ptrdiff_t src = dec_pos + (ptrdiff_t)j;
        if (src >= 0 && (size_t)src < digits_len)
            out[pos++] = digits[src];
        else
            out[pos++] = '0';
    }
    out[pos] = '\0';
    return out;
}

const char *sqli_result_get_date_string(sqli_result_t *result, size_t col_index)
{
    static _Thread_local char out[SQLI_TEMPORAL_MAX_TEXT];
    out[0] = '\0';
    sqli_date_t date;
    if (sqli_result_get_date(result, (size_t)col_index, &date) != SQLI_OK)
        return out;
    size_t required;
    bool is_null;
    if (sqli_date_format(&date, out, sizeof(out), &required, &is_null) != SQLI_OK)
        return out;
    result->last_was_null = is_null;
    return out;
}

const char *sqli_result_get_datetime_string(sqli_result_t *result, size_t col_index)
{
    static _Thread_local char out[SQLI_TEMPORAL_MAX_TEXT];
    out[0] = '\0';
    struct sqli_datetime value = {0};
    size_t required;
    bool is_null;
    if (sqli_result_get_datetime(result, (size_t)col_index, &value) != SQLI_OK)
        return out;
    if (sqli_datetime_format(&value, out, sizeof(out), &required, &is_null) != SQLI_OK)
        out[0] = '\0';
    else
        result->last_was_null = is_null;
    /* Preserve SQL-style spacing in this convenience getter. */
    char *separator = strchr(out, 'T');
    if (separator != NULL)
        *separator = ' ';
    return out;
}

const char *sqli_result_get_interval_string(sqli_result_t *result, size_t col_index)
{
    static _Thread_local char out[SQLI_TEMPORAL_MAX_TEXT];
    out[0] = '\0';
    struct sqli_interval value = {0};
    size_t required;
    bool is_null;
    if (sqli_result_get_interval(result, (size_t)col_index, &value) != SQLI_OK)
        return out;
    if (sqli_interval_format(&value, out, sizeof(out), &required, &is_null) != SQLI_OK)
        out[0] = '\0';
    else
        result->last_was_null = is_null;
    return out;
}

sqli_status sqli_result_stream_bytes(sqli_result_t *result, size_t col_index,
                                     size_t chunk_size, sqli_stream_chunk_cb cb,
                                     void *ctx)
{
    if (result == NULL || cb == NULL || chunk_size == 0)
        return SQLI_INVALID_STATE;
    if (col_index >= (size_t)result->column_count)
        return SQLI_INVALID_STATE;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null)
        return SQLI_OK;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    if (sqli_is_lob_like_type((uint8_t)col->type)) {
        uint8_t *lob = NULL;
        size_t lob_len = 0;
        sqli_status frc = sqli_fetchblob_materialize(result, col_index, &lob, &lob_len);
        if (frc == SQLI_OK) {
            if (lob_len == 0 || lob == NULL) {
                free(lob);
                return SQLI_OK;
            }
            size_t off = 0;
            while (off < lob_len) {
                size_t n = lob_len - off;
                if (n > chunk_size)
                    n = chunk_size;
                if (cb(lob + off, n, ctx) != 0) {
                    free(lob);
                    return SQLI_ERR;
                }
                off += n;
            }
            free(lob);
            return SQLI_OK;
        }
        free(lob);
    }

    size_t data_start = 0, data_len = 0, span = 0;
    sqli_status rc = sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                              &data_start, &data_len, &span);
    if (rc != SQLI_OK)
        return rc;

    const uint8_t *buf = result->tuple_buffer + data_start;
    size_t off = 0;
    while (off < data_len) {
        size_t n = data_len - off;
        if (n > chunk_size)
            n = chunk_size;
        if (cb(buf + off, n, ctx) != 0)
            return SQLI_ERR;
        off += n;
    }
    return SQLI_OK;
}

sqli_status sqli_result_get_timestamp(sqli_result_t *result, size_t col_index,
                                      sqli_timestamp_t *out)
{
    if (result == NULL || out == NULL || col_index >= (size_t)result->column_count)
        return SQLI_INVALID_STATE;

    memset(out, 0, sizeof(*out));
    out->is_null = true;

    if (result->current_row < 0 || result->tuple_len == 0)
        return SQLI_INVALID_STATE;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null) {
        return SQLI_OK;
    }

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    uint8_t type = (uint8_t)col->type;

    if (type == SQLI_TYPE_DATE) {
        sqli_date_t date_val;
        sqli_status rc = sqli_result_get_date(result, (size_t)col_index, &date_val);
        if (rc != SQLI_OK) return rc;
        out->is_null = date_val.is_null;
        out->year = date_val.year;
        out->month = date_val.month;
        out->day = date_val.day;
        out->hour = 0;
        out->minute = 0;
        out->second = 0;
        out->microsecond = 0;
        return SQLI_OK;
    }

    if (type == SQLI_TYPE_DATETIME) {
        struct sqli_datetime value = {0};
        sqli_status rc = sqli_result_get_datetime(result, (size_t)col_index, &value);
        if (rc != SQLI_OK) return rc;
        sqli_datetime_parts_t dt;
        rc = sqli_datetime_get_parts(&value, &dt);
        if (rc != SQLI_OK) return rc;
        out->is_null = dt.is_null;
        if (dt.is_null) return SQLI_OK;
        out->year = dt.range.first <= SQLI_FIELD_YEAR ? dt.year : 1970;
        out->month = dt.range.first <= SQLI_FIELD_MONTH && dt.range.last >= SQLI_FIELD_MONTH ? dt.month : 1;
        out->day = dt.range.first <= SQLI_FIELD_DAY && dt.range.last >= SQLI_FIELD_DAY ? dt.day : 1;
        out->hour = dt.hour;
        out->minute = dt.minute;
        out->second = dt.second;
        out->microsecond = (int)(dt.nanosecond / 1000);
        return SQLI_OK;
    }

    /* Fallback for string-based types */
    if (type == SQLI_TYPE_CHAR || type == SQLI_TYPE_VARCHAR ||
        type == SQLI_TYPE_NCHAR || type == SQLI_TYPE_NVCHAR ||
        type == SQLI_TYPE_LVARCHAR) {
        const char *s = sqli_result_get_string(result, col_index);
        if (s == NULL || *s == '\0') {
            return SQLI_OK;
        }

        int y = 1970, m = 1, d = 1, h = 0, mi = 0, sec = 0, frac = 0;
        int parsed = sscanf(s, "%d-%d-%d %d:%d:%d.%d", &y, &m, &d, &h, &mi, &sec, &frac);
        if (parsed >= 3) {
            out->is_null = false;
            out->year = y;
            out->month = m;
            out->day = d;
            out->hour = (parsed >= 4) ? h : 0;
            out->minute = (parsed >= 5) ? mi : 0;
            out->second = (parsed >= 6) ? sec : 0;
            if (parsed >= 7) {
                const char *dot = strchr(s, '.');
                if (dot != NULL) {
                    dot++;
                    int scale = 0;
                    while (dot[scale] >= '0' && dot[scale] <= '9' && scale < 6) {
                        scale++;
                    }
                    int scale_factors[7] = { 1000000, 100000, 10000, 1000, 100, 10, 1 };
                    out->microsecond = frac * scale_factors[scale];
                } else {
                    out->microsecond = 0;
                }
            } else {
                out->microsecond = 0;
            }
            return SQLI_OK;
        }
    }

    return SQLI_ERR;
}

sqli_status sqli_result_get_epoch_sec(sqli_result_t *result, size_t col_index, int64_t *out_sec)
{
    if (out_sec == NULL) return SQLI_INVALID_STATE;
    *out_sec = 0;
    sqli_timestamp_t ts;
    sqli_status rc = sqli_result_get_timestamp(result, col_index, &ts);
    if (rc != SQLI_OK) return rc;
    if (ts.is_null) return SQLI_OK;
    *out_sec = sqli_timestamp_to_epoch_sec(&ts);
    return SQLI_OK;
}

sqli_status sqli_result_get_epoch_ms(sqli_result_t *result, size_t col_index, int64_t *out_ms)
{
    if (out_ms == NULL) return SQLI_INVALID_STATE;
    *out_ms = 0;
    sqli_timestamp_t ts;
    sqli_status rc = sqli_result_get_timestamp(result, col_index, &ts);
    if (rc != SQLI_OK) return rc;
    if (ts.is_null) return SQLI_OK;
    *out_ms = sqli_timestamp_to_epoch_ms(&ts);
    return SQLI_OK;
}

sqli_status sqli_result_get_epoch_days(sqli_result_t *result, size_t col_index, int32_t *out_days)
{
    if (out_days == NULL) return SQLI_INVALID_STATE;
    *out_days = 0;
    sqli_timestamp_t ts;
    sqli_status rc = sqli_result_get_timestamp(result, col_index, &ts);
    if (rc != SQLI_OK) return rc;
    if (ts.is_null) return SQLI_OK;
    *out_days = sqli_timestamp_to_epoch_days(&ts);
    return SQLI_OK;
}
