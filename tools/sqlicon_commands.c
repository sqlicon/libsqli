#include "sqlicon.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- */
/* Shared command helpers                                           */
/* ---------------------------------------------------------------- */

static void print_sql_string_literal(FILE *out, const char *s)
{
    fputc('\'', out);
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\'')
            fputc('\'', out);
        fputc(*p, out);
    }
    fputc('\'', out);
}

static sqlicon_exit_code parse_required_identifier_arg(const char *arg,
                                                       char *out_ident,
                                                       size_t out_ident_cap,
                                                       const char *cmd_name)
{
    const char *p = skip_spaces_str(arg);
    if (*p == '\0') {
        fprintf(stderr, "error: .%s requires table name\n", cmd_name);
        return SQLICON_EXIT_MISUSE;
    }
    size_t len = 0;
    while (p[len] != '\0' && !is_space_char(p[len]))
        len++;
    if (len == 0 || len >= out_ident_cap) {
        fprintf(stderr, "error: .%s invalid table name\n", cmd_name);
        return SQLICON_EXIT_MISUSE;
    }
    memcpy(out_ident, p, len);
    out_ident[len] = '\0';
    if (!is_simple_sql_identifier(out_ident)) {
        fprintf(stderr, "error: .%s expects simple identifier [A-Za-z_][A-Za-z0-9_]*\n",
                cmd_name);
        return SQLICON_EXIT_MISUSE;
    }
    return SQLICON_EXIT_OK;
}

static bool append_sql_identifier_raw(char *dst, size_t dst_cap, size_t *pos, const char *ident)
{
    if (!is_simple_sql_identifier(ident))
        return false;
    size_t len = strlen(ident);
    if (*pos >= dst_cap || len >= dst_cap - *pos)
        return false;
    memcpy(dst + *pos, ident, len);
    *pos += len;
    dst[*pos] = '\0';
    return true;
}

/* ---------------------------------------------------------------- */
/* .width                                                           */
/* ---------------------------------------------------------------- */

sqlicon_exit_code command_width(sqlicon_runtime *rt, const char *arg)
{
    const char *p = skip_spaces_str(arg);
    if (*p == '\0') {
        fprintf(stderr, "error: .width expects 'IDX WIDTH' or 'off'\n");
        return SQLICON_EXIT_MISUSE;
    }
    if (strcmp(p, "off") == 0 || strcmp(p, "reset") == 0) {
        memset(rt->width_overrides, 0, sizeof(rt->width_overrides));
        return SQLICON_EXIT_OK;
    }

    char *end = NULL;
    long idx = strtol(p, &end, 10);
    if (end == p || !is_space_char(*end)) {
        fprintf(stderr, "error: .width expects numeric IDX WIDTH\n");
        return SQLICON_EXIT_MISUSE;
    }
    const char *wstr = skip_spaces_str(end);
    if (*wstr == '\0') {
        fprintf(stderr, "error: .width expects WIDTH\n");
        return SQLICON_EXIT_MISUSE;
    }
    end = NULL;
    long width = strtol(wstr, &end, 10);
    if (end == wstr || *skip_spaces_str(end) != '\0') {
        fprintf(stderr, "error: .width expects numeric WIDTH\n");
        return SQLICON_EXIT_MISUSE;
    }
    if (idx < 1 || idx > 256 || width < 0 || width > 10000) {
        fprintf(stderr, "error: .width range IDX=1..256 WIDTH=0..10000\n");
        return SQLICON_EXIT_MISUSE;
    }
    rt->width_overrides[idx - 1] = (int)width;
    return SQLICON_EXIT_OK;
}

/* ---------------------------------------------------------------- */
/* .use                                                             */
/* ---------------------------------------------------------------- */

sqlicon_exit_code command_use_database(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg)
{
    char db[256];
    sqlicon_exit_code rc = parse_required_identifier_arg(arg, db, sizeof(db), "use");
    if (rc != SQLICON_EXIT_OK)
        return rc;

    char sql[320];
    if (snprintf(sql, sizeof(sql), "DATABASE %s", db) >= (int)sizeof(sql)) {
        fprintf(stderr, "error: .use SQL too long\n");
        return SQLICON_EXIT_MISUSE;
    }
    return execute_sql_statement(conn, sql, false, false, rt);
}

/* ---------------------------------------------------------------- */
/* .databases                                                       */
/* ---------------------------------------------------------------- */

sqlicon_exit_code command_list_databases(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg)
{
    const char *p = skip_spaces_str(arg);
    if (*p != '\0') {
        fprintf(stderr, "error: .databases takes no arguments\n");
        return SQLICON_EXIT_MISUSE;
    }
    return execute_sql_statement(
        conn,
        "SELECT name FROM sysmaster:sysdatabases ORDER BY name",
        false, false, rt);
}

/* ---------------------------------------------------------------- */
/* .status                                                          */
/* ---------------------------------------------------------------- */

