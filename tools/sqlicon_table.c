#include "libsqli/sqli_temporal.h"
#include "libsqli/sqli_decimal.h"
#include "sqlicon.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Type-specific text access                                        */
/* ---------------------------------------------------------------- */

const char *result_text_by_type(sqli_result_t *result, int col_index, int ctype)
{
    switch (ctype) {
    case SQLI_TYPE_DECIMAL:
    case SQLI_TYPE_MONEY:
        return sqli_result_get_decimal_string(result, col_index);
    case SQLI_TYPE_DATE:
        return sqli_result_get_date_string(result, col_index);
    case SQLI_TYPE_DATETIME:
        return sqli_result_get_datetime_string(result, col_index);
    case SQLI_TYPE_INTERVAL:
        return sqli_result_get_interval_string(result, col_index);
    default:
        return sqli_result_get_string(result, col_index);
    }
}

/* ---------------------------------------------------------------- */
/* Table buffer                                                     */
/* ---------------------------------------------------------------- */

sqli_status table_buffer_init(sqlicon_table_buffer *tb, int cols)
{
    memset(tb, 0, sizeof(*tb));
    if (cols < 0 || (size_t)cols > SIZE_MAX / sizeof(char *) ||
        (size_t)cols > SIZE_MAX / sizeof(size_t))
        return SQLI_OUT_OF_RANGE;
    tb->cols = cols;
    if (cols == 0)
        return SQLI_OK;
    tb->widths = calloc((size_t)cols, sizeof(size_t));
    tb->col_names = calloc((size_t)cols, sizeof(char *));
    return tb->widths != NULL && tb->col_names != NULL ? SQLI_OK : SQLI_ALLOC_FAIL;
}

void table_buffer_destroy(sqlicon_table_buffer *tb)
{
    if (tb == NULL)
        return;
    for (int r = 0; r < tb->rows; r++) {
        for (int c = 0; c < tb->cols; c++)
            free(tb->data[r].cells[c]);
        free(tb->data[r].cells);
        free(tb->data[r].is_null);
    }
    free(tb->data);
    if (tb->col_names != NULL) {
        for (int c = 0; c < tb->cols; c++)
            free(tb->col_names[c]);
    }
    free(tb->col_names);
    free(tb->widths);
    memset(tb, 0, sizeof(*tb));
}

static sqli_status table_buffer_add_row(sqlicon_table_buffer *tb, char **cells, bool *is_null)
{
    if (tb->rows >= tb->cap) {
        enum { initial_rows = 32 };
        if (tb->cap > INT_MAX / 2)
            return SQLI_LIMIT_EXCEEDED;
        int new_cap = tb->cap == 0 ? initial_rows : tb->cap * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(*tb->data))
            return SQLI_LIMIT_EXCEEDED;
        sqlicon_row_data *grown = realloc(tb->data, (size_t)new_cap * sizeof(*grown));
        if (grown == NULL)
            return SQLI_ALLOC_FAIL;
        tb->data = grown;
        tb->cap = new_cap;
    }
    tb->data[tb->rows++] = (sqlicon_row_data){.cells = cells, .is_null = is_null};
    return SQLI_OK;
}

