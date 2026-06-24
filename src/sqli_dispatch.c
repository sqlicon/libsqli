#define _GNU_SOURCE
#include "sqli_internal.h"

#include "sqli_tcp.h"
#include "sqli_log.h"
#include "sqli_msg_catalog.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <endian.h>
#include <poll.h>
#include <sys/socket.h>
#include <iconv.h>

#define SQLI_MAX_TUPLE_BYTES (64u * 1024u * 1024u)

struct sqli_batch_result {
    size_t count;
    size_t success_count;
    size_t error_count;
    sqli_batch_item_result *items;
};

static sqli_status receive_done(int fd, sqli_result_t *r, sqli_conn_t *conn);
static sqli_status receive_error(sqli_conn_t *conn, int fd, sqli_result_t *r);
static sqli_status sqli_tuple_locate_column(const sqli_column_info *col_info,
                                            const uint8_t *tuple_buf,
                                            size_t tuple_len, size_t *data_start,
                                            size_t *data_len, size_t *span);

static bool grow_capacity_doubling(size_t current, size_t needed, size_t *out_cap)
{
    if (out_cap == NULL)
        return false;
    if (current == 0)
        current = 1;
    while (current < needed) {
        if (current > (SIZE_MAX / 2))
            return false;
        current *= 2u;
    }
    *out_cap = current;
    return true;
}

static sqli_status socket_read_exact_fallback(int fd, uint8_t *buf, size_t count)
{
    ssize_t total = 0;
    while ((size_t)total < count) {
        ssize_t n = sqli_tcp_read(fd, buf + (size_t)total, count - (size_t)total);
        if (n <= 0)
            return SQLI_IO_ERROR;
        total += n;
    }
    return SQLI_OK;
}

static void set_error_info_from_sqerr(sqli_conn_t *conn, sqli_status status,
                                      uint16_t opcode, int sqlcode, int isamcode,
                                      const char *sqlstate, const char *server_msg)
{
    if (conn == NULL)
        return;

    sqli_error_info *e = &conn->error_info;
    memset(e, 0, sizeof(*e));
    e->has_error = 1;
    e->status = status;
    e->sqlcode = sqlcode;
    e->isamcode = isamcode;
    e->opcode = opcode;
    if (sqlstate != NULL && sqlstate[0] != '\0') {
        snprintf(e->sqlstate, sizeof(e->sqlstate), "%.*s", 5, sqlstate);
    }

    const char *op = sqli_opcode_name(opcode);
    snprintf(e->opcode_name, sizeof(e->opcode_name), "%s", op ? op : "SQ_UNKNOWN");
    if (conn->error_context[0] != '\0')
        snprintf(e->context, sizeof(e->context), "%s", conn->error_context);
    else
        snprintf(e->context, sizeof(e->context), "dispatch");

    if (server_msg != NULL)
        snprintf(e->server_message, sizeof(e->server_message), "%s", server_msg);

    const char *sql_msg = sqli_sql_message_lookup(sqlcode);
    if (sql_msg != NULL)
        snprintf(e->sql_message, sizeof(e->sql_message), "%s", sql_msg);

    const char *isam_msg = sqli_isam_message_lookup(isamcode);
    if (isam_msg != NULL)
        snprintf(e->isam_message, sizeof(e->isam_message), "%s", isam_msg);

    if (e->sqlstate[0] != '\0') {
        snprintf(e->message, sizeof(e->message),
                 "SQLCODE %d (ISAM %d, SQLSTATE %s): %.180s",
                 sqlcode, isamcode, e->sqlstate,
                 e->server_message[0] ? e->server_message :
                 (e->sql_message[0] ? e->sql_message : "server error"));
    } else {
        snprintf(e->message, sizeof(e->message),
                 "SQLCODE %d (ISAM %d): %.200s",
                 sqlcode, isamcode,
                 e->server_message[0] ? e->server_message :
                 (e->sql_message[0] ? e->sql_message : "server error"));
    }
    snprintf(conn->errmsg, sizeof(conn->errmsg), "%s", e->message);
}

/* ----------------------------------------------------------------
 * Helper: read exactly N bytes from socket
 * ---------------------------------------------------------------- */

static sqli_status read_exact(sqli_conn_t *conn, int fd, uint8_t *buf, size_t count)
{
    if (conn == NULL || conn->socket_fd != fd || conn->read_buf == NULL) {
        return socket_read_exact_fallback(fd, buf, count);
    }

    size_t copied = 0;
    while (copied < count) {
        size_t avail = (conn->read_buf_len > conn->read_buf_pos)
                     ? (conn->read_buf_len - conn->read_buf_pos) : 0;
        if (avail == 0) {
            ssize_t n = sqli_tcp_read_some(fd, conn->read_buf, conn->read_buf_cap);
            if (n <= 0)
                return SQLI_IO_ERROR;
            conn->read_buf_pos = 0;
            conn->read_buf_len = (size_t)n;
            avail = conn->read_buf_len;
        }
        size_t need = count - copied;
        size_t take = (avail < need) ? avail : need;
        memcpy(buf + copied, conn->read_buf + conn->read_buf_pos, take);
        conn->read_buf_pos += take;
        copied += take;
    }
    return SQLI_OK;
}

static ssize_t buffered_peek_byte(sqli_conn_t *conn, int fd, uint8_t *out)
{
    if (conn == NULL || out == NULL)
        return -1;
    if (conn->socket_fd != fd || conn->read_buf == NULL)
        return -1;
    if (conn->read_buf_len <= conn->read_buf_pos)
        return 0;
    *out = conn->read_buf[conn->read_buf_pos];
    return 1;
}

static bool buffered_has_data(sqli_conn_t *conn, int fd)
{
    if (conn == NULL)
        return false;
    if (conn->socket_fd != fd || conn->read_buf == NULL)
        return false;
    return conn->read_buf_len > conn->read_buf_pos;
}

static ssize_t sqli_peek_bytes(sqli_conn_t *conn, int fd, uint8_t *buf, size_t count)
{
    if (buf == NULL || count == 0)
        return -1;

    if (conn != NULL && conn->socket_fd == fd && conn->read_buf != NULL) {
        size_t avail = (conn->read_buf_len > conn->read_buf_pos)
                     ? (conn->read_buf_len - conn->read_buf_pos) : 0;
        if (avail >= count) {
            memcpy(buf, conn->read_buf + conn->read_buf_pos, count);
            return (ssize_t)count;
        }
        if (avail > 0) {
            memcpy(buf, conn->read_buf + conn->read_buf_pos, avail);
            return (ssize_t)avail;
        }
    }

    return sqli_tcp_peek(fd, buf, count);
}

/* ----------------------------------------------------------------
 * Helper: read a big-endian 16-bit value from socket
 * ---------------------------------------------------------------- */