sqlicon_exit_code command_status(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg)
{
    const char *p = skip_spaces_str(arg);
    if (*p != '\0') {
        fprintf(stderr, "error: .status takes no arguments\n");
        return SQLICON_EXIT_MISUSE;
    }
    sqlicon_exit_code rc = execute_sql_statement(
        conn,
        "SELECT DBINFO('dbname') AS dbname, USER AS session_user, "
        "(SELECT COUNT(*) FROM systables WHERE tabid >= 100 AND tabtype = 'T') AS user_tables "
        "FROM systables WHERE tabid = 1",
        false, false, rt);
    if (rc != SQLICON_EXIT_OK)
        return rc;

    bool close_after = false;
    FILE *out = runtime_acquire_output(rt, &close_after);
    if (out == NULL) {
        fprintf(stderr, "error: cannot open output destination\n");
        return SQLICON_EXIT_MISUSE;
    }
    fprintf(out, "mode=%s\n", output_mode_name(rt->mode));
    fprintf(out, "headers=%s\n", rt->headers ? "on" : "off");
    fprintf(out, "bail=%s\n", rt->bail_on_error ? "on" : "off");
    fprintf(out, "timer=%s\n", rt->timer_on ? "on" : "off");
    fprintf(out, "nullvalue=%s\n", rt->null_repr);
    fprintf(out, "output=%s\n",
            rt->out_owned ? rt->out_path : "stdout");
    fprintf(out, "once=%s\n",
            (rt->once_path[0] != '\0') ? rt->once_path : "(none)");
    fprintf(out, "host=%s\n", rt->conn_host);
    fprintf(out, "port=%s\n", rt->conn_port);
    fprintf(out, "server=%s\n", rt->conn_server);
    fprintf(out, "database=%s\n", rt->conn_database);
    fprintf(out, "user=%s\n", rt->conn_user);
    fprintf(out, "client_locale=%s\n", rt->conn_client_locale);
    fprintf(out, "db_locale=%s\n", rt->conn_db_locale);
    fprintf(out, "profile=%s\n",
            rt->conn_profile[0] != '\0' ? rt->conn_profile : "(none)");
    fflush(out);
    runtime_release_output(out, close_after);
    return SQLICON_EXIT_OK;
}

/* ---------------------------------------------------------------- */
/* SQL type name formatter (like dbweb coltype.ec)                  */
/* ---------------------------------------------------------------- */

static void format_sql_type(int coltype, int collength, char *buf, size_t buf_cap)
{
    int base = coltype & 0xFFFFFEFF; /* mask null bit */
    buf[0] = '\0';

    switch (base) {
    case 0: snprintf(buf, buf_cap, "CHAR(%d)", collength); break;
    case 1: snprintf(buf, buf_cap, "SMALLINT"); break;
    case 2: snprintf(buf, buf_cap, "INTEGER"); break;
    case 3: snprintf(buf, buf_cap, "FLOAT"); break;
    case 4: snprintf(buf, buf_cap, "SMALLFLOAT"); break;
    case 5: { /* DECIMAL */
        int m = collength / 256;
        int n = collength % 256;
        if (n == 255 || n == 0)
            snprintf(buf, buf_cap, "DECIMAL(%d)", m);
        else
            snprintf(buf, buf_cap, "DECIMAL(%d,%d)", m, n);
        break;
    }
    case 6: snprintf(buf, buf_cap, "SERIAL"); break;
    case 7: snprintf(buf, buf_cap, "DATE"); break;
    case 8: { /* MONEY */
        int m = collength / 256;
        int n = collength % 256;
        if (n == 255 || n == 0)
            snprintf(buf, buf_cap, "MONEY(%d)", m);
        else
            snprintf(buf, buf_cap, "MONEY(%d,%d)", m, n);
        break;
    }
    case 9: snprintf(buf, buf_cap, "NULL"); break;
    case 10: { /* DATETIME */
        int hi = (collength >> 8) & 0xFF;
        int lo = collength & 0xFF;
        const char *dt_units[] = {
            [0] = "YEAR", [2] = "MONTH", [4] = "DAY",
            [6] = "HOUR", [8] = "MINUTE", [0x0A] = "SECOND",
            [0x0B] = "FRACTION(1)", [0x0C] = "FRACTION",
            [0x0D] = "FRACTION", [0x0E] = "FRACTION(4)",
            [0x0F] = "FRACTION(5)"
        };
        const char *hi_unit = (hi >= 0 && hi <= 0x0F) ? dt_units[hi] : "UNKNOWN";
        const char *lo_unit = (lo >= 0 && lo <= 0x0F) ? dt_units[lo] : "UNKNOWN";
        snprintf(buf, buf_cap, "DATETIME %s TO %s", hi_unit, lo_unit);
        break;
    }
    case 11: snprintf(buf, buf_cap, "BYTE"); break;
    case 12: snprintf(buf, buf_cap, "TEXT"); break;
    case 13: { /* VARCHAR */
        int c = (collength < 0) ? (collength + 65536) : collength;
        int m = c % 256;
        int n = c / 256;
        if (n == 255 || n == 0)
            snprintf(buf, buf_cap, "VARCHAR(%d)", m);
        else
            snprintf(buf, buf_cap, "VARCHAR(%d,%d)", m, n);
        break;
    }
    case 14: { /* INTERVAL */
        int hi = (collength >> 8) & 0xFF;
        int lo = collength & 0xFF;
        const char *iv_units[] = {
            [0] = "YEAR", [2] = "MONTH", [4] = "DAY",
            [6] = "HOUR", [8] = "MINUTE", [0x0A] = "SECOND",
            [0x0B] = "FRACTION(1)", [0x0C] = "FRACTION",
            [0x0D] = "FRACTION", [0x0E] = "FRACTION(4)",
            [0x0F] = "FRACTION(5)"
        };
        const char *hi_unit = (hi >= 0 && hi <= 0x0F) ? iv_units[hi] : "UNKNOWN";
        const char *lo_unit = (lo >= 0 && lo <= 0x0F) ? iv_units[lo] : "UNKNOWN";
        snprintf(buf, buf_cap, "INTERVAL %s TO %s", hi_unit, lo_unit);
        break;
    }
    case 15: snprintf(buf, buf_cap, "NCHAR(%d)", collength); break;
    case 16: { /* NVARCHAR */
        int c = (collength < 0) ? (collength + 65536) : collength;
        int m = c % 256;
        int n = c / 256;
        if (n == 255 || n == 0)
            snprintf(buf, buf_cap, "NVARCHAR(%d)", m);
        else
            snprintf(buf, buf_cap, "NVARCHAR(%d,%d)", m, n);
        break;
    }
    case 17: snprintf(buf, buf_cap, "INT8"); break;
    case 18: snprintf(buf, buf_cap, "SERIAL8"); break;
    case 40: snprintf(buf, buf_cap, "LVARCHAR(%d)", collength); break;
    case 41: snprintf(buf, buf_cap, "BOOLEAN"); break;
    case 45: snprintf(buf, buf_cap, "BOOLEAN"); break;
    case 52: snprintf(buf, buf_cap, "BIGINT"); break;
    case 53: snprintf(buf, buf_cap, "BIGSERIAL"); break;
    case 101: snprintf(buf, buf_cap, "CLOB(%d)", collength); break;
    case 102: snprintf(buf, buf_cap, "BLOB(%d)", collength); break;
    default: snprintf(buf, buf_cap, "TYPE(%d)", coltype); break;
    }
}

