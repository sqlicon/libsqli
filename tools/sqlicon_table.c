#include "sqlicon.h"

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

void table_buffer_init(sqlicon_table_buffer *tb, int cols)
{
    memset(tb, 0, sizeof(*tb));
    tb->cols = cols;
    tb->widths = calloc((size_t)cols, sizeof(size_t));
    tb->col_names = calloc((size_t)cols, sizeof(char *));
}

void table_buffer_destroy(sqlicon_table_buffer *tb)
{
    if (tb == NULL)
        return;
    for (int r = 0; r < tb->rows; r++) {
        if (tb->data[r].cells == NULL)
            continue;
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
}

static int table_buffer_add_row(sqlicon_table_buffer *tb, char **cells)
{
    if (tb->rows >= tb->cap) {
        int new_cap = (tb->cap == 0) ? 32 : tb->cap * 2;
        sqlicon_row_data *grown = realloc(tb->data, (size_t)new_cap * sizeof(*grown));
        if (grown == NULL)
            return -1;
        tb->data = grown;
        tb->cap = new_cap;
    }
    tb->data[tb->rows].cells = cells;
    tb->rows++;
    return 0;
}

void table_buffer_collect(sqlicon_table_buffer *tb, sqli_result_t *result,
                          const sqlicon_runtime *rt)
{
    for (int c = 0; c < tb->cols; c++) {
        const char *name = sqli_result_column_name(result, c);
        if (name == NULL)
            name = "";
        tb->col_names[c] = strdup(name);
        if (tb->col_names[c] == NULL)
            tb->col_names[c] = strdup("");
        if (tb->col_names[c] == NULL)
            return;
        tb->widths[c] = strlen(tb->col_names[c]);
    }

    while (sqli_result_next(result)) {
        char **cells = calloc((size_t)tb->cols, sizeof(char *));
        bool *is_null = calloc((size_t)tb->cols, sizeof(bool));
        if (cells == NULL)
            break;
        if (is_null == NULL) {
            free(cells);
            break;
        }

        bool row_failed = false;
        for (int i = 0; i < tb->cols; i++) {
            if (sqli_result_is_null(result, i)) {
                cells[i] = strdup(rt->null_repr);
                is_null[i] = true;
            } else {
                int ctype = sqli_result_column_type(result, i);
                char numbuf[128];
                switch (ctype) {
                case SQLI_TYPE_SMALLINT:
                case SQLI_TYPE_INT:
                case SQLI_TYPE_SERIAL:
                case SQLI_TYPE_BOOL:
                case SQLI_TYPE_DBOOLEAN:
                    snprintf(numbuf, sizeof(numbuf), "%d", (int)sqli_result_get_int(result, i));
                    cells[i] = strdup(numbuf);
                    break;
                case SQLI_TYPE_BIGINT:
                case SQLI_TYPE_BIGSERIAL:
                case SQLI_TYPE_SERIAL8:
                case SQLI_TYPE_INT8:
                    snprintf(numbuf, sizeof(numbuf), "%lld", (long long)sqli_result_get_int64(result, i));
                    cells[i] = strdup(numbuf);
                    break;
                case SQLI_TYPE_FLOAT:
                case SQLI_TYPE_SMFLOAT:
                    snprintf(numbuf, sizeof(numbuf), "%.17g", sqli_result_get_double(result, i));
                    cells[i] = strdup(numbuf);
                    break;
                default: {
                    const char *val = result_text_by_type(result, i, ctype);
                    cells[i] = strdup(val != NULL ? val : "");
                    break;
                }
                }
            }
            if (cells[i] == NULL) {
                row_failed = true;
                break;
            }
            size_t vlen = strlen(cells[i]);
            if (vlen > tb->widths[i])
                tb->widths[i] = vlen;
        }

        if (row_failed || table_buffer_add_row(tb, cells) != 0) {
            for (int c = 0; c < tb->cols; c++)
                free(cells[c]);
            free(cells);
            free(is_null);
            break;
        }
        tb->data[tb->rows - 1].is_null = is_null;
    }
}
