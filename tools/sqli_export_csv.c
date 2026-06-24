#define _POSIX_C_SOURCE 200809L
#include "libsqli/sqli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>

static void csv_write_field(FILE *f, const char *s)
{
    if (f == NULL || s == NULL) {
        return;
    }
    size_t len = strlen(s);
    if (len == 0)
        return;

    if (strpbrk(s, ",\"\n\r") == NULL) {
        (void)fwrite(s, 1, len, f);
        return;
    }

    fputc('"', f);
    const char *p = s;
    const char *seg = s;
    while (*p != '\0') {
        if (*p == '"') {
            size_t seg_len = (size_t)(p - seg);
            if (seg_len > 0)
                (void)fwrite(seg, 1, seg_len, f);
            (void)fwrite("\"\"", 1, 2, f);
            p++;
            seg = p;
            continue;
        }
        p++;
    }
    if (p > seg)
        (void)fwrite(seg, 1, (size_t)(p - seg), f);
    fputc('"', f);
}

static const char *to_bool_text(bool value)
{
    return value ? "true" : "false";
}

static void write_cell(FILE *out, sqli_result_t *res, int col, int ctype,
                       char *num_buf, size_t num_cap)
{
    if (out == NULL || res == NULL || num_buf == NULL || num_cap == 0)
        return;

    switch (ctype) {
    case SQLI_TYPE_SMALLINT:
    case SQLI_TYPE_INT:
    case SQLI_TYPE_SERIAL: {
        int v = (int)sqli_result_get_int(res, col);
        if (sqli_result_was_null(res))
            return;
        snprintf(num_buf, num_cap, "%d", v);
        csv_write_field(out, num_buf);
        return;
    }
    case SQLI_TYPE_BIGINT:
    case SQLI_TYPE_BIGSERIAL:
    case SQLI_TYPE_SERIAL8:
    case SQLI_TYPE_INT8: {
        int64_t v = (int64_t)sqli_result_get_int64(res, col);
        if (sqli_result_was_null(res))
            return;
        snprintf(num_buf, num_cap, "%" PRId64, v);
        csv_write_field(out, num_buf);
        return;
    }
    case SQLI_TYPE_FLOAT:
    case SQLI_TYPE_SMFLOAT: {
        double v = sqli_result_get_double(res, col);
        if (sqli_result_was_null(res))
            return;
        snprintf(num_buf, num_cap, "%.17g", v);
        csv_write_field(out, num_buf);
        return;
    }
    case SQLI_TYPE_DECIMAL:
    case SQLI_TYPE_MONEY: {
        const char *s = sqli_result_get_decimal_string(res, col);
        if (sqli_result_was_null(res))
            return;
        csv_write_field(out, s ? s : "");
        return;
    }
    case SQLI_TYPE_DATE: {
        const char *s = sqli_result_get_date_string(res, col);
        if (sqli_result_was_null(res))
            return;
        csv_write_field(out, s ? s : "");
        return;
    }
    case SQLI_TYPE_DATETIME: {
        const char *s = sqli_result_get_datetime_string(res, col);
        if (sqli_result_was_null(res))
            return;
        csv_write_field(out, s ? s : "");
        return;
    }
    case SQLI_TYPE_INTERVAL: {
        const char *s = sqli_result_get_interval_string(res, col);
        if (sqli_result_was_null(res))
            return;
        csv_write_field(out, s ? s : "");
        return;
    }
    case SQLI_TYPE_BOOL:
    case SQLI_TYPE_DBOOLEAN: {
        bool v = sqli_result_get_bool(res, col);
        if (sqli_result_was_null(res))
            return;
        csv_write_field(out, to_bool_text(v));
        return;
    }
    default: {
        const char *s = sqli_result_get_string(res, col);
        if (sqli_result_was_null(res))
            return;
        csv_write_field(out, s ? s : "");
        return;
    }
    }
}

static unsigned parse_u_env(const char *name, unsigned dflt)
{
    const char *v = getenv(name);
    if (v == NULL || *v == '\0')
        return dflt;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (end == v || *end != '\0' || n == 0 || n > 1000000ul)
        return dflt;
    return (unsigned)n;
}

static bool parse_bool_env(const char *name, bool dflt)
{
    const char *v = getenv(name);
    if (v == NULL || *v == '\0')
        return dflt;
    if (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
        strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0)
        return true;
    if (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
        strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0)
        return false;
    return dflt;
}

typedef struct {
    FILE *out;
    bool wrote_header;
    unsigned long rows;
    int col_count;
    int *col_types;
} export_stream_ctx;

static void export_stream_ctx_free(export_stream_ctx *ctx)
{
    if (ctx == NULL)
        return;
    free(ctx->col_types);
    ctx->col_types = NULL;
    ctx->col_count = 0;
}

static int export_row_cb(sqli_result_t *res, void *ctx_ptr)
{
    export_stream_ctx *ctx = (export_stream_ctx *)ctx_ptr;
    if (ctx == NULL || ctx->out == NULL || res == NULL)
        return 1;

    int cols = sqli_result_columns(res);
    if (ctx->col_count == 0) {
        ctx->col_count = cols;
        ctx->col_types = calloc((size_t)cols, sizeof(*ctx->col_types));
        if (ctx->col_types == NULL)
            return 1;
        for (int c = 0; c < cols; c++)
            ctx->col_types[c] = sqli_result_column_type(res, c);
    }

    if (!ctx->wrote_header) {
        for (int c = 0; c < cols; c++) {
            if (c > 0)
                fputc(',', ctx->out);
            csv_write_field(ctx->out, sqli_result_column_name(res, c));
        }
        fputc('\n', ctx->out);
        ctx->wrote_header = true;
    }

    char num_buf[128];
    for (int c = 0; c < cols; c++) {
        if (c > 0)
            fputc(',', ctx->out);
        int ctype = (ctx->col_types != NULL && c < ctx->col_count)
            ? ctx->col_types[c] : sqli_result_column_type(res, c);
        write_cell(ctx->out, res, c, ctype, num_buf, sizeof(num_buf));
    }
    fputc('\n', ctx->out);
    ctx->rows++;
    return 0;
}

