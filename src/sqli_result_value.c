#define _GNU_SOURCE
#include "sqli_internal.h"
#include "sqli_charset.h"
#include "sqli_protocol_internal.h"
#include "sqli_result_internal.h"

#include <stdio.h>
#include <stdlib.h>
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

static sqli_status sqli_extract_current_value(sqli_result_t *result, int col_index,
                                              uint8_t *out, size_t *out_len)
{
    if (result == NULL || out == NULL || out_len == NULL ||
        col_index < 0 || col_index >= result->column_count ||
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
    if (start > result->tuple_len || start + len > result->tuple_len)
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

    int64_t v = 0;
    for (int i = 0; i < expon; i++) {
        uint8_t d = (i < (int)ndgts) ? digits[i] : 0;
        if (d > 99)
            return false;
        v = (v * 100) + d;
    }
    if (negative)
        v = -v;
    *out = v;
    return true;
}

int32_t sqli_result_get_int(sqli_result_t *result, int col_index)
{
    if (result == NULL || col_index < 0 || col_index >= result->column_count)
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
        return (int32_t)v64;
    }

    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        return 0;

    size_t start = result->cur_col_data_start[col_index];
    size_t len = result->cur_col_data_len[col_index];
    if (len == 0 || start > result->tuple_len || start + len > result->tuple_len)
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

    if (type == SQLI_TYPE_INT8 || type == SQLI_TYPE_SERIAL8) {
        if (len < 10)
            return 0;
        bool is_null = false;
        int64_t v = sqli_decode_ifx_int8(buf, &is_null);
        if (is_null)
            return INT32_MIN;
        return (int32_t)v;
    }

    if (len >= 4) {
        uint32_t u = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                     ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
        return (int32_t)u;
    }

    uint32_t u = 0;
    for (size_t i = 0; i < len; i++)
        u = (u << 8) | (uint32_t)buf[i];
    if ((buf[0] & 0x80) != 0)
        u |= (~(uint32_t)0) << (len * 8);
    return (int32_t)u;
}

int64_t sqli_result_get_int64(sqli_result_t *result, int col_index)
{
    if (result == NULL || col_index < 0 || col_index >= result->column_count)
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
    if (len == 0 || start > result->tuple_len || start + len > result->tuple_len)
        return 0;
    const uint8_t *buf = result->tuple_buffer + start;

    if (type == SQLI_TYPE_INT8 || type == SQLI_TYPE_SERIAL8) {
        if (len < 10)
            return 0;
        bool is_null = false;
        int64_t v = sqli_decode_ifx_int8(buf, &is_null);
        if (is_null)
            return INT64_MIN;
        return v;
    }

    if (len >= 8)
        return (int64_t)((uint64_t)buf[0] << 56 | (uint64_t)buf[1] << 48 |
                         (uint64_t)buf[2] << 40 | (uint64_t)buf[3] << 32 |
                         (uint64_t)buf[4] << 24 | (uint64_t)buf[5] << 16 |
                         (uint64_t)buf[6] << 8  | (uint64_t)buf[7]);

    if (len == 0)
        return 0;
    uint64_t u = 0;
    for (size_t i = 0; i < len; i++)
        u = (u << 8) | (uint64_t)buf[i];
    if (len < 8 && (buf[0] & 0x80) != 0)
        u |= (~(uint64_t)0) << (len * 8);
    return (int64_t)u;
}

double sqli_result_get_double(sqli_result_t *result, int col_index)
{
    if (result == NULL || col_index < 0 || col_index >= result->column_count)
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
        while (frac_digits-- > 0)
            v /= 10.0L;
        if (negative)
            v = -v;
        return (double)v;
    }

    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        return 0.0;

    size_t start = result->cur_col_data_start[col_index];
    size_t len = result->cur_col_data_len[col_index];
    if (len < 4 || start > result->tuple_len || start + len > result->tuple_len)
        return 0.0;
    const uint8_t *buf = result->tuple_buffer + start;

    /* Wire format is big-endian IEEE 754 (4-byte single or 8-byte double precision) */
    if (len == 4 || type == SQLI_TYPE_SMFLOAT) {
        uint32_t bits32 = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                          ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
        float fval;
        memcpy(&fval, &bits32, 4);
        return (double)fval;
    }

    if (len >= 8) {
        uint64_t bits = ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48) |
                        ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32) |
                        ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16) |
                        ((uint64_t)buf[6] << 8)  | (uint64_t)buf[7];
        double val;
        memcpy(&val, &bits, 8);
        return val;
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

