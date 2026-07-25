#define _POSIX_C_SOURCE 200809L
/*
 * sysdbslocale_dump — connect to the sysmaster database over a Unix
 * domain socket (onipcstr) and print every row of sysdbslocale.
 *
 * Usage: sysdbslocale_dump -s <service>
 *
 * The Unix socket path is derived as /INFORMIXTMP/<service>.str, and
 * <service> is also used as the INFORMIXSERVER name for the connection
 * (see sqli_connect_params.server). Connection is selected as a Unix
 * socket by giving `service` (the wire param, not the CLI -s value) a
 * path starting with '/' (see sqli_handshake.c: use_unix_socket
 * detection) — on that path, sqli_connect() overrides username to the
 * current Unix user and clears password internally.
 *
 * DB_LOCALE uses "en_US.8859-1", not "en_US.iso-8859-1": Informix's
 * GLS mapping (see cvtmap under $INFORMIXDIR/gls/etc) names codesets
 * without the "iso-" prefix in the DB_LOCALE suffix; the "iso-"-prefixed
 * form causes the server to drop the connection during the protocol
 * exchange.
 */

#include "libsqli/sqli.h"

#include <stdio.h>
#include <string.h>

static void print_usage(const char *prog)
{
    fprintf(stderr, "usage: %s -s <service>\n", prog);
}

int main(int argc, char **argv)
{
    const char *service = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            service = argv[++i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }
    if (service == NULL || service[0] == '\0') {
        print_usage(argv[0]);
        return 1;
    }

    char socket_path[512];
    if (snprintf(socket_path, sizeof(socket_path), "/INFORMIXTMP/%s.str", service)
        >= (int)sizeof(socket_path)) {
        fprintf(stderr, "error: service name too long\n");
        return 1;
    }

    sqli_conn_t *conn = NULL;
    sqli_result_t *result = NULL;

    if (sqli_create(&conn) != SQLI_OK) {
        fprintf(stderr, "sqli_create failed\n");
        return 1;
    }

    sqli_connect_params params;
    memset(&params, 0, sizeof(params));
    params.hostname  = "";               /* required non-NULL, unused for IPC */
    params.service   = socket_path;      /* leading '/' -> Unix socket mode */
    params.server    = service;
    params.database  = "sysmaster";
    params.db_locale = "en_US.8859-1";

    if (sqli_connect(conn, &params) != SQLI_OK) {
        fprintf(stderr, "connect failed (%s): %s\n", socket_path, sqli_error(conn));
        sqli_destroy(conn);
        return 1;
    }

    if (sqli_query(conn, "SELECT * FROM sysdbslocale", &result) != SQLI_OK) {
        fprintf(stderr, "query failed: %s\n", sqli_error(conn));
        sqli_close(conn);
        sqli_destroy(conn);
        return 1;
    }

    int cols = sqli_result_columns(result);
    for (int c = 0; c < cols; c++) {
        printf("%s%s", c > 0 ? " | " : "", sqli_result_column_name(result, c));
    }
    printf("\n");

    long long row_count = 0;
    while (sqli_result_next(result)) {
        for (int c = 0; c < cols; c++) {
            if (c > 0)
                printf(" | ");
            if (sqli_result_is_null(result, c)) {
                printf("(null)");
                continue;
            }
            switch (sqli_result_column_type(result, c)) {
            case SQLI_TYPE_SMALLINT:
            case SQLI_TYPE_INT:
            case SQLI_TYPE_SERIAL:
                printf("%d", sqli_result_get_int(result, c));
                break;
            case SQLI_TYPE_INT8:
            case SQLI_TYPE_BIGINT:
            case SQLI_TYPE_SERIAL8:
            case SQLI_TYPE_BIGSERIAL:
                printf("%lld", (long long)sqli_result_get_int64(result, c));
                break;
            default:
                printf("%s", sqli_result_get_string(result, c));
                break;
            }
        }
        printf("\n");
        row_count++;
    }

    printf("-- %lld row(s)\n", row_count);

    sqli_result_destroy(result);
    sqli_close(conn);
    sqli_destroy(conn);
    return 0;
}
