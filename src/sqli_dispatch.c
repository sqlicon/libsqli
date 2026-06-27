#define _GNU_SOURCE
#include "sqli_internal.h"
#include "sqli_protocol_internal.h"
#include "sqli_result_internal.h"

#include "sqli_tcp.h"
#include "sqli_log.h"
#include "sqli_msg_catalog.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <endian.h>
#include <poll.h>
#include <sys/socket.h>

#define SQLI_MAX_TUPLE_BYTES (64u * 1024u * 1024u)

static sqli_status receive_done(int fd, sqli_result_t *r, sqli_conn_t *conn);
static sqli_status receive_error(sqli_conn_t *conn, int fd, sqli_result_t *r);
static sqli_status drain_bytes(sqli_conn_t *conn, int fd, size_t count);

static int sqli_eot_wait_ms(void)
{
    const char *value = getenv("SQLI_EOT_WAIT_MS");
    if (value == NULL || value[0] == '\0')
        return 1;

    char *end = NULL;
    long wait_ms = strtol(value, &end, 10);
    if (end == value || *end != '\0' || wait_ms < 0 || wait_ms > 1000)
        return 1;
    return (int)wait_ms;
}

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

bool sqli_protocol_has_buffered_data(sqli_conn_t *conn, int fd)
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
    uint16_t len = 0;
    sqli_status rc = read_be16(conn, fd, &len);
    if (rc != SQLI_OK)
        return rc;
    if (len == 0) {
        if (out_size > 0)
            out[0] = '\0';
        return SQLI_OK;
    }

    size_t remaining = (size_t)len;
    size_t copy_len = 0;
    if (out != NULL && out_size > 0) {
        copy_len = remaining;
        if (copy_len >= out_size)
            copy_len = out_size - 1;
    }

    if (copy_len > 0) {
        rc = read_exact(conn, fd, (uint8_t *)out, copy_len);
        if (rc != SQLI_OK)
            return rc;
    }
    if (out != NULL && out_size > 0)
        out[copy_len] = '\0';

    remaining -= copy_len;
    if (remaining > 0) {
        rc = drain_bytes(conn, fd, remaining);
        if (rc != SQLI_OK)
            return rc;
    }

    if ((len & 1u) != 0u) {
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

bool sqli_is_lob_like_type(uint8_t type)
{
    return type == SQLI_TYPE_TEXT || type == SQLI_TYPE_BYTE ||
           type == SQLI_TYPE_BLOB || type == SQLI_TYPE_CLOB;
}

sqli_status sqli_fetchblob_materialize(sqli_result_t *result, int col_index,
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

int64_t sqli_decode_ifx_int8(const uint8_t *buf10, bool *is_null)
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