static sqli_status read_be16(sqli_conn_t *conn, int fd, uint16_t *val)
{
    uint8_t buf[2];
    sqli_status rc = read_exact(conn, fd, buf, 2);
    if (rc != SQLI_OK)
        return rc;
    *val = (uint16_t)((buf[0] << 8) | buf[1]);
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * Helper: read a big-endian 32-bit value from socket
 * ---------------------------------------------------------------- */

static sqli_status read_be32(sqli_conn_t *conn, int fd, uint32_t *val)
{
    uint8_t buf[4];
    sqli_status rc = read_exact(conn, fd, buf, 4);
    if (rc != SQLI_OK)
        return rc;
    *val = (uint32_t)((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * Helper: read a length-prefixed string from socket (2-byte BE len)
 * ---------------------------------------------------------------- */

static sqli_status read_str(sqli_conn_t *conn, int fd, char *out, size_t out_size)
{
    uint16_t len;
    sqli_status rc = read_be16(conn, fd, &len);
    if (rc != SQLI_OK)
        return rc;
    if (len == 0) {
        if (out_size > 0)
            out[0] = '\0';
        return SQLI_OK;
    }
    if ((size_t)len >= out_size)
        len = (uint16_t)(out_size - 1);
    rc = read_exact(conn, fd, (uint8_t *)out, (size_t)len);
    if (rc != SQLI_OK)
        return rc;
    out[len] = '\0';
    if (len % 2 != 0) {
        uint8_t pad;
        rc = read_exact(conn, fd, &pad, 1);
        if (rc != SQLI_OK)
            return rc;
    }
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * Helper: drain N bytes from socket
 * ---------------------------------------------------------------- */

static sqli_status drain_bytes(sqli_conn_t *conn, int fd, size_t count)
{
    uint8_t tmp[256];
    while (count > 0) {
        size_t chunk = count > sizeof(tmp) ? sizeof(tmp) : count;
        sqli_status rc = read_exact(conn, fd, tmp, chunk);
        if (rc != SQLI_OK)
            return rc;
        count -= chunk;
    }
    return SQLI_OK;
}

static sqli_status append_dynamic_bytes(uint8_t **buf, size_t *len, size_t *cap,
                                        const uint8_t *src, size_t src_len)
{
    if (buf == NULL || len == NULL || cap == NULL)
        return SQLI_INVALID_STATE;
    if (src_len == 0)
        return SQLI_OK;
    if (*len > SIZE_MAX - src_len)
        return SQLI_ALLOC_FAIL;

    size_t need = *len + src_len;
    if (need > *cap) {
        size_t new_cap = (*cap == 0) ? 256u : *cap;
        while (new_cap < need) {
            if (new_cap > SIZE_MAX / 2u)
                return SQLI_ALLOC_FAIL;
            new_cap *= 2u;
        }
        uint8_t *nb = realloc(*buf, new_cap);
        if (nb == NULL)
            return SQLI_ALLOC_FAIL;
        *buf = nb;
        *cap = new_cap;
    }
    memcpy(*buf + *len, src, src_len);
    *len += src_len;
    return SQLI_OK;
}

static sqli_status read_padded_chunk(sqli_conn_t *conn, int fd, uint16_t chunk_len, uint8_t **accum,
                                     size_t *accum_len, size_t *accum_cap)
{
    uint8_t stack_buf[4096];
    uint8_t *tmp = stack_buf;
    uint8_t *heap = NULL;
    size_t n = (size_t)chunk_len;
    sqli_status rc = SQLI_OK;

    if (n > sizeof(stack_buf)) {
        heap = malloc(n);
        if (heap == NULL)
            return SQLI_ALLOC_FAIL;
        tmp = heap;
    }
    if (n > 0) {
        rc = read_exact(conn, fd, tmp, n);
        if (rc != SQLI_OK)
            goto out;
    }
    if ((n & 1u) != 0u) {
        uint8_t pad = 0;
        rc = read_exact(conn, fd, &pad, 1);
        if (rc != SQLI_OK)
            goto out;
    }
    rc = append_dynamic_bytes(accum, accum_len, accum_cap, tmp, n);

out:
    free(heap);
    return rc;
}

static int sqli_is_lob_like_type(uint8_t type)
{
    return type == SQLI_TYPE_TEXT || type == SQLI_TYPE_BYTE ||
           type == SQLI_TYPE_BLOB || type == SQLI_TYPE_CLOB;
}

static sqli_status sqli_fetchblob_materialize(sqli_result_t *result, int col_index,
                                              uint8_t **blob_buf, size_t *blob_len)
{
    if (result == NULL || blob_buf == NULL || blob_len == NULL ||
        col_index < 0 || col_index >= result->column_count)
        return SQLI_INVALID_STATE;
    *blob_buf = NULL;
    *blob_len = 0;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    if (!sqli_is_lob_like_type((uint8_t)col->type))
        return SQLI_INVALID_STATE;
    if (result->owner_conn == NULL || result->owner_conn->state != SQLI_CONN_READY ||
        result->owner_conn->socket_fd < 0 || result->stmt_id < 0)
        return SQLI_INVALID_STATE;

    size_t dstart = 0, dlen = 0, dspan = 0;
    sqli_status rc = sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                              &dstart, &dlen, &dspan);
    if (rc != SQLI_OK || dlen == 0 ||
        dstart > result->tuple_len || dstart + dlen > result->tuple_len)
        return SQLI_INVALID_STATE;

    int fd = result->owner_conn->socket_fd;
    uint8_t id_msg[4] = {
        0, SQLI_SQ_ID,
        (uint8_t)((result->stmt_id >> 8) & 0xFF),
        (uint8_t)(result->stmt_id & 0xFF)
    };
    if (sqli_tcp_send(fd, id_msg, sizeof(id_msg)) != (ssize_t)sizeof(id_msg))
        return SQLI_IO_ERROR;
    uint8_t fetch_msg[2] = {0, SQLI_SQ_FETCHBLOB};
    if (sqli_tcp_send(fd, fetch_msg, sizeof(fetch_msg)) != (ssize_t)sizeof(fetch_msg))
        return SQLI_IO_ERROR;
    if (sqli_tcp_send(fd, result->tuple_buffer + dstart, dlen) != (ssize_t)dlen)
        return SQLI_IO_ERROR;
    if ((dlen & 1u) != 0u) {
        uint8_t pad = 0;
        if (sqli_tcp_send(fd, &pad, 1) != 1)
            return SQLI_IO_ERROR;
    }

    sqli_conn_t *conn = result->owner_conn;

    uint8_t *accum = NULL;
    size_t accum_len = 0;
    size_t accum_cap = 0;

    for (;;) {
        uint16_t opcode = 0;
        rc = read_be16(conn, fd, &opcode);
        if (rc != SQLI_OK)
            break;

        if (opcode == SQLI_SQ_BLOB) {
            uint16_t chunk_len = 0;
            rc = read_be16(conn, fd, &chunk_len);
            if (rc != SQLI_OK)
                break;
            rc = read_padded_chunk(conn, fd, chunk_len, &accum, &accum_len, &accum_cap);
            if (rc != SQLI_OK)
                break;
            continue;
        }
        if (opcode == SQLI_SQ_LODATA) {
            uint16_t op_type = 0;
            uint32_t file_size_u32 = 0;
            rc = read_be16(conn, fd, &op_type);
            if (rc != SQLI_OK) break;
            rc = read_be32(conn, fd, &file_size_u32);
            if (rc != SQLI_OK) break;
            int32_t file_size = (int32_t)file_size_u32;
            if (file_size < 0) {
                rc = SQLI_PROTO_ERROR;
                break;
            }
            size_t remaining = (size_t)file_size;
            while (remaining > 0) {
                uint16_t chunk_len = 0;
                rc = read_be16(conn, fd, &chunk_len);
                if (rc != SQLI_OK) break;
                if ((size_t)chunk_len > remaining) {
                    rc = SQLI_PROTO_ERROR;
                    break;
                }
                rc = read_padded_chunk(conn, fd, chunk_len, &accum, &accum_len, &accum_cap);
                if (rc != SQLI_OK) break;
                remaining -= (size_t)chunk_len;
            }
            if (rc != SQLI_OK)
                break;
            continue;
        }
        if (opcode == SQLI_SQ_DONE) {
            rc = drain_bytes(conn, fd, 22);
            if (rc != SQLI_OK)
                break;
            continue;
        }
        if (opcode == SQLI_SQ_EOT) {
            rc = SQLI_OK;
            break;
        }
        if (opcode == SQLI_SQ_ERR) {
            sqli_result_t tmp_result;
            memset(&tmp_result, 0, sizeof(tmp_result));
            rc = receive_error(result->owner_conn, fd, &tmp_result);
            sqli_result_cleanup(&tmp_result);
            if (rc == SQLI_OK)
                rc = SQLI_PROTO_ERROR;
            break;
        }
        if (opcode == SQLI_SQ_FILE) {
            rc = SQLI_PROTO_ERROR;
            break;
        }
        rc = SQLI_PROTO_ERROR;
        break;
    }
    if (rc != SQLI_OK) {
        free(accum);
        return rc;
    }

    *blob_buf = accum;
    *blob_len = accum_len;
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * SQ_EOT — send end-of-transmission marker
 * ---------------------------------------------------------------- */

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

static int sqli_eot_wait_ms(void)
{
    const char *v = getenv("SQLI_EOT_WAIT_MS");
    if (v == NULL || *v == '\0')
        return 1;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < 0 || n > 1000)
        return 1;
    return (int)n;
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

static void sqli_best_effort_close_release(sqli_conn_t *conn, int stmt_id)
{
    sqli_status rc = sqli_send_stmt_control(conn, stmt_id, 10); /* SQ_CLOSE */
    if (rc != SQLI_OK)
        sqli_log(SQLI_LOG_DEBUG, "close stmt failed rc=%d", rc);
    rc = sqli_send_stmt_control(conn, stmt_id, SQLI_SQ_RELEASE);
    if (rc != SQLI_OK)
        sqli_log(SQLI_LOG_DEBUG, "release stmt failed rc=%d", rc);
}

/* Client-compatible SQ_SFETCH direction codes used by IfxResultSet:
 *   1: forward fetch path (next)
 *   4: last row (server computes tail)
 *   6: absolute/tupid addressing path (first/previous/absolute/relative)
 */
enum {
    SQLI_SFETCH_NEXT = 1,
    SQLI_SFETCH_LAST = 4,
    SQLI_SFETCH_ABSOLUTE = 6
};

/* ----------------------------------------------------------------
 * DESCRIBE (opcode 8) — parse column metadata
 * ---------------------------------------------------------------- */

static sqli_status receive_describe(int fd, sqli_result_t *r, sqli_conn_t *conn)
{
    uint16_t stmt_type, stmt_id, tuple_size, nfields;
    uint32_t cost_val;
    uint32_t string_table_size;
    sqli_status rc;
    int extended_describe = (conn != NULL && conn->caps.extended_describe);

    /* Header (verified against server traffic):
     * statementType(2) + statementID(2) + cost(4) +
     * tupleSize(2) + nfields(2) + stringTableSize(2) = 14 bytes
     * Note: server sends tupleSize and stringTableSize as 2 bytes
     */

    rc = read_be16(conn, fd, &stmt_type);
    if (rc != SQLI_OK) return rc;
    r->statement_type = stmt_type;

    rc = read_be16(conn, fd, &stmt_id);
    if (rc != SQLI_OK) return rc;
    r->stmt_id = (int32_t)stmt_id;

    rc = read_be32(conn, fd, &cost_val);
    if (rc != SQLI_OK) return rc;
    (void)cost_val;

    rc = read_be16(conn, fd, &tuple_size);
    if (rc != SQLI_OK) return rc;
    (void)tuple_size;

    rc = read_be16(conn, fd, &nfields);
    if (rc != SQLI_OK) return rc;
    r->column_count = (int)nfields;

    if (extended_describe) {
        rc = read_be32(conn, fd, &string_table_size);
        if (rc != SQLI_OK) return rc;
    } else {
        uint16_t strtab16;
        rc = read_be16(conn, fd, &strtab16);
        if (rc != SQLI_OK) return rc;
        string_table_size = strtab16;
    }
    sqli_log(SQLI_LOG_DEBUG, "DESCRIBE: stmt_type=%u stmt_id=%u nfields=%u strTabSize=%u",
             stmt_type, stmt_id, nfields, string_table_size);

    if (nfields > 0) {
        r->columns = calloc(nfields, sizeof(sqli_column_info));
        if (r->columns == NULL)
            return SQLI_ALLOC_FAIL;
    }

    /* Per-column (matches Client receiveDescribe()):
     * fieldIndex(4) + startPos(4) + type(2) + extInfo(4) +
     * extOwnerName(readChar) + extName(readChar) +
     * ref(2) + align(2) + sourceType(4) + encLen(4)
     * readChar = 2-byte BE length + data + 1-byte padding if length is odd
     */
    for (uint16_t i = 0; i < nfields; i++) {
        sqli_column_info *col = &r->columns[i];
        uint32_t field_index, start_pos;
        uint16_t type_raw;

        rc = read_be32(conn, fd, &field_index);
        if (rc != SQLI_OK) return rc;
        col->field_index = (int32_t)field_index;

        rc = read_be32(conn, fd, &start_pos);
        if (rc != SQLI_OK) return rc;
        col->col_start_pos = start_pos;

        rc = read_be16(conn, fd, &type_raw);
        if (rc != SQLI_OK) return rc;
        col->flags = type_raw;
        col->type = (sqli_column_type)(type_raw & 0x00FF);

        if (!extended_describe) {
            uint16_t enc_len16;
            rc = read_be16(conn, fd, &enc_len16);
            if (rc != SQLI_OK) return rc;
            col->encoded_length = enc_len16;
            sqli_log(SQLI_LOG_DEBUG,
                     "  col[%u]: fidx=%u spos=%u type=%u encLen=%u name=[%s]",
                     i, field_index, start_pos, type_raw, col->encoded_length, col->name);
            continue;
        }

        {
            uint32_t ext_info, source_type, enc_len;
            uint16_t ref_val, align_val;

            rc = read_be32(conn, fd, &ext_info);
            if (rc != SQLI_OK) return rc;

            /* extOwnerName: readChar() — 2-byte len + data + padding */
            {
                uint16_t str_len;
                rc = read_be16(conn, fd, &str_len);
                if (rc != SQLI_OK) return rc;
                if (str_len > 0) {
                    uint8_t buf[256];
                    size_t to_read = str_len > sizeof(buf) ? sizeof(buf) : str_len;
                    rc = read_exact(conn, fd, buf, to_read);
                    if (rc != SQLI_OK) return rc;
                    size_t copy_len = to_read < sizeof(col->name) - 1 ? to_read : sizeof(col->name) - 1;
                    memcpy(col->name, buf, copy_len);
                    col->name[copy_len] = '\0';
                    if (str_len & 1) {
                        uint8_t pad;
                        read_exact(conn, fd, &pad, 1);
                        (void)pad;
                    }
                }
            }

            /* extName: readChar() — 2-byte len + data + padding */
            {
                uint16_t str_len;
                rc = read_be16(conn, fd, &str_len);
                if (rc != SQLI_OK) return rc;
                if (str_len > 0) {
                    uint8_t buf[256];
                    size_t to_read = str_len > sizeof(buf) ? sizeof(buf) : str_len;
                    rc = read_exact(conn, fd, buf, to_read);
                    if (rc != SQLI_OK) return rc;
                    if (str_len & 1) {
                        uint8_t pad;
                        read_exact(conn, fd, &pad, 1);
                        (void)pad;
                    }
                }
            }

            rc = read_be16(conn, fd, &ref_val);
            if (rc != SQLI_OK) return rc;
            rc = read_be16(conn, fd, &align_val);
            if (rc != SQLI_OK) return rc;

            rc = read_be32(conn, fd, &source_type);
            if (rc != SQLI_OK) return rc;

            rc = read_be32(conn, fd, &enc_len);
            if (rc != SQLI_OK) return rc;
            col->encoded_length = enc_len;

            sqli_log(SQLI_LOG_DEBUG,
                     "  col[%u]: fidx=%u spos=%u type=%u extInfo=%u srcType=%u encLen=%u name=[%s]",
                     i, field_index, start_pos, type_raw, ext_info, source_type, enc_len, col->name);
        }
    }

    /* Drain string table: NUL-delimited column names */
    if (string_table_size > 0) {
        uint8_t *strtab = malloc(string_table_size + (string_table_size & 1));
        if (strtab) {
            rc = read_exact(conn, fd, strtab, string_table_size);
            if (rc != SQLI_OK) { free(strtab); return rc; }
            if (string_table_size & 1) {
                uint8_t pad;
                read_exact(conn, fd, &pad, 1);
            }
            /* Parse NUL-delimited names and assign to columns */
            char *tok = (char *)strtab;
            for (uint16_t i = 0; i < nfields && *tok; i++) {
                strncpy(r->columns[i].name, tok, sizeof(r->columns[i].name) - 1);
                r->columns[i].name[sizeof(r->columns[i].name) - 1] = '\0';
                tok += strlen(tok) + 1;
            }
            free(strtab);
        }
    }

    r->cursor = -1;
    r->current_row = -1;
    r->eof = 0;

    sqli_log(SQLI_LOG_DEBUG, "DESCRIBE: %u columns", nfields);
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * TUPLE (opcode 14) — parse row data
 * ---------------------------------------------------------------- */

static sqli_status receive_tuple(int fd, sqli_result_t *r, sqli_conn_t *conn)
{
    uint16_t warnings;
    uint32_t tuple_size = 0;
    sqli_status rc;

    rc = read_be16(conn, fd, &warnings);
    if (rc != SQLI_OK) return rc;

    /* Client reads TUPLE size as 4-byte int */
    rc = read_be32(conn, fd, &tuple_size);
    if (rc != SQLI_OK) return rc;
    if (tuple_size > SQLI_MAX_TUPLE_BYTES) {
        if (conn != NULL) {
            set_error_context(conn, "dispatch/tuple", SQLI_SQ_TUPLE);
            set_error_fmt(conn, "tuple too large: %u bytes (limit=%u)",
                          tuple_size, (unsigned)SQLI_MAX_TUPLE_BYTES);
        }
        return SQLI_PROTO_ERROR;
    }
    if (r->rows_arena_len > SIZE_MAX - (size_t)tuple_size)
        return SQLI_ALLOC_FAIL;

    if (r->rows_arena == NULL || r->rows_arena_cap < (r->rows_arena_len + (size_t)tuple_size)) {
        size_t needed = r->rows_arena_len + (size_t)tuple_size;
        size_t new_cap = (r->rows_arena_cap > 0) ? r->rows_arena_cap : 32768u;
        if (!grow_capacity_doubling(new_cap, needed, &new_cap))
            return SQLI_ALLOC_FAIL;
        ptrdiff_t first_row_offset = (r->row_count > 0)
            ? (ptrdiff_t)(r->rows[0] - r->rows_arena)
            : 0;
        uint8_t *new_arena = realloc(r->rows_arena, new_cap);
        if (new_arena == NULL)
            return SQLI_ALLOC_FAIL;
        r->rows_arena = new_arena;
        r->rows_arena_cap = new_cap;
        if (r->row_count > 0) {
            ptrdiff_t delta = (ptrdiff_t)(r->rows_arena + (size_t)first_row_offset - r->rows[0]);
            for (int i = 0; i < r->row_count; i++)
                r->rows[i] += delta;
        }
    }

    uint8_t *row_buf = r->rows_arena + r->rows_arena_len;
    rc = read_exact(conn, fd, row_buf, tuple_size);
    if (rc != SQLI_OK)
        return rc;
    r->rows_arena_len += (size_t)tuple_size;
    if (tuple_size % 2 != 0) {
        uint8_t pad;
        rc = read_exact(conn, fd, &pad, 1);
        if (rc != SQLI_OK)
            return rc;
    }

    /* Grow rows array if needed */
    if (r->row_count >= r->row_capacity) {
        int new_cap = r->row_capacity == 0 ? 4 : r->row_capacity * 2;

        uint8_t **new_rows = realloc(r->rows, (size_t)new_cap * sizeof(uint8_t *));
        if (new_rows == NULL)
            return SQLI_ALLOC_FAIL;
        r->rows = new_rows;

        size_t *new_lens = realloc(r->row_lens, (size_t)new_cap * sizeof(size_t));
        if (new_lens == NULL)
            return SQLI_ALLOC_FAIL;
        r->row_lens = new_lens;

        r->row_capacity = new_cap;
    }

    r->rows_use_arena = true;
    r->rows[r->row_count] = row_buf;
    r->row_lens[r->row_count] = tuple_size;
    r->row_count++;
    r->adaptive_tuple_bytes_total += (uint64_t)tuple_size;
    if (r->adaptive_tuple_count < UINT32_MAX)
        r->adaptive_tuple_count++;
    r->warnings = warnings;

    sqli_log(SQLI_LOG_DEBUG, "TUPLE #%d: %u bytes", r->row_count - 1, tuple_size);
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * DONE (opcode 15) — parse completion info
 * ---------------------------------------------------------------- */

static int sqli_is_likely_opcode(uint16_t v);
static int64_t sqli_decode_ifx_int8(const uint8_t *buf10, bool *is_null);

static sqli_status receive_done(int fd, sqli_result_t *r, sqli_conn_t *conn)
{
    uint16_t warnings;
    int64_t rows_affected = 0;
    int64_t row_id = 0;

    sqli_status rc;

    /* Client reads DONE as: warnings + rows + rowid + sqlerrd[1].
     * Some legacy traces include optional errd[2..3]. */
    rc = read_be16(conn, fd, &warnings);
    if (rc != SQLI_OK) return rc;
    r->warnings = warnings;

    /* rows_affected: 8 bytes if long_row_id (USVER9_0350), else 4 */
    if (conn != NULL && conn->caps.long_row_id) {
        uint8_t buf[8];
        rc = read_exact(conn, fd, buf, 8);
        if (rc != SQLI_OK) return rc;
        rows_affected = ((int64_t)buf[0] << 56) | ((int64_t)buf[1] << 48) |
                        ((int64_t)buf[2] << 40) | ((int64_t)buf[3] << 32) |
                        ((int64_t)buf[4] << 24) | ((int64_t)buf[5] << 16) |
                        ((int64_t)buf[6] << 8)  | (int64_t)buf[7];
        rc = read_exact(conn, fd, buf, 8);
        if (rc != SQLI_OK) return rc;
        row_id = ((int64_t)buf[0] << 56) | ((int64_t)buf[1] << 48) |
                 ((int64_t)buf[2] << 40) | ((int64_t)buf[3] << 32) |
                 ((int64_t)buf[4] << 24) | ((int64_t)buf[5] << 16) |
                 ((int64_t)buf[6] << 8)  | (int64_t)buf[7];
    } else {
        uint8_t buf[4];
        rc = read_exact(conn, fd, buf, 4);
        if (rc != SQLI_OK) return rc;
        rows_affected = (int64_t)((int32_t)((uint32_t)buf[0] << 24 |
                                            (uint32_t)buf[1] << 16 |
                                            (uint32_t)buf[2] << 8  |
                                            (uint32_t)buf[3]));
        rc = read_exact(conn, fd, buf, 4);
        if (rc != SQLI_OK) return rc;
        row_id = (int64_t)((uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 |
                           (uint32_t)buf[2] << 8  | (uint32_t)buf[3]);
    }

    r->rows_affected = rows_affected;
    (void)row_id;

    uint32_t errd1;
    rc = read_be32(conn, fd, &errd1);
    if (rc != SQLI_OK) return rc;
    if (errd1 != 0 && rows_affected > 0) {
        r->generated_serial = (int64_t)(int32_t)errd1;
        r->has_generated_serial = true;
    }

    /* Adaptive tail read: only consume errd2/errd3 when they are actually
     * present; otherwise keep stream aligned for the next opcode. */
    uint8_t peek2[2];
    ssize_t pn = sqli_peek_bytes(conn, fd, peek2, sizeof(peek2));
    if (pn == (ssize_t)sizeof(peek2)) {
        uint16_t next = (uint16_t)((peek2[0] << 8) | peek2[1]);
        if (!sqli_is_likely_opcode(next) && next != 0) {
            uint32_t errd2, errd3;
            rc = read_be32(conn, fd, &errd2);
            if (rc != SQLI_OK) return rc;
            rc = read_be32(conn, fd, &errd3);
            if (rc != SQLI_OK) return rc;
            (void)errd2; (void)errd3;
        } else if (next == 0) {
            uint8_t peek8[8];
            ssize_t p8 = sqli_peek_bytes(conn, fd, peek8, sizeof(peek8));
            if (p8 == (ssize_t)sizeof(peek8)) {
                uint16_t after8 = (uint16_t)((peek8[6] << 8) | peek8[7]);
                if (sqli_is_likely_opcode(after8)) {
                    uint32_t errd2, errd3;
                    rc = read_be32(conn, fd, &errd2);
                    if (rc != SQLI_OK) return rc;
                    rc = read_be32(conn, fd, &errd3);
                    if (rc != SQLI_OK) return rc;
                    (void)errd2; (void)errd3;
                }
            }
        }
    }

    r->eof = 1;
    r->saw_done = true;
    sqli_log(SQLI_LOG_DEBUG, "DONE: %ld rows affected", (long)rows_affected);
    return SQLI_OK;
}

static sqli_status receive_insertdone(int fd, sqli_conn_t *conn, sqli_result_t *result)
{
    /* SQ_INSERTDONE payload starts with SERIAL8 in Informix INT8 format (10 bytes).
     * Some server variants append an extra generated value field. */
    uint8_t serial8_raw[10];
    sqli_status rc = read_exact(conn, fd, serial8_raw, sizeof(serial8_raw));
    if (rc != SQLI_OK)
        return rc;

    bool is_null = false;
    int64_t generated = sqli_decode_ifx_int8(serial8_raw, &is_null);
    if (result != NULL && !is_null) {
        result->generated_serial8 = generated;
        result->has_generated_serial8 = true;
    }

    /* Consume optional trailing data in 2-byte steps until the next opcode
     * boundary is visible. Live servers can append more than one short field
     * for BYTE/TEXT insert paths, so keep draining within a bounded window. */
    for (int extra = 0; extra < 512; extra += 2) {
        uint8_t peek[2];
        ssize_t n = sqli_peek_bytes(conn, fd, peek, sizeof(peek));
        if (n != (ssize_t)sizeof(peek))
            break;
        uint16_t next = (uint16_t)((peek[0] << 8) | peek[1]);
        if (next == SQLI_SQ_DONE ||
            next == SQLI_SQ_EOT ||
            next == SQLI_SQ_ERR ||
            next == SQLI_SQ_TUPLE ||
            next == SQLI_SQ_INSERTDONE ||
            next == SQLI_SQ_XACTSTAT ||
            next == SQLI_SQ_EXIT ||
            next == 55 || /* SQ_COST */
            next == SQLI_SQ_INFO) {
            break;
        }
        rc = drain_bytes(conn, fd, 2u);
        if (rc != SQLI_OK)
            return rc;
    }

    return SQLI_OK;
}

static int64_t sqli_decode_ifx_int8(const uint8_t *buf10, bool *is_null)
{
    if (is_null != NULL)
        *is_null = true;
    if (buf10 == NULL)
        return 0;

    int16_t sign = (int16_t)(((uint16_t)buf10[0] << 8) | buf10[1]);
    uint32_t lo32 = ((uint32_t)buf10[2] << 24) | ((uint32_t)buf10[3] << 16) |
                    ((uint32_t)buf10[4] << 8)  | (uint32_t)buf10[5];
    uint32_t hi32 = ((uint32_t)buf10[6] << 24) | ((uint32_t)buf10[7] << 16) |
                    ((uint32_t)buf10[8] << 8)  | (uint32_t)buf10[9];
    uint64_t mag = ((uint64_t)hi32 << 32) | (uint64_t)lo32;

    if (sign == 0)
        return 0;
    if (is_null != NULL)
        *is_null = false;

    if (sign < 0) {
        if (mag >= ((uint64_t)INT64_MAX + 1u))
            return INT64_MIN;
        return -(int64_t)mag;
    }
    return (mag > (uint64_t)INT64_MAX) ? INT64_MAX : (int64_t)mag;
}

static int sqli_is_likely_opcode(uint16_t v)
{
    return v == SQLI_SQ_DBOPEN_FLAGS ||
           v == SQLI_SQ_COMMAND || v == SQLI_SQ_PREPARE || v == SQLI_SQ_ID ||
           v == SQLI_SQ_BIND || v == SQLI_SQ_EXECUTE || v == SQLI_SQ_DESCRIBE ||
           v == SQLI_SQ_NFETCH || v == SQLI_SQ_RELEASE || v == SQLI_SQ_EOT ||
           v == SQLI_SQ_ERR || v == SQLI_SQ_TUPLE || v == SQLI_SQ_DONE ||
           v == SQLI_SQ_BEGIN || v == SQLI_SQ_DBOPEN || v == SQLI_SQ_DBCLOSE ||
           v == SQLI_SQ_CLOSE || v == SQLI_SQ_FETCHBLOB || v == SQLI_SQ_BLOB ||
           v == SQLI_SQ_VERSION ||
           v == SQLI_SQ_EXIT || v == SQLI_SQ_INFO || v == SQLI_SQ_INSERTDONE ||
           v == SQLI_SQ_XACTSTAT || v == SQLI_SQ_RET_TYPE || v == SQLI_SQ_PROTOCOLS ||
           v == SQLI_SQ_LODATA || v == SQLI_SQ_FILE || v == SQLI_SQ_COST;
}

static int sqli_is_cost_follow_opcode(uint16_t v)
{
    /* SQ_COST payload-detection helper:
     * treat only strong message boundaries as "no-payload" markers. */
    return v == SQLI_SQ_EOT ||
           v == SQLI_SQ_DONE ||
           v == SQLI_SQ_ERR ||
           v == SQLI_SQ_TUPLE ||
           v == SQLI_SQ_DESCRIBE ||
           v == SQLI_SQ_INFO ||
           v == SQLI_SQ_INSERTDONE ||
           v == SQLI_SQ_XACTSTAT ||
           v == SQLI_SQ_EXIT;
}

static sqli_status receive_cost(sqli_conn_t *conn, int fd)
{
    uint8_t peek[4];
    ssize_t n = sqli_peek_bytes(conn, fd, peek, sizeof(peek));
    if (n >= 2) {
        uint16_t maybe_next = (uint16_t)((peek[0] << 8) | peek[1]);
        if (sqli_is_cost_follow_opcode(maybe_next)) {
            sqli_log(SQLI_LOG_DEBUG, "SQ_COST: no payload in this server variant");
            return SQLI_OK;
        }
    }
    if (n >= 4) {
        uint16_t maybe_next = (uint16_t)((peek[2] << 8) | peek[3]);
        if (sqli_is_cost_follow_opcode(maybe_next) && peek[0] == 0 && peek[1] == 0) {
            sqli_log(SQLI_LOG_DEBUG, "SQ_COST: no payload (next opcodes detected)");
            return SQLI_OK;
        }
    }

    uint32_t estimated_rows = 0;
    uint32_t cost = 0;
    sqli_status rc = read_be32(conn, fd, &estimated_rows);
    if (rc != SQLI_OK)
        return rc;
    rc = read_be32(conn, fd, &cost);
    if (rc != SQLI_OK)
        return rc;
    sqli_log(SQLI_LOG_DEBUG, "SQ_COST: estimated_rows=%u cost=%u",
             estimated_rows, cost);
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * ERROR (opcode 13) — parse error response
 *
 * Layout depends on sqlcode:
 *   -619: skip 4[+2 if Remove64KLimit] bytes, length-prefixed msg
 *   -368: nothing more (no message)
 *   0/-937/100: 5-byte fixed SQLState, length-prefixed msg (no offset)
 *   else: statementOffset (2 or 4 bytes per Remove64KLimit), msg
 * ---------------------------------------------------------------- */

static sqli_status receive_error(sqli_conn_t *conn, int fd, sqli_result_t *r)
{
    int16_t sqlcode, isamcode;
    sqli_status rc;
    char sqlstate[6] = {0};
    char msg[512] = {0};

    rc = read_be16(conn, fd, (uint16_t *)&sqlcode);
    if (rc != SQLI_OK) return rc;

    rc = read_be16(conn, fd, (uint16_t *)&isamcode);
    if (rc != SQLI_OK) return rc;

    r->error_code = sqlcode;
    int remove64k = (conn != NULL && conn->caps.remove_64k_limit);

    if (sqlcode == -619) {
        drain_bytes(conn, fd, remove64k ? 6 : 4);
        read_str(conn, fd, msg, sizeof(msg));
        sqli_log(SQLI_LOG_ERROR, "ERROR -619: %s", msg);
    } else if (sqlcode == -368) {
        sqli_log(SQLI_LOG_ERROR, "ERROR -368 (no message)");
    } else if (sqlcode == 0 || sqlcode == -937 || sqlcode == 100) {
        read_exact(conn, fd, (uint8_t *)sqlstate, 5);
        sqlstate[5] = '\0';
        read_str(conn, fd, msg, sizeof(msg));
        sqli_log(SQLI_LOG_ERROR, "ERROR %d [%s]: %s (isamcode=%d)",
                 sqlcode, sqlstate, msg, isamcode);
    } else {
        drain_bytes(conn, fd, remove64k ? 4 : 2);
        read_str(conn, fd, msg, sizeof(msg));
        sqli_log(SQLI_LOG_ERROR, "ERROR %d isamcode=%d: %s",
                 sqlcode, isamcode, msg);
    }

    if (conn != NULL)
        set_error_info_from_sqerr(conn, SQLI_PROTO_ERROR, SQLI_SQ_ERR,
                                  (int)sqlcode, (int)isamcode, sqlstate, msg);

    r->eof = 1;
    r->saw_error = true;
    return SQLI_PROTO_ERROR;
}

/* ----------------------------------------------------------------
 * SQ_INFO drain — consume all items from an SQ_INFO response
 * ---------------------------------------------------------------- */

static sqli_status drain_sq_info(sqli_conn_t *conn, int fd)
{
    for (;;) {
        uint16_t item_type;
        sqli_status rc = read_be16(conn, fd, &item_type);
        if (rc != SQLI_OK) return rc;
        if (item_type == 0)
            break;
        uint16_t data_len;
        rc = read_be16(conn, fd, &data_len);
        if (rc != SQLI_OK) return rc;
        size_t to_drain = data_len + (data_len % 2 != 0 ? 1 : 0);
        if (to_drain > 0) {
            rc = drain_bytes(conn, fd, to_drain);
            if (rc != SQLI_OK) return rc;
        }
    }
    return SQLI_OK;
}

static sqli_status receive_command(sqli_conn_t *conn, int fd)
{
    uint8_t peek[2];
    ssize_t n = sqli_tcp_peek(fd, peek, sizeof(peek));
    if (n == (ssize_t)sizeof(peek)) {
        uint16_t next = (uint16_t)((peek[0] << 8) | peek[1]);
        if (sqli_is_likely_opcode(next))
            return SQLI_OK;
    }

    uint32_t payload = 0;
    sqli_status rc = read_be32(conn, fd, &payload);
    if (rc != SQLI_OK)
        return rc;
    sqli_log(SQLI_LOG_DEBUG, "SQ_COMMAND payload=%u", payload);
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * Central dispatch loop — reads opcodes and dispatches to parsers
 * ---------------------------------------------------------------- */

sqli_status sqli_receive_dispatch(int fd, sqli_result_t *result, sqli_conn_t *conn)
{
    sqli_status rc = SQLI_OK;

    if (conn != NULL && result != NULL && result->owner_conn == NULL)
        result->owner_conn = conn;
    while (!result->eof) {
        uint16_t opcode;
        rc = read_be16(conn, fd, &opcode);
        if (rc != SQLI_OK) {
            sqli_log(SQLI_LOG_DEBUG, "dispatch: read_be16 failed rc=%d", rc);
            goto out;
        }
        sqli_log(SQLI_LOG_DEBUG, "dispatch: read opcode=%d", opcode);

        switch (opcode) {
        case SQLI_SQ_DBOPEN_FLAGS: /* 0 */
        {
            uint16_t flags = 0;
            rc = read_be16(conn, fd, &flags);
            if (rc != SQLI_OK)
                goto out;
            sqli_log(SQLI_LOG_DEBUG, "dispatch: SQ_DBOPEN_FLAGS=%u", (unsigned)flags);
        }
            continue;
        case SQLI_SQ_COMMAND: /* 1 */
            sqli_log(SQLI_LOG_DEBUG, "dispatch: SQ_COMMAND");
            rc = receive_command(conn, fd);
            if (rc != SQLI_OK)
                goto out;
            continue;
        case SQLI_SQ_DESCRIBE: /* 8 */
            rc = receive_describe(fd, result, conn);
            break;
        case SQLI_SQ_TUPLE: /* 14 */
            rc = receive_tuple(fd, result, conn);
            break;
        case SQLI_SQ_DONE: /* 15 */
            rc = receive_done(fd, result, conn);
            break;
        case SQLI_SQ_ERR: /* 13 */
            rc = receive_error(conn, fd, result);
            break;
        case SQLI_SQ_CLOSE: /* SQ_CLOSE — acknowledged */
            sqli_log(SQLI_LOG_DEBUG, "dispatch: SQ_CLOSE ack");
            continue;
        case SQLI_SQ_RELEASE: /* 11 */
            sqli_log(SQLI_LOG_DEBUG, "dispatch: SQ_RELEASE");
            continue;
        case SQLI_SQ_EOT: /* 12 — message-group separator, not end-of-results */
        {
            uint8_t b = 0;
            ssize_t rn = buffered_peek_byte(conn, fd, &b);
            if (rn <= 0)
                rn = sqli_tcp_peek(fd, &b, 1);
            if (rn <= 0) {
                int wait_ms = sqli_eot_wait_ms();
                if (wait_ms > 0) {
                    struct pollfd pfd;
                    pfd.fd = fd;
                    pfd.events = POLLIN;
                    pfd.revents = 0;
                    (void)poll(&pfd, 1, wait_ms);
                    rn = buffered_peek_byte(conn, fd, &b);
                    if (rn <= 0)
                        rn = sqli_tcp_peek(fd, &b, 1);
                }
            }
            if (rn <= 0) {
                int can_end_group = 1;
                if (conn != NULL &&
                    ((strncmp(conn->error_context, "query/prepare_recv", 18) == 0) ||
                     (strncmp(conn->error_context, "query_stream/prepare_recv", 25) == 0))) {
                    if (result == NULL ||
                        (result->statement_type == 0 &&
                         result->stmt_id == 0 &&
                         result->column_count == 0)) {
                        can_end_group = 0;
                    }
                }
                if (can_end_group) {
                    sqli_log(SQLI_LOG_DEBUG, "dispatch: EOT boundary/end-of-group");
                    result->eof = 1;
                } else {
                    sqli_log(SQLI_LOG_DEBUG, "dispatch: EOT boundary, awaiting first DESCRIBE/DONE");
                }
            }
        }
            continue;
        case SQLI_SQ_EXIT: /* 56 */
            result->eof = 1;
            rc = SQLI_IO_ERROR;
            break;
        case SQLI_SQ_PREPARE: /* 2 — server open/fetch ack in some flows */
            sqli_log(SQLI_LOG_DEBUG, "dispatch: SQ_PREPARE ack");
            continue;
        case SQLI_SQ_DBOPEN: /* 36 — echo of our DBOPEN, drain + EOT */
            sqli_log(SQLI_LOG_DEBUG, "dispatch: SQ_DBOPEN echo");
        {
            uint16_t name_len;
            rc = read_be16(conn, fd, &name_len);
            if (rc != SQLI_OK) goto out;
            drain_bytes(conn, fd, (size_t)(name_len + 2));
            /* Drain trailing SQ_EOT */
            uint16_t eot_op;
            rc = read_be16(conn, fd, &eot_op);
            if (rc != SQLI_OK) goto out;
            (void)eot_op;
        }
        continue;
        case SQLI_SQ_DBCLOSE: /* 37 — close-database ack/echo */
            sqli_log(SQLI_LOG_DEBUG, "dispatch: SQ_DBCLOSE ack");
            continue;
        case SQLI_SQ_XACTSTAT: /* 99 — server transaction status */
        {
            uint16_t xact_len;
            rc = read_be16(conn, fd, &xact_len);
            if (rc != SQLI_OK) goto out;
            drain_bytes(conn, fd, (size_t)xact_len);
            /* Drain trailing SQ_EOT if present */
            uint16_t eot_op;
            rc = read_be16(conn, fd, &eot_op);
            if (rc != SQLI_OK) goto out;
            if (eot_op == SQLI_SQ_EOT) continue;
            /* Not EOT — push back? No, just continue and let loop re-read */
        }
        continue;
        case SQLI_SQ_INFO: /* 81 — informational items, drain */
            rc = drain_sq_info(conn, fd);
            if (rc != SQLI_OK) goto out;
            continue;
        case SQLI_SQ_INSERTDONE: /* 94 — serial8/bigserial values after INSERT */
            rc = receive_insertdone(fd, conn, result);
            if (rc != SQLI_OK) goto out;
            continue;
        case SQLI_SQ_COST: /* SQ_COST */
            rc = receive_cost(conn, fd);
            if (rc != SQLI_OK) goto out;
            continue;
      default:
        {
            uint8_t peek[8];
            ssize_t n = sqli_tcp_peek(fd, peek, sizeof(peek));
            char hex[3 * sizeof(peek) + 1];
            size_t p = 0;
            for (ssize_t i = 0; i < n && i < (ssize_t)sizeof(peek); i++) {
                int written = snprintf(hex + p, sizeof(hex) - p, "%02x%s",
                                       peek[i], (i + 1 < n) ? " " : "");
                if (written < 0)
                    break;
                p += (size_t)written;
                if (p >= sizeof(hex))
                    break;
            }
            if (p < sizeof(hex))
                hex[p] = '\0';
            else
                hex[sizeof(hex) - 1] = '\0';

            if (conn != NULL) {
                conn->error_info.has_error = 1;
                conn->error_info.status = SQLI_PROTO_ERROR;
                conn->error_info.opcode = opcode;
                snprintf(conn->error_info.opcode_name, sizeof(conn->error_info.opcode_name),
                         "%s", sqli_opcode_name(opcode));
                if (conn->error_context[0] != '\0')
                    snprintf(conn->error_info.context, sizeof(conn->error_info.context),
                             "%s", conn->error_context);
                else
                    snprintf(conn->error_info.context, sizeof(conn->error_info.context), "dispatch");
                conn->error_info.unknown_opcode = opcode;
                conn->error_info.unknown_opcode_count++;
                snprintf(conn->error_info.message, sizeof(conn->error_info.message),
                         "Unknown SQLI opcode %u in %s", opcode, conn->error_info.context);
                snprintf(conn->errmsg, sizeof(conn->errmsg), "%s", conn->error_info.message);
            }

            if (n > 0) {
                sqli_log(SQLI_LOG_WARN,
                         "dispatch: unknown opcode=%u (0x%04x), next bytes: %s",
                         opcode, opcode, hex);
            } else {
                sqli_log(SQLI_LOG_WARN, "dispatch: unknown opcode=%u (0x%04x)",
                         opcode, opcode);
            }
            if (conn != NULL && conn->strict_protocol) {
                result->eof = 1;
                rc = SQLI_PROTO_ERROR;
                goto out;
            }
        }
            continue;
        }
        if (rc != SQLI_OK)
            goto out;
    }
    rc = SQLI_OK;

out:
    return rc;
}

/* ----------------------------------------------------------------
 * Public result API
 * ---------------------------------------------------------------- */

static sqli_status sqli_validate_query_options(sqli_conn_t *conn,
                                               const sqli_query_options *options,
                                               sqli_query_options *resolved)
{
    if (resolved == NULL || conn == NULL)
        return SQLI_INVALID_STATE;

    resolved->cursor_type = conn->cursor_type;
    resolved->holdability = conn->holdability;
    if (resolved->cursor_type != SQLI_CURSOR_FORWARD_ONLY &&
        resolved->cursor_type != SQLI_CURSOR_SCROLL_INSENSITIVE)
        resolved->cursor_type = SQLI_CURSOR_FORWARD_ONLY;
    if (resolved->holdability != SQLI_CURSOR_HOLD_OVER_COMMIT &&
        resolved->holdability != SQLI_CURSOR_CLOSE_AT_COMMIT)
        resolved->holdability = SQLI_CURSOR_CLOSE_AT_COMMIT;
    if (options != NULL)
        *resolved = *options;

    if (resolved->cursor_type != SQLI_CURSOR_FORWARD_ONLY &&
        resolved->cursor_type != SQLI_CURSOR_SCROLL_INSENSITIVE) {
        set_error_context(conn, "query/options", 0);
        set_error(conn, "invalid cursor type");
        return SQLI_INVALID_STATE;
    }
    if (resolved->holdability != SQLI_CURSOR_HOLD_OVER_COMMIT &&
        resolved->holdability != SQLI_CURSOR_CLOSE_AT_COMMIT) {
        set_error_context(conn, "query/options", 0);
        set_error(conn, "invalid cursor holdability");
        return SQLI_INVALID_STATE;
    }
    return SQLI_OK;
}

sqli_status sqli_query_ex(sqli_conn_t *conn, const char *sql,
                          const sqli_query_options *options,
                          sqli_result_t **result)
{
    if (conn == NULL || sql == NULL || result == NULL)
        return SQLI_INVALID_STATE;

    if (conn->state != SQLI_CONN_READY) {
        set_error_context(conn, "query/precheck", 0);
        set_error(conn, "connection not ready");
        return SQLI_INVALID_STATE;
    }

    sqli_query_options resolved_options;
    sqli_status rc = sqli_validate_query_options(conn, options, &resolved_options);
    if (rc != SQLI_OK)
        return rc;

    sqli_result_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        set_error_context(conn, "query/alloc", 0);
        set_error(conn, "memory allocation failed");
        return SQLI_ALLOC_FAIL;
    }
    r->owner_conn = conn;
    r->cursor_type = conn->cursor_type;
    r->holdability = conn->holdability;
    r->cursor_type = resolved_options.cursor_type;
    r->holdability = resolved_options.holdability;

    /* Phase 1: PREPARE — server returns DESCRIBE + DONE */
    rc = sqli_send_prepare(conn, sql);
    if (rc != SQLI_OK) {
        set_error_context(conn, "query/prepare_send", SQLI_SQ_PREPARE);
        set_error(conn, "failed to send prepare");
        free(r);
        return rc;
    }

    set_error_context(conn, "query/prepare_recv", SQLI_SQ_PREPARE);
    r->saw_done = false;
    r->saw_error = false;
    rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
    if (rc != SQLI_OK) {
        if (!conn->error_info.has_error)
            set_error(conn, "error receiving prepare response");
        sqli_result_destroy(r);
        return rc;
    }

    int stmt_id = r->stmt_id;
    /* stmt_id == 0 is valid; only -1 (or negative) means "no statement" */
    if (stmt_id < 0) {
        set_error_context(conn, "query/prepare_recv", SQLI_SQ_PREPARE);
        set_error(conn, "no statement ID from prepare");
        sqli_result_destroy(r);
        return SQLI_PROTO_ERROR;
    }

    /* Reset EOF flag between phases */
    r->eof = 0;

    /* Phase 2: for SELECT mimic Client flow:
     *   OPEN (+EOT) -> read ack/EOT
     *   FETCH (+optional RET_TYPE + EOT) -> read TUPLE/DONE
     * For DML use EXECUTE. */
    if (r->statement_type == 2) {
        rc = sqli_send_open(conn->socket_fd, stmt_id,
                            resolved_options.cursor_type,
                            resolved_options.holdability);
        if (rc != SQLI_OK) {
            set_error_context(conn, "query/open_send", SQLI_SQ_PREPARE);
            set_error(conn, "failed to send open");
            sqli_result_destroy(r);
            return rc;
        }
        set_error_context(conn, "query/open_recv", SQLI_SQ_PREPARE);
        r->saw_done = false;
        r->saw_error = false;
        rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
        if (rc != SQLI_OK) {
            if (!conn->error_info.has_error)
                set_error(conn, "error receiving open response");
            sqli_result_destroy(r);
            return rc;
        }

        r->eof = 0;
        if (resolved_options.cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
            rc = sqli_send_scroll_fetch(conn->socket_fd, stmt_id, r, 1, 1);
        } else {
            rc = sqli_send_fetch(conn->socket_fd, stmt_id, r);
        }
        if (rc != SQLI_OK) {
            set_error_context(conn, "query/fetch_send", SQLI_SQ_NFETCH);
            set_error(conn, "failed to send fetch");
            sqli_result_destroy(r);
            return rc;
        }
    } else {
        rc = sqli_send_execute(conn->socket_fd, stmt_id);
        if (rc != SQLI_OK) {
            set_error_context(conn, "query/execute_send", SQLI_SQ_EXECUTE);
            set_error(conn, "failed to send execute");
            sqli_result_destroy(r);
            return rc;
        }
    }

    if (r->statement_type == 2) {
        /* SELECT cursor fetch loop: keep fetching until a fetch cycle
         * returns no additional rows. */
        for (;;) {
            int prev_rows = r->row_count;
            set_error_context(conn, "query/fetch_recv", SQLI_SQ_NFETCH);
            r->saw_done = false;
            r->saw_error = false;
            rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
            if (rc != SQLI_OK) {
                if (!conn->error_info.has_error)
                    set_error(conn, "error receiving fetch response");
                sqli_result_destroy(r);
                return rc;
            }

            if (r->row_count <= prev_rows)
                break;

            r->eof = 0;
            if (resolved_options.cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
                rc = sqli_send_scroll_fetch(conn->socket_fd, stmt_id, r, 1,
                                            (int32_t)(prev_rows + 1));
            } else {
                rc = sqli_send_fetch(conn->socket_fd, stmt_id, r);
            }
            if (rc != SQLI_OK) {
                set_error_context(conn, "query/fetch_send", SQLI_SQ_NFETCH);
                set_error(conn, "failed to send fetch");
                sqli_result_destroy(r);
                return rc;
            }
        }
        if (resolved_options.cursor_type != SQLI_CURSOR_SCROLL_INSENSITIVE)
            sqli_best_effort_close_release(conn, stmt_id);
        if (resolved_options.cursor_type != SQLI_CURSOR_SCROLL_INSENSITIVE)
            r->stmt_id = -1;
    } else {
        int guard = 0;
        for (;;) {
            set_error_context(conn, "query/execute_recv", SQLI_SQ_EXECUTE);
            r->saw_done = false;
            r->saw_error = false;
            rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
            if (rc != SQLI_OK) {
                if (!conn->error_info.has_error)
                    set_error(conn, "error receiving execute response");
                sqli_result_destroy(r);
                return rc;
            }
            if (r->saw_done || r->saw_error)
                break;

            if (buffered_has_data(conn, conn->socket_fd)) {
                guard++;
                if (guard > 64) {
                    set_error_context(conn, "query/execute_recv", SQLI_SQ_EXECUTE);
                    set_error(conn, "execute response did not converge to DONE");
                    sqli_result_destroy(r);
                    return SQLI_PROTO_ERROR;
                }
                continue;
            }

            uint8_t b = 0;
            ssize_t rn = sqli_tcp_peek(conn->socket_fd, &b, 1);
            if (rn <= 0) {
                struct pollfd pfd;
                pfd.fd = conn->socket_fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                (void)poll(&pfd, 1, 20);
                rn = sqli_tcp_peek(conn->socket_fd, &b, 1);
                if (rn <= 0)
                    break;
            }
            guard++;
            if (guard > 64) {
                set_error_context(conn, "query/execute_recv", SQLI_SQ_EXECUTE);
                set_error(conn, "execute response did not converge to DONE");
                sqli_result_destroy(r);
                return SQLI_PROTO_ERROR;
            }
        }

        /* Some server variants emit additional DONE/COST/EOT groups for
         * LOB statements after the first converged DONE. Drain them so the
         * next command starts on a clean opcode boundary. */
        for (int extra = 0; extra < 8; extra++) {
            if (buffered_has_data(conn, conn->socket_fd)) {
                /* Drain immediately from internal buffer before polling socket. */
            } else {
            struct pollfd pfd;
            pfd.fd = conn->socket_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int prc = poll(&pfd, 1, 20);
            if (prc <= 0 || !(pfd.revents & POLLIN))
                break;
            }

            sqli_result_t tail;
            memset(&tail, 0, sizeof(tail));
            tail.owner_conn = conn;
            tail.eof = 0;
            tail.saw_done = false;
            tail.saw_error = false;
            rc = sqli_receive_dispatch(conn->socket_fd, &tail, conn);
            sqli_result_cleanup(&tail);
            if (rc != SQLI_OK) {
                if (!conn->error_info.has_error)
                    set_error(conn, "error draining execute tail response");
                sqli_result_destroy(r);
                return rc;
            }
        }
    }

    *result = r;
    return SQLI_OK;
}

sqli_status sqli_query(sqli_conn_t *conn, const char *sql, sqli_result_t **result)
{
    return sqli_query_ex(conn, sql, NULL, result);
}

static void sqli_result_clear_rows_only(sqli_result_t *r)
{
    if (r == NULL)
        return;
    if (r->rows != NULL) {
        if (!r->rows_use_arena) {
            for (int i = 0; i < r->row_count; i++)
                free(r->rows[i]);
        }
        free(r->rows);
        r->rows = NULL;
    }
    free(r->rows_arena);
    r->rows_arena = NULL;
    r->rows_arena_len = 0;
    r->rows_arena_cap = 0;
    r->rows_use_arena = false;
    free(r->row_lens);
    r->row_lens = NULL;
    r->row_count = 0;
    r->row_capacity = 0;
    r->cursor = -1;
    r->current_row = -1;
    r->tuple_buffer = NULL;
    r->tuple_len = 0;
    r->cur_cache_row = -1;
}

static sqli_status sqli_stream_deliver_rows(sqli_conn_t *conn, sqli_result_t *r,
                                            sqli_row_callback on_row, void *ctx,
                                            int64_t *row_counter)
{
    if (r == NULL)
        return SQLI_INVALID_STATE;
    if (on_row == NULL)
        return SQLI_OK;

    while (sqli_result_next(r)) {
        if (on_row(r, ctx) != 0) {
            set_error_context(conn, "query/stream_callback", 0);
            set_error(conn, "row callback aborted streaming");
            return SQLI_ERR;
        }
        if (row_counter != NULL)
            (*row_counter)++;
    }
    return SQLI_OK;
}

sqli_status sqli_query_stream(sqli_conn_t *conn, const char *sql,
                              sqli_row_callback on_row, void *ctx,
                              int64_t *out_rows)
{
    if (conn == NULL || sql == NULL)
        return SQLI_INVALID_STATE;
    if (out_rows != NULL)
        *out_rows = 0;

    if (conn->state != SQLI_CONN_READY) {
        set_error_context(conn, "query_stream/precheck", 0);
        set_error(conn, "connection not ready");
        return SQLI_INVALID_STATE;
    }

    sqli_result_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        set_error_context(conn, "query_stream/alloc", 0);
        set_error(conn, "memory allocation failed");
        return SQLI_ALLOC_FAIL;
    }
    r->owner_conn = conn;

    sqli_status rc = sqli_send_prepare(conn, sql);
    if (rc != SQLI_OK) {
        set_error_context(conn, "query_stream/prepare_send", SQLI_SQ_PREPARE);
        set_error(conn, "failed to send prepare");
        sqli_result_destroy(r);
        return rc;
    }

    set_error_context(conn, "query_stream/prepare_recv", SQLI_SQ_PREPARE);
    rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
    if (rc != SQLI_OK) {
        if (!conn->error_info.has_error)
            set_error(conn, "error receiving prepare response");
        sqli_result_destroy(r);
        return rc;
    }

    int stmt_id = r->stmt_id;
    if (stmt_id < 0) {
        set_error_context(conn, "query_stream/prepare_recv", SQLI_SQ_PREPARE);
        set_error(conn, "no statement ID from prepare");
        sqli_result_destroy(r);
        return SQLI_PROTO_ERROR;
    }

    r->eof = 0;
    int64_t delivered = 0;

    if (r->statement_type == 2) {
        rc = sqli_send_open(conn->socket_fd, stmt_id,
                            conn->cursor_type, conn->holdability);
        if (rc != SQLI_OK) {
            set_error_context(conn, "query_stream/open_send", SQLI_SQ_PREPARE);
            set_error(conn, "failed to send open");
            sqli_result_destroy(r);
            return rc;
        }
        set_error_context(conn, "query_stream/open_recv", SQLI_SQ_PREPARE);
        rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
        if (rc != SQLI_OK) {
            if (!conn->error_info.has_error)
                set_error(conn, "error receiving open response");
            sqli_result_destroy(r);
            return rc;
        }
        r->eof = 0;
        if (conn->cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
            rc = sqli_send_scroll_fetch(conn->socket_fd, stmt_id, r, 1, 1);
        } else {
            rc = sqli_send_fetch(conn->socket_fd, stmt_id, r);
        }
        if (rc != SQLI_OK) {
            set_error_context(conn, "query_stream/fetch_send", SQLI_SQ_NFETCH);
            set_error(conn, "failed to send fetch");
            sqli_result_destroy(r);
            return rc;
        }

        for (;;) {
            set_error_context(conn, "query_stream/fetch_recv", SQLI_SQ_NFETCH);
            rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
            if (rc != SQLI_OK) {
                if (!conn->error_info.has_error)
                    set_error(conn, "error receiving fetch response");
                sqli_result_destroy(r);
                return rc;
            }

            int got = r->row_count;
            rc = sqli_stream_deliver_rows(conn, r, on_row, ctx, &delivered);
            if (rc != SQLI_OK) {
                sqli_result_destroy(r);
                return rc;
            }

            sqli_result_clear_rows_only(r);
            if (got <= 0)
                break;

            r->eof = 0;
            if (conn->cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
                rc = sqli_send_scroll_fetch(conn->socket_fd, stmt_id, r, 1,
                                            (int32_t)(got + 1));
            } else {
                rc = sqli_send_fetch(conn->socket_fd, stmt_id, r);
            }
            if (rc != SQLI_OK) {
                set_error_context(conn, "query_stream/fetch_send", SQLI_SQ_NFETCH);
                set_error(conn, "failed to send fetch");
                sqli_result_destroy(r);
                return rc;
            }
        }
        sqli_best_effort_close_release(conn, stmt_id);
        r->stmt_id = -1;
    } else {
        rc = sqli_send_execute(conn->socket_fd, stmt_id);
        if (rc != SQLI_OK) {
            set_error_context(conn, "query_stream/execute_send", SQLI_SQ_EXECUTE);
            set_error(conn, "failed to send execute");
            sqli_result_destroy(r);
            return rc;
        }
        set_error_context(conn, "query_stream/execute_recv", SQLI_SQ_EXECUTE);
        rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
        if (rc != SQLI_OK) {
            if (!conn->error_info.has_error)
                set_error(conn, "error receiving execute response");
            sqli_result_destroy(r);
            return rc;
        }
        rc = sqli_stream_deliver_rows(conn, r, on_row, ctx, &delivered);
        if (rc != SQLI_OK) {
            sqli_result_destroy(r);
            return rc;
        }
    }

    if (out_rows != NULL)
        *out_rows = delivered;
    sqli_result_destroy(r);
    return SQLI_OK;
}

static void sqli_sleep_ms(uint32_t delay_ms)
{
    if (delay_ms == 0)
        return;
    struct timespec req;
    req.tv_sec = (time_t)(delay_ms / 1000u);
    req.tv_nsec = (long)((delay_ms % 1000u) * 1000000u);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}

static int sqli_sql_is_read_only(const char *sql)
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

sqli_status sqli_query_with_retry(sqli_conn_t *conn, const char *sql,
                                  uint32_t max_retries, sqli_result_t **result)
{
    if (conn == NULL || sql == NULL || result == NULL)
        return SQLI_INVALID_STATE;

    *result = NULL;
    const int read_only = sqli_sql_is_read_only(sql);
    sqli_status rc = SQLI_ERR;
    for (uint32_t attempt = 0;; attempt++) {
        rc = sqli_query(conn, sql, result);
        if (rc == SQLI_OK)
            return SQLI_OK;
        if (attempt >= max_retries)
            return rc;
        if (!read_only) {
            sqli_log(SQLI_LOG_WARN,
                     "query retry skipped for non-read-only statement");
            return rc;
        }

        bool should_retry = false;
        uint32_t delay_ms = 0;
        if (sqli_retry_recommend(conn, attempt, &should_retry, &delay_ms) != SQLI_OK ||
            !should_retry) {
            return rc;
        }

        sqli_log(SQLI_LOG_WARN,
                 "query retry attempt=%u delay_ms=%u rc=%d msg=%s",
                 attempt + 1u, delay_ms, (int)rc,
                 sqli_error(conn) ? sqli_error(conn) : "-");
        sqli_sleep_ms(delay_ms);
    }
}

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

static size_t sqli_fixed_width_for_type(uint8_t type)
{
    switch (type) {
    case SQLI_TYPE_BOOL:     return 1;
    case SQLI_TYPE_DBOOLEAN: return 1;
    case SQLI_TYPE_SMALLINT: return 2;
    case SQLI_TYPE_INT:
    case SQLI_TYPE_DATE:
    case SQLI_TYPE_SERIAL:
    case SQLI_TYPE_BYTE:
    case SQLI_TYPE_SMFLOAT:  return 4;
    case SQLI_TYPE_FLOAT:
    case SQLI_TYPE_BIGINT:
    case SQLI_TYPE_BIGSERIAL:return 8;
    case SQLI_TYPE_INT8:
    case SQLI_TYPE_SERIAL8:  return 10;
    default:                 return 0;
    }
}

static uint8_t sqli_decimal_precision_from_encoded(uint32_t encoded_length)
{
    uint8_t p = (uint8_t)((encoded_length >> 8) & 0xFFu);
    if (p == 0 || p > 32)
        return 0;
    return p;
}

static size_t sqli_decimal_packed_width_from_encoded(uint32_t encoded_length)
{
    uint8_t precision = sqli_decimal_precision_from_encoded(encoded_length);
    if (precision == 0)
        return 0;
    /* Observed SQLI fetch format for DECIMAL/NUMERIC:
     * [exponent/sign byte][BCD digits][padding]
     * width = ceil((precision + 3) / 2). */
    return (size_t)((precision + 3u) / 2u);
}

static size_t sqli_temporal_packed_width_from_encoded(uint8_t type, uint32_t encoded_length)
{
    if (type != SQLI_TYPE_DATETIME && type != SQLI_TYPE_INTERVAL)
        return 0;
    uint8_t qlen = (uint8_t)((encoded_length >> 8) & 0xFFu);
    if (qlen == 0)
        return 0;
    return 1u + (size_t)((qlen + 1u) / 2u);
}

static void sqli_base100_complement(uint8_t *digits, size_t ndgts)
{
    uint8_t digit = 100;
    for (size_t i = ndgts; i > 0; i--) {
        size_t p = i - 1;
        if (digits[p] != 0 || digit != 100) {
            digits[p] = (uint8_t)(digit - digits[p]);
            digit = 99;
        }
    }
}

static int sqli_type_is_two_byte_prefixed(uint8_t type)
{
    switch (type) {
    case SQLI_TYPE_TEXT:
    case SQLI_TYPE_LVARCHAR:
    case SQLI_TYPE_BLOB:
    case SQLI_TYPE_CLOB:
    case SQLI_TYPE_DECIMAL:
    case SQLI_TYPE_MONEY:
        return 1;
    default:
        return 0;
    }
}

static int sqli_type_all_zero_is_null_sentinel(uint8_t type)
{
    return type == SQLI_TYPE_DECIMAL || type == SQLI_TYPE_MONEY ||
           type == SQLI_TYPE_DATETIME || type == SQLI_TYPE_INTERVAL;
}

static sqli_status sqli_tuple_locate_column(const sqli_column_info *col_info,
                                            const uint8_t *tuple_buf,
                                            size_t tuple_len, size_t *data_start,
                                            size_t *data_len, size_t *span)
{
    if (col_info == NULL || tuple_buf == NULL || data_start == NULL ||
        data_len == NULL || span == NULL)
        return SQLI_INVALID_STATE;

    size_t base = (size_t)col_info->col_start_pos;
    uint8_t type = (uint8_t)col_info->type;
    size_t width = sqli_fixed_width_for_type(type);

    if (base >= tuple_len)
        return SQLI_PROTO_ERROR;

    /* CHAR/NCHAR can arrive in either fixed-width or 1-byte-prefixed layout. */
    if (type == SQLI_TYPE_CHAR || type == SQLI_TYPE_NCHAR) {
        if (width == 0 && col_info->encoded_length > 0)
            width = (size_t)col_info->encoded_length;

        /* Heuristic: live Informix rows often encode CHAR as len8+payload.
         * Accept prefixed layout only for short values to avoid colliding
         * with regular fixed-width text where first byte is printable ASCII. */
        if (base + 1 <= tuple_len) {
            uint8_t len8 = tuple_buf[base];
            size_t payload = (size_t)len8;
            size_t start = base + 1;
            if (len8 > 0 && len8 < 32 && start + payload <= tuple_len) {
                *data_start = start;
                *data_len = payload;
                *span = 1 + payload;
                return SQLI_OK;
            }
        }
    }

    if (type == SQLI_TYPE_DECIMAL || type == SQLI_TYPE_MONEY) {
        size_t packed = sqli_decimal_packed_width_from_encoded(col_info->encoded_length);
        if (packed > 0 && base + packed <= tuple_len) {
            *data_start = base;
            *data_len = packed;
            *span = packed;
            return SQLI_OK;
        }
    }

    if (type == SQLI_TYPE_DATETIME || type == SQLI_TYPE_INTERVAL) {
        size_t packed = sqli_temporal_packed_width_from_encoded(type, col_info->encoded_length);
        if (packed > 0 && base + packed <= tuple_len) {
            *data_start = base;
            *data_len = packed;
            *span = packed;
            return SQLI_OK;
        }
    }

    if (type == SQLI_TYPE_BOOL) {
        /* Informix BOOLEAN (type 41) can be encoded as:
         *   [4-byte zero prefix][1-byte len][payload] */
        if (base + 5 <= tuple_len) {
            uint32_t marker = ((uint32_t)tuple_buf[base] << 24) |
                              ((uint32_t)tuple_buf[base + 1] << 16) |
                              ((uint32_t)tuple_buf[base + 2] << 8) |
                              (uint32_t)tuple_buf[base + 3];
            uint8_t len8 = tuple_buf[base + 4];
            size_t start = base + 5;
            size_t payload = (size_t)len8;
            if (marker == 0 && start + payload <= tuple_len) {
                *data_start = start;
                *data_len = payload;
                *span = 5 + payload;
                return SQLI_OK;
            }
        }
    }

    if (type == SQLI_TYPE_LVARCHAR) {
        /* Observed live LVARCHAR layout:
         *   [4-byte marker][1-byte len][payload]
         * Keep a len16 fallback for server variants. */
        if (base + 5 <= tuple_len) {
            uint32_t marker = ((uint32_t)tuple_buf[base] << 24) |
                              ((uint32_t)tuple_buf[base + 1] << 16) |
                              ((uint32_t)tuple_buf[base + 2] << 8) |
                              (uint32_t)tuple_buf[base + 3];
            uint8_t len8 = tuple_buf[base + 4];
            size_t start = base + 5;
            size_t payload = (size_t)len8;
            if (marker == 0 && start + payload <= tuple_len) {
                *data_start = start;
                *data_len = payload;
                *span = 5 + payload;
                return SQLI_OK;
            }
        }
        if (base + 2 <= tuple_len) {
            uint16_t len16 = (uint16_t)((tuple_buf[base] << 8) | tuple_buf[base + 1]);
            size_t payload = (size_t)len16;
            size_t start = base + 2;
            if (start + payload <= tuple_len) {
                *data_start = start;
                *data_len = payload;
                *span = 2 + payload;
                return SQLI_OK;
            }
        }
        return SQLI_PROTO_ERROR;
    }

    if (width > 0) {
        if (base + width > tuple_len)
            return SQLI_PROTO_ERROR;
        *data_start = base;
        *data_len = width;
        *span = width;
        return SQLI_OK;
    }

    if (type == SQLI_TYPE_VARCHAR || type == SQLI_TYPE_NVCHAR ||
        type == SQLI_TYPE_CHAR || type == SQLI_TYPE_NCHAR ||
        !sqli_type_is_two_byte_prefixed(type)) {
        uint8_t len8 = tuple_buf[base];
        size_t payload = (size_t)len8;
        size_t start = base + 1;
        if (start + payload > tuple_len)
            return SQLI_PROTO_ERROR;
        *data_start = start;
        *data_len = payload;
        *span = 1 + payload;
        return SQLI_OK;
    }

    if (base + 2 > tuple_len)
        return SQLI_PROTO_ERROR;
    uint16_t len16 = (uint16_t)((tuple_buf[base] << 8) | tuple_buf[base + 1]);
    size_t payload = (size_t)len16;
    size_t start = base + 2;
    if (start + payload > tuple_len)
        return SQLI_PROTO_ERROR;
    *data_start = start;
    *data_len = payload;
    *span = 2 + payload;
    return SQLI_OK;
}

static int sqli_is_stringy_type(uint8_t type)
{
    return type == SQLI_TYPE_CHAR || type == SQLI_TYPE_NCHAR ||
           type == SQLI_TYPE_VARCHAR || type == SQLI_TYPE_NVCHAR ||
           type == SQLI_TYPE_LVARCHAR;
}

static void sqli_result_prepare_row_cache(sqli_result_t *result)
{
    if (result == NULL || result->column_count <= 0 || result->tuple_buffer == NULL) {
        if (result != NULL)
            result->cur_cache_row = -1;
        return;
    }

    if (result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL) {
        result->cur_col_data_start = calloc((size_t)result->column_count, sizeof(size_t));
        result->cur_col_data_len = calloc((size_t)result->column_count, sizeof(size_t));
        result->cur_col_is_null = calloc((size_t)result->column_count, sizeof(uint8_t));
        if (result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
            result->cur_col_is_null == NULL) {
            free(result->cur_col_data_start);
            free(result->cur_col_data_len);
            free(result->cur_col_is_null);
            result->cur_col_data_start = NULL;
            result->cur_col_data_len = NULL;
            result->cur_col_is_null = NULL;
            result->cur_cache_row = -1;
            return;
        }
    }

    size_t offset = 0;
    for (int i = 0; i < result->column_count; i++) {
        const sqli_column_info *col = &result->columns[i];
        uint8_t type = (uint8_t)col->type;
        size_t data_start = 0, data_len = 0, span = 0;
        result->columns[i].col_start_pos = (uint32_t)offset;
        sqli_status rc = SQLI_OK;

        if (offset >= result->tuple_len) {
            rc = SQLI_PROTO_ERROR;
        } else if (type == SQLI_TYPE_CHAR || type == SQLI_TYPE_NCHAR || type == SQLI_TYPE_BOOL) {
            rc = sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                          &data_start, &data_len, &span);
        } else {
            size_t width = sqli_fixed_width_for_type(type);
            if (width > 0) {
                if (offset + width <= result->tuple_len) {
                    data_start = offset;
                    data_len = width;
                    span = width;
                } else {
                    rc = SQLI_PROTO_ERROR;
                }
            } else if (type == SQLI_TYPE_DECIMAL || type == SQLI_TYPE_MONEY) {
                size_t packed = sqli_decimal_packed_width_from_encoded(col->encoded_length);
                if (packed > 0 && offset + packed <= result->tuple_len) {
                    data_start = offset;
                    data_len = packed;
                    span = packed;
                } else {
                    rc = sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                                  &data_start, &data_len, &span);
                }
            } else if (type == SQLI_TYPE_DATETIME || type == SQLI_TYPE_INTERVAL) {
                size_t packed = sqli_temporal_packed_width_from_encoded(type, col->encoded_length);
                if (packed > 0 && offset + packed <= result->tuple_len) {
                    data_start = offset;
                    data_len = packed;
                    span = packed;
                } else {
                    rc = sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                                  &data_start, &data_len, &span);
                }
            } else if (type == SQLI_TYPE_LVARCHAR) {
                rc = sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                              &data_start, &data_len, &span);
            } else if (type == SQLI_TYPE_VARCHAR || type == SQLI_TYPE_NVCHAR) {
                size_t payload = (size_t)result->tuple_buffer[offset];
                size_t start = offset + 1;
                if (start + payload <= result->tuple_len) {
                    data_start = start;
                    data_len = payload;
                    span = 1 + payload;
                } else {
                    rc = SQLI_PROTO_ERROR;
                }
            } else if (!sqli_type_is_two_byte_prefixed(type)) {
                size_t payload = (size_t)result->tuple_buffer[offset];
                size_t start = offset + 1;
                if (start + payload <= result->tuple_len) {
                    data_start = start;
                    data_len = payload;
                    span = 1 + payload;
                } else {
                    rc = SQLI_PROTO_ERROR;
                }
            } else {
                if (offset + 2 <= result->tuple_len) {
                    uint16_t len16 = (uint16_t)((result->tuple_buffer[offset] << 8) |
                                                result->tuple_buffer[offset + 1]);
                    size_t payload = (size_t)len16;
                    size_t start = offset + 2;
                    if (start + payload <= result->tuple_len) {
                        data_start = start;
                        data_len = payload;
                        span = 2 + payload;
                    } else {
                        rc = SQLI_PROTO_ERROR;
                    }
                } else {
                    rc = SQLI_PROTO_ERROR;
                }
            }
        }
        if (rc != SQLI_OK || span == 0 || data_start > result->tuple_len ||
            data_start + data_len > result->tuple_len) {
            result->cur_col_data_start[i] = result->tuple_len;
            result->cur_col_data_len[i] = 0;
            result->cur_col_is_null[i] = 1;
            offset = result->tuple_len;
            continue;
        }
        result->cur_col_data_start[i] = data_start;
        result->cur_col_data_len[i] = data_len;
        offset += span;

        int is_null = 0;
        if (data_len == 0) {
            if (!sqli_is_stringy_type(type) &&
                (sqli_type_is_two_byte_prefixed(type) ||
                 sqli_type_all_zero_is_null_sentinel(type))) {
                is_null = 1;
            }
        } else {
            const uint8_t *p = result->tuple_buffer + data_start;
            if (type == SQLI_TYPE_DATE && data_len >= 4) {
                int32_t days = (int32_t)(((uint32_t)p[0] << 24) |
                                         ((uint32_t)p[1] << 16) |
                                         ((uint32_t)p[2] << 8)  |
                                         (uint32_t)p[3]);
                if (days == INT32_MIN)
                    is_null = 1;
            } else if ((type == SQLI_TYPE_INT8 || type == SQLI_TYPE_SERIAL8) &&
                       data_len >= 2 && p[0] == 0 && p[1] == 0) {
                is_null = 1;
            } else if (sqli_type_all_zero_is_null_sentinel(type)) {
                int all_zero = 1;
                for (size_t k = 0; k < data_len; k++) {
                    if (p[k] != 0) {
                        all_zero = 0;
                        break;
                    }
                }
                if (all_zero)
                    is_null = 1;
            }
        }
        result->cur_col_is_null[i] = (uint8_t)is_null;
    }
    result->cur_cache_row = result->current_row;
}

static int sqli_result_is_null_internal(sqli_result_t *result, int col_index)
{
    if (result == NULL || col_index < 0 || col_index >= result->column_count)
        return 1;
    if (result->current_row < 0 || result->tuple_buffer == NULL || result->tuple_len == 0)
        return 1;

    if (result->cur_cache_row != result->current_row)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row == result->current_row && result->cur_col_is_null != NULL)
        return result->cur_col_is_null[col_index] ? 1 : 0;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    size_t data_start = 0, data_len = 0, span = 0;
    if (sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                 &data_start, &data_len, &span) != SQLI_OK)
        return 1;

    uint8_t type = (uint8_t)col->type;
    if (data_len == 0) {
        if (sqli_is_stringy_type(type))
            return 0;
        if (sqli_type_is_two_byte_prefixed(type) ||
            sqli_type_all_zero_is_null_sentinel(type))
            return 1;
        return 0;
    }

    const uint8_t *p = result->tuple_buffer + data_start;
    if (type == SQLI_TYPE_DATE && data_len >= 4) {
        int32_t days = (int32_t)(((uint32_t)p[0] << 24) |
                                 ((uint32_t)p[1] << 16) |
                                 ((uint32_t)p[2] << 8)  |
                                 (uint32_t)p[3]);
        if (days == INT32_MIN)
            return 1;
    }
    if ((type == SQLI_TYPE_INT8 || type == SQLI_TYPE_SERIAL8) &&
        data_len >= 2 && p[0] == 0 && p[1] == 0)
        return 1;

    int all_zero = 1;
    for (size_t i = 0; i < data_len; i++) {
        if (p[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    if (all_zero && sqli_type_all_zero_is_null_sentinel(type))
        return 1;
    return 0;
}

bool sqli_result_next(sqli_result_t *result)
{
    if (result == NULL)
        return false;
    int next = result->cursor + 1;
    if (next >= result->row_count)
        return false;
    result->cursor = next;
    result->tuple_buffer = result->rows[next];
    result->tuple_len = result->row_lens[next];
    result->current_row = next;
    result->cur_cache_row = -1;
    sqli_result_prepare_row_cache(result);
    return true;
}

static bool sqli_result_move_to_index(sqli_result_t *result, int row_index)
{
    if (result == NULL)
        return false;
    if (row_index < 0 || row_index >= result->row_count)
        return false;

    result->cursor = row_index;
    result->tuple_buffer = result->rows[row_index];
    result->tuple_len = result->row_lens[row_index];
    result->current_row = row_index;
    result->cur_cache_row = -1;
    sqli_result_prepare_row_cache(result);
    return true;
}

static bool sqli_result_is_server_scrollable(const sqli_result_t *result)
{
    if (result == NULL || result->owner_conn == NULL)
        return false;
    if (result->cursor_type != SQLI_CURSOR_SCROLL_INSENSITIVE)
        return false;
    if (result->statement_type != 2 || result->stmt_id < 0)
        return false;
    if (result->owner_conn->state != SQLI_CONN_READY || result->owner_conn->socket_fd < 0)
        return false;
    return true;
}

static bool sqli_result_server_refetch(sqli_result_t *result, uint16_t scroll_type, int32_t index)
{
    if (!sqli_result_is_server_scrollable(result))
        return false;

    sqli_result_clear_rows_only(result);
    result->eof = 0;
    sqli_status rc = sqli_send_scroll_fetch(result->owner_conn->socket_fd, result->stmt_id,
                                            result, scroll_type, index);
    if (rc != SQLI_OK)
        return false;

    rc = sqli_receive_dispatch(result->owner_conn->socket_fd, result, result->owner_conn);
    if (rc != SQLI_OK || result->row_count <= 0)
        return false;

    return sqli_result_move_to_index(result, 0);
}

bool sqli_result_previous(sqli_result_t *result)
{
    if (result == NULL)
        return false;
    return sqli_result_move_to_index(result, result->cursor - 1);
}

bool sqli_result_first(sqli_result_t *result)
{
    if (sqli_result_server_refetch(result, SQLI_SFETCH_ABSOLUTE, 1))
        return true;
    return sqli_result_move_to_index(result, 0);
}

bool sqli_result_last(sqli_result_t *result)
{
    if (sqli_result_server_refetch(result, SQLI_SFETCH_LAST, 0))
        return true;
    if (result == NULL || result->row_count <= 0)
        return false;
    return sqli_result_move_to_index(result, result->row_count - 1);
}

bool sqli_result_absolute(sqli_result_t *result, int row_1based)
{
    if (result == NULL)
        return false;
    if (row_1based <= 0)
        return false;
    if (sqli_result_server_refetch(result, SQLI_SFETCH_ABSOLUTE, row_1based))
        return true;
    return sqli_result_move_to_index(result, row_1based - 1);
}

bool sqli_result_relative(sqli_result_t *result, int offset)
{
    if (result == NULL)
        return false;
    if (result->cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
        int row = sqli_result_row_number(result);
        if (row > 0) {
            int target = row + offset;
            if (target > 0 && sqli_result_server_refetch(result, SQLI_SFETCH_ABSOLUTE, target))
                return true;
        }
    }
    return sqli_result_move_to_index(result, result->cursor + offset);
}

int sqli_result_row_number(sqli_result_t *result)
{
    if (result == NULL || result->cursor < 0)
        return 0;
    return result->cursor + 1;
}

int64_t sqli_result_rows_affected(sqli_result_t *result)
{
    if (result == NULL)
        return 0;
    return result->rows_affected;
}

bool sqli_result_has_generated_serial(sqli_result_t *result)
{
    if (result == NULL)
        return false;
    return result->has_generated_serial;
}

int64_t sqli_result_generated_serial(sqli_result_t *result)
{
    if (result == NULL || !result->has_generated_serial)
        return 0;
    return result->generated_serial;
}

bool sqli_result_has_generated_serial8(sqli_result_t *result)
{
    if (result == NULL)
        return false;
    return result->has_generated_serial8;
}

int64_t sqli_result_generated_serial8(sqli_result_t *result)
{
    if (result == NULL || !result->has_generated_serial8)
        return 0;
    return result->generated_serial8;
}

int sqli_result_columns(sqli_result_t *result)
{
    if (result == NULL)
        return 0;
    return result->column_count;
}

void sqli_result_destroy(sqli_result_t *result)
{
    if (result == NULL)
        return;
    if (result->owner_conn != NULL &&
        result->owner_conn->state == SQLI_CONN_READY &&
        result->statement_type == 2 && result->stmt_id >= 0) {
        sqli_best_effort_close_release(result->owner_conn, result->stmt_id);
    }
    sqli_result_cleanup(result);
    free(result);
}

/* ----------------------------------------------------------------
 * Column value extraction from tuple buffer
 *
 * Fixed-width types: SMALLINT=2, INT/SERIAL/DATE/BYTE=4, INT8=10,
 *   FLOAT=8, SMFLOAT=4, BIGINT/BIGSERIAL=8, BOOL=1
 * Variable-width (1-byte prefix): VARCHAR, NVCHAR
 * Variable-width (2-byte prefix): CHAR, TEXT, BLOB, CLOB, INTERVAL,
 *   NCHAR, DECIMAL, MONEY, DATETIME
 * ---------------------------------------------------------------- */

sqli_status extract_value_from_tuple(const sqli_column_info *col_info,
                                     const uint8_t *tuple_buf, size_t tuple_len,
                                     uint8_t *out, size_t *out_len)
{
    if (col_info == NULL || tuple_buf == NULL || out == NULL || out_len == NULL)
        return SQLI_INVALID_STATE;

    size_t data_start = 0;
    size_t data_len = 0;
    size_t span = 0;
    sqli_status rc = sqli_tuple_locate_column(col_info, tuple_buf, tuple_len,
                                              &data_start, &data_len, &span);
    if (rc != SQLI_OK)
        return rc;

    size_t copy_len = *out_len < data_len ? *out_len : data_len;
    memcpy(out, tuple_buf + data_start, copy_len);
    *out_len = copy_len;
    return SQLI_OK;
}

static sqli_status sqli_extract_current_value(sqli_result_t *result, int col_index,
                                              uint8_t *out, size_t *out_len)
{
    if (result == NULL || out == NULL || out_len == NULL ||
        col_index < 0 || col_index >= result->column_count ||
        result->tuple_buffer == NULL)
        return SQLI_INVALID_STATE;

    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL)
        return SQLI_PROTO_ERROR;

    size_t start = result->cur_col_data_start[col_index];
    size_t len = result->cur_col_data_len[col_index];
    if (start > result->tuple_len || start + len > result->tuple_len)
        return SQLI_PROTO_ERROR;

    size_t copy_len = *out_len < len ? *out_len : len;
    memcpy(out, result->tuple_buffer + start, copy_len);
    *out_len = copy_len;
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * Typed extractors
 * ---------------------------------------------------------------- */

int32_t sqli_result_get_int(sqli_result_t *result, int col_index)
{
    if (result == NULL || col_index < 0 || col_index >= result->column_count)
        return 0;
    if (result->current_row < 0 || result->tuple_len == 0)
        return 0;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null)
        return 0;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    uint8_t type = (uint8_t)col->type;
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        return 0;

    size_t start = result->cur_col_data_start[col_index];
    size_t len = result->cur_col_data_len[col_index];
    if (len == 0 || start > result->tuple_len || start + len > result->tuple_len)
        return 0;
    const uint8_t *buf = result->tuple_buffer + start;

    if (type == SQLI_TYPE_BOOL || type == SQLI_TYPE_DBOOLEAN) {
        uint8_t b = buf[0];
        switch (b) {
        case 0:
        case '0':
        case 'f':
        case 'F':
        case 'n':
        case 'N':
            return 0;
        default:
            return 1;
        }
    }

    if (len >= 4) {
        uint32_t u = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                     ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
        return (int32_t)u;
    }

    uint32_t u = 0;
    for (size_t i = 0; i < len; i++)
        u = (u << 8) | (uint32_t)buf[i];
    if ((buf[0] & 0x80) != 0)
        u |= (~(uint32_t)0) << (len * 8);
    return (int32_t)u;
}

int64_t sqli_result_get_int64(sqli_result_t *result, int col_index)
{
    if (result == NULL || col_index < 0 || col_index >= result->column_count)
        return 0;
    if (result->current_row < 0 || result->tuple_len == 0)
        return 0;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null)
        return 0;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    uint8_t type = (uint8_t)col->type;
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        return 0;

    size_t start = result->cur_col_data_start[col_index];
    size_t len = result->cur_col_data_len[col_index];
    if (len == 0 || start > result->tuple_len || start + len > result->tuple_len)
        return 0;
    const uint8_t *buf = result->tuple_buffer + start;

    if (type == SQLI_TYPE_INT8 || type == SQLI_TYPE_SERIAL8) {
        if (len < 10)
            return 0;
        bool is_null = false;
        int64_t v = sqli_decode_ifx_int8(buf, &is_null);
        if (is_null)
            return INT64_MIN;
        return v;
    }

    if (len >= 8)
        return (int64_t)((uint64_t)buf[0] << 56 | (uint64_t)buf[1] << 48 |
                         (uint64_t)buf[2] << 40 | (uint64_t)buf[3] << 32 |
                         (uint64_t)buf[4] << 24 | (uint64_t)buf[5] << 16 |
                         (uint64_t)buf[6] << 8  | (uint64_t)buf[7]);

    if (len == 0)
        return 0;
    uint64_t u = 0;
    for (size_t i = 0; i < len; i++)
        u = (u << 8) | (uint64_t)buf[i];
    if (len < 8 && (buf[0] & 0x80) != 0)
        u |= (~(uint64_t)0) << (len * 8);
    return (int64_t)u;
}

double sqli_result_get_double(sqli_result_t *result, int col_index)
{
    if (result == NULL || col_index < 0 || col_index >= result->column_count)
        return 0.0;
    if (result->current_row < 0 || result->tuple_len == 0)
        return 0.0;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null)
        return 0.0;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    uint8_t type = (uint8_t)col->type;

    if (type == SQLI_TYPE_DECIMAL || type == SQLI_TYPE_MONEY) {
        uint8_t raw[64];
        size_t len = sizeof(raw);
        sqli_status rc = sqli_extract_current_value(result, col_index, raw, &len);
        if (rc != SQLI_OK || len < 2)
            return 0.0;

        int expon = (int8_t)raw[0];
        uint8_t digits[63];
        size_t ndgts = len - 1;
        memcpy(digits, raw + 1, ndgts);

        int negative = 0;
        if ((expon & 0x80) == 0) {
            sqli_base100_complement(digits, ndgts);
            expon ^= 0x7F;
            negative = 1;
        }
        expon = (expon & 0x7F) - 64;

        long double v = 0.0L;
        for (size_t i = 0; i < ndgts; i++) {
            if (digits[i] > 99)
                return 0.0;
            v = (v * 100.0L) + (long double)digits[i];
        }

        int frac_digits = (int)(ndgts * 2) - (expon * 2);
        while (frac_digits-- > 0)
            v /= 10.0L;
        if (negative)
            v = -v;
        return (double)v;
    }

    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL)
        return 0.0;

    size_t start = result->cur_col_data_start[col_index];
    size_t len = result->cur_col_data_len[col_index];
    if (len < 8 || start > result->tuple_len || start + len > result->tuple_len)
        return 0.0;
    const uint8_t *buf = result->tuple_buffer + start;

    /* Wire format is big-endian IEEE 754 */
    uint64_t bits = ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48) |
                    ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32) |
                    ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16) |
                    ((uint64_t)buf[6] << 8)  | (uint64_t)buf[7];
    double val;
    memcpy(&val, &bits, 8);
    return val;
}

