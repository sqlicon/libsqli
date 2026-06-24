#define _POSIX_C_SOURCE 200809L
#include "libsqli/sqli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int query_ok(sqli_conn_t *conn, const char *sql)
{
    sqli_result_t *res = NULL;
    if (sqli_query(conn, sql, &res) != SQLI_OK || res == NULL) {
        fprintf(stderr, "query failed: %s\n", sqli_error(conn));
        return 0;
    }
    int ok = 0;
    if (sqli_result_next(res))
        ok = sqli_result_get_int(res, 0);
    sqli_result_destroy(res);
    return ok == 1;
}

static int exec_insert_string(sqli_conn_t *conn, const char *table)
{
    char sql[512];
    snprintf(sql, sizeof(sql), "INSERT INTO %s (id, c_varchar) VALUES (?, ?)", table);
    int pc = 0;
    sqli_stmt_t *st = NULL;
    if (sqli_prepare(conn, sql, &pc, &st) != SQLI_OK || st == NULL) {
        fprintf(stderr, "prepare string failed: %s\n", sqli_error(conn));
        return 0;
    }
    if (pc != 2) {
        sqli_stmt_destroy(st);
        return 0;
    }
    int ok = 1;
    if (sqli_bind_int(st, 1, 8008) != SQLI_OK) ok = 0;
    if (ok && sqli_bind_string(st, 2, "prep_string") != SQLI_OK) { fprintf(stderr, "bind string failed\n"); ok = 0; }
    if (ok && sqli_execute(st) != SQLI_OK) { fprintf(stderr, "execute string failed: %s\n", sqli_error(conn)); ok = 0; }
    while (ok && sqli_stmt_next(st)) {}
    if (!ok) {
        sqli_stmt_destroy(st);
        return 0;
    }
    snprintf(sql, sizeof(sql),
             "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM %s WHERE id=8008 AND c_varchar='prep_string'",
             table);
    int qok = query_ok(conn, sql);
    sqli_stmt_destroy(st);
    return qok;
}

static int exec_insert_ints(sqli_conn_t *conn, const char *table)
{
    char sql[512];
    snprintf(sql, sizeof(sql), "INSERT INTO %s (id, c_int, c_big) VALUES (?, ?, ?)", table);
    int pc = 0;
    sqli_stmt_t *st = NULL;
    if (sqli_prepare(conn, sql, &pc, &st) != SQLI_OK || st == NULL) {
        fprintf(stderr, "prepare ints failed: %s\n", sqli_error(conn));
        return 0;
    }
    if (pc != 3) {
        sqli_stmt_destroy(st);
        return 0;
    }
    int ok = 1;
    if (sqli_bind_int(st, 1, 8107) != SQLI_OK) ok = 0;
    if (ok && sqli_bind_int(st, 2, 123456789) != SQLI_OK) ok = 0;
    if (ok && sqli_bind_int64(st, 3, 1234567890123LL) != SQLI_OK) ok = 0;
    if (ok && sqli_execute(st) != SQLI_OK) { fprintf(stderr, "execute ints failed: %s\n", sqli_error(conn)); ok = 0; }
    while (ok && sqli_stmt_next(st)) {}
    if (!ok) {
        sqli_stmt_destroy(st);
        return 0;
    }
    snprintf(sql, sizeof(sql),
             "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok "
             "FROM %s WHERE id=8107 AND c_int=123456789 AND c_big=1234567890123",
             table);
    int qok = query_ok(conn, sql);
    sqli_stmt_destroy(st);
    return qok;
}

static int exec_insert_decimal(sqli_conn_t *conn, const char *table)
{
    char sql[512];
    snprintf(sql, sizeof(sql), "INSERT INTO %s (id, c_dec) VALUES (?, ?)", table);
    int pc = 0;
    sqli_stmt_t *st = NULL;
    if (sqli_prepare(conn, sql, &pc, &st) != SQLI_OK || st == NULL) {
        fprintf(stderr, "prepare decimal failed: %s\n", sqli_error(conn));
        return 0;
    }
    if (pc != 2) {
        sqli_stmt_destroy(st);
        return 0;
    }
    int ok = 1;
    if (sqli_bind_int(st, 1, 8207) != SQLI_OK) ok = 0;
    if (ok && sqli_bind_decimal(st, 2, "54321.4321") != SQLI_OK) ok = 0;
    if (ok && sqli_execute(st) != SQLI_OK) { fprintf(stderr, "execute decimal failed: %s\n", sqli_error(conn)); ok = 0; }
    while (ok && sqli_stmt_next(st)) {}
    if (!ok) {
        sqli_stmt_destroy(st);
        return 0;
    }
    snprintf(sql, sizeof(sql),
             "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok "
             "FROM %s WHERE id=8207 AND c_dec=54321.4321",
             table);
    int qok = query_ok(conn, sql);
    sqli_stmt_destroy(st);
    return qok;
}