const char *sqli_result_get_string(sqli_result_t *result, int col_index)
{
    if (result == NULL || col_index < 0 || col_index >= result->column_count)
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

sqli_status sqli_result_get_string_len(sqli_result_t *result, int col_index,
                                       char *out, size_t *out_len)
{
    if (result == NULL || out == NULL || out_len == NULL || *out_len == 0)
        return SQLI_INVALID_STATE;
    if (col_index < 0 || col_index >= result->column_count)
        return SQLI_INVALID_STATE;
    if (result->current_row < 0 || result->tuple_len == 0)
        return SQLI_INVALID_STATE;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL)
        return SQLI_INVALID_STATE;

    result->last_was_null = result->cur_col_is_null[col_index] ? true : false;
    if (result->last_was_null) {
        out[0] = '\0';
        *out_len = 0;
        return SQLI_OK;
    }

    if (sqli_is_legacy_lob_type((uint8_t)col->type)) {
        uint8_t *lob = NULL;
        size_t lob_len = 0;
        sqli_status frc = sqli_fetchblob_materialize(result, col_index, &lob, &lob_len);
        if (frc == SQLI_OK) {
            if (lob_len == 0 || lob == NULL) {
                out[0] = '\0';
                *out_len = 0;
                free(lob);
                return SQLI_OK;
            }
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
            if (col->type == SQLI_TYPE_TEXT && conn != NULL && conn->decode_cs_ready) {
                char stack_buf[512];
                char *conv_buf = stack_buf;
                size_t conv_cap = sizeof(stack_buf);
                bool allocated = false;
                if (lob_len * 4 + 1 > conv_cap) {
                    conv_cap = lob_len * 4 + 1;
                    conv_buf = malloc(conv_cap);
                    if (conv_buf == NULL) {
                        free(lob);
                        return SQLI_ALLOC_FAIL;
                    }
                    allocated = true;
                }
                size_t conv_len = conv_cap;
                if (sqli_charset_decoder_convert(&conn->decode_cs, (const char *)lob, lob_len,
                                                 conv_buf, &conv_len)) {
                    size_t avail = *out_len - 1;
                    size_t copy = conv_len < avail ? conv_len : avail;
                    memcpy(out, conv_buf, copy);
                    out[copy] = '\0';
                    *out_len = copy;
                    if (allocated)
                        free(conv_buf);
                    free(lob);
                    return SQLI_OK;
                }
                if (allocated)
                    free(conv_buf);
            }
            size_t avail = *out_len - 1;
            size_t copy = lob_len < avail ? lob_len : avail;
            memcpy(out, lob, copy);
            out[copy] = '\0';
            *out_len = copy;
            free(lob);
            return SQLI_OK;
        }
        free(lob);
        out[0] = '\0';
        *out_len = 0;
        return SQLI_OK;
    }

    if (col->type == SQLI_TYPE_DECIMAL || col->type == SQLI_TYPE_MONEY) {
        const char *dec_str = sqli_result_get_decimal_string(result, col_index);
        size_t dlen = strlen(dec_str);
        size_t avail = *out_len - 1;
        size_t copy = dlen < avail ? dlen : avail;
        memcpy(out, dec_str, copy);
        out[copy] = '\0';
        *out_len = copy;
        return SQLI_OK;
    }

    if (col->type == SQLI_TYPE_DATETIME) {
        const char *dt_str = sqli_result_get_datetime_string(result, col_index);
        size_t dlen = strlen(dt_str);
        size_t avail = *out_len - 1;
        size_t copy = dlen < avail ? dlen : avail;
        memcpy(out, dt_str, copy);
        out[copy] = '\0';
        *out_len = copy;
        return SQLI_OK;
    }

    if (col->type == SQLI_TYPE_INTERVAL) {
        const char *iv_str = sqli_result_get_interval_string(result, col_index);
        size_t ilen = strlen(iv_str);
        size_t avail = *out_len - 1;
        size_t copy = ilen < avail ? ilen : avail;
        memcpy(out, iv_str, copy);
        out[copy] = '\0';
        *out_len = copy;
        return SQLI_OK;
    }

    size_t data_start = result->cur_col_data_start[col_index];
    size_t data_len = result->cur_col_data_len[col_index];
    if (data_len == 0 || data_start > result->tuple_len || data_start + data_len > result->tuple_len) {
        out[0] = '\0';
        *out_len = 0;
        return SQLI_OK;
    }

    const char *raw = (const char *)(result->tuple_buffer + data_start);
    size_t raw_len = data_len;
    sqli_conn_t *conn = result->owner_conn;
    bool trim_trailing_spaces = (conn == NULL) ? true : conn->trim_trailing_spaces;
    if (trim_trailing_spaces && sqli_is_stringy_type((uint8_t)col->type)) {
        while (raw_len > 0 && raw[raw_len - 1] == ' ')
            raw_len--;
    }

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
        char stack_buf[512];
        char *conv_buf = stack_buf;
        size_t conv_cap = sizeof(stack_buf);
        bool allocated = false;
        if (raw_len * 4 + 1 > conv_cap) {
            conv_cap = raw_len * 4 + 1;
            conv_buf = malloc(conv_cap);
            if (conv_buf == NULL)
                return SQLI_ALLOC_FAIL;
            allocated = true;
        }
        size_t conv_len = conv_cap;
        if (sqli_charset_decoder_convert(&conn->decode_cs, raw, raw_len,
                                         conv_buf, &conv_len)) {
            size_t avail = *out_len - 1;
            size_t copy = conv_len < avail ? conv_len : avail;
            memcpy(out, conv_buf, copy);
            out[copy] = '\0';
            *out_len = copy;
            if (allocated)
                free(conv_buf);
            return SQLI_OK;
        }
        if (allocated)
            free(conv_buf);
    }

    size_t avail = *out_len - 1;
    size_t copy = raw_len < avail ? raw_len : avail;
    memcpy(out, raw, copy);
    out[copy] = '\0';
    *out_len = copy;
    return SQLI_OK;
}

