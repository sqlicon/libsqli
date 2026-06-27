#include "sqli_internal.h"
#include "sqli_protocol_internal.h"
#include "sqli_result_internal.h"

#include <stdlib.h>

/* Client-compatible SQ_SFETCH direction codes used by IfxResultSet. */
enum {
    SQLI_SFETCH_LAST = 4,
    SQLI_SFETCH_ABSOLUTE = 6
};

void sqli_result_clear_rows(sqli_result_t *result)
{
    if (result == NULL)
        return;
    if (result->rows != NULL) {
        if (!result->rows_use_arena) {
            for (int i = 0; i < result->row_count; i++)
                free(result->rows[i]);
        }
        free(result->rows);
        result->rows = NULL;
    }
    free(result->rows_arena);
    result->rows_arena = NULL;
    result->rows_arena_len = 0;
    result->rows_arena_cap = 0;
    result->rows_use_arena = false;
    free(result->row_lens);
    result->row_lens = NULL;
    result->row_count = 0;
    result->row_capacity = 0;
    result->cursor = -1;
    result->current_row = -1;
    result->tuple_buffer = NULL;
    result->tuple_len = 0;
    result->cur_cache_row = -1;
}

static size_t sqli_fixed_width_for_type(uint8_t type)
{
    switch (type) {
    case SQLI_TYPE_BOOL:     return 1;
    case SQLI_TYPE_DBOOLEAN: return 1;
    case SQLI_TYPE_SMALLINT: return 2;
    case SQLI_TYPE_INT:
    case SQLI_TYPE_DATE:
    case SQLI_TYPE_SERIAL:
    case SQLI_TYPE_BYTE:
    case SQLI_TYPE_SMFLOAT:  return 4;
    case SQLI_TYPE_FLOAT:
    case SQLI_TYPE_BIGINT:
    case SQLI_TYPE_BIGSERIAL:return 8;
    case SQLI_TYPE_INT8:
    case SQLI_TYPE_SERIAL8:  return 10;
    default:                 return 0;
    }
}

static uint8_t sqli_decimal_precision_from_encoded(uint32_t encoded_length)
{
    uint8_t p = (uint8_t)((encoded_length >> 8) & 0xFFu);
    if (p == 0 || p > 32)
        return 0;
    return p;
}

static size_t sqli_decimal_packed_width_from_encoded(uint32_t encoded_length)
{
    uint8_t precision = sqli_decimal_precision_from_encoded(encoded_length);
    if (precision == 0)
        return 0;
    /* Observed SQLI fetch format for DECIMAL/NUMERIC:
     * [exponent/sign byte][BCD digits][padding]
     * width = ceil((precision + 3) / 2). */
    return (size_t)((precision + 3u) / 2u);
}

static size_t sqli_temporal_packed_width_from_encoded(uint8_t type, uint32_t encoded_length)
{
    if (type != SQLI_TYPE_DATETIME && type != SQLI_TYPE_INTERVAL)
        return 0;
    uint8_t qlen = (uint8_t)((encoded_length >> 8) & 0xFFu);
    if (qlen == 0)
        return 0;
    return 1u + (size_t)((qlen + 1u) / 2u);
}

void sqli_base100_complement(uint8_t *digits, size_t ndgts)
{
    uint8_t digit = 100;
    for (size_t i = ndgts; i > 0; i--) {
        size_t p = i - 1;
        if (digits[p] != 0 || digit != 100) {
            digits[p] = (uint8_t)(digit - digits[p]);
            digit = 99;
        }
    }
}

static int sqli_type_is_two_byte_prefixed(uint8_t type)
{
    switch (type) {
    case SQLI_TYPE_TEXT:
    case SQLI_TYPE_LVARCHAR:
    case SQLI_TYPE_BLOB:
    case SQLI_TYPE_CLOB:
    case SQLI_TYPE_DECIMAL:
    case SQLI_TYPE_MONEY:
        return 1;
    default:
        return 0;
    }
}

