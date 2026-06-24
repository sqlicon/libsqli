/*
 * Wire protocol capture: connects to a real Informix server and logs
 * all bytes sent/received via SQLI_LOG_BYTES=1 in sqli_tcp.c.
 *
 * Usage:
 *   export SQLI_LOG_BYTES=1
 *   ./sqli_test --capture <host> <port> <user> <pass> <db>
 *
 * After capture, examine the [BYT] lines in stderr for wire protocol data.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "libsqli/sqli.h"

__attribute__((constructor)) static void init_sigpipe(void)
{
    signal(SIGPIPE, SIG_IGN);
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <host> <port> <user> <password> <database>\n", prog);
}

int main(int argc, char *argv[])
{
    if (argc < 6) {
        print_usage(argv[0]);
        return 1;
    }

    const char *hostname = argv[1];
    const char *port     = argv[2];
    const char *username = argv[3];
    const char *password = argv[4];
    const char *database = argv[5];

    /* Enable debug logging for handshake phases */
    sqli_log_set_level(SQLI_LOG_DEBUG);

    fprintf(stderr, "[CAP] Connecting to %s:%s db=%s user=%s\n",
            hostname, port, database, username);

    /* Step 1: Create connection */
    sqli_conn_t *conn = NULL;
    if (sqli_create(&conn) != SQLI_OK) {
        fprintf(stderr, "[CAP] sqli_create failed\n");
        return 1;
    }

    sqli_connect_params params;
    memset(&params, 0, sizeof(params));
    params.hostname = hostname;
    params.service  = port;
    params.username = username;
    params.password = password;
    params.database = database;

    /* Step 2: Connect (full handshake) */
    fprintf(stderr, "[CAP] Calling sqli_connect...\n");
    sqli_status rc = sqli_connect(conn, &params);
    fprintf(stderr, "[CAP] sqli_connect returned: %d\n", rc);

    if (rc != SQLI_OK) {
        const char *err = sqli_error(conn);
        fprintf(stderr, "[CAP] Error: %s\n", err ? err : "(null)");
        sqli_destroy(conn);
        return 1;
    }

    fprintf(stderr, "[CAP] CONNECTED OK\n");

    /* Step 3: Simple query — SELECT */
    fprintf(stderr, "[CAP] Running SELECT 1, 2, 3...\n");
    sqli_result_t *result = NULL;
    rc = sqli_query(conn, "SELECT 1, 2, 3", &result);
    fprintf(stderr, "[CAP] sqli_query(SELECT 1,2,3) returned: %d\n", rc);

    if (rc == SQLI_OK && result != NULL) {
        int ncols = sqli_result_columns(result);
        int64_t nrows = sqli_result_rows_affected(result);
        fprintf(stderr, "[CAP] Columns: %d, Rows: %ld\n", ncols, (long)nrows);

        int has_next;
        int row = 0;
        while ((has_next = sqli_result_next(result)) != 0) {
            row++;
            fprintf(stderr, "[CAP] Row %d: [%d, %d, %d]\n",
                    row,
                    sqli_result_get_int(result, 0),
                    sqli_result_get_int(result, 1),
                    sqli_result_get_int(result, 2));
        }
        sqli_result_destroy(result);
    }

    /* Step 4: DDL query — DESCRIBE(0 cols) */
    fprintf(stderr, "[CAP] Running CREATE TEMP TABLE...\n");
    rc = sqli_query(conn, "CREATE TEMP TABLE t1(x INT)", &result);
    fprintf(stderr, "[CAP] CREATE TABLE returned: %d\n", rc);
    if (rc == SQLI_OK && result != NULL) {
        sqli_result_destroy(result);
    }

    /* Step 5: Transaction — BEGIN */
    fprintf(stderr, "[CAP] BEGIN...\n");
    rc = sqli_begin(conn);
    fprintf(stderr, "[CAP] BEGIN returned: %d\n", rc);

    /* Step 6: Query inside transaction */
    fprintf(stderr, "[CAP] SELECT 42 inside txn...\n");
    rc = sqli_query(conn, "SELECT 42", &result);
    if (rc == SQLI_OK && result != NULL) {
        sqli_result_destroy(result);
    }

    /* Step 7: COMMIT */
    fprintf(stderr, "[CAP] COMMIT...\n");
    rc = sqli_commit(conn);
    fprintf(stderr, "[CAP] COMMIT returned: %d\n", rc);

    /* Step 8: Another transaction — BEGIN/ROLLBACK */
    fprintf(stderr, "[CAP] BEGIN again...\n");
    sqli_begin(conn);
    fprintf(stderr, "[CAP] ROLLBACK...\n");
    sqli_rollback(conn);

    /* Step 9: Close (sends SQ_EXIT) */
    fprintf(stderr, "[CAP] Closing connection...\n");
    sqli_close(conn);

    fprintf(stderr, "[CAP] DONE\n");
    fprintf(stderr, "\n=== Byte capture complete ===\n");
    fprintf(stderr, "All wire protocol bytes were logged above with [BYT] prefix.\n");
    fprintf(stderr, "Use them to build mock server responses.\n");

    sqli_destroy(conn);
    return 0;
}