const char *sqli_result_get_string(sqli_result_t *result, int col_index)
{
    if (result == NULL || col_index < 0 || col_index >= result->column_count)
        return "";
    if (result->current_row < 0 || result->tuple_len == 0)
        return "";

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL)
        return "";

    result->last_was_null = result->cur_col_is_null[col_index] ? true : false;
    if (result->last_was_null)
        return "";

    size_t data_start = result->cur_col_data_start[col_index];
    size_t data_len = result->cur_col_data_len[col_index];
    if (data_len == 0 || data_start > result->tuple_len || data_start + data_len > result->tuple_len)
        return "";

    static _Thread_local char str_buf[4096];
    static _Thread_local char utf8_buf[12288];
    size_t copy = data_len < sizeof(str_buf) - 1 ? data_len : sizeof(str_buf) - 1;
    memcpy(str_buf, result->tuple_buffer + data_start, copy);

    sqli_conn_t *conn = result->owner_conn;
    bool trim_trailing_spaces = (conn == NULL) ? true : conn->trim_trailing_spaces;
    if (trim_trailing_spaces && sqli_is_stringy_type((uint8_t)col->type)) {
        while (copy > 0 && str_buf[copy - 1] == ' ')
            copy--;
    }
    str_buf[copy] = '\0';

    if (conn != NULL && conn->decode_cd_ready && conn->decode_cd != (iconv_t)-1) {
        (void)iconv(conn->decode_cd, NULL, NULL, NULL, NULL);
        char *in_ptr = str_buf;
        size_t in_left = copy;
        char *out_ptr = utf8_buf;
        size_t out_left = sizeof(utf8_buf) - 1;
        size_t irc = iconv(conn->decode_cd, &in_ptr, &in_left, &out_ptr, &out_left);
        if (irc != (size_t)-1) {
            *out_ptr = '\0';
            return utf8_buf;
        }
    }
    if (conn != NULL && !conn->decode_locale_checked) {
        conn->decode_locale_checked = true;
        if (conn->client_locale != NULL && conn->db_locale != NULL &&
            ((strcasestr(conn->client_locale, "utf8") != NULL) ||
             (strcasestr(conn->client_locale, "utf-8") != NULL)) &&
            ((strcasestr(conn->db_locale, "cp1252") != NULL) ||
             (strcasestr(conn->db_locale, "1252") != NULL))) {
            conn->decode_cp1252_utf8 = true;
        }
    }

    if (conn != NULL && conn->decode_cp1252_utf8) {
        if (!conn->decode_cd_ready) {
            conn->decode_cd = iconv_open("UTF-8", "WINDOWS-1252");
            if (conn->decode_cd != (iconv_t)-1)
                conn->decode_cd_ready = true;
            else
                conn->decode_cp1252_utf8 = false;
        }
        if (conn->decode_cd_ready && conn->decode_cd != (iconv_t)-1) {
            (void)iconv(conn->decode_cd, NULL, NULL, NULL, NULL);
            char *in_ptr = str_buf;
            size_t in_left = copy;
            char *out_ptr = utf8_buf;
            size_t out_left = sizeof(utf8_buf) - 1;
            size_t irc = iconv(conn->decode_cd, &in_ptr, &in_left, &out_ptr, &out_left);
            if (irc != (size_t)-1) {
                *out_ptr = '\0';
                return utf8_buf;
            }
        }
    }
    if (conn != NULL && conn->client_locale != NULL && conn->db_locale != NULL &&
        ((strcasestr(conn->client_locale, "utf8") != NULL) ||
         (strcasestr(conn->client_locale, "utf-8") != NULL)) &&
        ((strcasestr(conn->db_locale, "cp1252") != NULL) ||
         (strcasestr(conn->db_locale, "1252") != NULL))) {
        iconv_t cd = iconv_open("UTF-8", "WINDOWS-1252");
        if (cd != (iconv_t)-1) {
            char *in_ptr = str_buf;
            size_t in_left = copy;
            char *out_ptr = utf8_buf;
            size_t out_left = sizeof(utf8_buf) - 1;
            size_t irc = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
            iconv_close(cd);
            if (irc != (size_t)-1) {
                *out_ptr = '\0';
                return utf8_buf;
            }
        }
    }
    return str_buf;
}