/* ---------------------------------------------------------------- */
/* .schema                                                          */
/* ---------------------------------------------------------------- */

sqlicon_exit_code command_schema_table(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg)
{
    char table[256];
    sqlicon_exit_code rc = parse_required_identifier_arg(arg, table, sizeof(table), "schema");
    if (rc != SQLICON_EXIT_OK)
        return rc;

    char sql[2048];
    if (snprintf(sql, sizeof(sql),
                 "SELECT c.colno, c.colname, c.coltype, c.collength "
                 "FROM syscolumns c, systables t "
                 "WHERE t.tabid = c.tabid "
                 "AND t.tabname = '%s' "
                 "ORDER BY c.colno", table) >= (int)sizeof(sql)) {
        fprintf(stderr, "error: .schema SQL too long\n");
        return SQLICON_EXIT_MISUSE;
    }

    sqli_result_t *result = NULL;
    sqli_status qrc = sqli_query(conn, sql, &result);
    if (qrc != SQLI_OK || result == NULL) {
        fprintf(stderr, "error: .schema query failed: %s\n",
                sqli_error(conn) ? sqli_error(conn) : "(unknown)");
        if (result != NULL)
            sqli_result_destroy(result);
        return SQLICON_EXIT_SQL_ERROR;
    }

    bool close_after = false;
    FILE *out = runtime_acquire_output(rt, &close_after);
    if (out == NULL) {
        sqli_result_destroy(result);
        fprintf(stderr, "error: cannot open output destination\n");
        return SQLICON_EXIT_MISUSE;
    }

    fprintf(out, "%-20s %-30s %s\n", "Column", "Type", "Nullable");
    fprintf(out, "%-20s %-30s %s\n", "--------------------", "------------------------------", "--------");

    while (sqli_result_next(result)) {
        int colno = (int)sqli_result_get_int(result, 0);
        const char *colname = sqli_result_get_string(result, 1);
        int coltype = (int)sqli_result_get_int(result, 2);
        int collength = (int)sqli_result_get_int(result, 3);

        (void)colno;
        char type_buf[128];
        format_sql_type(coltype, collength, type_buf, sizeof(type_buf));

        /* Nullability from coltype bit 0x100 (dbweb: coltype.ec line 29) */
        bool is_nullable = ((coltype & 0x100) == 0);
        fprintf(out, "%-20s %-30s %s\n", colname, type_buf,
                is_nullable ? "YES" : "NO");
    }

    sqli_result_destroy(result);
    fflush(out);
    runtime_release_output(out, close_after);
    return SQLICON_EXIT_OK;
}

/* ---------------------------------------------------------------- */
/* .indexes                                                         */
/* ---------------------------------------------------------------- */