static int sqli_type_all_zero_is_null_sentinel(uint8_t type)
{
    return type == SQLI_TYPE_DECIMAL || type == SQLI_TYPE_MONEY ||
           type == SQLI_TYPE_DATETIME || type == SQLI_TYPE_INTERVAL;
}

sqli_status sqli_tuple_locate_column(const sqli_column_info *col_info,
                                     const uint8_t *tuple_buf,
                                     size_t tuple_len, size_t *data_start,
                                     size_t *data_len, size_t *span)
{
    if (col_info == NULL || tuple_buf == NULL || data_start == NULL ||
        data_len == NULL || span == NULL)
        return SQLI_INVALID_STATE;

    size_t base = (size_t)col_info->col_start_pos;
    uint8_t type = (uint8_t)col_info->type;
    size_t width = sqli_fixed_width_for_type(type);

    if (base >= tuple_len)
        return SQLI_PROTO_ERROR;

    /* CHAR/NCHAR can arrive in either fixed-width or 1-byte-prefixed layout. */
    if (type == SQLI_TYPE_CHAR || type == SQLI_TYPE_NCHAR) {
        if (width == 0 && col_info->encoded_length > 0)
            width = (size_t)col_info->encoded_length;

        /* Heuristic: live Informix rows often encode CHAR as len8+payload.
         * Accept prefixed layout only for short values to avoid colliding
         * with regular fixed-width text where first byte is printable ASCII. */
        if (base + 1 <= tuple_len) {
            uint8_t len8 = tuple_buf[base];
            size_t payload = (size_t)len8;
            size_t start = base + 1;
            if (len8 > 0 && len8 < 32 && start + payload <= tuple_len) {
                *data_start = start;
                *data_len = payload;
                *span = 1 + payload;
                return SQLI_OK;
            }
        }
    }

    if (type == SQLI_TYPE_DECIMAL || type == SQLI_TYPE_MONEY) {
        size_t packed = sqli_decimal_packed_width_from_encoded(col_info->encoded_length);
        if (packed > 0 && base + packed <= tuple_len) {
            *data_start = base;
            *data_len = packed;
            *span = packed;
            return SQLI_OK;
        }
    }

    if (type == SQLI_TYPE_DATETIME || type == SQLI_TYPE_INTERVAL) {
        size_t packed = sqli_temporal_packed_width_from_encoded(type, col_info->encoded_length);
        if (packed > 0 && base + packed <= tuple_len) {
            *data_start = base;
            *data_len = packed;
            *span = packed;
            return SQLI_OK;
        }
    }

    if (type == SQLI_TYPE_BOOL) {
        /* Informix BOOLEAN (type 41) can be encoded as:
         *   [4-byte zero prefix][1-byte len][payload] */
        if (base + 5 <= tuple_len) {
            uint32_t marker = ((uint32_t)tuple_buf[base] << 24) |
                              ((uint32_t)tuple_buf[base + 1] << 16) |
                              ((uint32_t)tuple_buf[base + 2] << 8) |
                              (uint32_t)tuple_buf[base + 3];
            uint8_t len8 = tuple_buf[base + 4];
            size_t start = base + 5;
            size_t payload = (size_t)len8;
            if (marker == 0 && start + payload <= tuple_len) {
                *data_start = start;
                *data_len = payload;
                *span = 5 + payload;
                return SQLI_OK;
            }
        }
    }

    if (type == SQLI_TYPE_LVARCHAR) {
        /* Observed live LVARCHAR layout:
         *   [4-byte marker][1-byte len][payload]
         * Keep a len16 fallback for server variants. */
        if (base + 5 <= tuple_len) {
            uint32_t marker = ((uint32_t)tuple_buf[base] << 24) |
                              ((uint32_t)tuple_buf[base + 1] << 16) |
                              ((uint32_t)tuple_buf[base + 2] << 8) |
                              (uint32_t)tuple_buf[base + 3];
            uint8_t len8 = tuple_buf[base + 4];
            size_t start = base + 5;
            size_t payload = (size_t)len8;
            if (marker == 0 && start + payload <= tuple_len) {
                *data_start = start;
                *data_len = payload;
                *span = 5 + payload;
                return SQLI_OK;
            }
        }
        if (base + 2 <= tuple_len) {
            uint16_t len16 = (uint16_t)((tuple_buf[base] << 8) | tuple_buf[base + 1]);
            size_t payload = (size_t)len16;
            size_t start = base + 2;
            if (start + payload <= tuple_len) {
                *data_start = start;
                *data_len = payload;
                *span = 2 + payload;
                return SQLI_OK;
            }
        }
        return SQLI_PROTO_ERROR;
    }

    if (width > 0) {
        if (base + width > tuple_len)
            return SQLI_PROTO_ERROR;
        *data_start = base;
        *data_len = width;
        *span = width;
        return SQLI_OK;
    }

    if (type == SQLI_TYPE_VARCHAR || type == SQLI_TYPE_NVCHAR ||
        type == SQLI_TYPE_CHAR || type == SQLI_TYPE_NCHAR ||
        !sqli_type_is_two_byte_prefixed(type)) {
        uint8_t len8 = tuple_buf[base];
        size_t payload = (size_t)len8;
        size_t start = base + 1;
        if (start + payload > tuple_len)
            return SQLI_PROTO_ERROR;
        *data_start = start;
        *data_len = payload;
        *span = 1 + payload;
        return SQLI_OK;
    }

    if (base + 2 > tuple_len)
        return SQLI_PROTO_ERROR;
    uint16_t len16 = (uint16_t)((tuple_buf[base] << 8) | tuple_buf[base + 1]);
    size_t payload = (size_t)len16;
    size_t start = base + 2;
    if (start + payload > tuple_len)
        return SQLI_PROTO_ERROR;
    *data_start = start;
    *data_len = payload;
    *span = 2 + payload;
    return SQLI_OK;
}