sqli_status sqli_result_get_string_len(sqli_result_t *result, int col_index,
                                       char *out, size_t *out_len)
{
    if (result == NULL || out == NULL || out_len == NULL || *out_len == 0)
        return SQLI_INVALID_STATE;
    if (col_index < 0 || col_index >= result->column_count)
        return SQLI_INVALID_STATE;
    if (result->current_row < 0 || result->tuple_len == 0)
        return SQLI_INVALID_STATE;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL)
        sqli_result_prepare_row_cache(result);
    if (result->cur_cache_row != result->current_row ||
        result->cur_col_data_start == NULL || result->cur_col_data_len == NULL ||
        result->cur_col_is_null == NULL)
        return SQLI_INVALID_STATE;

    result->last_was_null = result->cur_col_is_null[col_index] ? true : false;
    if (result->last_was_null) {
        out[0] = '\0';
        *out_len = 0;
        return SQLI_OK;
    }

    size_t data_start = result->cur_col_data_start[col_index];
    size_t data_len = result->cur_col_data_len[col_index];
    if (data_len == 0 || data_start > result->tuple_len || data_start + data_len > result->tuple_len) {
        out[0] = '\0';
        *out_len = 0;
        return SQLI_OK;
    }

    size_t avail = *out_len - 1;
    size_t copy = data_len < avail ? data_len : avail;
    memcpy(out, result->tuple_buffer + data_start, copy);
    out[copy] = '\0';

    sqli_conn_t *conn = result->owner_conn;
    bool trim_trailing_spaces = (conn == NULL) ? true : conn->trim_trailing_spaces;
    if (trim_trailing_spaces && sqli_is_stringy_type((uint8_t)col->type)) {
        while (copy > 0 && out[copy - 1] == ' ')
            copy--;
        out[copy] = '\0';
    }
    *out_len = copy;
    return SQLI_OK;
}