static const char *parse_s_env(const char *name)
{
    const char *v = getenv(name);
    return (v != NULL && *v != '\0') ? v : NULL;
}

int main(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr,
                "usage: %s <host> <port> <database> <user> <password> <sql|TABLE:table> <out.csv> [server]\n",
                argv[0]);
        return 2;
    }

    const char *host = argv[1];
    const char *port = argv[2];
    const char *database = argv[3];
    const char *user = argv[4];
    const char *password = argv[5];
    const char *sql = argv[6];
    const char *out_path = argv[7];
    const char *server = (argc >= 9) ? argv[8] : "";

    const char *client_locale = getenv("SQLI_CLIENT_LOCALE");
    const char *db_locale = getenv("SQLI_DB_LOCALE");
    if (client_locale == NULL || *client_locale == '\0')
        client_locale = "en_US.utf8";
    if (db_locale == NULL || *db_locale == '\0')
        db_locale = "de_DE.CP1252";

    sqli_conn_t *conn = NULL;
    FILE *out = NULL;
    int rc = 1;

    if (sqli_create(&conn) != SQLI_OK || conn == NULL) {
        fprintf(stderr, "sqli_create failed\n");
        goto cleanup;
    }

    sqli_connect_params p;
    memset(&p, 0, sizeof(p));
    p.hostname = host;
    p.service = port;
    p.server = server;
    p.database = database;
    p.username = user;
    p.password = password;
    p.client_locale = client_locale;
    p.db_locale = db_locale;

    if (sqli_connect(conn, &p) != SQLI_OK) {
        fprintf(stderr, "connect failed: %s\n", sqli_error(conn));
        goto cleanup;
    }

    out = fopen(out_path, "w");
    if (out == NULL) {
        fprintf(stderr, "cannot open output file: %s\n", out_path);
        goto cleanup;
    }
    (void)setvbuf(out, NULL, _IOFBF, 1u << 20);

    if (strncmp(sql, "TABLE:", 6) == 0) {
        const char *table = sql + 6;
        if (*table == '\0') {
            fprintf(stderr, "invalid TABLE: syntax\n");
            goto cleanup;
        }

        unsigned page_size = parse_u_env("SQLI_EXPORT_PAGE_SIZE", 5000);
        bool use_paging = parse_bool_env("SQLI_EXPORT_USE_PAGING", false);
        const char *order_by = parse_s_env("SQLI_EXPORT_ORDER_BY");
        unsigned offset = 0;
        char qbuf[1024];
        export_stream_ctx sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.out = out;

        if (!use_paging) {
            int64_t delivered = 0;
            if (order_by != NULL) {
                snprintf(qbuf, sizeof(qbuf), "SELECT * FROM %s ORDER BY %s", table, order_by);
            } else {
                snprintf(qbuf, sizeof(qbuf), "SELECT * FROM %s", table);
            }
            if (sqli_query_stream(conn, qbuf, export_row_cb, &sctx, &delivered) != SQLI_OK) {
                fprintf(stderr, "query failed: %s\n", sqli_error(conn));
                export_stream_ctx_free(&sctx);
                goto cleanup;
            }
            fprintf(stderr, "exported_rows=%lu\n", sctx.rows);
            export_stream_ctx_free(&sctx);
        } else for (;;) {
            int64_t delivered = 0;
            if (order_by != NULL) {
                snprintf(qbuf, sizeof(qbuf),
                         "SELECT SKIP %u FIRST %u * FROM %s ORDER BY %s",
                         offset, page_size, table, order_by);
            } else {
                snprintf(qbuf, sizeof(qbuf),
                         "SELECT SKIP %u FIRST %u * FROM %s",
                         offset, page_size, table);
            }
            if (sqli_query_stream(conn, qbuf, export_row_cb, &sctx, &delivered) != SQLI_OK) {
                fprintf(stderr, "query failed: %s\n", sqli_error(conn));
                export_stream_ctx_free(&sctx);
                goto cleanup;
            }

            int got = (int)delivered;
            fprintf(stderr, "exported_rows=%lu page_rows=%d offset=%u\n",
                    sctx.rows, got, offset);
            fflush(stderr);

            if (got <= 0 || (unsigned)got < page_size)
                break;
            offset += (unsigned)got;
            fflush(out);
        }
        export_stream_ctx_free(&sctx);
    } else {
        export_stream_ctx sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.out = out;
        int64_t delivered = 0;

        if (sqli_query_stream(conn, sql, export_row_cb, &sctx, &delivered) != SQLI_OK) {
            fprintf(stderr, "query failed: %s\n", sqli_error(conn));
            export_stream_ctx_free(&sctx);
            goto cleanup;
        }
        fprintf(stderr, "exported_rows=%lu\n", sctx.rows);
        export_stream_ctx_free(&sctx);
    }

    rc = 0;

cleanup:
    if (out != NULL)
        fclose(out);
    if (conn != NULL) {
        sqli_close(conn);
        sqli_destroy(conn);
    }
    return rc;
}
