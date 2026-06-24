#include "sqlicon.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------------------------------------------------------------- */
/* Value formatters                                                 */
/* ---------------------------------------------------------------- */

static void print_csv_value(FILE *out, const char *s)
{
    bool need_quote = false;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            need_quote = true;
            break;
        }
    }
    if (!need_quote) {
        fputs(s, out);
        return;
    }
    fputc('"', out);
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '"')
            fputc('"', out);
        fputc(*p, out);
    }
    fputc('"', out);
}

static void print_json_string(FILE *out, const char *s)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        unsigned char ch = *p;
        switch (ch) {
        case '\"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\b':
            fputs("\\b", out);
            break;
        case '\f':
            fputs("\\f", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (ch < 0x20) {
                fprintf(out, "\\u%04x", (unsigned int)ch);
            } else {
                fputc((int)ch, out);
            }
            break;
        }
    }
    fputc('"', out);
}

static void print_markdown_cell(FILE *out, const char *s)
{
    if (s == NULL)
        return;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '|')
            fputs("\\|", out);
        else if (*p == '\n' || *p == '\r')
            fputs("<br>", out);
        else
            fputc(*p, out);
    }
}

/* ---------------------------------------------------------------- */
/* Output mode renderers                                            */
/* ---------------------------------------------------------------- */

static void print_aligned(FILE *out, const sqlicon_runtime *rt, const sqlicon_table_buffer *tb)
{
    size_t widths[256];
    for (int c = 0; c < tb->cols && c < 256; c++) {
        widths[c] = tb->widths[c];
        if (rt->width_overrides[c] > 0 && (size_t)rt->width_overrides[c] > widths[c])
            widths[c] = (size_t)rt->width_overrides[c];
    }

    if (rt->headers) {
        for (int c = 0; c < tb->cols; c++) {
            size_t w = (c < 256) ? widths[c] : tb->widths[c];
            fprintf(out, "%-*s", (int)w, tb->col_names[c]);
            if (c + 1 < tb->cols)
                fputs(" | ", out);
        }
        fputc('\n', out);
        for (int c = 0; c < tb->cols; c++) {
            size_t w = (c < 256) ? widths[c] : tb->widths[c];
            for (size_t i = 0; i < w; i++)
                fputc('-', out);
            if (c + 1 < tb->cols)
                fputs("-+-", out);
        }
        fputc('\n', out);
    }

    for (int r = 0; r < tb->rows; r++) {
        for (int c = 0; c < tb->cols; c++) {
            size_t w = (c < 256) ? widths[c] : tb->widths[c];
            fprintf(out, "%-*s", (int)w, tb->data[r].cells[c]);
            if (c + 1 < tb->cols)
                fputs(" | ", out);
        }
        fputc('\n', out);
    }
}

static void print_csv_mode(FILE *out, const sqlicon_runtime *rt, const sqlicon_table_buffer *tb)
{
    if (rt->headers) {
        for (int c = 0; c < tb->cols; c++) {
            print_csv_value(out, tb->col_names[c]);
            if (c + 1 < tb->cols)
                fputc(',', out);
        }
        fputc('\n', out);
    }

    for (int r = 0; r < tb->rows; r++) {
        for (int c = 0; c < tb->cols; c++) {
            print_csv_value(out, tb->data[r].cells[c]);
            if (c + 1 < tb->cols)
                fputc(',', out);
        }
        fputc('\n', out);
    }
}

static void print_line(FILE *out, const sqlicon_table_buffer *tb)
{
    for (int r = 0; r < tb->rows; r++) {
        for (int c = 0; c < tb->cols; c++)
            fprintf(out, "%s = %s\n", tb->col_names[c], tb->data[r].cells[c]);
        fputc('\n', out);
    }
}

static void print_json(FILE *out, const sqlicon_table_buffer *tb)
{
    fputc('[', out);
    for (int r = 0; r < tb->rows; r++) {
        if (r > 0)
            fputc(',', out);
        fputc('{', out);
        for (int c = 0; c < tb->cols; c++) {
            if (c > 0)
                fputc(',', out);
            print_json_string(out, tb->col_names[c]);
            fputc(':', out);
            if (tb->data[r].is_null != NULL && tb->data[r].is_null[c]) {
                fputs("null", out);
            } else {
                print_json_string(out, tb->data[r].cells[c] != NULL ? tb->data[r].cells[c] : "");
            }
        }
        fputc('}', out);
    }
    fputs("]\n", out);
}

static void print_markdown(FILE *out, const sqlicon_runtime *rt, const sqlicon_table_buffer *tb)
{
    if (rt->headers) {
        fputc('|', out);
        for (int c = 0; c < tb->cols; c++) {
            fputc(' ', out);
            print_markdown_cell(out, tb->col_names[c]);
            fputc(' ', out);
            fputc('|', out);
        }
        fputc('\n', out);

        fputc('|', out);
        for (int c = 0; c < tb->cols; c++) {
            fputs(" --- |", out);
        }
        fputc('\n', out);
    }

    for (int r = 0; r < tb->rows; r++) {
        fputc('|', out);
        for (int c = 0; c < tb->cols; c++) {
            fputc(' ', out);
            print_markdown_cell(out, tb->data[r].cells[c]);
            fputc(' ', out);
            fputc('|', out);
        }
        fputc('\n', out);
    }
}

/* ---------------------------------------------------------------- */
/* Public API                                                       */
/* ---------------------------------------------------------------- */

void print_result_rows(FILE *out, const sqlicon_runtime *rt, sqli_result_t *result)
{
    int cols = sqli_result_columns(result);
    sqlicon_table_buffer tb;
    table_buffer_init(&tb, cols);
    table_buffer_collect(&tb, result, rt);

    switch (rt->mode) {
    case SQLICON_OUTPUT_ALIGNED:
        print_aligned(out, rt, &tb);
        break;
    case SQLICON_OUTPUT_CSV:
        print_csv_mode(out, rt, &tb);
        break;
    case SQLICON_OUTPUT_LINE:
        print_line(out, &tb);
        break;
    case SQLICON_OUTPUT_JSON:
        print_json(out, &tb);
        break;
    case SQLICON_OUTPUT_MARKDOWN:
        print_markdown(out, rt, &tb);
        break;
    }

    table_buffer_destroy(&tb);
}