sqli_status sqli_result_get_bytes(sqli_result_t *result, int col_index,
                                  uint8_t *out, size_t *out_len)
{
    if (result == NULL || out == NULL || out_len == NULL)
        return SQLI_INVALID_STATE;

    const sqli_column_info *col = NULL;

    if (col_index >= 0 && col_index < result->column_count)
        col = &result->columns[(size_t)col_index];

    if (col != NULL) {
        result->last_was_null = sqli_result_is_null_internal(result, col_index);
        if (result->last_was_null) {
            *out_len = 0;
            return SQLI_OK;
        }
        if (sqli_is_lob_like_type((uint8_t)col->type)) {
            uint8_t *lob = NULL;
            size_t lob_len = 0;
            sqli_status frc = sqli_fetchblob_materialize(result, col_index, &lob, &lob_len);
            if (frc == SQLI_OK && lob != NULL) {
                size_t copy_len = *out_len < lob_len ? *out_len : lob_len;
                memcpy(out, lob, copy_len);
                *out_len = copy_len;
                free(lob);
                return SQLI_OK;
            }
            free(lob);
        }
        return sqli_extract_current_value(result, col_index, out, out_len);
    }

    size_t available = result->tuple_len;
    size_t copy_len = *out_len < available ? *out_len : available;
    memcpy(out, result->tuple_buffer, copy_len);
    *out_len = copy_len;
    return SQLI_OK;
}

