/*
 * sqliconn — minimal demo: connect to an Informix server and run a query.
 *
 * Usage: sqliconn <host> <port> <database> <user> <password> [sql]
 *
 * Defaults sql to: SELECT DBINFO('version','full') FROM sysmaster:sysdual
 */

#include "libsqli/sqli.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *error_class_name(sqli_error_class klass)
{
    switch (klass) {
    case SQLI_ERROR_CLASS_NONE:
        return "none";
    case SQLI_ERROR_CLASS_AUTH:
        return "auth";
    case SQLI_ERROR_CLASS_NETWORK:
        return "network";
    case SQLI_ERROR_CLASS_PROTOCOL:
        return "protocol";
    case SQLI_ERROR_CLASS_SERVER:
        return "server";
    case SQLI_ERROR_CLASS_DATA:
        return "data";
    case SQLI_ERROR_CLASS_UNKNOWN:
    default:
        return "unknown";
    }
}

static int sql_is_read_only(const char *sql)
{
    if (sql == NULL)
        return 0;

    const unsigned char *p = (const unsigned char *)sql;
    for (;;) {
        while (*p != '\0' && isspace(*p))
            p++;
        if (p[0] == '-' && p[1] == '-') {
            p += 2;
            while (*p != '\0' && *p != '\n')
                p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p[0] != '\0' && !(p[0] == '*' && p[1] == '/'))
                p++;
            if (p[0] == '*' && p[1] == '/')
                p += 2;
            continue;
        }
        break;
    }

    char tok[16];
    size_t n = 0;
    while (*p != '\0' && isalpha(*p) && n < sizeof(tok) - 1) {
        tok[n++] = (char)toupper(*p);
        p++;
    }
    tok[n] = '\0';
    if (n == 0)
        return 0;

    return strcmp(tok, "SELECT") == 0 ||
           strcmp(tok, "WITH") == 0 ||
           strcmp(tok, "SHOW") == 0 ||
           strcmp(tok, "DESCRIBE") == 0 ||
           strcmp(tok, "EXPLAIN") == 0 ||
           strcmp(tok, "VALUES") == 0;
}

static unsigned parse_uint_env(const char *name, unsigned fallback)
{
    const char *v = getenv(name);
    if (v == NULL || *v == '\0')
        return fallback;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (end == v || *end != '\0' || n > 1000ul)
        return fallback;
    return (unsigned)n;
}