sqli_status sqli_result_get_bytes(sqli_result_t *result, int col_index,
                                  uint8_t *out, size_t *out_len)
{
    if (result == NULL || out == NULL || out_len == NULL)
        return SQLI_INVALID_STATE;

    const sqli_column_info *col = NULL;

    if (col_index >= 0 && col_index < result->column_count)
        col = &result->columns[(size_t)col_index];

    if (col != NULL) {
        result->last_was_null = sqli_result_is_null_internal(result, col_index);
        if (result->last_was_null) {
            *out_len = 0;
            return SQLI_OK;
        }
        if (sqli_is_lob_like_type((uint8_t)col->type)) {
            uint8_t *lob = NULL;
            size_t lob_len = 0;
            sqli_status frc = sqli_fetchblob_materialize(result, col_index, &lob, &lob_len);
            if (frc == SQLI_OK) {
                if (lob_len == 0 || lob == NULL) {
                    *out_len = 0;
                    free(lob);
                    return SQLI_OK;
                }
                size_t copy_len = *out_len < lob_len ? *out_len : lob_len;
                memcpy(out, lob, copy_len);
                *out_len = copy_len;
                free(lob);
                return SQLI_OK;
            }
            free(lob);
        }
        return sqli_extract_current_value(result, col_index, out, out_len);
    }

    size_t available = result->tuple_len;
    size_t copy_len = *out_len < available ? *out_len : available;
    memcpy(out, result->tuple_buffer, copy_len);
    *out_len = copy_len;
    return SQLI_OK;
}

bool sqli_result_is_null(sqli_result_t *result, int col_index)
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

bool sqli_result_get_bool(sqli_result_t *result, int col_index)
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

static int sqli_qual_length(uint32_t encoded_length)
{
    return (int)((encoded_length >> 8) & 0xFF);
}

static int sqli_qual_start(uint32_t encoded_length)
{
    return (int)((encoded_length >> 4) & 0x0F);
}

static int sqli_qual_end(uint32_t encoded_length)
{
    return (int)(encoded_length & 0x0F);
}

static int sqli_parse_digits_to_int(const char *digits, size_t n)
{
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        if (digits[i] < '0' || digits[i] > '9')
            return 0;
        v = v * 10 + (digits[i] - '0');
    }
    return v;
}

