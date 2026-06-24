#define _GNU_SOURCE
#include "sqli_internal.h"

#include "sqli_tcp.h"
#include "sqli_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static sqli_status verify_txn_ready(sqli_conn_t *conn, const char *op)
{
    if (conn == NULL) {
        return SQLI_INVALID_STATE;
    }
    if (conn->state != SQLI_CONN_READY) {
        set_error_context(conn, op, 0);
        set_error(conn, "connection not ready");
        return SQLI_INVALID_STATE;
    }
    if (conn->socket_fd < 0) {
        set_error_context(conn, op, 0);
        set_error(conn, "socket not connected");
        return SQLI_INVALID_STATE;
    }
    (void)op;
    return SQLI_OK;
}

/* Receive and discard a transaction command response */
static void drain_txn_response(sqli_conn_t *conn)
{
    sqli_result_t result;
    memset(&result, 0, sizeof(result));
    sqli_receive_dispatch(conn->socket_fd, &result, conn);
    sqli_result_cleanup(&result);
}

static sqli_status apply_session_sql(sqli_conn_t *conn, const char *sql)
{
    sqli_result_t *result = NULL;
    sqli_status rc = sqli_query(conn, sql, &result);
    if (result != NULL)
        sqli_result_destroy(result);
    return rc;
}

static sqli_status send_char_payload(sqli_conn_t *conn, const char *name)
{
    if (conn == NULL || name == NULL)
        return SQLI_INVALID_STATE;

    uint8_t *enc = NULL;
    size_t len = 0;
    sqli_status rc = sqli_conn_encode_client_to_db(conn, name, &enc, &len);
    if (rc != SQLI_OK)
        return rc;
    if (enc == NULL || len > 0xFFFFu) {
        free(enc);
        return SQLI_INVALID_STATE;
    }

    uint8_t hdr[2];
    hdr[0] = (uint8_t)(len >> 8);
    hdr[1] = (uint8_t)(len & 0xFFu);
    rc = (sqli_tcp_send(conn->socket_fd, hdr, 2) == 2) ? SQLI_OK : SQLI_IO_ERROR;
    if (rc == SQLI_OK && len > 0) {
        rc = (sqli_tcp_send(conn->socket_fd, enc, len) == (ssize_t)len) ? SQLI_OK : SQLI_IO_ERROR;
    }
    if (rc == SQLI_OK && (len & 1u) != 0u) {
        uint8_t pad = 0;
        rc = (sqli_tcp_send(conn->socket_fd, &pad, 1) == 1) ? SQLI_OK : SQLI_IO_ERROR;
    }
    free(enc);
    return rc;
}