sqlicon_exit_code command_indexes_table(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg)
{
    char table[256];
    sqlicon_exit_code rc = parse_required_identifier_arg(arg, table, sizeof(table), "indexes");
    if (rc != SQLICON_EXIT_OK)
        return rc;

    /* First get tabid */
    char sql[2048];
    int tabid = 0;
    if (snprintf(sql, sizeof(sql),
                 "SELECT tabid FROM systables WHERE tabname = '%s'", table) >= (int)sizeof(sql)) {
        fprintf(stderr, "error: .indexes SQL too long\n");
        return SQLICON_EXIT_MISUSE;
    }
    sqli_result_t *tab_res = NULL;
    sqli_status qrc = sqli_query(conn, sql, &tab_res);
    if (qrc != SQLI_OK || tab_res == NULL || !sqli_result_next(tab_res)) {
        fprintf(stderr, "error: table '%s' not found\n", table);
        if (tab_res != NULL)
            sqli_result_destroy(tab_res);
        return SQLICON_EXIT_SQL_ERROR;
    }
    tabid = (int)sqli_result_get_int(tab_res, 0);
    sqli_result_destroy(tab_res);

    /* Get index list */
    if (snprintf(sql, sizeof(sql),
                 "SELECT i.idxname, i.idxtype, i.part1, i.part2, i.part3, i.part4, "
                 "i.part5, i.part6, i.part7, i.part8, i.part9, i.part10, "
                 "i.part11, i.part12, i.part13, i.part14, i.part15, i.part16 "
                 "FROM sysindexes i, systables t "
                 "WHERE t.tabid = i.tabid "
                 "AND t.tabname = '%s' "
                 "AND i.idxname IS NOT NULL "
                 "ORDER BY i.idxname", table) >= (int)sizeof(sql)) {
        fprintf(stderr, "error: .indexes SQL too long\n");
        return SQLICON_EXIT_MISUSE;
    }

    sqli_result_t *idx_res = NULL;
    qrc = sqli_query(conn, sql, &idx_res);
    if (qrc != SQLI_OK || idx_res == NULL) {
        fprintf(stderr, "error: .indexes query failed: %s\n",
                sqli_error(conn) ? sqli_error(conn) : "(unknown)");
        if (idx_res != NULL)
            sqli_result_destroy(idx_res);
        return SQLICON_EXIT_SQL_ERROR;
    }

    bool close_after = false;
    FILE *out = runtime_acquire_output(rt, &close_after);
    if (out == NULL) {
        sqli_result_destroy(idx_res);
        fprintf(stderr, "error: cannot open output destination\n");
        return SQLICON_EXIT_MISUSE;
    }

    while (sqli_result_next(idx_res)) {
        char idxname_buf[256], idxtype_buf[32];
        /* Capture into local buffers to avoid static buffer aliasing */
        snprintf(idxname_buf, sizeof(idxname_buf), "%s",
                 result_text_by_type(idx_res, 0, sqli_result_column_type(idx_res, 0)));
        snprintf(idxtype_buf, sizeof(idxtype_buf), "%s",
                 result_text_by_type(idx_res, 1, sqli_result_column_type(idx_res, 1)));

        fprintf(out, "%s (%s):\n", idxname_buf, idxtype_buf);

        /* Collect column names for this index */
        char col_names[16][128];
        int col_count = 0;
        for (int p = 2; p < 18; p++) {
            if (sqli_result_is_null(idx_res, p))
                continue;
            int part = (int)sqli_result_get_int(idx_res, p);
            if (part == 0)
                continue;
            int abs_part = (part < 0) ? -part : part;

            /* Look up column name */
            char col_sql[512];
            if (snprintf(col_sql, sizeof(col_sql),
                         "SELECT colname FROM syscolumns WHERE tabid = %d AND colno = %d",
                         tabid, abs_part) >= (int)sizeof(col_sql))
                continue;
            sqli_result_t *col_res = NULL;
            if (sqli_query(conn, col_sql, &col_res) == SQLI_OK && col_res != NULL) {
                if (sqli_result_next(col_res)) {
                    snprintf(col_names[col_count], sizeof(col_names[col_count]),
                             "%s%s", sqli_result_get_string(col_res, 0),
                             part < 0 ? " DESC" : "");
                    col_count++;
                }
                sqli_result_destroy(col_res);
            }
        }

        for (int c = 0; c < col_count; c++) {
            fprintf(out, "  %s\n", col_names[c]);
        }
        fprintf(out, "\n");
    }

    sqli_result_destroy(idx_res);
    fflush(out);
    runtime_release_output(out, close_after);
    return SQLICON_EXIT_OK;
}

/* ---------------------------------------------------------------- */
/* .views                                                           */
/* ---------------------------------------------------------------- */

sqlicon_exit_code command_views_table(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg)
{
    const char *p = skip_spaces_str(arg);
    char table[256] = {0};

    if (*p != '\0') {
        sqlicon_exit_code rc = parse_required_identifier_arg(p, table, sizeof(table), "views");
        if (rc != SQLICON_EXIT_OK)
            return rc;
    }

    char sql[2048];
    if (*table != '\0') {
        if (snprintf(sql, sizeof(sql),
                     "SELECT t.tabname, v.seqno, v.viewtext "
                     "FROM sysviews v, systables t "
                     "WHERE t.tabid = v.tabid "
                     "AND t.tabname = '%s' "
                     "ORDER BY v.seqno", table) >= (int)sizeof(sql)) {
            fprintf(stderr, "error: .views SQL too long\n");
            return SQLICON_EXIT_MISUSE;
        }
    } else {
        if (snprintf(sql, sizeof(sql),
                     "SELECT t.tabname, v.seqno, v.viewtext "
                     "FROM sysviews v, systables t "
                     "WHERE t.tabid = v.tabid "
                     "AND t.tabid >= 100 "
                     "ORDER BY t.tabname, v.seqno") >= (int)sizeof(sql)) {
            fprintf(stderr, "error: .views SQL too long\n");
            return SQLICON_EXIT_MISUSE;
        }
    }

    sqli_result_t *result = NULL;
    sqli_status qrc = sqli_query(conn, sql, &result);
    if (qrc != SQLI_OK || result == NULL) {
        fprintf(stderr, "error: .views query failed: %s\n",
                sqli_error(conn) ? sqli_error(conn) : "(unknown)");
        if (result != NULL)
            sqli_result_destroy(result);
        return SQLICON_EXIT_SQL_ERROR;
    }

    bool close_after = false;
    FILE *out = runtime_acquire_output(rt, &close_after);
    if (out == NULL) {
        sqli_result_destroy(result);
        fprintf(stderr, "error: cannot open output destination\n");
        return SQLICON_EXIT_MISUSE;
    }

    char current_view[256] = {0};
    char view_text[65536] = {0};

    while (sqli_result_next(result)) {
        char tabname_buf[256];
        /* Capture tabname first to avoid static buffer aliasing */
        snprintf(tabname_buf, sizeof(tabname_buf), "%s", sqli_result_get_string(result, 0));
        int seqno = (int)sqli_result_get_int(result, 1);
        const char *viewtext = sqli_result_get_string(result, 2);

        if (strcmp(tabname_buf, current_view) != 0) {
            if (current_view[0] != '\0') {
                fprintf(out, "\n%s\n\n", view_text);
            }
            snprintf(current_view, sizeof(current_view), "%s", tabname_buf);
            view_text[0] = '\0';
            fprintf(out, "CREATE VIEW %s AS\n", tabname_buf);
        }

        if (seqno == 1) {
            strcat(view_text, viewtext);
        } else {
            strcat(view_text, " ");
            strcat(view_text, viewtext);
        }
    }

    if (current_view[0] != '\0') {
        fprintf(out, "\n%s\n", view_text);
    }

    sqli_result_destroy(result);
    fflush(out);
    runtime_release_output(out, close_after);
    return SQLICON_EXIT_OK;
}

