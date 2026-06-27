#include "sqli_internal.h"

#include <stdio.h>
#include <stdlib.h>

struct sqli_batch_result {
    size_t count;
    size_t success_count;
    size_t error_count;
    sqli_batch_item_result *items;
};

static void sqli_batch_fill_error_item(sqli_conn_t *conn, sqli_batch_item_result *it,
                                       sqli_status rc)
{
    if (it == NULL)
        return;
    it->status = rc;
    it->rows_affected = 0;
    it->sqlcode = 0;
    it->isamcode = 0;
    it->opcode = 0;
    it->message[0] = '\0';

    if (conn == NULL)
        return;

    if (conn->error_info.has_error) {
        it->sqlcode = conn->error_info.sqlcode;
        it->isamcode = conn->error_info.isamcode;
        it->opcode = conn->error_info.opcode;
        if (conn->error_info.message[0] != '\0')
            snprintf(it->message, sizeof(it->message), "%s", conn->error_info.message);
    }
    if (it->message[0] == '\0' && sqli_error(conn) != NULL)
        snprintf(it->message, sizeof(it->message), "%s", sqli_error(conn));
}

sqli_status sqli_batch_execute(sqli_conn_t *conn, const char **sql_list,
                               size_t sql_count, sqli_batch_result_t **out_batch)
{
    if (conn == NULL || sql_list == NULL || out_batch == NULL)
        return SQLI_INVALID_STATE;
    *out_batch = NULL;

    if (conn->state != SQLI_CONN_READY) {
        set_error_context(conn, "batch_execute/precheck", 0);
        set_error(conn, "connection not ready");
        return SQLI_INVALID_STATE;
    }

    sqli_batch_result_t *batch = calloc(1, sizeof(*batch));
    if (batch == NULL)
        return SQLI_ALLOC_FAIL;
    batch->count = sql_count;
    if (sql_count > 0) {
        batch->items = calloc(sql_count, sizeof(*batch->items));
        if (batch->items == NULL) {
            free(batch);
            return SQLI_ALLOC_FAIL;
        }
    }

    sqli_status rc = sqli_batch_begin(conn);
    if (rc != SQLI_OK) {
        sqli_batch_result_destroy(batch);
        return rc;
    }

    for (size_t i = 0; i < sql_count; i++) {
        sqli_batch_item_result *it = &batch->items[i];
        const char *sql = sql_list[i];

        if (sql == NULL || sql[0] == '\0') {
            it->status = SQLI_INVALID_STATE;
            snprintf(it->message, sizeof(it->message), "empty SQL statement");
            batch->error_count++;
            continue;
        }

        sqli_result_t *res = NULL;
        rc = sqli_query(conn, sql, &res);
        if (rc == SQLI_OK && res != NULL) {
            it->status = SQLI_OK;
            it->rows_affected = sqli_result_rows_affected(res);
            batch->success_count++;
            sqli_result_destroy(res);
            continue;
        }

        if (res != NULL)
            sqli_result_destroy(res);
        sqli_batch_fill_error_item(conn, it, rc);
        batch->error_count++;
    }

    rc = sqli_batch_end(conn);
    if (rc != SQLI_OK) {
        sqli_batch_result_destroy(batch);
        return rc;
    }

    *out_batch = batch;
    return SQLI_OK;
}

size_t sqli_batch_result_count(const sqli_batch_result_t *batch)
{
    return batch ? batch->count : 0u;
}

size_t sqli_batch_result_success_count(const sqli_batch_result_t *batch)
{
    return batch ? batch->success_count : 0u;
}

size_t sqli_batch_result_error_count(const sqli_batch_result_t *batch)
{
    return batch ? batch->error_count : 0u;
}

sqli_status sqli_batch_result_item(const sqli_batch_result_t *batch, size_t index,
                                   sqli_batch_item_result *out_item)
{
    if (batch == NULL || out_item == NULL || index >= batch->count)
        return SQLI_INVALID_STATE;
    *out_item = batch->items[index];
    return SQLI_OK;
}

void sqli_batch_result_destroy(sqli_batch_result_t *batch)
{
    if (batch == NULL)
        return;
    free(batch->items);
    free(batch);
}