bool sqli_result_is_null(sqli_result_t *result, int col_index)
{
    bool is_null = sqli_result_is_null_internal(result, col_index) != 0;
    if (result != NULL)
        result->last_was_null = is_null;
    return is_null;
}

bool sqli_result_was_null(sqli_result_t *result)
{
    if (result == NULL)
        return true;
    return result->last_was_null;
}

bool sqli_result_get_bool(sqli_result_t *result, int col_index)
{
    int32_t v = sqli_result_get_int(result, col_index);
    return v != 0;
}

static int sqli_base100_decode_parts(const uint8_t *raw, size_t len,
                                     uint8_t *digits, size_t *ndgts,
                                     int *frac_digits, int *negative)
{
    if (raw == NULL || len < 2 || digits == NULL || ndgts == NULL ||
        frac_digits == NULL || negative == NULL)
        return 0;
    int expon = (int8_t)raw[0];
    *ndgts = len - 1;
    memcpy(digits, raw + 1, *ndgts);
    *negative = 0;
    if ((expon & 0x80) == 0) {
        sqli_base100_complement(digits, *ndgts);
        expon ^= 0x7F;
        *negative = 1;
    }
    expon = (expon & 0x7F) - 64;
    *frac_digits = (int)(*ndgts * 2) - (expon * 2);
    return 1;
}