static void print_error_diagnostics(sqli_conn_t *conn, const char *where, sqli_status rc,
                                    const char *sql, unsigned max_retries)
{
    fprintf(stderr, "%s failed (%d): %s\n", where, rc, sqli_error(conn));

    sqli_error_info ei;
    if (sqli_error_get_info(conn, &ei) == SQLI_OK && ei.has_error) {
        fprintf(stderr,
                "error_info: status=%d sqlcode=%d isam=%d sqlstate=%s opcode=%u(%s) context=%s unknown=%u count=%u\n",
                (int)ei.status, ei.sqlcode, ei.isamcode,
                ei.sqlstate[0] ? ei.sqlstate : "-",
                (unsigned)ei.opcode,
                ei.opcode_name[0] ? ei.opcode_name : "-",
                ei.context[0] ? ei.context : "-",
                (unsigned)ei.unknown_opcode,
                (unsigned)ei.unknown_opcode_count);
        if (ei.sql_message[0])
            fprintf(stderr, "sql_message: %s\n", ei.sql_message);
        if (ei.isam_message[0])
            fprintf(stderr, "isam_message: %s\n", ei.isam_message);
        if (ei.server_message[0])
            fprintf(stderr, "server_message: %s\n", ei.server_message);
    }

    sqli_error_class klass = SQLI_ERROR_CLASS_UNKNOWN;
    bool retryable = false;
    if (sqli_error_classify(conn, &klass, &retryable) == SQLI_OK) {
        fprintf(stderr, "error_class: class=%s retryable=%s\n",
                error_class_name(klass), retryable ? "yes" : "no");
    }

    if (sql != NULL) {
        int read_only = sql_is_read_only(sql);
        bool should_retry = false;
        uint32_t delay_ms = 0;
        (void)sqli_retry_recommend(conn, 0, &should_retry, &delay_ms);
        fprintf(stderr,
                "retry_policy: sql_read_only=%s max_retries=%u classifier_retry=%s first_delay_ms=%u auto_retry=%s\n",
                read_only ? "yes" : "no",
                max_retries,
                should_retry ? "yes" : "no",
                (unsigned)delay_ms,
                (read_only && should_retry && max_retries > 0) ? "yes" : "no");
        if (!read_only)
            fprintf(stderr, "retry_reason: disabled for non-read-only SQL\n");
        else if (!should_retry)
            fprintf(stderr, "retry_reason: classifier marked failure non-retryable\n");
        else if (max_retries == 0)
            fprintf(stderr, "retry_reason: max_retries=0\n");
    }
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: %s <host> <port> <database> <user> <password> [server] [sql]\n",
                argv[0]);
        return 1;
    }

    const char *host     = argv[1];
    const char *port     = argv[2];
    const char *database = argv[3];
    const char *user     = argv[4];
    const char *password = argv[5];

    /* Optional server name (DBSERVERNAME/DBSERVERALIASES) — use "" to omit */
    const char *server   = (argc >= 7) ? argv[6] : "";
    const char *sql      = (argc >= 8) ? argv[7]
                         : "SELECT DBINFO('version','full') FROM sysmaster:sysdual";
    unsigned max_retries = parse_uint_env("SQLI_MAX_RETRIES", 1);
    const char *client_locale = getenv("SQLI_CLIENT_LOCALE");
    const char *db_locale = getenv("SQLI_DB_LOCALE");
    if (client_locale == NULL || *client_locale == '\0')
        client_locale = "en_US.utf8";
    if (db_locale == NULL || *db_locale == '\0')
        db_locale = "en_US.8859-1";

    sqli_log_set_level(SQLI_LOG_DEBUG);

    sqli_conn_t *conn = NULL;
    sqli_status rc = sqli_create(&conn);
    if (rc != SQLI_OK) {
        fprintf(stderr, "sqli_create failed: %d\n", rc);
        return 1;
    }

    sqli_connect_params params;
    memset(&params, 0, sizeof(params));
    params.hostname = host;
    params.service  = port;
    params.server   = server;
    params.database = database;
    params.username = user;
    params.password = password;
    params.client_locale = client_locale;
    params.db_locale     = db_locale;

    fprintf(stderr, "connecting to %s:%s database=%s user=%s client_locale=%s db_locale=%s\n",
            host, port, database, user, client_locale, db_locale);

    rc = sqli_connect(conn, &params);
    if (rc != SQLI_OK) {
        print_error_diagnostics(conn, "sqli_connect", rc, NULL, 0);
        sqli_destroy(conn);
        return 1;
    }

    fprintf(stderr, "connected, running: %s\n", sql);
    fprintf(stderr, "retry_config: max_retries=%u sql_read_only=%s\n",
            max_retries, sql_is_read_only(sql) ? "yes" : "no");

    sqli_result_t *result = NULL;
    rc = sqli_query_with_retry(conn, sql, max_retries, &result);
    if (rc != SQLI_OK) {
        print_error_diagnostics(conn, "sqli_query", rc, sql, max_retries);
        sqli_close(conn);
        sqli_destroy(conn);
        return 1;
    }

    int ncols = sqli_result_columns(result);
    fprintf(stderr, "columns: %d\n", ncols);

    int row = 0;
    while (sqli_result_next(result)) {
        row++;
        printf("row %d:", row);
        for (int c = 0; c < ncols; c++) {
            const char *name = sqli_result_column_name(result, c);
            int ctype = sqli_result_column_type(result, c);
            switch (ctype) {
            case SQLI_TYPE_SMALLINT:
            case SQLI_TYPE_INT:
            case SQLI_TYPE_SERIAL:
            case SQLI_TYPE_DATE:
                printf("  %s=%d", name ? name : "?", (int)sqli_result_get_int(result, c));
                break;
            case SQLI_TYPE_BIGINT:
            case SQLI_TYPE_BIGSERIAL:
            case SQLI_TYPE_SERIAL8:
            case SQLI_TYPE_INT8:
                printf("  %s=%lld", name ? name : "?", (long long)sqli_result_get_int64(result, c));
                break;
            case SQLI_TYPE_FLOAT:
            case SQLI_TYPE_SMFLOAT:
            case SQLI_TYPE_DECIMAL:
            case SQLI_TYPE_MONEY:
                printf("  %s=%g", name ? name : "?", sqli_result_get_double(result, c));
                break;
            case SQLI_TYPE_BOOL:
            case SQLI_TYPE_DBOOLEAN:
                printf("  %s=%s", name ? name : "?", sqli_result_get_int(result, c) ? "true" : "false");
                break;
            default: {
                const char *val = sqli_result_get_string(result, c);
                printf("  %s=%s", name ? name : "?", val ? val : "(null)");
                break;
            }
            }
        }
        printf("\n");
    }

    fprintf(stderr, "%lld row(s) affected\n", (long long)sqli_result_rows_affected(result));
    if (sqli_result_has_generated_serial(result))
        fprintf(stderr, "generated_serial: %lld\n",
                (long long)sqli_result_generated_serial(result));
    if (sqli_result_has_generated_serial8(result))
        fprintf(stderr, "generated_serial8: %lld\n",
                (long long)sqli_result_generated_serial8(result));

    sqli_result_destroy(result);
    sqli_close(conn);
    sqli_destroy(conn);
    return 0;
}