static int sqli_extract_temporal_digits(const uint8_t *raw, size_t len,
                                        uint32_t encoded_length,
                                        char *digits, size_t digits_cap,
                                        int *negative)
{
    uint8_t b100[63];
    size_t ndgts = 0;
    int frac_digits = 0;
    int qlen = sqli_qual_length(encoded_length);
    int qend = sqli_qual_end(encoded_length);
    if (len < 2 || len - 1 > sizeof(b100) || qlen <= 0 ||
        (size_t)qlen >= digits_cap ||
        !sqli_base100_decode_parts(raw, len, b100, &ndgts, &frac_digits, negative))
        return 0;

    /* Temporal decimals are aligned to the qualifier's SECOND position.
     * The wire omits leading zero pairs and adjusts its exponent. Restore
     * those positions before splitting digits into calendar/time fields. */
    int exponent = (int)ndgts - frac_digits / 2;
    int expected_exponent = (qlen + 10 - qend + 1) / 2;
    int leading_pairs = expected_exponent - exponent;
    memset(digits, '0', (size_t)qlen);
    for (size_t i = 0; i < ndgts; i++) {
        if (b100[i] > 99)
            return 0;
        int pos = 2 * (leading_pairs + (int)i);
        if (pos >= 0 && pos < qlen)
            digits[pos] = (char)('0' + b100[i] / 10);
        if (pos + 1 >= 0 && pos + 1 < qlen)
            digits[pos + 1] = (char)('0' + b100[i] % 10);
    }
    digits[qlen] = '\0';
    return qlen;
}

static sqli_status sqli_get_temporal_payload(sqli_result_t *result, int col_index,
                                             uint8_t *raw, size_t *raw_len,
                                             const sqli_column_info **col_out)
{
    if (result == NULL || raw == NULL || raw_len == NULL ||
        col_out == NULL || col_index < 0 || col_index >= result->column_count)
        return SQLI_INVALID_STATE;
    if (result->current_row < 0 || result->tuple_len == 0)
        return SQLI_INVALID_STATE;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null) {
        *raw_len = 0;
        *col_out = &result->columns[(size_t)col_index];
        return SQLI_OK;
    }

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    *col_out = col;
    return sqli_extract_current_value(result, col_index, raw, raw_len);
}