bool sqli_is_stringy_type(uint8_t type)
{
    return type == SQLI_TYPE_CHAR || type == SQLI_TYPE_NCHAR ||
           type == SQLI_TYPE_VARCHAR || type == SQLI_TYPE_NVCHAR ||
           type == SQLI_TYPE_LVARCHAR;
}

void sqli_result_prepare_row_cache(sqli_result_t *result)
{
    if (result == NULL || result->column_count <= 0 || result->tuple_buffer == NULL) {
        if (result != NULL)
            result->cur_cache_row = -1;
        return;
    }

    if (result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL) {
        result->cur_col_data_start = calloc((size_t)result->column_count, sizeof(size_t));
        result->cur_col_data_len = calloc((size_t)result->column_count, sizeof(size_t));
        result->cur_col_is_null = calloc((size_t)result->column_count, sizeof(uint8_t));
        if (result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
            result->cur_col_is_null == NULL) {
            free(result->cur_col_data_start);
            free(result->cur_col_data_len);
            free(result->cur_col_is_null);
            result->cur_col_data_start = NULL;
            result->cur_col_data_len = NULL;
            result->cur_col_is_null = NULL;
            result->cur_cache_row = -1;
            return;
        }
    }

    size_t offset = 0;
    for (int i = 0; i < result->column_count; i++) {
        const sqli_column_info *col = &result->columns[i];
        uint8_t type = (uint8_t)col->type;
        size_t data_start = 0, data_len = 0, span = 0;
        result->columns[i].col_start_pos = (uint32_t)offset;
        sqli_status rc = SQLI_OK;

        if (offset >= result->tuple_len) {
            rc = SQLI_PROTO_ERROR;
        } else if (type == SQLI_TYPE_CHAR || type == SQLI_TYPE_NCHAR || type == SQLI_TYPE_BOOL) {
            rc = sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                          &data_start, &data_len, &span);
        } else {
            size_t width = sqli_fixed_width_for_type(type);
            if (width > 0) {
                if (offset + width <= result->tuple_len) {
                    data_start = offset;
                    data_len = width;
                    span = width;
                } else {
                    rc = SQLI_PROTO_ERROR;
                }
            } else if (type == SQLI_TYPE_DECIMAL || type == SQLI_TYPE_MONEY) {
                size_t packed = sqli_decimal_packed_width_from_encoded(col->encoded_length);
                if (packed > 0 && offset + packed <= result->tuple_len) {
                    data_start = offset;
                    data_len = packed;
                    span = packed;
                } else {
                    rc = sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                                  &data_start, &data_len, &span);
                }
            } else if (type == SQLI_TYPE_DATETIME || type == SQLI_TYPE_INTERVAL) {
                size_t packed = sqli_temporal_packed_width_from_encoded(type, col->encoded_length);
                if (packed > 0 && offset + packed <= result->tuple_len) {
                    data_start = offset;
                    data_len = packed;
                    span = packed;
                } else {
                    rc = sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                                  &data_start, &data_len, &span);
                }
            } else if (type == SQLI_TYPE_LVARCHAR) {
                rc = sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                              &data_start, &data_len, &span);
            } else if (type == SQLI_TYPE_VARCHAR || type == SQLI_TYPE_NVCHAR) {
                size_t payload = (size_t)result->tuple_buffer[offset];
                size_t start = offset + 1;
                if (start + payload <= result->tuple_len) {
                    data_start = start;
                    data_len = payload;
                    span = 1 + payload;
                } else {
                    rc = SQLI_PROTO_ERROR;
                }
            } else if (!sqli_type_is_two_byte_prefixed(type)) {
                size_t payload = (size_t)result->tuple_buffer[offset];
                size_t start = offset + 1;
                if (start + payload <= result->tuple_len) {
                    data_start = start;
                    data_len = payload;
                    span = 1 + payload;
                } else {
                    rc = SQLI_PROTO_ERROR;
                }
            } else {
                if (offset + 2 <= result->tuple_len) {
                    uint16_t len16 = (uint16_t)((result->tuple_buffer[offset] << 8) |
                                                result->tuple_buffer[offset + 1]);
                    size_t payload = (size_t)len16;
                    size_t start = offset + 2;
                    if (start + payload <= result->tuple_len) {
                        data_start = start;
                        data_len = payload;
                        span = 2 + payload;
                    } else {
                        rc = SQLI_PROTO_ERROR;
                    }
                } else {
                    rc = SQLI_PROTO_ERROR;
                }
            }
        }
        if (rc != SQLI_OK || span == 0 || data_start > result->tuple_len ||
            data_start + data_len > result->tuple_len) {
            result->cur_col_data_start[i] = result->tuple_len;
            result->cur_col_data_len[i] = 0;
            result->cur_col_is_null[i] = 1;
            offset = result->tuple_len;
            continue;
        }
        result->cur_col_data_start[i] = data_start;
        result->cur_col_data_len[i] = data_len;
        offset += span;

        int is_null = 0;
        if (data_len == 0) {
            if (!sqli_is_stringy_type(type) &&
                (sqli_type_is_two_byte_prefixed(type) ||
                 sqli_type_all_zero_is_null_sentinel(type))) {
                is_null = 1;
            }
        } else {
            const uint8_t *p = result->tuple_buffer + data_start;
            if (type == SQLI_TYPE_DATE && data_len >= 4) {
                int32_t days = (int32_t)(((uint32_t)p[0] << 24) |
                                         ((uint32_t)p[1] << 16) |
                                         ((uint32_t)p[2] << 8)  |
                                         (uint32_t)p[3]);
                if (days == INT32_MIN)
                    is_null = 1;
            } else if ((type == SQLI_TYPE_INT8 || type == SQLI_TYPE_SERIAL8) &&
                       data_len >= 2 && p[0] == 0 && p[1] == 0) {
                is_null = 1;
            } else if (sqli_type_all_zero_is_null_sentinel(type)) {
                int all_zero = 1;
                for (size_t k = 0; k < data_len; k++) {
                    if (p[k] != 0) {
                        all_zero = 0;
                        break;
                    }
                }
                if (all_zero)
                    is_null = 1;
            }
        }
        result->cur_col_is_null[i] = (uint8_t)is_null;
    }
    result->cur_cache_row = result->current_row;
}