/* Owned display text. LOB placeholders never fetch data or expose locators. */
static sqli_status collect_cell(sqli_result_t *result, size_t column, char **out)
{
    enum { number_capacity = 128 };
    char number[number_capacity];
    const char *text = number;
    int length = 0;
    int32_t integer;
    int64_t wide;
    double floating;
    bool is_null;
    sqli_status status;
    switch (sqli_result_column_type(result, column)) {
    case SQLI_TYPE_SMALLINT: case SQLI_TYPE_INT: case SQLI_TYPE_SERIAL:
    case SQLI_TYPE_BOOL: case SQLI_TYPE_DBOOLEAN:
        status = sqli_result_get_int(result, column, &integer, &is_null);
        if (status != SQLI_OK || is_null)
            return status != SQLI_OK ? status : SQLI_NULL_VALUE;
        length = snprintf(number, sizeof(number), "%d", (int)integer);
        break;
    case SQLI_TYPE_BIGINT: case SQLI_TYPE_BIGSERIAL:
    case SQLI_TYPE_SERIAL8: case SQLI_TYPE_INT8:
        status = sqli_result_get_int64(result, column, &wide, &is_null);
        if (status != SQLI_OK || is_null)
            return status != SQLI_OK ? status : SQLI_NULL_VALUE;
        length = snprintf(number, sizeof(number), "%lld", (long long)wide);
        break;
    case SQLI_TYPE_FLOAT: case SQLI_TYPE_SMFLOAT:
        status = sqli_result_get_double(result, column, &floating, &is_null);
        if (status != SQLI_OK || is_null)
            return status != SQLI_OK ? status : SQLI_NULL_VALUE;
        length = snprintf(number, sizeof(number), "%.17g", floating);
        break;
    case SQLI_TYPE_BLOB: text = "<BLOB>"; break;
    case SQLI_TYPE_CLOB: text = "<CLOB>"; break;
    case SQLI_TYPE_BYTE: text = "<BYTE>"; break;
    default: {
        size_t required = 0;
        bool is_null = false;
        sqli_status status = sqli_result_get_string_len(result, column, NULL, 0, &required, &is_null);
        if (status != SQLI_OK)
            return status;
        if (is_null || required == 0)
            return SQLI_INVALID_STATE;
        char *value = malloc(required);
        if (value == NULL)
            return SQLI_ALLOC_FAIL;
        status = sqli_result_get_string_len(result, column, value, required, &required, &is_null);
        /* Renderers consume C strings; reject embedded NUL instead of losing data. */
        if (status == SQLI_OK && (is_null || required == 0 ||
            memchr(value, '\0', required - 1) != NULL))
            status = SQLI_PROTO_ERROR;
        if (status != SQLI_OK) {
            free(value);
            return status;
        }
        *out = value;
        return SQLI_OK;
    }
    }
    if (length < 0 || (size_t)length >= sizeof(number))
        return SQLI_OUT_OF_RANGE;
    *out = sqlicon_strdup(text);
    return *out != NULL ? SQLI_OK : SQLI_ALLOC_FAIL;
}

sqli_status table_buffer_collect(sqlicon_table_buffer *tb, sqli_result_t *result,
                                 const sqlicon_runtime *rt)
{
    for (int c = 0; c < tb->cols; c++) {
        const char *name = sqli_result_column_name(result, (size_t)c);
        tb->col_names[c] = sqlicon_strdup(name != NULL ? name : "");
        if (tb->col_names[c] == NULL)
            return SQLI_ALLOC_FAIL;
        tb->widths[c] = strlen(tb->col_names[c]);
    }
    sqli_status status;
    while ((status = sqli_result_fetch(result)) == SQLI_OK) {
        char **cells = calloc((size_t)tb->cols, sizeof(char *));
        bool *is_null = calloc((size_t)tb->cols, sizeof(bool));
        if (cells == NULL || is_null == NULL) {
            free(cells);
            free(is_null);
            return SQLI_ALLOC_FAIL;
        }
        for (int c = 0; c < tb->cols; c++) {
            is_null[c] = sqli_result_is_null(result, (size_t)c);
            if (is_null[c]) {
                cells[c] = sqlicon_strdup(rt->null_repr);
                status = cells[c] != NULL ? SQLI_OK : SQLI_ALLOC_FAIL;
            } else {
                status = collect_cell(result, (size_t)c, &cells[c]);
            }
            if (status != SQLI_OK)
                break;
            size_t length = strlen(cells[c]);
            if (length > INT_MAX) {
                status = SQLI_LIMIT_EXCEEDED;
                break;
            }
            if (length > tb->widths[c])
                tb->widths[c] = length;
        }
        if (status == SQLI_OK)
            status = table_buffer_add_row(tb, cells, is_null);
        if (status != SQLI_OK) {
            for (int c = 0; c < tb->cols; c++)
                free(cells[c]);
            free(cells);
            free(is_null);
            return status;
        }
    }
    return status == SQLI_EOF ? SQLI_OK : status;
}