/* ---------------------------------------------------------------- */
/* .constraints                                                     */
/* ---------------------------------------------------------------- */

sqlicon_exit_code command_constraints_table(sqli_conn_t *conn, sqlicon_runtime *rt,
                                            const char *arg)
{
    char table[256];
    sqlicon_exit_code rc = parse_required_identifier_arg(arg, table, sizeof(table), "constraints");
    if (rc != SQLICON_EXIT_OK)
        return rc;

    char sql[3072];
    if (snprintf(sql, sizeof(sql),
                 "SELECT sc.constrname, sc.constrtype, "
                 "CASE sc.constrtype "
                 "  WHEN 'P' THEN 'PRIMARY KEY' "
                 "  WHEN 'R' THEN 'FOREIGN KEY' "
                 "  WHEN 'U' THEN 'UNIQUE' "
                 "  WHEN 'C' THEN 'CHECK' "
                 "  WHEN 'N' THEN 'NOT NULL' "
                 "  ELSE 'OTHER' END AS kind, "
                 "sc.idxname, rt.tabname AS ref_table, sr.updrule, sr.delrule "
                 "FROM sysconstraints sc, systables t, OUTER sysreferences sr, OUTER systables rt "
                 "WHERE t.tabid = sc.tabid "
                 "AND sr.constrid = sc.constrid "
                 "AND rt.tabid = sr.ptabid "
                 "AND t.tabname = '%s' "
                 "ORDER BY sc.constrname", table) >= (int)sizeof(sql)) {
        fprintf(stderr, "error: .constraints SQL too long\n");
        return SQLICON_EXIT_MISUSE;
    }
    return execute_sql_statement(conn, sql, false, false, rt);
}

/* ---------------------------------------------------------------- */
/* .dump                                                            */
/* ---------------------------------------------------------------- */

static sqlicon_exit_code dump_single_table_to_output(sqli_conn_t *conn, FILE *out,
                                                     const char *table,
                                                     bool emit_txn_wrapper)
{
    char sql[1024];
    if (snprintf(sql, sizeof(sql), "SELECT * FROM %s", table) >= (int)sizeof(sql)) {
        fprintf(stderr, "error: table name too long for .dump\n");
        return SQLICON_EXIT_MISUSE;
    }

    sqli_result_t *result = NULL;
    sqli_status qrc = sqli_query(conn, sql, &result);
    if (qrc != SQLI_OK || result == NULL) {
        fprintf(stderr, "error: .dump query failed: %s\n",
                sqli_error(conn) ? sqli_error(conn) : "(unknown)");
        if (result != NULL)
            sqli_result_destroy(result);
        return SQLICON_EXIT_SQL_ERROR;
    }

    if (emit_txn_wrapper)
        fprintf(out, "BEGIN WORK;\n");
    int cols = sqli_result_columns(result);
    while (sqli_result_next(result)) {
        fprintf(out, "INSERT INTO %s VALUES (", table);
        for (int i = 0; i < cols; i++) {
            if (i > 0)
                fputs(", ", out);
            if (sqli_result_is_null(result, i)) {
                fputs("NULL", out);
                continue;
            }
            int ctype = sqli_result_column_type(result, i);
            switch (ctype) {
            case SQLI_TYPE_SMALLINT:
            case SQLI_TYPE_INT:
            case SQLI_TYPE_SERIAL:
            case SQLI_TYPE_BOOL:
            case SQLI_TYPE_DBOOLEAN:
                fprintf(out, "%d", (int)sqli_result_get_int(result, i));
                break;
            case SQLI_TYPE_BIGINT:
            case SQLI_TYPE_BIGSERIAL:
            case SQLI_TYPE_SERIAL8:
            case SQLI_TYPE_INT8:
                fprintf(out, "%lld", (long long)sqli_result_get_int64(result, i));
                break;
            case SQLI_TYPE_FLOAT:
            case SQLI_TYPE_SMFLOAT:
                fprintf(out, "%.17g", sqli_result_get_double(result, i));
                break;
            case SQLI_TYPE_BYTE:
            case SQLI_TYPE_BLOB: {
                if (dump_fetch_lob_enabled()) {
                    uint8_t buf[4096];
                    size_t blen = sizeof(buf);
                    if (sqli_result_get_bytes(result, i, buf, &blen) == SQLI_OK) {
                        fputs("X'", out);
                        for (size_t k = 0; k < blen; k++)
                            fprintf(out, "%02X", (unsigned int)buf[k]);
                        fputc('\'', out);
                    } else {
                        const char *sv = sqli_result_get_string(result, i);
                        print_sql_string_literal(out, sv != NULL ? sv : "");
                    }
                } else {
                    const char *sv = sqli_result_get_string(result, i);
                    print_sql_string_literal(out, sv != NULL ? sv : "");
                }
                break;
            }
            default: {
                const char *sv = result_text_by_type(result, i, ctype);
                print_sql_string_literal(out, sv != NULL ? sv : "");
                break;
            }
            }
        }
        fprintf(out, ");\n");
    }
    if (emit_txn_wrapper)
        fprintf(out, "COMMIT WORK;\n");
    sqli_result_destroy(result);
    return SQLICON_EXIT_OK;
}