bool sqli_result_is_null_internal(sqli_result_t *result, int col_index)
{
    if (result == NULL || col_index < 0 || col_index >= result->column_count)
        return 1;
    if (result->current_row < 0 || result->tuple_buffer == NULL || result->tuple_len == 0)
        return 1;

    if (result->cur_cache_row != result->current_row)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row == result->current_row && result->cur_col_is_null != NULL)
        return result->cur_col_is_null[col_index] ? 1 : 0;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    size_t data_start = 0, data_len = 0, span = 0;
    if (sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                 &data_start, &data_len, &span) != SQLI_OK)
        return 1;

    uint8_t type = (uint8_t)col->type;
    if (data_len == 0) {
        if (sqli_is_stringy_type(type))
            return 0;
        if (sqli_type_is_two_byte_prefixed(type) ||
            sqli_type_all_zero_is_null_sentinel(type))
            return 1;
        return 0;
    }

    const uint8_t *p = result->tuple_buffer + data_start;
    if (type == SQLI_TYPE_DATE && data_len >= 4) {
        int32_t days = (int32_t)(((uint32_t)p[0] << 24) |
                                 ((uint32_t)p[1] << 16) |
                                 ((uint32_t)p[2] << 8)  |
                                 (uint32_t)p[3]);
        if (days == INT32_MIN)
            return 1;
    }
    if ((type == SQLI_TYPE_INT8 || type == SQLI_TYPE_SERIAL8) &&
        data_len >= 2 && p[0] == 0 && p[1] == 0)
        return 1;

    int all_zero = 1;
    for (size_t i = 0; i < data_len; i++) {
        if (p[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    if (all_zero && sqli_type_all_zero_is_null_sentinel(type))
        return 1;
    return 0;
}

bool sqli_result_next(sqli_result_t *result)
{
    if (result == NULL)
        return false;
    int next = result->cursor + 1;
    if (next >= result->row_count)
        return false;
    result->cursor = next;
    result->tuple_buffer = result->rows[next];
    result->tuple_len = result->row_lens[next];
    result->current_row = next;
    result->cur_cache_row = -1;
    sqli_result_prepare_row_cache(result);
    return true;
}

static bool sqli_result_move_to_index(sqli_result_t *result, int row_index)
{
    if (result == NULL)
        return false;
    if (row_index < 0 || row_index >= result->row_count)
        return false;

    result->cursor = row_index;
    result->tuple_buffer = result->rows[row_index];
    result->tuple_len = result->row_lens[row_index];
    result->current_row = row_index;
    result->cur_cache_row = -1;
    sqli_result_prepare_row_cache(result);
    return true;
}

static bool sqli_result_is_server_scrollable(const sqli_result_t *result)
{
    if (result == NULL || result->owner_conn == NULL)
        return false;
    if (result->cursor_type != SQLI_CURSOR_SCROLL_INSENSITIVE)
        return false;
    if (result->statement_type != 2 || result->stmt_id < 0)
        return false;
    if (result->owner_conn->state != SQLI_CONN_READY || result->owner_conn->socket_fd < 0)
        return false;
    return true;
}

static bool sqli_result_server_refetch(sqli_result_t *result, uint16_t scroll_type, int32_t index)
{
    if (!sqli_result_is_server_scrollable(result))
        return false;

    sqli_result_clear_rows(result);
    result->eof = 0;
    sqli_status rc = sqli_send_scroll_fetch(result->owner_conn->socket_fd, result->stmt_id,
                                            result, scroll_type, index);
    if (rc != SQLI_OK)
        return false;

    rc = sqli_receive_dispatch(result->owner_conn->socket_fd, result, result->owner_conn);
    if (rc != SQLI_OK || result->row_count <= 0)
        return false;

    return sqli_result_move_to_index(result, 0);
}

bool sqli_result_previous(sqli_result_t *result)
{
    if (result == NULL)
        return false;
    return sqli_result_move_to_index(result, result->cursor - 1);
}

bool sqli_result_first(sqli_result_t *result)
{
    if (sqli_result_server_refetch(result, SQLI_SFETCH_ABSOLUTE, 1))
        return true;
    return sqli_result_move_to_index(result, 0);
}

bool sqli_result_last(sqli_result_t *result)
{
    if (sqli_result_server_refetch(result, SQLI_SFETCH_LAST, 0))
        return true;
    if (result == NULL || result->row_count <= 0)
        return false;
    return sqli_result_move_to_index(result, result->row_count - 1);
}

bool sqli_result_absolute(sqli_result_t *result, int row_1based)
{
    if (result == NULL)
        return false;
    if (row_1based <= 0)
        return false;
    if (sqli_result_server_refetch(result, SQLI_SFETCH_ABSOLUTE, row_1based))
        return true;
    return sqli_result_move_to_index(result, row_1based - 1);
}

bool sqli_result_relative(sqli_result_t *result, int offset)
{
    if (result == NULL)
        return false;
    if (result->cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
        int row = sqli_result_row_number(result);
        if (row > 0) {
            int target = row + offset;
            if (target > 0 && sqli_result_server_refetch(result, SQLI_SFETCH_ABSOLUTE, target))
                return true;
        }
    }
    return sqli_result_move_to_index(result, result->cursor + offset);
}

int sqli_result_row_number(sqli_result_t *result)
{
    if (result == NULL || result->cursor < 0)
        return 0;
    return result->cursor + 1;
}

int64_t sqli_result_rows_affected(sqli_result_t *result)
{
    if (result == NULL)
        return 0;
    return result->rows_affected;
}

bool sqli_result_has_generated_serial(sqli_result_t *result)
{
    if (result == NULL)
        return false;
    return result->has_generated_serial;
}

int64_t sqli_result_generated_serial(sqli_result_t *result)
{
    if (result == NULL || !result->has_generated_serial)
        return 0;
    return result->generated_serial;
}

bool sqli_result_has_generated_serial8(sqli_result_t *result)
{
    if (result == NULL)
        return false;
    return result->has_generated_serial8;
}

int64_t sqli_result_generated_serial8(sqli_result_t *result)
{
    if (result == NULL || !result->has_generated_serial8)
        return 0;
    return result->generated_serial8;
}

int sqli_result_columns(sqli_result_t *result)
{
    if (result == NULL)
        return 0;
    return result->column_count;
}

void sqli_result_destroy(sqli_result_t *result)
{
    if (result == NULL)
        return;
    if (result->owner_conn != NULL &&
        result->owner_conn->state == SQLI_CONN_READY &&
        result->statement_type == 2 && result->stmt_id >= 0) {
        sqli_stmt_close_release(result->owner_conn, result->stmt_id);
    }
    sqli_result_cleanup(result);
    free(result);
}

/* ----------------------------------------------------------------
 * Column value extraction from tuple buffer
 *
 * Fixed-width types: SMALLINT=2, INT/SERIAL/DATE/BYTE=4, INT8=10,
 *   FLOAT=8, SMFLOAT=4, BIGINT/BIGSERIAL=8, BOOL=1
 * Variable-width (1-byte prefix): VARCHAR, NVCHAR
 * Variable-width (2-byte prefix): CHAR, TEXT, BLOB, CLOB, INTERVAL,
 *   NCHAR, DECIMAL, MONEY, DATETIME
 * ---------------------------------------------------------------- */
