#define _POSIX_C_SOURCE 200809L
#include "libsqli/sqli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_item(size_t index, const sqli_batch_item_result *item)
{
    printf("item[%zu]: status=%d rows=%lld sqlcode=%d isam=%d opcode=%u message=%s\n",
           index,
           (int)item->status,
           (long long)item->rows_affected,
           item->sqlcode,
           item->isamcode,
           (unsigned)item->opcode,
           item->message[0] != '\0' ? item->message : "<empty>");
}

int main(int argc, char **argv)
{
    sqli_conn_t *conn = NULL;
    sqli_connect_params params;
    sqli_batch_result_t *batch = NULL;
    const char *default_sqls[] = {
        "SELECT FIRST 1 tabname FROM systables",
        "SELECT FIRST 1 owner FROM systables"
    };
    const char **sqls = default_sqls;
    size_t sql_count = sizeof(default_sqls) / sizeof(default_sqls[0]);
    int rc = 1;

    if (argc < 8) {
        fprintf(stderr,
                "usage: %s <host> <port> <db> <user> <password> <server> [client_locale] [db_locale] [sql ...]\n",
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
    params.db_locale = (argc >= 9) ? argv[8] : "en_US.utf8";
    if (argc >= 10) {
        sqls = (const char **)&argv[9];
        sql_count = (size_t)(argc - 9);
    }

    if (sqli_create(&conn) != SQLI_OK || conn == NULL) {
        fprintf(stderr, "create failed\n");
        return 1;
    }

    if (sqli_connect(conn, &params) != SQLI_OK) {
        fprintf(stderr, "connect failed: %s\n", sqli_error(conn));
        goto out;
    }

    if (sqli_batch_execute(conn, sqls, sql_count, &batch) != SQLI_OK) {
        fprintf(stderr, "batch_execute failed: %s\n", sqli_error(conn));
        goto out;
    }

    printf("batch: count=%zu success=%zu error=%zu\n",
           sqli_batch_result_count(batch),
           sqli_batch_result_success_count(batch),
           sqli_batch_result_error_count(batch));

    for (size_t i = 0; i < sqli_batch_result_count(batch); i++) {
        sqli_batch_item_result item;
        if (sqli_batch_result_item(batch, i, &item) != SQLI_OK) {
            fprintf(stderr, "batch_result_item failed at index %zu\n", i);
            goto out;
        }
        print_item(i, &item);
    }

    rc = 0;

out:
    sqli_batch_result_destroy(batch);
    if (conn != NULL) {
        sqli_close(conn);
        sqli_destroy(conn);
    }
    return rc;
}