sqlicon_exit_code command_dump_table(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg)
{
    const char *p = skip_spaces_str(arg);
    char table[256];
    if (*p != '\0') {
        size_t len = 0;
        while (p[len] != '\0' && !is_space_char(p[len]))
            len++;
        if (len == 0 || len >= sizeof(table)) {
            fprintf(stderr, "error: .dump invalid table name\n");
            return SQLICON_EXIT_MISUSE;
        }
        memcpy(table, p, len);
        table[len] = '\0';
        if (!is_simple_sql_identifier(table)) {
            fprintf(stderr, "error: .dump expects simple identifier [A-Za-z_][A-Za-z0-9_]*\n");
            return SQLICON_EXIT_MISUSE;
        }
    }

    bool close_after = false;
    FILE *out = runtime_acquire_output(rt, &close_after);
    if (out == NULL) {
        fprintf(stderr, "error: cannot open output destination\n");
        return SQLICON_EXIT_MISUSE;
    }

    sqlicon_exit_code rc = SQLICON_EXIT_OK;
    if (*p != '\0') {
        rc = dump_single_table_to_output(conn, out, table, true);
    } else {
        sqli_result_t *tables = NULL;
        sqli_status qrc = sqli_query(conn,
                                     "SELECT tabname FROM systables "
                                     "WHERE tabid >= 100 AND tabtype = 'T' "
                                     "AND owner = USER "
                                     "ORDER BY tabname",
                                     &tables);
        if (qrc != SQLI_OK || tables == NULL) {
            fprintf(stderr, "error: .dump table-list query failed: %s\n",
                    sqli_error(conn) ? sqli_error(conn) : "(unknown)");
            if (tables != NULL)
                sqli_result_destroy(tables);
            runtime_release_output(out, close_after);
            return SQLICON_EXIT_SQL_ERROR;
        }

        fprintf(out, "-- sqlicon dump (all user tables)\n");
        fprintf(out, "BEGIN WORK;\n");
        while (sqli_result_next(tables)) {
            const char *name = sqli_result_get_string(tables, 0);
            if (name == NULL || !is_simple_sql_identifier(name)) {
                fprintf(stderr, "error: .dump encountered unsupported table identifier: %s\n",
                        name != NULL ? name : "(null)");
                rc = SQLICON_EXIT_SQL_ERROR;
                break;
            }
            fprintf(out, "\n-- table: %s\n", name);
            rc = dump_single_table_to_output(conn, out, name, false);
            if (rc != SQLICON_EXIT_OK)
                break;
        }
        if (rc == SQLICON_EXIT_OK)
            fprintf(out, "COMMIT WORK;\n");
        else
            fprintf(out, "ROLLBACK WORK;\n");
        sqli_result_destroy(tables);
    }

    fflush(out);
    runtime_release_output(out, close_after);
    return rc;
}

/* ---------------------------------------------------------------- */
/* .import CSV                                                      */
/* ---------------------------------------------------------------- */

static bool parse_csv_fields(const char *line, char ***out_fields, int *out_count)
{
    *out_fields = NULL;
    *out_count = 0;
    size_t fields_cap = 0;
    char **fields = NULL;

    size_t i = 0;
    size_t n = strlen(line);
    for (;;) {
        if (i > n)
            break;

        bool quoted = false;
        if (i < n && line[i] == '"') {
            quoted = true;
            i++;
        }

        size_t cap = 64;
        size_t len = 0;
        char *buf = malloc(cap);
        if (buf == NULL)
            goto fail;

        while (i < n) {
            char ch = line[i];
            if (quoted) {
                if (ch == '"') {
                    if ((i + 1) < n && line[i + 1] == '"') {
                        ch = '"';
                        i += 2;
                    } else {
                        i++;
                        quoted = false;
                        continue;
                    }
                } else {
                    i++;
                }
            } else {
                if (ch == ',')
                    break;
                i++;
            }

            if (len + 1 >= cap) {
                size_t new_cap = cap * 2;
                char *grown = realloc(buf, new_cap);
                if (grown == NULL) {
                    free(buf);
                    goto fail;
                }
                buf = grown;
                cap = new_cap;
            }
            buf[len++] = ch;
        }
        buf[len] = '\0';

        if (quoted) {
            free(buf);
            goto fail;
        }

        if (*out_count >= (int)fields_cap) {
            size_t new_cap = (fields_cap == 0) ? 8 : fields_cap * 2;
            char **grown = realloc(fields, new_cap * sizeof(*grown));
            if (grown == NULL) {
                free(buf);
                goto fail;
            }
            fields = grown;
            fields_cap = new_cap;
        }
        fields[*out_count] = buf;
        (*out_count)++;

        if (i >= n)
            break;
        if (line[i] == ',')
            i++;
    }

    *out_fields = fields;
    return true;

fail:
    if (fields != NULL) {
        for (int k = 0; k < *out_count; k++)
            free(fields[k]);
        free(fields);
    }
    *out_fields = NULL;
    *out_count = 0;
    return false;
}

static void free_csv_fields(char **fields, int count)
{
    if (fields == NULL)
        return;
    for (int i = 0; i < count; i++)
        free(fields[i]);
    free(fields);
}

static sqlicon_exit_code validate_import_rows(FILE *fp, int col_count, int start_line_no)
{
    char line[65536];
    int line_no = start_line_no;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_no++;
        strip_trailing_inplace(line);
        if (line[0] == '\0')
            continue;
        char **fields = NULL;
        int field_count = 0;
        if (!parse_csv_fields(line, &fields, &field_count)) {
            fprintf(stderr, "error: .import invalid CSV row at line %d\n", line_no);
            free_csv_fields(fields, field_count);
            return SQLICON_EXIT_MISUSE;
        }
        free_csv_fields(fields, field_count);
        if (field_count > col_count) {
            fprintf(stderr, "error: .import row has too many columns at line %d\n", line_no);
            return SQLICON_EXIT_MISUSE;
        }
    }
    return SQLICON_EXIT_OK;
}