const char *sqli_result_get_decimal_string(sqli_result_t *result, int col_index)
{
    static _Thread_local char out[256];
    out[0] = '\0';

    if (result == NULL || col_index < 0 || col_index >= result->column_count)
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

const char *sqli_result_get_date_string(sqli_result_t *result, int col_index)
{
    static _Thread_local char out[SQLI_TEMPORAL_MAX_TEXT];
    out[0] = '\0';
    if (col_index < 0)
        return out;
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

static bool sqli_format_temporal_fields(char *out, size_t capacity,
                                        const int fields[6], int start, int end,
                                        int fraction, int scale,
                                        bool interval, bool negative)
{
    size_t used = 0;
    bool started = false;
    if (negative) {
        if (capacity < 2)
            return false;
        out[used++] = '-';
    }
    for (int code = start; code <= end && code <= 10; code += 2) {
        const char *separator = "";
        if (started)
            separator = code <= 4 ? "-" : code == 6 ? " " : ":";
        int width = interval && !started ? 1 : code == 0 ? 4 : 2;
        int n = snprintf(out + used, capacity - used, "%s%0*d",
                         separator, width, fields[code / 2]);
        if (n < 0 || (size_t)n >= capacity - used)
            return false;
        used += (size_t)n;
        started = true;
    }
    if (scale > 0) {
        int n = snprintf(out + used, capacity - used, ".%0*d", scale, fraction);
        if (n < 0 || (size_t)n >= capacity - used)
            return false;
    }
    return true;
}

const char *sqli_result_get_datetime_string(sqli_result_t *result, int col_index)
{
    static _Thread_local char out[128];
    out[0] = '\0';
    sqli_datetime_value dt;
    if (sqli_result_get_datetime(result, col_index, &dt) != SQLI_OK || dt.is_null)
        return out;
    const int fields[] = {dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second};
    if (!sqli_format_temporal_fields(out, sizeof(out), fields,
                                    dt.start_qualifier, dt.end_qualifier,
                                    dt.fraction, dt.fraction_scale, false, false))
        out[0] = '\0';
    return out;
}

const char *sqli_result_get_interval_string(sqli_result_t *result, int col_index)
{
    static _Thread_local char out[128];
    out[0] = '\0';
    sqli_interval_value iv;
    if (sqli_result_get_interval(result, col_index, &iv) != SQLI_OK || iv.is_null)
        return out;
    const int fields[] = {iv.year, iv.month, iv.day, iv.hour, iv.minute, iv.second};
    if (!sqli_format_temporal_fields(out, sizeof(out), fields,
                                    iv.start_qualifier, iv.end_qualifier,
                                    iv.fraction, iv.fraction_scale, true, iv.negative))
        out[0] = '\0';
    return out;
}

sqli_status sqli_result_get_datetime(sqli_result_t *result, int col_index,
                                     sqli_datetime_value *out)
{
    if (result == NULL || out == NULL)
        return SQLI_INVALID_STATE;
    memset(out, 0, sizeof(*out));
    out->year = out->month = out->day = -1;
    out->hour = out->minute = out->second = -1;
    out->fraction = 0;
    out->fraction_scale = 0;

    uint8_t raw[128];
    size_t raw_len = sizeof(raw);
    const sqli_column_info *col = NULL;
    sqli_status rc = sqli_get_temporal_payload(result, col_index, raw, &raw_len, &col);
    if (rc != SQLI_OK)
        return rc;
    if (raw_len == 0) {
        out->is_null = 1;
        return SQLI_OK;
    }

    int qlen = sqli_qual_length(col->encoded_length);
    int qstart = sqli_qual_start(col->encoded_length);
    int qend = sqli_qual_end(col->encoded_length);
    int first_width = qlen - (qend - qstart);
    if (first_width <= 0)
        first_width = (qstart == 0) ? 4 : 2;
    out->start_qualifier = (uint8_t)qstart;
    out->end_qualifier = (uint8_t)qend;
    out->first_field_width = (uint8_t)first_width;

    char digits[160];
    int negative = 0;
    int dlen = sqli_extract_temporal_digits(raw, raw_len, col->encoded_length,
                                                digits, sizeof(digits), &negative);
    if (dlen <= 0 || negative)
        return SQLI_PROTO_ERROR;

    int pos = 0;
    int fields_end = qend > 10 ? 10 : qend;
    for (int code = qstart; code <= fields_end; code += 2) {
        int w = (code == qstart) ? first_width : 2;
        if (pos + w > dlen)
            break;
        int val = sqli_parse_digits_to_int(digits + pos, (size_t)w);
        switch (code) {
        case 0: out->year = val; break;
        case 2: out->month = val; break;
        case 4: out->day = val; break;
        case 6: out->hour = val; break;
        case 8: out->minute = val; break;
        case 10: out->second = val; break;
        default: break;
        }
        pos += w;
    }

    if (qend > 10) {
        int frac_w = qend - 10;
        if (frac_w > 0 && pos + frac_w <= dlen) {
            out->fraction = sqli_parse_digits_to_int(digits + pos, (size_t)frac_w);
            out->fraction_scale = frac_w;
        }
    }
    out->is_null = 0;
    return SQLI_OK;
}

sqli_status sqli_result_get_interval(sqli_result_t *result, int col_index,
                                     sqli_interval_value *out)
{
    if (result == NULL || out == NULL)
        return SQLI_INVALID_STATE;
    memset(out, 0, sizeof(*out));
    out->year = out->month = out->day = 0;
    out->hour = out->minute = out->second = 0;

    uint8_t raw[128];
    size_t raw_len = sizeof(raw);
    const sqli_column_info *col = NULL;
    sqli_status rc = sqli_get_temporal_payload(result, col_index, raw, &raw_len, &col);
    if (rc != SQLI_OK)
        return rc;
    if (raw_len == 0) {
        out->is_null = 1;
        return SQLI_OK;
    }

    int qlen = sqli_qual_length(col->encoded_length);
    int qstart = sqli_qual_start(col->encoded_length);
    int qend = sqli_qual_end(col->encoded_length);
    int first_width = qlen - (qend - qstart);
    if (first_width <= 0)
        first_width = 2;
    out->start_qualifier = (uint8_t)qstart;
    out->end_qualifier = (uint8_t)qend;
    out->first_field_width = (uint8_t)first_width;

    if (raw_len < 2)
        return SQLI_PROTO_ERROR;

    int expon = (int8_t)raw[0];
    uint8_t dec_dgts[64];
    size_t dec_ndgts = raw_len - 1;
    if (dec_ndgts > sizeof(dec_dgts))
        dec_ndgts = sizeof(dec_dgts);
    memcpy(dec_dgts, raw + 1, dec_ndgts);

    int negative = 0;
    if ((expon & 0x80) == 0) {
        sqli_base100_complement(dec_dgts, dec_ndgts);
        expon ^= 0x7F;
        negative = 1;
    }
    out->negative = negative ? 1 : 0;
    expon = (expon & 0x7F) - 64;

    while (dec_ndgts > 0 && dec_dgts[dec_ndgts - 1] == 0)
        dec_ndgts--;

    int bexpon = (qlen + 10 - qend + 1) / 2;
    uint8_t dtbuf[32];
    memset(dtbuf, 0, sizeof(dtbuf));
    if (dec_ndgts > 0 && bexpon >= expon) {
        size_t offset = (size_t)(bexpon - expon);
        if (offset < sizeof(dtbuf)) {
            size_t copy = dec_ndgts;
            if (offset + copy > sizeof(dtbuf))
                copy = sizeof(dtbuf) - offset;
            memcpy(dtbuf + offset, dec_dgts, copy);
        }
    }

    int flen = first_width;
    int dtbufIndex = 0;
    int currentField = qstart;

    if (qstart != 12) {
        int i = flen / 2;
        if ((flen & 1) > 0)
            i++;
        int val = 0;
        while (dtbufIndex < i && dtbufIndex < (int)sizeof(dtbuf)) {
            val = val * 100 + dtbuf[dtbufIndex++];
        }
        switch (qstart) {
        case 0: out->year = val; break;
        case 2: out->month = val; break;
        case 4: out->day = val; break;
        case 6: out->hour = val; break;
        case 8: out->minute = val; break;
        case 10: out->second = val; break;
        default: break;
        }
        currentField = qstart + 2;
    }

    int fields_end = qend > 10 ? 10 : qend;
    while (currentField <= fields_end) {
        int val = (dtbufIndex < (int)sizeof(dtbuf)) ? dtbuf[dtbufIndex++] : 0;
        switch (currentField) {
        case 0: out->year = val; break;
        case 2: out->month = val; break;
        case 4: out->day = val; break;
        case 6: out->hour = val; break;
        case 8: out->minute = val; break;
        case 10: out->second = val; break;
        default: break;
        }
        currentField += 2;
    }

    if (qend > 10) {
        int scale = qend - 10;
        int nbytes = (scale + 1) / 2;
        int frac_val = 0;
        for (int b = 0; b < nbytes; b++) {
            frac_val = frac_val * 100 + ((dtbufIndex < (int)sizeof(dtbuf)) ? dtbuf[dtbufIndex++] : 0);
        }
        if (scale & 1)
            frac_val /= 10;
        out->fraction = frac_val;
        out->fraction_scale = scale;
    }

    out->is_null = 0;
    return SQLI_OK;
}

sqli_status sqli_result_stream_bytes(sqli_result_t *result, int col_index,
                                     size_t chunk_size, sqli_stream_chunk_cb cb,
                                     void *ctx)
{
    if (result == NULL || cb == NULL || chunk_size == 0)
        return SQLI_INVALID_STATE;
    if (col_index < 0 || col_index >= result->column_count)
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

sqli_status sqli_result_get_timestamp(sqli_result_t *result, int col_index,
                                      sqli_timestamp_t *out)
{
    if (result == NULL || out == NULL || col_index < 0 || col_index >= result->column_count)
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
        sqli_datetime_value dt;
        sqli_status rc = sqli_result_get_datetime(result, col_index, &dt);
        if (rc != SQLI_OK) return rc;
        out->is_null = dt.is_null;
        if (dt.is_null) return SQLI_OK;

        out->year = (dt.year >= 0) ? dt.year : 1970;
        out->month = (dt.month >= 0) ? dt.month : 1;
        out->day = (dt.day >= 0) ? dt.day : 1;
        out->hour = (dt.hour >= 0) ? dt.hour : 0;
        out->minute = (dt.minute >= 0) ? dt.minute : 0;
        out->second = (dt.second >= 0) ? dt.second : 0;

        int microsecond = 0;
        if (dt.fraction_scale > 0 && dt.fraction_scale <= 5) {
            int scale_factors[6] = { 1000000, 100000, 10000, 1000, 100, 10 };
            microsecond = dt.fraction * scale_factors[dt.fraction_scale];
        }
        out->microsecond = microsecond;
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

sqli_status sqli_result_get_epoch_sec(sqli_result_t *result, int col_index, int64_t *out_sec)
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

sqli_status sqli_result_get_epoch_ms(sqli_result_t *result, int col_index, int64_t *out_ms)
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

sqli_status sqli_result_get_epoch_days(sqli_result_t *result, int col_index, int32_t *out_days)
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
