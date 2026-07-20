#define _POSIX_C_SOURCE 200809L
#include "libsqli/sqli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_query_count(sqli_conn_t *conn, const char *pattern)
{
    char sql[512];
    sqli_result_t *res = NULL;

    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM mtst "
             "WHERE 1=1 AND (m21_nr LIKE '%s' OR m21_ben1 LIKE '%s' OR "
             "m21_ben2 LIKE '%s' OR m21_ben3 LIKE '%s')",
             pattern, pattern, pattern, pattern);

    if (sqli_query(conn, sql, &res) != SQLI_OK || res == NULL) {
        fprintf(stderr, "query count failed: %s\n", sqli_error(conn));
        return 1;
    }

    if (!sqli_result_next(res)) {
        fprintf(stderr, "query count returned no rows\n");
        sqli_result_destroy(res);
        return 1;
    }

    printf("query_count=%s\n", sqli_result_get_decimal_string(res, 0));
    sqli_result_destroy(res);
    return 0;
}

static int run_prepare_count(sqli_conn_t *conn, const char *pattern)
{
    const char *sql =
        "SELECT COUNT(*) FROM mtst "
        "WHERE 1=1 AND (m21_nr LIKE ? OR m21_ben1 LIKE ? OR "
        "m21_ben2 LIKE ? OR m21_ben3 LIKE ?)";
    sqli_stmt_t *stmt = NULL;
    int param_count = 0;

    if (sqli_prepare(conn, sql, &param_count, &stmt) != SQLI_OK || stmt == NULL) {
        fprintf(stderr, "prepare count failed: %s\n", sqli_error(conn));
        return 1;
    }

    if (param_count != 4) {
        fprintf(stderr, "prepare count param_count=%d\n", param_count);
        sqli_stmt_destroy(stmt);
        return 1;
    }

    if (sqli_bind_string(stmt, 1, pattern) != SQLI_OK ||
        sqli_bind_string(stmt, 2, pattern) != SQLI_OK ||
        sqli_bind_string(stmt, 3, pattern) != SQLI_OK ||
        sqli_bind_string(stmt, 4, pattern) != SQLI_OK) {
        fprintf(stderr, "bind count failed\n");
        sqli_stmt_destroy(stmt);
        return 1;
    }

    if (sqli_execute(stmt) != SQLI_OK) {
        fprintf(stderr, "execute count failed: %s\n", sqli_error(conn));
        sqli_stmt_destroy(stmt);
        return 1;
    }

    if (!sqli_stmt_next(stmt)) {
        fprintf(stderr, "execute count returned no rows\n");
        sqli_stmt_destroy(stmt);
        return 1;
    }

    printf("prepare_count=%s\n",
           sqli_result_get_decimal_string(sqli_stmt_result(stmt), 0));
    sqli_stmt_destroy(stmt);
    return 0;
}

static int run_query_list(sqli_conn_t *conn, const char *pattern)
{
    char sql[1024];
    sqli_result_t *res = NULL;
    int rows = 0;

    snprintf(sql, sizeof(sql),
             "SELECT SKIP 0 FIRST 3 "
             "m21_nr, m21_ben1, m21_ben2, m21_ben3, m21_brpr1, m21_ekpr, m21_hart, m21_statu "
             "FROM mtst WHERE 1=1 AND "
             "(m21_nr LIKE '%s' OR m21_ben1 LIKE '%s' OR m21_ben2 LIKE '%s' OR m21_ben3 LIKE '%s') "
             "ORDER BY m21_nr",
             pattern, pattern, pattern, pattern);

    if (sqli_query(conn, sql, &res) != SQLI_OK || res == NULL) {
        fprintf(stderr, "query list failed: %s\n", sqli_error(conn));
        return 1;
    }

    while (sqli_result_next(res)) {
        rows++;
        printf("query_list_row[%d]=%s\n", rows, sqli_result_get_string(res, 0));
    }

    printf("query_list_rows=%d\n", rows);
    sqli_result_destroy(res);
    return rows > 0 ? 0 : 1;
}

static int run_prepare_list(sqli_conn_t *conn, const char *pattern)
{
    const char *sql =
        "SELECT SKIP 0 FIRST 3 "
        "m21_nr, m21_ben1, m21_ben2, m21_ben3, m21_brpr1, m21_ekpr, m21_hart, m21_statu "
        "FROM mtst WHERE 1=1 AND "
        "(m21_nr LIKE ? OR m21_ben1 LIKE ? OR m21_ben2 LIKE ? OR m21_ben3 LIKE ?) "
        "ORDER BY m21_nr";
    sqli_stmt_t *stmt = NULL;
    int param_count = 0;
    int rows = 0;

    if (sqli_prepare(conn, sql, &param_count, &stmt) != SQLI_OK || stmt == NULL) {
        fprintf(stderr, "prepare list failed: %s\n", sqli_error(conn));
        return 1;
    }

    if (param_count != 4) {
        fprintf(stderr, "prepare list param_count=%d\n", param_count);
        sqli_stmt_destroy(stmt);
        return 1;
    }

    if (sqli_bind_string(stmt, 1, pattern) != SQLI_OK ||
        sqli_bind_string(stmt, 2, pattern) != SQLI_OK ||
        sqli_bind_string(stmt, 3, pattern) != SQLI_OK ||
        sqli_bind_string(stmt, 4, pattern) != SQLI_OK) {
        fprintf(stderr, "bind list failed\n");
        sqli_stmt_destroy(stmt);
        return 1;
    }

    if (sqli_execute(stmt) != SQLI_OK) {
        fprintf(stderr, "execute list failed: %s\n", sqli_error(conn));
        sqli_stmt_destroy(stmt);
        return 1;
    }

    while (sqli_stmt_next(stmt)) {
        rows++;
        printf("prepare_list_row[%d]=%s\n", rows,
               sqli_result_get_string(sqli_stmt_result(stmt), 0));
    }

    printf("prepare_list_rows=%d\n", rows);
    sqli_stmt_destroy(stmt);
    return rows > 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    sqli_conn_t *conn = NULL;
    sqli_connect_params params;
    const char *pattern;
    int rc = 1;

    if (argc < 8) {
        fprintf(stderr,
                "usage: %s <host> <port> <db> <user> <password> <server> [client_locale] [db_locale] [pattern]\n",
                argv[0]);
        return 2;
    }

    memset(&params, 0, sizeof(params));
    params.hostname = argv[1];
    params.service = argv[2];
    params.database = argv[3];
    params.username = argv[4];
    params.password = argv[5];
    params.server = argv[6];
    params.client_locale = (argc >= 8) ? argv[7] : "en_US.utf8";
    params.db_locale = (argc >= 9) ? argv[8] : "de_DE.1252";
    pattern = (argc >= 10) ? argv[9] : "00000001%";

    if (sqli_create(&conn) != SQLI_OK || conn == NULL) {
        fprintf(stderr, "create failed\n");
        return 1;
    }

    if (sqli_connect(conn, &params) != SQLI_OK) {
        fprintf(stderr, "connect failed: %s\n", sqli_error(conn));
        sqli_destroy(conn);
        return 1;
    }

    rc = 0;
    rc |= run_query_count(conn, pattern);
    rc |= run_prepare_count(conn, pattern);
    rc |= run_query_list(conn, pattern);
    rc |= run_prepare_list(conn, pattern);

    sqli_close(conn);
    sqli_destroy(conn);
    return rc;
}