static void sqli_days_to_ymd_ifx(int32_t ifx_days, int *y, int *m, int *d)
{
    int64_t z = (int64_t)ifx_days - 25568; /* IFX wire epoch (1899-12-31) -> Unix epoch days */
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp + (mp < 10 ? 3 : -9);
    yy += (mm <= 2);
    if (y) *y = (int)yy;
    if (m) *m = (int)mm;
    if (d) *d = (int)dd;
}

static int sqli_qual_length(uint32_t encoded_length)
{
    return (int)((encoded_length >> 8) & 0xFF);
}

static int sqli_qual_start(uint32_t encoded_length)
{
    return (int)((encoded_length >> 4) & 0x0F);
}

static int sqli_qual_end(uint32_t encoded_length)
{
    return (int)(encoded_length & 0x0F);
}

static int sqli_parse_digits_to_int(const char *digits, size_t n)
{
    int v = 0;
    for (size_t i = 0; i < n; i++) {
        if (digits[i] < '0' || digits[i] > '9')
            return 0;
        v = v * 10 + (digits[i] - '0');
    }
    return v;
}

static int sqli_extract_temporal_digits(const uint8_t *raw, size_t len,
                                        char *digits, size_t digits_cap,
                                        int *negative)
{
    uint8_t b100[63];
    size_t ndgts = 0;
    int frac_digits = 0;
    if (!sqli_base100_decode_parts(raw, len, b100, &ndgts, &frac_digits, negative))
        return 0;
    (void)frac_digits;

    size_t p = 0;
    for (size_t i = 0; i < ndgts; i++) {
        if (b100[i] > 99 || p + 2 >= digits_cap)
            return 0;
        digits[p++] = (char)('0' + (b100[i] / 10));
        digits[p++] = (char)('0' + (b100[i] % 10));
    }
    digits[p] = '\0';
    return (int)p;
}