static int exec_insert_float(sqli_conn_t *conn, const char *table)
{
    char sql[512];
    snprintf(sql, sizeof(sql), "INSERT INTO %s (id, c_float) VALUES (?, ?)", table);
    int pc = 0;
    sqli_stmt_t *st = NULL;
    if (sqli_prepare(conn, sql, &pc, &st) != SQLI_OK || st == NULL) {
        fprintf(stderr, "prepare float failed: %s\n", sqli_error(conn));
        return 0;
    }
    if (pc != 2) {
        sqli_stmt_destroy(st);
        return 0;
    }
    int ok = 1;
    if (sqli_bind_int(st, 1, 8307) != SQLI_OK) ok = 0;
    if (ok && sqli_bind_double(st, 2, 98.75) != SQLI_OK) ok = 0;
    if (ok && sqli_execute(st) != SQLI_OK) { fprintf(stderr, "execute float failed: %s\n", sqli_error(conn)); ok = 0; }
    while (ok && sqli_stmt_next(st)) {}
    if (!ok) {
        sqli_stmt_destroy(st);
        return 0;
    }
    snprintf(sql, sizeof(sql),
             "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok "
             "FROM %s WHERE id=8307 AND c_float=98.75",
             table);
    int qok = query_ok(conn, sql);
    sqli_stmt_destroy(st);
    return qok;
}

static int exec_insert_date(sqli_conn_t *conn, const char *table)
{
    char sql[512];
    snprintf(sql, sizeof(sql), "INSERT INTO %s (id, c_date) VALUES (?, ?)", table);
    int pc = 0;
    sqli_stmt_t *st = NULL;
    if (sqli_prepare(conn, sql, &pc, &st) != SQLI_OK || st == NULL) {
        fprintf(stderr, "prepare date failed: %s\n", sqli_error(conn));
        return 0;
    }
    if (pc != 2) {
        sqli_stmt_destroy(st);
        return 0;
    }
    int ok = 1;
    if (sqli_bind_int(st, 1, 8405) != SQLI_OK) ok = 0;
    if (ok && sqli_bind_date(st, 2, "2026-06-20") != SQLI_OK) ok = 0;
    if (ok && sqli_execute(st) != SQLI_OK) { fprintf(stderr, "execute date failed: %s\n", sqli_error(conn)); ok = 0; }
    while (ok && sqli_stmt_next(st)) {}
    if (!ok) {
        sqli_stmt_destroy(st);
        return 0;
    }
    snprintf(sql, sizeof(sql),
             "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok "
             "FROM %s WHERE id=8405 AND c_date=DATE('2026-06-20')",
             table);
    int qok = query_ok(conn, sql);
    sqli_stmt_destroy(st);
    return qok;
}

int main(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr,
                "usage: %s <host> <port> <db> <user> <password> <table> <server> [client_locale] [db_locale]\n",
                argv[0]);
        return 2;
    }

    const char *host = argv[1];
    const char *port = argv[2];
    const char *db = argv[3];
    const char *user = argv[4];
    const char *pass = argv[5];
    const char *table = argv[6];
    const char *server = argv[7];
    const char *client_locale = (argc >= 9) ? argv[8] : "en_US.utf8";
    const char *db_locale = (argc >= 10) ? argv[9] : "de_DE.CP1252";

    sqli_conn_t *conn = NULL;
    if (sqli_create(&conn) != SQLI_OK || conn == NULL) {
        fprintf(stderr, "create failed\n");
        return 1;
    }

    sqli_connect_params p;
    memset(&p, 0, sizeof(p));
    p.hostname = host;
    p.service = port;
    p.server = server;
    p.database = db;
    p.username = user;
    p.password = pass;
    p.client_locale = client_locale;
    p.db_locale = db_locale;

    if (sqli_connect(conn, &p) != SQLI_OK) {
        fprintf(stderr, "connect failed: %s\n", sqli_error(conn));
        sqli_destroy(conn);
        return 1;
    }

    int dt008 = exec_insert_string(conn, table);
    int dt107 = exec_insert_ints(conn, table);
    int dt207 = exec_insert_decimal(conn, table);
    int dt307 = exec_insert_float(conn, table);
    int dt405 = exec_insert_date(conn, table);

    printf("DT-008=%s\n", dt008 ? "PASS" : "FAIL");
    printf("DT-107=%s\n", dt107 ? "PASS" : "FAIL");
    printf("DT-207=%s\n", dt207 ? "PASS" : "FAIL");
    printf("DT-307=%s\n", dt307 ? "PASS" : "FAIL");
    printf("DT-405=%s\n", dt405 ? "PASS" : "FAIL");

    sqli_close(conn);
    sqli_destroy(conn);
    return (dt008 && dt107 && dt207 && dt307 && dt405) ? 0 : 1;
}