static bool build_import_column_list(char *out, size_t out_cap,
                                     char **headers, int col_count)
{
    size_t pos = 0;
    out[0] = '\0';
    for (int i = 0; i < col_count; i++) {
        if (i > 0) {
            if (pos + 1 >= out_cap)
                return false;
            out[pos++] = ',';
            out[pos] = '\0';
        }
        if (!append_sql_identifier_raw(out, out_cap, &pos, headers[i]))
            return false;
    }
    return true;
}

static bool build_import_insert_sql(char *sql, size_t sql_cap, const char *table,
                                    char **headers, int col_count,
                                    char **fields, int field_count)
{
    size_t pos = 0;
    int n = snprintf(sql + pos, sql_cap - pos, "INSERT INTO %s (", table);
    if (n < 0 || (size_t)n >= sql_cap - pos)
        return false;
    pos += (size_t)n;
    for (int i = 0; i < col_count; i++) {
        if (i > 0) {
            n = snprintf(sql + pos, sql_cap - pos, ",");
            if (n < 0 || (size_t)n >= sql_cap - pos)
                return false;
            pos += (size_t)n;
        }
        if (!append_sql_identifier_raw(sql, sql_cap, &pos, headers[i]))
            return false;
    }
    n = snprintf(sql + pos, sql_cap - pos, ") VALUES (");
    if (n < 0 || (size_t)n >= sql_cap - pos)
        return false;
    pos += (size_t)n;
    for (int i = 0; i < col_count; i++) {
        if (i > 0) {
            n = snprintf(sql + pos, sql_cap - pos, ",");
            if (n < 0 || (size_t)n >= sql_cap - pos)
                return false;
            pos += (size_t)n;
        }
        if (i >= field_count || str_case_eq(fields[i], "NULL")) {
            n = snprintf(sql + pos, sql_cap - pos, "NULL");
            if (n < 0 || (size_t)n >= sql_cap - pos)
                return false;
            pos += (size_t)n;
        } else {
            if (pos + 1 >= sql_cap)
                return false;
            sql[pos++] = '\'';
            for (const char *q = fields[i]; *q != '\0'; q++) {
                if (*q == '\'') {
                    if (pos + 2 >= sql_cap)
                        return false;
                    sql[pos++] = '\'';
                    sql[pos++] = '\'';
                } else {
                    if (pos + 1 >= sql_cap)
                        return false;
                    sql[pos++] = *q;
                }
            }
            if (pos + 1 >= sql_cap)
                return false;
            sql[pos++] = '\'';
            sql[pos] = '\0';
        }
    }
    n = snprintf(sql + pos, sql_cap - pos, ")");
    if (n < 0 || (size_t)n >= sql_cap - pos)
        return false;
    return true;
}