static sqli_status sqli_get_temporal_payload(sqli_result_t *result, int col_index,
                                             uint8_t *raw, size_t *raw_len,
                                             const sqli_column_info **col_out)
{
    if (result == NULL || raw == NULL || raw_len == NULL ||
        col_out == NULL || col_index < 0 || col_index >= result->column_count)
        return SQLI_INVALID_STATE;
    if (result->current_row < 0 || result->tuple_len == 0)
        return SQLI_INVALID_STATE;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null) {
        *raw_len = 0;
        *col_out = &result->columns[(size_t)col_index];
        return SQLI_OK;
    }

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    *col_out = col;
    return sqli_extract_current_value(result, col_index, raw, raw_len);
}

const char *sqli_result_get_decimal_string(sqli_result_t *result, int col_index)
{
    static _Thread_local char out[256];
    out[0] = '\0';

    if (result == NULL || col_index < 0 || col_index >= result->column_count)
        return out;
    if (result->current_row < 0 || result->tuple_len == 0)
        return out;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null)
        return out;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    uint8_t t = (uint8_t)col->type;
    if (t != SQLI_TYPE_DECIMAL && t != SQLI_TYPE_MONEY)
        return sqli_result_get_string(result, col_index);

    uint8_t raw[64];
    size_t len = sizeof(raw);
    if (sqli_extract_current_value(result, col_index, raw, &len) != SQLI_OK)
        return out;

    uint8_t b100[63];
    size_t ndgts = 0;
    int frac_digits = 0;
    int negative = 0;
    if (!sqli_base100_decode_parts(raw, len, b100, &ndgts, &frac_digits, &negative))
        return out;
    int scale = (int)(col->encoded_length & 0xFF);
    if (scale < 0 || scale > 30)
        scale = 6;

    char digits[192];
    size_t digits_len = 0;
    int is_zero = 1;
    for (size_t i = 0; i < ndgts; i++) {
        if (b100[i] > 99 || digits_len + 2 >= sizeof(digits))
            return out;
        uint8_t hi = (uint8_t)(b100[i] / 10);
        uint8_t lo = (uint8_t)(b100[i] % 10);
        digits[digits_len++] = (char)('0' + hi);
        digits[digits_len++] = (char)('0' + lo);
        if (hi != 0 || lo != 0)
            is_zero = 0;
    }
    if (digits_len == 0) {
        digits[0] = '0';
        digits_len = 1;
        is_zero = 1;
    }

    if (frac_digits < 0)
        frac_digits = 0;
    ptrdiff_t dec_pos = (ptrdiff_t)digits_len - (ptrdiff_t)frac_digits;

    size_t pos = 0;
    if (negative && !is_zero && pos + 1 < sizeof(out))
        out[pos++] = '-';

    ptrdiff_t int_start = 0;
    ptrdiff_t int_end = dec_pos;
    if (int_end < 0)
        int_end = 0;
    while (int_start + 1 < int_end && digits[int_start] == '0')
        int_start++;
    if (int_end - int_start <= 0) {
        if (pos + 1 < sizeof(out))
            out[pos++] = '0';
    } else {
        for (ptrdiff_t i = int_start; i < int_end && pos + 1 < sizeof(out); i++)
            out[pos++] = digits[i];
    }

    if (scale > 0 && pos + 1 < sizeof(out))
        out[pos++] = '.';

    for (int j = 0; j < scale && pos + 1 < sizeof(out); j++) {
        ptrdiff_t src = dec_pos + (ptrdiff_t)j;
        if (src >= 0 && (size_t)src < digits_len)
            out[pos++] = digits[src];
        else
            out[pos++] = '0';
    }
    out[pos] = '\0';
    return out;
}

const char *sqli_result_get_date_string(sqli_result_t *result, int col_index)
{
    static _Thread_local char out[64];
    out[0] = '\0';
    sqli_date_value dv;
    if (sqli_result_get_date(result, col_index, &dv) != SQLI_OK || dv.is_null)
        return out;
    if (dv.year > 0 && dv.month > 0 && dv.day > 0) {
        snprintf(out, sizeof(out), "%04d-%02d-%02d", dv.year, dv.month, dv.day);
    } else {
        snprintf(out, sizeof(out), "%d", dv.days_since_ifx_epoch);
    }
    return out;
}

const char *sqli_result_get_datetime_string(sqli_result_t *result, int col_index)
{
    static _Thread_local char out[128];
    out[0] = '\0';

    sqli_datetime_value dt;
    if (sqli_result_get_datetime(result, col_index, &dt) != SQLI_OK || dt.is_null)
        return out;

    if (dt.year >= 0) {
        int n = snprintf(out, sizeof(out), "%04d", dt.year);
        if (dt.month >= 0 && n > 0 && (size_t)n < sizeof(out))
            n += snprintf(out + n, sizeof(out) - (size_t)n, "-%02d", dt.month);
        if (dt.day >= 0 && n > 0 && (size_t)n < sizeof(out))
            n += snprintf(out + n, sizeof(out) - (size_t)n, "-%02d", dt.day);
        if (dt.hour >= 0 && n > 0 && (size_t)n < sizeof(out))
            n += snprintf(out + n, sizeof(out) - (size_t)n, " %02d", dt.hour);
        if (dt.minute >= 0 && n > 0 && (size_t)n < sizeof(out))
            n += snprintf(out + n, sizeof(out) - (size_t)n, ":%02d", dt.minute);
        if (dt.second >= 0 && n > 0 && (size_t)n < sizeof(out))
            n += snprintf(out + n, sizeof(out) - (size_t)n, ":%02d", dt.second);
        if (dt.fraction_scale > 0 && n > 0 && (size_t)n < sizeof(out))
            (void)snprintf(out + n, sizeof(out) - (size_t)n, ".%0*u",
                           dt.fraction_scale, dt.fraction);
    }
    return out;
}

const char *sqli_result_get_interval_string(sqli_result_t *result, int col_index)
{
    static _Thread_local char out[128];
    out[0] = '\0';

    sqli_interval_value iv;
    if (sqli_result_get_interval(result, col_index, &iv) != SQLI_OK || iv.is_null)
        return out;

    /* Canonical style: [-]Y-M D H:M:S[.f] with omitted leading sections */
    int n = 0;
    if (iv.negative)
        n += snprintf(out + n, sizeof(out) - (size_t)n, "-");
    if (iv.year || iv.month) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, "%d-%02d", iv.year, iv.month);
    }
    if (iv.day || iv.hour || iv.minute || iv.second || iv.fraction_scale > 0) {
        if (n > 0 && (size_t)n < sizeof(out))
            n += snprintf(out + n, sizeof(out) - (size_t)n, " ");
        n += snprintf(out + n, sizeof(out) - (size_t)n, "%d %02d:%02d:%02d",
                      iv.day, iv.hour, iv.minute, iv.second);
        if (iv.fraction_scale > 0 && (size_t)n < sizeof(out)) {
            (void)snprintf(out + n, sizeof(out) - (size_t)n, ".%0*u",
                           iv.fraction_scale, iv.fraction);
        }
    } else if (n == 0) {
        snprintf(out, sizeof(out), "0");
    }
    return out;
}

sqli_status sqli_result_get_date(sqli_result_t *result, int col_index,
                                 sqli_date_value *out)
{
    if (result == NULL || out == NULL)
        return SQLI_INVALID_STATE;
    memset(out, 0, sizeof(*out));
    out->year = -1;
    out->month = -1;
    out->day = -1;

    if (sqli_result_is_null_internal(result, col_index)) {
        out->is_null = 1;
        result->last_was_null = 1;
        return SQLI_OK;
    }

    int32_t days = sqli_result_get_int(result, col_index);
    out->days_since_ifx_epoch = days;
    sqli_days_to_ymd_ifx(days, &out->year, &out->month, &out->day);
    out->is_null = 0;
    return SQLI_OK;
}

sqli_status sqli_result_get_datetime(sqli_result_t *result, int col_index,
                                     sqli_datetime_value *out)
{
    if (result == NULL || out == NULL)
        return SQLI_INVALID_STATE;
    memset(out, 0, sizeof(*out));
    out->year = out->month = out->day = -1;
    out->hour = out->minute = out->second = -1;
    out->fraction = 0;
    out->fraction_scale = 0;

    uint8_t raw[128];
    size_t raw_len = sizeof(raw);
    const sqli_column_info *col = NULL;
    sqli_status rc = sqli_get_temporal_payload(result, col_index, raw, &raw_len, &col);
    if (rc != SQLI_OK)
        return rc;
    if (raw_len == 0) {
        out->is_null = 1;
        return SQLI_OK;
    }

    int qlen = sqli_qual_length(col->encoded_length);
    int qstart = sqli_qual_start(col->encoded_length);
    int qend = sqli_qual_end(col->encoded_length);
    int first_width = qlen - (qend - qstart);
    if (first_width <= 0)
        first_width = (qstart == 0) ? 4 : 2;
    out->start_qualifier = (uint8_t)qstart;
    out->end_qualifier = (uint8_t)qend;
    out->first_field_width = (uint8_t)first_width;

    char digits[160];
    int negative = 0;
    int dlen = sqli_extract_temporal_digits(raw, raw_len, digits, sizeof(digits), &negative);
    if (dlen <= 0 || negative)
        return SQLI_PROTO_ERROR;

    int pos = 0;
    int fields_end = qend > 10 ? 10 : qend;
    for (int code = qstart; code <= fields_end; code += 2) {
        int w = (code == qstart) ? first_width : 2;
        if (pos + w > dlen)
            break;
        int val = sqli_parse_digits_to_int(digits + pos, (size_t)w);
        switch (code) {
        case 0: out->year = val; break;
        case 2: out->month = val; break;
        case 4: out->day = val; break;
        case 6: out->hour = val; break;
        case 8: out->minute = val; break;
        case 10: out->second = val; break;
        default: break;
        }
        pos += w;
    }

    if (qend > 10) {
        int frac_w = qend - 10;
        if (frac_w > 0 && pos + frac_w <= dlen) {
            out->fraction = sqli_parse_digits_to_int(digits + pos, (size_t)frac_w);
            out->fraction_scale = frac_w;
        }
    }
    out->is_null = 0;
    return SQLI_OK;
}

sqli_status sqli_result_get_interval(sqli_result_t *result, int col_index,
                                     sqli_interval_value *out)
{
    if (result == NULL || out == NULL)
        return SQLI_INVALID_STATE;
    memset(out, 0, sizeof(*out));
    out->year = out->month = out->day = 0;
    out->hour = out->minute = out->second = 0;

    uint8_t raw[128];
    size_t raw_len = sizeof(raw);
    const sqli_column_info *col = NULL;
    sqli_status rc = sqli_get_temporal_payload(result, col_index, raw, &raw_len, &col);
    if (rc != SQLI_OK)
        return rc;
    if (raw_len == 0) {
        out->is_null = 1;
        return SQLI_OK;
    }

    int qlen = sqli_qual_length(col->encoded_length);
    int qstart = sqli_qual_start(col->encoded_length);
    int qend = sqli_qual_end(col->encoded_length);
    int first_width = qlen - (qend - qstart);
    if (first_width <= 0)
        first_width = 2;
    out->start_qualifier = (uint8_t)qstart;
    out->end_qualifier = (uint8_t)qend;
    out->first_field_width = (uint8_t)first_width;

    char digits[160];
    int negative = 0;
    int dlen = sqli_extract_temporal_digits(raw, raw_len, digits, sizeof(digits), &negative);
    if (dlen <= 0)
        return SQLI_PROTO_ERROR;
    out->negative = negative ? 1 : 0;

    int pos = 0;
    int fields_end = qend > 10 ? 10 : qend;
    for (int code = qstart; code <= fields_end; code += 2) {
        int w = (code == qstart) ? first_width : 2;
        if (pos + w > dlen)
            break;
        int val = sqli_parse_digits_to_int(digits + pos, (size_t)w);
        switch (code) {
        case 0: out->year = val; break;
        case 2: out->month = val; break;
        case 4: out->day = val; break;
        case 6: out->hour = val; break;
        case 8: out->minute = val; break;
        case 10: out->second = val; break;
        default: break;
        }
        pos += w;
    }

    if (qend > 10) {
        int frac_w = qend - 10;
        if (frac_w > 0 && pos + frac_w <= dlen) {
            out->fraction = sqli_parse_digits_to_int(digits + pos, (size_t)frac_w);
            out->fraction_scale = frac_w;
        }
    }
    out->is_null = 0;
    return SQLI_OK;
}

sqli_status sqli_result_stream_bytes(sqli_result_t *result, int col_index,
                                     size_t chunk_size, sqli_stream_chunk_cb cb,
                                     void *ctx)
{
    if (result == NULL || cb == NULL || chunk_size == 0)
        return SQLI_INVALID_STATE;
    if (col_index < 0 || col_index >= result->column_count)
        return SQLI_INVALID_STATE;

    result->last_was_null = sqli_result_is_null_internal(result, col_index);
    if (result->last_was_null)
        return SQLI_OK;

    const sqli_column_info *col = &result->columns[(size_t)col_index];
    if (sqli_is_lob_like_type((uint8_t)col->type)) {
        uint8_t *lob = NULL;
        size_t lob_len = 0;
        sqli_status frc = sqli_fetchblob_materialize(result, col_index, &lob, &lob_len);
        if (frc == SQLI_OK && lob != NULL) {
            size_t off = 0;
            while (off < lob_len) {
                size_t n = lob_len - off;
                if (n > chunk_size)
                    n = chunk_size;
                if (cb(lob + off, n, ctx) != 0) {
                    free(lob);
                    return SQLI_ERR;
                }
                off += n;
            }
            free(lob);
            return SQLI_OK;
        }
        free(lob);
    }

    size_t data_start = 0, data_len = 0, span = 0;
    sqli_status rc = sqli_tuple_locate_column(col, result->tuple_buffer, result->tuple_len,
                                              &data_start, &data_len, &span);
    if (rc != SQLI_OK)
        return rc;

    const uint8_t *buf = result->tuple_buffer + data_start;
    size_t off = 0;
    while (off < data_len) {
        size_t n = data_len - off;
        if (n > chunk_size)
            n = chunk_size;
        if (cb(buf + off, n, ctx) != 0)
            return SQLI_ERR;
        off += n;
    }
    return SQLI_OK;
}
