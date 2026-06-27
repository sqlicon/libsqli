#include "sqli_internal.h"
#include "sqli_protocol_internal.h"

#include "sqli_log.h"
#include "sqli_tcp.h"

#include <stdlib.h>

sqli_status sqli_send_eot(int fd)
{
    uint8_t msg[2] = {0, SQLI_SQ_EOT};
    ssize_t n = sqli_tcp_send(fd, msg, 2);
    if (n != 2)
        return SQLI_IO_ERROR;
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * SQ_PREPARE — send PREPARE + NDESCRIBE + WANTDONE + EOT (phase 1)
 *
 * Client always uses SQ_PREPARE (opcode 2), never SQ_COMMAND (opcode 1).
 * Server returns DESCRIBE + DONE. Caller then sends SQ_EXECUTE (phase 2).
 * ---------------------------------------------------------------- */

sqli_status sqli_send_prepare(sqli_conn_t *conn, const char *sql)
{
    if (conn == NULL || sql == NULL)
        return SQLI_INVALID_STATE;

    int fd = conn->socket_fd;
    uint8_t *sql_db = NULL;
    size_t sql_len = 0;
    sqli_status enc_rc = sqli_conn_encode_client_to_db(conn, sql, &sql_db, &sql_len);
    if (enc_rc != SQLI_OK || sql_db == NULL)
        return (enc_rc == SQLI_OK) ? SQLI_ALLOC_FAIL : enc_rc;

    int qmarks = 0;
    for (const char *p = sql; *p != '\0'; p++) {
        if (*p == '?')
            qmarks++;
    }
    if (qmarks < 0)
        qmarks = 0;
    if (qmarks > 0xFFFF)
        qmarks = 0xFFFF;

    /* SQ_PREPARE (2) + numqmarks (2) */
    uint8_t hdr[4] = {
        0, SQLI_SQ_PREPARE,
        (uint8_t)((qmarks >> 8) & 0xFF),
        (uint8_t)(qmarks & 0xFF)
    };
    if (sqli_tcp_send(fd, hdr, 4) != 4) {
        free(sql_db);
        return SQLI_IO_ERROR;
    }

    /* SQL: 4-byte length if remove_64k_limit, else 2-byte */
    if (conn->caps.remove_64k_limit) {
        uint8_t len4[4] = {
            (uint8_t)((sql_len >> 24) & 0xFF),
            (uint8_t)((sql_len >> 16) & 0xFF),
            (uint8_t)((sql_len >>  8) & 0xFF),
            (uint8_t)( sql_len        & 0xFF)
        };
        if (sqli_tcp_send(fd, len4, 4) != 4) {
            free(sql_db);
            return SQLI_IO_ERROR;
        }
    } else {
        uint8_t len2[2] = {
            (uint8_t)((sql_len >> 8) & 0xFF),
            (uint8_t)( sql_len       & 0xFF)
        };
        if (sqli_tcp_send(fd, len2, 2) != 2) {
            free(sql_db);
            return SQLI_IO_ERROR;
        }
    }

    if (sqli_tcp_send(fd, sql_db, sql_len) != (ssize_t)sql_len) {
        free(sql_db);
        return SQLI_IO_ERROR;
    }
    if (sql_len % 2 != 0) {
        uint8_t pad = 0;
        sqli_tcp_send(fd, &pad, 1);
    }
    free(sql_db);

    /* SQ_NDESCRIBE (22) + SQ_WANTDONE (49) + SQ_EOT (12) */
    uint8_t tail[6] = {
        0, SQLI_SQ_NDESCRIBE,
        0, SQLI_SQ_WANTDONE,
        0, SQLI_SQ_EOT
    };
    if (sqli_tcp_send(fd, tail, 6) != 6)
        return SQLI_IO_ERROR;

    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * SQ_EXECUTE — send ID + EXECUTE + EOT (phase 2, DML only)
 * ---------------------------------------------------------------- */

sqli_status sqli_send_execute(int fd, int stmt_id)
{
    /* SQ_ID(4) + SQ_EXECUTE(2) + SQ_EOT(2) */
    uint8_t msg[8] = {
        0, SQLI_SQ_ID,
        (uint8_t)((stmt_id >> 8) & 0xFF),
        (uint8_t)( stmt_id       & 0xFF),
        0, SQLI_SQ_EXECUTE,
        0, SQLI_SQ_EOT
    };
    if (sqli_tcp_send(fd, msg, 8) != 8)
        return SQLI_IO_ERROR;
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * SQ_OPEN — open cursor (SELECT phase 2, step 1)
 *
 * Wire layout (from Client sendQuery):
 *   SQ_ID(4) SQ_CURNAME(2) cursor_len(2) cursor_bytes SQ_OPEN(2) SQ_EOT(2)
 * ---------------------------------------------------------------- */

sqli_status sqli_send_open(int fd, int stmt_id, sqli_cursor_type cursor_type,
                           sqli_cursor_holdability holdability)
{
    /* Match Client cursor naming style */
    static const uint8_t curname[] = "_ifxc000000000000";
    const uint16_t curlen = (uint16_t)(sizeof(curname) - 1);

    /* SQ_ID + SQ_CURNAME + cursor_name + SQ_OPEN + SQ_EOT */
    uint8_t open_hdr[8] = {
        0, SQLI_SQ_ID,
        (uint8_t)((stmt_id >> 8) & 0xFF),
        (uint8_t)( stmt_id       & 0xFF),
        0, 3,                      /* SQ_CURNAME */
        (uint8_t)(curlen >> 8),
        (uint8_t)(curlen & 0xFF)
    };
    if (sqli_tcp_send(fd, open_hdr, 8) != 8)
        return SQLI_IO_ERROR;
    if (sqli_tcp_send(fd, curname, (size_t)curlen) != (ssize_t)curlen)
        return SQLI_IO_ERROR;
    if (curlen & 1) {
        uint8_t pad = 0;
        if (sqli_tcp_send(fd, &pad, 1) != 1)
            return SQLI_IO_ERROR;
    }

    if (cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
        uint8_t sq_scroll[2] = {0, SQLI_SQ_SCROLL};
        if (sqli_tcp_send(fd, sq_scroll, 2) != 2)
            return SQLI_IO_ERROR;
    }
    if (holdability == SQLI_CURSOR_HOLD_OVER_COMMIT) {
        uint8_t sq_hold[2] = {0, SQLI_SQ_HOLD};
        if (sqli_tcp_send(fd, sq_hold, 2) != 2)
            return SQLI_IO_ERROR;
    }

    uint8_t sq_open[2] = {0, 6};  /* SQ_OPEN */
    if (sqli_tcp_send(fd, sq_open, 2) != 2)
        return SQLI_IO_ERROR;

    uint8_t sq_eot[2] = {0, SQLI_SQ_EOT};
    if (sqli_tcp_send(fd, sq_eot, 2) != 2)
        return SQLI_IO_ERROR;

    return SQLI_OK;
}

static int result_has_variable_columns(const sqli_result_t *result)
{
    if (result == NULL || result->columns == NULL || result->column_count <= 0)
        return 0;

    for (int i = 0; i < result->column_count; i++) {
        uint8_t t = (uint8_t)result->columns[i].type;
        switch (t) {
        case SQLI_TYPE_VARCHAR:
        case SQLI_TYPE_NVCHAR:
        case SQLI_TYPE_TEXT:
        case SQLI_TYPE_BYTE:
        case SQLI_TYPE_DECIMAL:
        case SQLI_TYPE_MONEY:
        case SQLI_TYPE_DATETIME:
        case SQLI_TYPE_INTERVAL:
        case SQLI_TYPE_BLOB:
        case SQLI_TYPE_CLOB:
            return 1;
        default:
            break;
        }
    }
    return 0;
}

static int result_has_lvarchar_columns(const sqli_result_t *result)
{
    if (result == NULL || result->columns == NULL || result->column_count <= 0)
        return 0;
    for (int i = 0; i < result->column_count; i++) {
        if ((uint8_t)result->columns[i].type == SQLI_TYPE_LVARCHAR)
            return 1;
    }
    return 0;
}

static uint32_t sqli_fetch_target_rows(void)
{
    const char *v = getenv("SQLI_FETCH_TARGET_ROWS");
    if (v == NULL || *v == '\0')
        return 1000u;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (end == v || *end != '\0' || n < 1ul || n > 65536ul)
        return 1000u;
    return (uint32_t)n;
}

static int sqli_is_variable_type(uint8_t t)
{
    switch (t) {
    case SQLI_TYPE_VARCHAR:
    case SQLI_TYPE_NVCHAR:
    case SQLI_TYPE_TEXT:
    case SQLI_TYPE_BYTE:
    case SQLI_TYPE_LVARCHAR:
    case SQLI_TYPE_DECIMAL:
    case SQLI_TYPE_MONEY:
    case SQLI_TYPE_DATETIME:
    case SQLI_TYPE_INTERVAL:
    case SQLI_TYPE_BLOB:
    case SQLI_TYPE_CLOB:
        return 1;
    default:
        return 0;
    }
}

static size_t sqli_column_payload_bytes(const sqli_column_info *c)
{
    if (c == NULL)
        return 8u;
    uint8_t t = (uint8_t)c->type;
    switch (t) {
    case SQLI_TYPE_BOOL:
    case SQLI_TYPE_DBOOLEAN: return 1u;
    case SQLI_TYPE_SMALLINT: return 2u;
    case SQLI_TYPE_INT:
    case SQLI_TYPE_DATE:
    case SQLI_TYPE_SERIAL: return 4u;
    case SQLI_TYPE_FLOAT:
    case SQLI_TYPE_BIGINT:
    case SQLI_TYPE_BIGSERIAL: return 8u;
    case SQLI_TYPE_INT8:
    case SQLI_TYPE_SERIAL8: return 10u;
    case SQLI_TYPE_CHAR:
    case SQLI_TYPE_NCHAR:
        return (c->encoded_length > 0) ? (size_t)c->encoded_length : 16u;
    case SQLI_TYPE_VARCHAR:
    case SQLI_TYPE_NVCHAR: {
        uint32_t n = c->encoded_length;
        if (n == 0 || n > 65535u) n = 64u;
        return 1u + (size_t)n;
    }
    case SQLI_TYPE_TEXT:
    case SQLI_TYPE_BYTE:
    case SQLI_TYPE_LVARCHAR:
    case SQLI_TYPE_BLOB:
    case SQLI_TYPE_CLOB: {
        uint32_t n = c->encoded_length;
        if (n == 0 || n > 1048576u) n = 256u;
        if (t == SQLI_TYPE_LVARCHAR)
            return 5u + (size_t)n;
        return 2u + (size_t)n;
    }
    case SQLI_TYPE_DECIMAL:
    case SQLI_TYPE_MONEY: {
        uint8_t p = (uint8_t)((c->encoded_length >> 8) & 0xFFu);
        if (p == 0 || p > 32) p = 16;
        return (size_t)((p + 3u) / 2u);
    }
    case SQLI_TYPE_DATETIME:
    case SQLI_TYPE_INTERVAL: {
        uint8_t qlen = (uint8_t)((c->encoded_length >> 8) & 0xFFu);
        if (qlen == 0) qlen = 8;
        return 1u + (size_t)((qlen + 1u) / 2u);
    }
    default:
        return 8u;
    }
}

static size_t sqli_estimate_tuple_bytes(const sqli_result_t *result)
{
    if (result == NULL || result->columns == NULL || result->column_count <= 0)
        return 256u;

    size_t max_end = 0u;
    for (int i = 0; i < result->column_count; i++) {
        const sqli_column_info *c = &result->columns[i];
        size_t end = (size_t)c->col_start_pos + sqli_column_payload_bytes(c);
        if (end > max_end)
            max_end = end;
    }
    if (result->column_count > 0) {
        const sqli_column_info *last = &result->columns[result->column_count - 1];
        if (sqli_is_variable_type((uint8_t)last->type)) {
            size_t via_last = (size_t)last->col_start_pos + sqli_column_payload_bytes(last) + 5u;
            if (via_last > max_end)
                max_end = via_last;
        }
    }
    return max_end > 0 ? max_end : 256u;
}

static uint32_t sqli_choose_fetch_bufsize(const sqli_result_t *result)
{
    uint32_t max_buf = 4194304u;
    if (result != NULL && result->owner_conn != NULL && result->owner_conn->fetch_buf_size >= 1024u)
        max_buf = result->owner_conn->fetch_buf_size;
    if (max_buf < 1024u)
        max_buf = 1024u;

    size_t max_tuple = 0u;
    if (result != NULL && result->adaptive_tuple_count > 0u) {
        max_tuple = (size_t)(result->adaptive_tuple_bytes_total / (uint64_t)result->adaptive_tuple_count);
    }
    size_t described_tuple = sqli_estimate_tuple_bytes(result);
    if (described_tuple > max_tuple)
        max_tuple = described_tuple;
    if (max_tuple == 0u)
        max_tuple = 256u;

    uint64_t desired = (uint64_t)max_tuple * (uint64_t)sqli_fetch_target_rows();
    if (desired == 0u) {
        if (max_tuple <= 2048u)
            desired = 4096u;
        else if (max_tuple <= 4096u)
            desired = 8192u;
        else
            desired = (uint64_t)max_tuple;
    }

    if (desired < 1024u)
        desired = 1024u;
    if (desired > (uint64_t)max_buf)
        desired = max_buf;

    return (uint32_t)desired;
}

/* ----------------------------------------------------------------
 * SQ_NFETCH — fetch rows (SELECT phase 2, step 2)
 *
 * Client sends optional SQ_RET_TYPE before SQ_NFETCH when variable-length
 * columns are present in the result metadata.
 * ---------------------------------------------------------------- */
sqli_status sqli_send_fetch(int fd, int stmt_id, sqli_result_t *result)
{
    uint8_t id_msg[4] = {
        0, SQLI_SQ_ID,
        (uint8_t)((stmt_id >> 8) & 0xFF),
        (uint8_t)( stmt_id       & 0xFF)
    };
    if (sqli_tcp_send(fd, id_msg, 4) != 4)
        return SQLI_IO_ERROR;

    if (result_has_variable_columns(result) &&
        !result_has_lvarchar_columns(result) &&
        (result == NULL || !result->ret_type_sent)) {
        uint8_t hdr[6] = {
            0, SQLI_SQ_RET_TYPE,
            0, 1, /* direction = FETCH */
            (uint8_t)((result->column_count >> 8) & 0xFF),
            (uint8_t)(result->column_count & 0xFF)
        };
        if (sqli_tcp_send(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr))
            return SQLI_IO_ERROR;

        for (int i = 0; i < result->column_count; i++) {
            const sqli_column_info *col = &result->columns[i];
            uint8_t colmsg[6] = {
                0, (uint8_t)col->type,
                (uint8_t)((col->encoded_length >> 24) & 0xFF),
                (uint8_t)((col->encoded_length >> 16) & 0xFF),
                (uint8_t)((col->encoded_length >> 8) & 0xFF),
                (uint8_t)(col->encoded_length & 0xFF)
            };
            if (sqli_tcp_send(fd, colmsg, sizeof(colmsg)) != (ssize_t)sizeof(colmsg))
                return SQLI_IO_ERROR;
        }
        if (result != NULL)
            result->ret_type_sent = true;
    }

    /* FETCH: SQ_NFETCH + bufsize + array_size(0) + EOT */
    uint32_t fetch_bufsize = sqli_choose_fetch_bufsize(result);
    if (result != NULL)
        result->adaptive_fetch_buf_size = fetch_bufsize;
    uint8_t fetch_msg[8] = {
        0, SQLI_SQ_NFETCH,
        (uint8_t)((fetch_bufsize >> 24) & 0xFF),
        (uint8_t)((fetch_bufsize >> 16) & 0xFF),
        (uint8_t)((fetch_bufsize >>  8) & 0xFF),
        (uint8_t)( fetch_bufsize        & 0xFF),
        0, 0                        /* array_size = 0 */
    };
    if (sqli_tcp_send(fd, fetch_msg, sizeof(fetch_msg)) != (ssize_t)sizeof(fetch_msg))
        return SQLI_IO_ERROR;

    uint8_t sq_eot[2] = {0, SQLI_SQ_EOT};
    if (sqli_tcp_send(fd, sq_eot, 2) != 2)
        return SQLI_IO_ERROR;

    return SQLI_OK;
}

sqli_status sqli_send_scroll_fetch(int fd, int stmt_id, sqli_result_t *result,
                                   uint16_t scroll_type, int32_t index)
{
    uint8_t id_msg[4] = {
        0, SQLI_SQ_ID,
        (uint8_t)((stmt_id >> 8) & 0xFF),
        (uint8_t)( stmt_id       & 0xFF)
    };
    if (sqli_tcp_send(fd, id_msg, 4) != 4)
        return SQLI_IO_ERROR;

    if (result_has_variable_columns(result) &&
        !result_has_lvarchar_columns(result) &&
        (result == NULL || !result->ret_type_sent)) {
        uint8_t hdr[6] = {
            0, SQLI_SQ_RET_TYPE,
            0, 1, /* direction = FETCH */
            (uint8_t)((result->column_count >> 8) & 0xFF),
            (uint8_t)(result->column_count & 0xFF)
        };
        if (sqli_tcp_send(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr))
            return SQLI_IO_ERROR;

        for (int i = 0; i < result->column_count; i++) {
            const sqli_column_info *col = &result->columns[i];
            uint8_t colmsg[6] = {
                0, (uint8_t)col->type,
                (uint8_t)((col->encoded_length >> 24) & 0xFF),
                (uint8_t)((col->encoded_length >> 16) & 0xFF),
                (uint8_t)((col->encoded_length >> 8) & 0xFF),
                (uint8_t)(col->encoded_length & 0xFF)
            };
            if (sqli_tcp_send(fd, colmsg, sizeof(colmsg)) != (ssize_t)sizeof(colmsg))
                return SQLI_IO_ERROR;
        }
        if (result != NULL)
            result->ret_type_sent = true;
    }

    uint32_t fetch_bufsize = sqli_choose_fetch_bufsize(result);
    if (result != NULL)
        result->adaptive_fetch_buf_size = fetch_bufsize;

    uint8_t msg[12] = {
        0, SQLI_SQ_SFETCH,
        (uint8_t)((scroll_type >> 8) & 0xFF),
        (uint8_t)( scroll_type       & 0xFF),
        (uint8_t)((index >> 24) & 0xFF),
        (uint8_t)((index >> 16) & 0xFF),
        (uint8_t)((index >>  8) & 0xFF),
        (uint8_t)( index        & 0xFF),
        (uint8_t)((fetch_bufsize >> 24) & 0xFF),
        (uint8_t)((fetch_bufsize >> 16) & 0xFF),
        (uint8_t)((fetch_bufsize >>  8) & 0xFF),
        (uint8_t)( fetch_bufsize        & 0xFF)
    };
    if (sqli_tcp_send(fd, msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        return SQLI_IO_ERROR;

    uint8_t sq_eot[2] = {0, SQLI_SQ_EOT};
    if (sqli_tcp_send(fd, sq_eot, 2) != 2)
        return SQLI_IO_ERROR;

    return SQLI_OK;
}

static sqli_status sqli_send_stmt_control(sqli_conn_t *conn, int stmt_id, uint8_t opcode)
{
    if (conn == NULL)
        return SQLI_INVALID_STATE;
    uint8_t msg[8] = {
        0, SQLI_SQ_ID,
        (uint8_t)((stmt_id >> 8) & 0xFF),
        (uint8_t)(stmt_id & 0xFF),
        0, opcode,
        0, SQLI_SQ_EOT
    };
    if (sqli_tcp_send(conn->socket_fd, msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        return SQLI_IO_ERROR;

    sqli_result_t ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.owner_conn = conn;
    sqli_status rc = sqli_receive_dispatch(conn->socket_fd, &ctrl, conn);
    sqli_result_cleanup(&ctrl);
    return rc;
}

void sqli_stmt_close_release(sqli_conn_t *conn, int stmt_id)
{
    sqli_status rc = sqli_send_stmt_control(conn, stmt_id, 10); /* SQ_CLOSE */
    if (rc != SQLI_OK)
        sqli_log(SQLI_LOG_DEBUG, "close stmt failed rc=%d", rc);
    rc = sqli_send_stmt_control(conn, stmt_id, SQLI_SQ_RELEASE);
    if (rc != SQLI_OK)
        sqli_log(SQLI_LOG_DEBUG, "release stmt failed rc=%d", rc);
}