sqlicon_exit_code command_import_csv(sqli_conn_t *conn, const char *arg)
{
    const char *p = skip_spaces_str(arg);
    if (*p == '\0') {
        fprintf(stderr, "error: .import requires FILE and TABLE\n");
        return SQLICON_EXIT_MISUSE;
    }

    const char *sp = p;
    while (*sp != '\0' && !is_space_char(*sp))
        sp++;
    if (*sp == '\0') {
        fprintf(stderr, "error: .import requires FILE and TABLE\n");
        return SQLICON_EXIT_MISUSE;
    }
    char *file_path = dup_span(p, (size_t)(sp - p));
    if (file_path == NULL)
        return SQLICON_EXIT_MISUSE;
    const char *table = skip_spaces_str(sp);
    if (*table == '\0') {
        free(file_path);
        fprintf(stderr, "error: .import requires TABLE\n");
        return SQLICON_EXIT_MISUSE;
    }

    FILE *fp = fopen(file_path, "r");
    free(file_path);
    if (fp == NULL) {
        fprintf(stderr, "error: .import cannot open file\n");
        return SQLICON_EXIT_MISUSE;
    }

    char line[65536];
    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        fprintf(stderr, "error: .import CSV is empty\n");
        return SQLICON_EXIT_MISUSE;
    }
    strip_trailing_inplace(line);

    char **headers = NULL;
    int col_count = 0;
    if (!parse_csv_fields(line, &headers, &col_count) || col_count <= 0) {
        fclose(fp);
        fprintf(stderr, "error: .import failed to parse CSV header\n");
        free_csv_fields(headers, col_count);
        return SQLICON_EXIT_MISUSE;
    }
    long data_start_pos = ftell(fp);

    bool tx_active = false;
    sqli_result_t *tx_res = NULL;
    if (sqli_query(conn, "BEGIN WORK", &tx_res) == SQLI_OK) {
        tx_active = true;
    } else {
        fprintf(stderr, "warning: .import transaction start failed, continuing without rollback safety: %s\n",
                sqli_error(conn) ? sqli_error(conn) : "(unknown)");
    }
    if (tx_res != NULL)
        sqli_result_destroy(tx_res);

    bool use_stage = false;
    char stage_table[128];
    stage_table[0] = '\0';
    char col_list[8192];
    if (!build_import_column_list(col_list, sizeof(col_list), headers, col_count)) {
        free_csv_fields(headers, col_count);
        fclose(fp);
        fprintf(stderr, "error: .import invalid or too long header columns\n");
        return SQLICON_EXIT_MISUSE;
    }

    if (!tx_active) {
        snprintf(stage_table, sizeof(stage_table), "sqli_imp_%ld_%ld",
                 (long)getpid(), (long)(time(NULL) % 1000000L));
        char stage_sql[17408];
        if (snprintf(stage_sql, sizeof(stage_sql),
                     "SELECT %s FROM %s WHERE 1=0 INTO TEMP %s WITH NO LOG",
                     col_list, table, stage_table) >= (int)sizeof(stage_sql)) {
            free_csv_fields(headers, col_count);
            fclose(fp);
            fprintf(stderr, "error: .import failed to build staging SQL\n");
            return SQLICON_EXIT_MISUSE;
        }
        sqli_result_t *stage_res = NULL;
        if (sqli_query(conn, stage_sql, &stage_res) != SQLI_OK) {
            if (stage_res != NULL)
                sqli_result_destroy(stage_res);
            free_csv_fields(headers, col_count);
            fclose(fp);
            fprintf(stderr, "error: .import staging table creation failed: %s\n",
                    sqli_error(conn) ? sqli_error(conn) : "(unknown)");
            return SQLICON_EXIT_SQL_ERROR;
        }
        if (stage_res != NULL)
            sqli_result_destroy(stage_res);
        use_stage = true;
    }
    if (!tx_active) {
        if (data_start_pos >= 0 && fseek(fp, data_start_pos, SEEK_SET) == 0) {
            sqlicon_exit_code vrc = validate_import_rows(fp, col_count, 1);
            if (vrc != SQLICON_EXIT_OK) {
                free_csv_fields(headers, col_count);
                fclose(fp);
                return vrc;
            }
            if (fseek(fp, data_start_pos, SEEK_SET) != 0) {
                free_csv_fields(headers, col_count);
                fclose(fp);
                fprintf(stderr, "error: .import failed to rewind CSV for import pass\n");
                return SQLICON_EXIT_MISUSE;
            }
        } else {
            free_csv_fields(headers, col_count);
            fclose(fp);
            fprintf(stderr, "error: .import cannot pre-validate CSV rows\n");
            return SQLICON_EXIT_MISUSE;
        }
    }

    int imported_rows = 0;
    int line_no = 1;
    sqlicon_exit_code rc = SQLICON_EXIT_OK;
    const char *insert_target = use_stage ? stage_table : table;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_no++;
        strip_trailing_inplace(line);
        if (line[0] == '\0')
            continue;

        char **fields = NULL;
        int field_count = 0;
        if (!parse_csv_fields(line, &fields, &field_count)) {
            fprintf(stderr, "error: .import invalid CSV row at line %d\n", line_no);
            rc = SQLICON_EXIT_MISUSE;
            break;
        }
        if (field_count > col_count) {
            free_csv_fields(fields, field_count);
            fprintf(stderr, "error: .import row has too many columns at line %d\n", line_no);
            rc = SQLICON_EXIT_MISUSE;
            break;
        }

        char sql[32768];
        if (!build_import_insert_sql(sql, sizeof(sql), insert_target,
                                     headers, col_count, fields, field_count))
            rc = SQLICON_EXIT_MISUSE;
        free_csv_fields(fields, field_count);
        if (rc != SQLICON_EXIT_OK) {
            fprintf(stderr, "error: .import statement too long\n");
            break;
        }
        sqli_result_t *ires = NULL;
        sqli_status erc = sqli_query(conn, sql, &ires);
        if (ires != NULL)
            sqli_result_destroy(ires);
        if (erc != SQLI_OK) {
            fprintf(stderr, "error: .import execute failed at line %d: %s\n", line_no,
                    sqli_error(conn) ? sqli_error(conn) : "(unknown)");
            rc = SQLICON_EXIT_SQL_ERROR;
            break;
        }
        imported_rows++;
    }

    if (tx_active) {
        sqli_result_t *end_res = NULL;
        if (rc == SQLICON_EXIT_OK) {
            if (sqli_query(conn, "COMMIT WORK", &end_res) != SQLI_OK) {
                if (end_res != NULL)
                    sqli_result_destroy(end_res);
                rc = SQLICON_EXIT_SQL_ERROR;
                fprintf(stderr, "error: .import commit failed: %s\n",
                        sqli_error(conn) ? sqli_error(conn) : "(unknown)");
            } else if (end_res != NULL) {
                sqli_result_destroy(end_res);
            }
        } else {
            (void)sqli_query(conn, "ROLLBACK WORK", &end_res);
            if (end_res != NULL)
                sqli_result_destroy(end_res);
        }
    }
    if (!tx_active && use_stage) {
        if (rc == SQLICON_EXIT_OK) {
            char merge_sql[17408];
            if (snprintf(merge_sql, sizeof(merge_sql),
                         "INSERT INTO %s (%s) SELECT %s FROM %s",
                         table, col_list, col_list, stage_table) >= (int)sizeof(merge_sql)) {
                rc = SQLICON_EXIT_MISUSE;
                fprintf(stderr, "error: .import merge SQL too long\n");
            } else {
                sqli_result_t *mres = NULL;
                if (sqli_query(conn, merge_sql, &mres) != SQLI_OK) {
                    if (mres != NULL)
                        sqli_result_destroy(mres);
                    rc = SQLICON_EXIT_SQL_ERROR;
                    fprintf(stderr, "error: .import final merge failed: %s\n",
                            sqli_error(conn) ? sqli_error(conn) : "(unknown)");
                } else if (mres != NULL) {
                    sqli_result_destroy(mres);
                }
            }
        }
        sqli_result_t *dres = NULL;
        char drop_sql[256];
        if (snprintf(drop_sql, sizeof(drop_sql), "DROP TABLE %s", stage_table) < (int)sizeof(drop_sql)) {
            (void)sqli_query(conn, drop_sql, &dres);
            if (dres != NULL)
                sqli_result_destroy(dres);
        }
    }

    free_csv_fields(headers, col_count);
    fclose(fp);
    if (rc == SQLICON_EXIT_OK)
        fprintf(stderr, "imported %d row(s)\n", imported_rows);
    return rc;
}