static sqli_status savepoint_send_core(sqli_conn_t *conn, uint8_t opcode,
                                       const char *name, bool include_unique_flag,
                                       bool unique_flag)
{
    sqli_status rc = verify_txn_ready(conn, "savepoint");
    if (rc != SQLI_OK)
        return rc;
    if (name == NULL || name[0] == '\0') {
        set_error_context(conn, "savepoint", opcode);
        set_error(conn, "savepoint name must not be empty");
        return SQLI_INVALID_STATE;
    }
    if (!conn->in_transaction) {
        if (!conn->autocommit) {
            rc = sqli_begin(conn);
            if (rc != SQLI_OK)
                return rc;
        } else {
            set_error_context(conn, "savepoint", opcode);
            set_error(conn, "savepoint requires an active transaction");
            return SQLI_INVALID_STATE;
        }
    }

    uint8_t op[2] = {0, opcode};
    if (sqli_tcp_send(conn->socket_fd, op, sizeof(op)) != (ssize_t)sizeof(op)) {
        set_error_context(conn, "savepoint", opcode);
        set_error(conn, "failed to send savepoint opcode");
        return SQLI_IO_ERROR;
    }

    rc = send_char_payload(conn, name);
    if (rc != SQLI_OK) {
        set_error_context(conn, "savepoint", opcode);
        set_error(conn, "failed to send savepoint name");
        return rc;
    }

    if (include_unique_flag) {
        uint8_t flag[2] = {0, unique_flag ? 1 : 0};
        if (sqli_tcp_send(conn->socket_fd, flag, sizeof(flag)) != (ssize_t)sizeof(flag)) {
            set_error_context(conn, "savepoint", opcode);
            set_error(conn, "failed to send savepoint flag");
            return SQLI_IO_ERROR;
        }
    }

    drain_txn_response(conn);
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * sqli_begin — send SQ_EOT + SQ_BEGIN (native opcode 0x0023)
 * ---------------------------------------------------------------- */

sqli_status sqli_begin(sqli_conn_t *conn)
{
    sqli_status rc = verify_txn_ready(conn, "begin");
    if (rc != SQLI_OK)
        return rc;

    if (conn->in_transaction) {
        set_error_context(conn, "begin", SQLI_SQ_BEGIN);
        set_error(conn, "transaction already in progress");
        return SQLI_INVALID_STATE;
    }

    sqli_log(SQLI_LOG_DEBUG, "begin");

    /* SQ_EOT(0x000C) + SQ_BEGIN(0x0023) */
    uint8_t msg[] = { 0x00, SQLI_SQ_EOT, 0x00, SQLI_SQ_BEGIN };
    if (sqli_tcp_send(conn->socket_fd, msg, 4) != 4) {
        set_error_context(conn, "begin", SQLI_SQ_BEGIN);
        set_error(conn, "failed to send BEGIN");
        return SQLI_IO_ERROR;
    }

    drain_txn_response(conn);

    conn->in_transaction = true;
    sqli_log(SQLI_LOG_INFO, "transaction started (isolation=%d)", conn->isolation);
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * sqli_commit — send SQ_EOT + SQ_CMMTWORK (native opcode 0x0013)
 * ---------------------------------------------------------------- */

sqli_status sqli_commit(sqli_conn_t *conn)
{
    sqli_status rc = verify_txn_ready(conn, "commit");
    if (rc != SQLI_OK)
        return rc;

    if (!conn->in_transaction) {
        set_error_context(conn, "commit", SQLI_SQ_CMMTWORK);
        set_error(conn, "no active transaction to commit");
        return SQLI_INVALID_STATE;
    }

    sqli_log(SQLI_LOG_DEBUG, "commit");

    /* SQ_EOT(0x000C) + SQ_CMMTWORK(0x0013) */
    uint8_t msg[] = { 0x00, SQLI_SQ_EOT, 0x00, SQLI_SQ_CMMTWORK };
    if (sqli_tcp_send(conn->socket_fd, msg, 4) != 4) {
        set_error_context(conn, "commit", SQLI_SQ_CMMTWORK);
        set_error(conn, "failed to send COMMIT");
        return SQLI_IO_ERROR;
    }

    drain_txn_response(conn);

    conn->in_transaction = false;
    sqli_log(SQLI_LOG_INFO, "transaction committed");
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * sqli_rollback — send SQ_EOT + SQ_RBWORK (native opcode 0x0014)
 *                 + 0x0000 padding (required by spec)
 * ---------------------------------------------------------------- */

sqli_status sqli_rollback(sqli_conn_t *conn)
{
    sqli_status rc = verify_txn_ready(conn, "rollback");
    if (rc != SQLI_OK)
        return rc;

    if (!conn->in_transaction) {
        set_error_context(conn, "rollback", SQLI_SQ_RBWORK);
        set_error(conn, "no active transaction to roll back");
        return SQLI_INVALID_STATE;
    }

    sqli_log(SQLI_LOG_DEBUG, "rollback");

    /* SQ_EOT(0x000C) + SQ_RBWORK(0x0014) + 0x0000 (padding short) */
    uint8_t msg[] = { 0x00, SQLI_SQ_EOT,
                      0x00, SQLI_SQ_RBWORK,
                      0x00, 0x00 };
    if (sqli_tcp_send(conn->socket_fd, msg, 6) != 6) {
        set_error_context(conn, "rollback", SQLI_SQ_RBWORK);
        set_error(conn, "failed to send ROLLBACK");
        return SQLI_IO_ERROR;
    }

    drain_txn_response(conn);

    conn->in_transaction = false;
    sqli_log(SQLI_LOG_INFO, "transaction rolled back");
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * sqli_set_autocommit / sqli_get_autocommit
 * ---------------------------------------------------------------- */

sqli_status sqli_set_autocommit(sqli_conn_t *conn, bool on)
{
    if (conn == NULL)
        return SQLI_INVALID_STATE;

    if (!on && conn->in_transaction) {
        conn->in_transaction = false;
        sqli_log(SQLI_LOG_WARN, "autocommit off: ended pending transaction");
    }

    conn->autocommit = on;
    sqli_log(SQLI_LOG_DEBUG, "autocommit=%d", on ? 1 : 0);
    return SQLI_OK;
}

bool sqli_get_autocommit(sqli_conn_t *conn)
{
    if (conn == NULL)
        return false;
    return conn->autocommit;
}

/* ----------------------------------------------------------------
 * sqli_set_isolation_level / sqli_get_isolation_level
 * ---------------------------------------------------------------- */

sqli_status sqli_set_isolation_level(sqli_conn_t *conn, sqli_isolation_level level)
{
    if (conn == NULL)
        return SQLI_INVALID_STATE;

    if ((int)level < 0 || (int)level > 4)
        return SQLI_INVALID_STATE;

    if (conn->in_transaction) {
        set_error(conn, "isolation level cannot be changed in active transaction");
        return SQLI_INVALID_STATE;
    }

    const char *sql = NULL;
    switch (level) {
    case SQLI_TXN_ISOLATION_COMMITTED:
        sql = "set isolation to committed read";
        break;
    case SQLI_TXN_ISOLATION_REPEATABLE_READ:
    case SQLI_TXN_ISOLATION_SERIALIZABLE:
        sql = "set isolation to repeatable read";
        break;
    case SQLI_TXN_ISOLATION_CURSOR:
        sql = "set isolation to cursor stability";
        break;
    default:
        return SQLI_INVALID_STATE;
    }

    if (conn->state == SQLI_CONN_READY && conn->socket_fd >= 0) {
        sqli_status rc = apply_session_sql(conn, sql);
        if (rc != SQLI_OK) {
            set_error_context(conn, "isolation", SQLI_SQ_ISOLEVEL);
            return rc;
        }
    }

    conn->isolation = level;
    sqli_log(SQLI_LOG_DEBUG, "isolation level set to %d", level);
    return SQLI_OK;
}

sqli_isolation_level sqli_get_isolation_level(sqli_conn_t *conn)
{
    if (conn == NULL)
        return SQLI_TXN_ISOLATION_COMMITTED;
    return conn->isolation;
}

/* ----------------------------------------------------------------
 * sqli_in_transaction
 * ---------------------------------------------------------------- */

bool sqli_in_transaction(sqli_conn_t *conn)
{
    if (conn == NULL)
        return false;
    return conn->in_transaction;
}

sqli_status sqli_set_lock_wait(sqli_conn_t *conn, int seconds)
{
    sqli_status rc = verify_txn_ready(conn, "lock_wait");
    if (rc != SQLI_OK)
        return rc;

    char sql[96];
    if (seconds < 0) {
        snprintf(sql, sizeof(sql), "set lock mode to not wait");
    } else {
        snprintf(sql, sizeof(sql), "set lock mode to wait %d", seconds);
    }
    rc = apply_session_sql(conn, sql);
    if (rc != SQLI_OK)
        set_error_context(conn, "lock_wait", SQLI_SQ_LOCKWAIT);
    return rc;
}

sqli_status sqli_savepoint_set(sqli_conn_t *conn, const char *name, bool unique_name)
{
    return savepoint_send_core(conn, SQLI_SQ_SQLISETSVPT, name, true, unique_name);
}

sqli_status sqli_savepoint_release(sqli_conn_t *conn, const char *name)
{
    return savepoint_send_core(conn, SQLI_SQ_SQLIRELSVPT, name, false, false);
}

sqli_status sqli_savepoint_rollback(sqli_conn_t *conn, const char *name)
{
    return savepoint_send_core(conn, SQLI_SQ_SQLIRBACKSVPT, name, false, false);
}

/* ----------------------------------------------------------------
 * Batch protocol framing
 * ---------------------------------------------------------------- */

sqli_status sqli_batch_begin(sqli_conn_t *conn)
{
    sqli_status rc = verify_txn_ready(conn, "batch_begin");
    if (rc != SQLI_OK)
        return rc;

    if (conn->in_batch) {
        set_error_context(conn, "batch_begin", SQLI_SQ_BATCHSTART);
        set_error(conn, "batch already active");
        return SQLI_INVALID_STATE;
    }

    uint8_t msg[2] = {0x00, SQLI_SQ_BATCHSTART};
    if (sqli_tcp_send(conn->socket_fd, msg, sizeof(msg)) != (ssize_t)sizeof(msg)) {
        set_error_context(conn, "batch_begin", SQLI_SQ_BATCHSTART);
        set_error(conn, "failed to send SQ_BATCHSTART");
        return SQLI_IO_ERROR;
    }

    conn->in_batch = true;
    sqli_log(SQLI_LOG_DEBUG, "batch started");
    return SQLI_OK;
}

sqli_status sqli_batch_end(sqli_conn_t *conn)
{
    sqli_status rc = verify_txn_ready(conn, "batch_end");
    if (rc != SQLI_OK)
        return rc;

    if (!conn->in_batch) {
        set_error_context(conn, "batch_end", SQLI_SQ_BATCHEND);
        set_error(conn, "no active batch");
        return SQLI_INVALID_STATE;
    }

    uint8_t msg[2] = {0x00, SQLI_SQ_BATCHEND};
    if (sqli_tcp_send(conn->socket_fd, msg, sizeof(msg)) != (ssize_t)sizeof(msg)) {
        set_error_context(conn, "batch_end", SQLI_SQ_BATCHEND);
        set_error(conn, "failed to send SQ_BATCHEND");
        return SQLI_IO_ERROR;
    }

    conn->in_batch = false;
    sqli_log(SQLI_LOG_DEBUG, "batch ended");
    return SQLI_OK;
}

bool sqli_in_batch(sqli_conn_t *conn)
{
    if (conn == NULL)
        return false;
    return conn->in_batch;
}
