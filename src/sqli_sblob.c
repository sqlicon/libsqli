#include "sqli_sblob_internal.h"
#include "libsqli/sqli_sblob.h"
#include "libsqli/sqli.h"
#include "sqli_internal.h"
#include "sqli_protocol_internal.h"
#include "sqli_tcp.h"
#include "sqli_log.h"
#include "sqli_endian.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SQLI_SBLOB_BUFSIZE 32000

sqli_status sqli_sblob_open_query(sqli_conn_t *conn, const char *open_sql, int *out_lofd)
{
    if (conn == NULL || open_sql == NULL || out_lofd == NULL)
        return SQLI_INVALID_STATE;
    *out_lofd = -1;

    sqli_result_t *res = NULL;
    sqli_status rc = sqli_query(conn, open_sql, &res);
    if (rc != SQLI_OK)
        return rc;

    if (!sqli_result_next(res)) {
        sqli_result_destroy(res);
        set_error(conn, "smartblob open query returned no rows");
        return SQLI_ERR;
    }

    if (sqli_result_is_null(res, 0)) {
        sqli_result_destroy(res);
        set_error(conn, "smartblob open query returned NULL handle");
        return SQLI_ERR;
    }

    int lofd = sqli_result_get_int(res, 0);
    sqli_result_destroy(res);

    if (lofd < 0) {
        set_error(conn, "invalid smartblob handle returned by server");
        return SQLI_ERR;
    }

    *out_lofd = lofd;
    return SQLI_OK;
}

sqli_status sqli_sblob_open(sqli_conn_t *conn, const char *locator_hex, int mode, int *out_lofd)
{
    if (conn == NULL || locator_hex == NULL || out_lofd == NULL)
        return SQLI_INVALID_STATE;

    char sql[512];
    int n = snprintf(sql, sizeof(sql), "SELECT ifx_lo_open('%s'::BLOB, %d) FROM sysmaster:sysdual", locator_hex, mode);
    if (n < 0 || (size_t)n >= sizeof(sql))
        return SQLI_INVALID_STATE;

    return sqli_sblob_open_query(conn, sql, out_lofd);
}

sqli_status sqli_sblob_open_clob(sqli_conn_t *conn, const char *locator_hex, int mode, int *out_lofd)
{
    return sqli_sblob_open(conn, locator_hex, mode, out_lofd);
}

sqli_status sqli_sblob_close(sqli_conn_t *conn, int lofd)
{
    if (conn == NULL || lofd < 0)
        return SQLI_INVALID_STATE;

    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT ifx_lo_close(%d) FROM sysmaster:sysdual", lofd);

    sqli_result_t *res = NULL;
    sqli_status rc = sqli_query(conn, sql, &res);
    if (res)
        sqli_result_destroy(res);
    return rc;
}

sqli_status sqli_sblob_read(sqli_conn_t *conn, int lofd, void *buf, size_t nbytes, size_t *bytes_read)
{
    if (conn == NULL || buf == NULL || bytes_read == NULL || lofd < 0)
        return SQLI_INVALID_STATE;

    *bytes_read = 0;
    if (nbytes == 0)
        return SQLI_OK;

    clear_error(conn);

    int fd = conn->socket_fd;
    if (fd < 0 || conn->state != SQLI_CONN_READY)
        return SQLI_INVALID_STATE;

    /* Build SQ_LODATA request:
     * Header: opcode=97(2), subCom=0(2), loFd(2), length(4 BE), bufferSize=32000(2), SQ_EOT=12(2) */
    uint8_t req[14];
    req[0] = 0; req[1] = 97; /* SQ_LODATA */
    req[2] = 0; req[3] = 0;  /* subCom = 0 (LO_READ) */
    req[4] = (uint8_t)((lofd >> 8) & 0xFF);
    req[5] = (uint8_t)(lofd & 0xFF);
    int32_t req_len = nbytes > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)nbytes;
    req[6] = (uint8_t)((req_len >> 24) & 0xFF);
    req[7] = (uint8_t)((req_len >> 16) & 0xFF);
    req[8] = (uint8_t)((req_len >> 8) & 0xFF);
    req[9] = (uint8_t)(req_len & 0xFF);
    req[10] = (uint8_t)((SQLI_SBLOB_BUFSIZE >> 8) & 0xFF);
    req[11] = (uint8_t)(SQLI_SBLOB_BUFSIZE & 0xFF);
    req[12] = 0; req[13] = SQLI_SQ_EOT;

    if (sqli_tcp_send(fd, req, sizeof(req)) != (ssize_t)sizeof(req)) {
        set_error(conn, "failed to send SQ_LODATA read request");
        return SQLI_IO_ERROR;
    }

    /* Response header: opcode(2), optype(2), fileSize(4) */
    uint16_t resp_op = 0;
    while (1) {
        uint8_t op_buf[2];
        if (sqli_tcp_read(fd, op_buf, 2) != 2) {
            set_error(conn, "failed to read SQ_LODATA response header");
            return SQLI_IO_ERROR;
        }
        resp_op = (uint16_t)((op_buf[0] << 8) | op_buf[1]);
        if (resp_op == SQLI_SQ_EOT)
            continue;
        break;
    }

    if (resp_op == SQLI_SQ_ERR) {
        sqli_result_t tmp_res;
        memset(&tmp_res, 0, sizeof(tmp_res));
        sqli_receive_error(conn, fd, &tmp_res);
        sqli_result_cleanup(&tmp_res);
        return SQLI_ERR;
    }

    if (resp_op != 97) {
        sqli_log(SQLI_LOG_ERROR, "unexpected SQ_LODATA response opcode: %u", resp_op);
        set_error(conn, "unexpected opcode in SQ_LODATA read response");
        return SQLI_PROTO_ERROR;
    }

    uint8_t body[6];
    if (sqli_tcp_read(fd, body, sizeof(body)) != (ssize_t)sizeof(body)) {
        set_error(conn, "failed to read SQ_LODATA response body");
        return SQLI_IO_ERROR;
    }

    int32_t file_size = (int32_t)(((uint32_t)body[2] << 24) | ((uint32_t)body[3] << 16) |
                                  ((uint32_t)body[4] << 8)  | (uint32_t)body[5]);
    if (file_size <= 0) {
        while (1) {
            uint8_t tr_op[2];
            if (sqli_tcp_read(fd, tr_op, 2) != 2) break;
            uint16_t top = (uint16_t)((tr_op[0] << 8) | tr_op[1]);
            if (top == SQLI_SQ_EOT) break;
            if (top == SQLI_SQ_DONE) {
                uint8_t done_buf[10];
                if (sqli_tcp_read(fd, done_buf, 10) != 10) return SQLI_IO_ERROR;
            }
        }
        *bytes_read = 0;
        return SQLI_OK;
    }

    size_t stream_expected = (size_t)file_size < (size_t)req_len ? (size_t)file_size : (size_t)req_len;
    size_t total_streamed = 0;
    size_t total_copied = 0;
    uint8_t *out_ptr = (uint8_t *)buf;

    while (total_streamed < stream_expected) {
        uint8_t clen_buf[2];
        if (sqli_tcp_read(fd, clen_buf, 2) != 2) {
            set_error(conn, "failed to read SQ_LODATA chunk length");
            return SQLI_IO_ERROR;
        }
        uint16_t chlen = (uint16_t)((clen_buf[0] << 8) | clen_buf[1]);
        if (chlen == 0)
            break;

        size_t to_copy = (size_t)chlen;
        if (total_copied + to_copy > nbytes)
            to_copy = nbytes > total_copied ? nbytes - total_copied : 0;

        if (to_copy > 0) {
            if (sqli_tcp_read(fd, out_ptr + total_copied, to_copy) != (ssize_t)to_copy) {
                set_error(conn, "failed to read SQ_LODATA chunk payload");
                return SQLI_IO_ERROR;
            }
            total_copied += to_copy;
        }
        if (to_copy < (size_t)chlen) {
            size_t discard = (size_t)chlen - to_copy;
            uint8_t discard_buf[512];
            while (discard > 0) {
                size_t d = discard > sizeof(discard_buf) ? sizeof(discard_buf) : discard;
                if (sqli_tcp_read(fd, discard_buf, d) != (ssize_t)d)
                    return SQLI_IO_ERROR;
                discard -= d;
            }
        }

        if (chlen & 1) {
            uint8_t pad;
            if (sqli_tcp_read(fd, &pad, 1) != 1)
                return SQLI_IO_ERROR;
        }

        total_streamed += chlen;
    }

    /* Drain trailing SQ_DONE / SQ_EOT */
    while (1) {
        uint8_t tr_op[2];
        if (sqli_tcp_read(fd, tr_op, 2) != 2) break;
        uint16_t top = (uint16_t)((tr_op[0] << 8) | tr_op[1]);
        if (top == SQLI_SQ_EOT) break;
        if (top == SQLI_SQ_DONE) {
            uint8_t done_buf[10];
            if (sqli_tcp_read(fd, done_buf, 10) != 10) return SQLI_IO_ERROR;
        }
    }

    *bytes_read = total_copied;
    clear_error(conn);
    return SQLI_OK;
}

sqli_status sqli_sblob_read_seek(sqli_conn_t *conn, int lofd, int64_t offset,
                                 void *buf, size_t nbytes, size_t *bytes_read)
{
    if (conn == NULL || buf == NULL || bytes_read == NULL || lofd < 0)
        return SQLI_INVALID_STATE;

    *bytes_read = 0;
    if (nbytes == 0)
        return SQLI_OK;

    clear_error(conn);

    int fd = conn->socket_fd;
    if (fd < 0 || conn->state != SQLI_CONN_READY)
        return SQLI_INVALID_STATE;

    /* Build SQ_LODATA read-with-seek request (subCom = 1):
     * Header: opcode=97(2), subCom=1(2), loFd(2), length(4 BE), bufferSize=32000(2)
     * Offset: 10-byte sign-magnitude [sign(2 BE), low32(4 BE), high32(4 BE)]
     * Whence: 1(2 BE)
     * SQ_EOT: 12(2 BE)
     */
    uint8_t req[26];
    size_t p = 0;
    req[p++] = 0; req[p++] = 97; /* SQ_LODATA */
    req[p++] = 0; req[p++] = 1;  /* subCom = 1 (LO_READWITHSEEK) */
    req[p++] = (uint8_t)((lofd >> 8) & 0xFF);
    req[p++] = (uint8_t)(lofd & 0xFF);
    int32_t req_len = nbytes > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)nbytes;
    req[p++] = (uint8_t)((req_len >> 24) & 0xFF);
    req[p++] = (uint8_t)((req_len >> 16) & 0xFF);
    req[p++] = (uint8_t)((req_len >> 8) & 0xFF);
    req[p++] = (uint8_t)(req_len & 0xFF);
    req[p++] = (uint8_t)((SQLI_SBLOB_BUFSIZE >> 8) & 0xFF);
    req[p++] = (uint8_t)(SQLI_SBLOB_BUFSIZE & 0xFF);

    int16_t sign = offset < 0 ? -1 : 1;
    uint64_t mag = offset < 0 ? (uint64_t)(-offset) : (uint64_t)offset;
    uint32_t low32 = (uint32_t)(mag & 0xFFFFFFFF);
    uint32_t high32 = (uint32_t)(mag >> 32);

    req[p++] = (uint8_t)((sign >> 8) & 0xFF);
    req[p++] = (uint8_t)(sign & 0xFF);
    req[p++] = (uint8_t)((low32 >> 24) & 0xFF);
    req[p++] = (uint8_t)((low32 >> 16) & 0xFF);
    req[p++] = (uint8_t)((low32 >> 8) & 0xFF);
    req[p++] = (uint8_t)(low32 & 0xFF);
    req[p++] = (uint8_t)((high32 >> 24) & 0xFF);
    req[p++] = (uint8_t)((high32 >> 16) & 0xFF);
    req[p++] = (uint8_t)((high32 >> 8) & 0xFF);
    req[p++] = (uint8_t)(high32 & 0xFF);

    req[p++] = 0; req[p++] = 1; /* whence = 1 (LO_SEEK_CUR) */
    req[p++] = 0; req[p++] = SQLI_SQ_EOT;

    if (sqli_tcp_send(fd, req, p) != (ssize_t)p) {
        set_error(conn, "failed to send SQ_LODATA read-with-seek request");
        return SQLI_IO_ERROR;
    }

    uint16_t resp_op = 0;
    while (1) {
        uint8_t op_buf[2];
        if (sqli_tcp_read(fd, op_buf, 2) != 2) {
            set_error(conn, "failed to read SQ_LODATA seek response opcode");
            return SQLI_IO_ERROR;
        }
        resp_op = (uint16_t)((op_buf[0] << 8) | op_buf[1]);
        if (resp_op == SQLI_SQ_EOT)
            continue;
        break;
    }

    if (resp_op == SQLI_SQ_ERR) {
        sqli_result_t tmp_res;
        memset(&tmp_res, 0, sizeof(tmp_res));
        sqli_receive_error(conn, fd, &tmp_res);
        sqli_result_cleanup(&tmp_res);
        return SQLI_ERR;
    }

    if (resp_op != 97) {
        set_error(conn, "unexpected opcode in SQ_LODATA seek response");
        return SQLI_PROTO_ERROR;
    }

    uint8_t body[6];
    if (sqli_tcp_read(fd, body, sizeof(body)) != (ssize_t)sizeof(body)) {
        set_error(conn, "failed to read SQ_LODATA seek response body");
        return SQLI_IO_ERROR;
    }

    int32_t remaining_size = (int32_t)(((uint32_t)body[2] << 24) | ((uint32_t)body[3] << 16) |
                                       ((uint32_t)body[4] << 8)  | (uint32_t)body[5]);
    if (remaining_size <= 0) {
        while (1) {
            uint8_t tr_op[2];
            if (sqli_tcp_read(fd, tr_op, 2) != 2) break;
            uint16_t top = (uint16_t)((tr_op[0] << 8) | tr_op[1]);
            if (top == SQLI_SQ_EOT) break;
            if (top == SQLI_SQ_DONE) {
                uint8_t done_buf[10];
                if (sqli_tcp_read(fd, done_buf, 10) != 10) return SQLI_IO_ERROR;
            }
        }
        *bytes_read = 0;
        return SQLI_OK;
    }

    size_t stream_expected = (size_t)remaining_size < (size_t)req_len ? (size_t)remaining_size : (size_t)req_len;
    size_t total_streamed = 0;
    size_t total_copied = 0;
    uint8_t *out_ptr = (uint8_t *)buf;

    while (total_streamed < stream_expected) {
        uint8_t clen_buf[2];
        if (sqli_tcp_read(fd, clen_buf, 2) != 2)
            return SQLI_IO_ERROR;
        uint16_t chlen = (uint16_t)((clen_buf[0] << 8) | clen_buf[1]);
        if (chlen == 0)
            break;

        size_t to_copy = (size_t)chlen;
        if (total_copied + to_copy > nbytes)
            to_copy = nbytes > total_copied ? nbytes - total_copied : 0;

        if (to_copy > 0) {
            if (sqli_tcp_read(fd, out_ptr + total_copied, to_copy) != (ssize_t)to_copy)
                return SQLI_IO_ERROR;
            total_copied += to_copy;
        }

        if (to_copy < (size_t)chlen) {
            size_t discard = (size_t)chlen - to_copy;
            uint8_t discard_buf[512];
            while (discard > 0) {
                size_t d = discard > sizeof(discard_buf) ? sizeof(discard_buf) : discard;
                if (sqli_tcp_read(fd, discard_buf, d) != (ssize_t)d)
                    return SQLI_IO_ERROR;
                discard -= d;
            }
        }

        if (chlen & 1) {
            uint8_t pad;
            if (sqli_tcp_read(fd, &pad, 1) != 1)
                return SQLI_IO_ERROR;
        }

        total_streamed += chlen;
    }

    /* Drain trailing SQ_DONE / SQ_EOT */
    while (1) {
        uint8_t tr_op[2];
        if (sqli_tcp_read(fd, tr_op, 2) != 2) break;
        uint16_t top = (uint16_t)((tr_op[0] << 8) | tr_op[1]);
        if (top == SQLI_SQ_EOT) break;
        if (top == SQLI_SQ_DONE) {
            uint8_t done_buf[10];
            if (sqli_tcp_read(fd, done_buf, 10) != 10) return SQLI_IO_ERROR;
        }
    }

    *bytes_read = total_copied;
    clear_error(conn);
    return SQLI_OK;
}

sqli_status sqli_sblob_write(sqli_conn_t *conn, int lofd, const void *buf, size_t nbytes, size_t *bytes_written)
{
    if (conn == NULL || buf == NULL || bytes_written == NULL || lofd < 0)
        return SQLI_INVALID_STATE;

    *bytes_written = 0;
    if (nbytes == 0)
        return SQLI_OK;

    clear_error(conn);

    int fd = conn->socket_fd;
    if (fd < 0 || conn->state != SQLI_CONN_READY)
        return SQLI_INVALID_STATE;

    /* Header: opcode=97(2), subCom=2(2), loFd(2), length(4 BE), bufferSize=32000(2) */
    uint8_t req[12];
    req[0] = 0; req[1] = 97; /* SQ_LODATA */
    req[2] = 0; req[3] = 2;  /* subCom = 2 (LO_WRITE) */
    req[4] = (uint8_t)((lofd >> 8) & 0xFF);
    req[5] = (uint8_t)(lofd & 0xFF);
    int32_t req_len = nbytes > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)nbytes;
    req[6] = (uint8_t)((req_len >> 24) & 0xFF);
    req[7] = (uint8_t)((req_len >> 16) & 0xFF);
    req[8] = (uint8_t)((req_len >> 8) & 0xFF);
    req[9] = (uint8_t)(req_len & 0xFF);
    req[10] = (uint8_t)((SQLI_SBLOB_BUFSIZE >> 8) & 0xFF);
    req[11] = (uint8_t)(SQLI_SBLOB_BUFSIZE & 0xFF);

    if (sqli_tcp_send(fd, req, sizeof(req)) != (ssize_t)sizeof(req)) {
        set_error(conn, "failed to send SQ_LODATA write header");
        return SQLI_IO_ERROR;
    }

    /* Stream chunks of at most 32000 bytes */
    size_t rem = nbytes;
    const uint8_t *cur = (const uint8_t *)buf;
    while (rem > 0) {
        uint16_t chlen = rem > SQLI_SBLOB_BUFSIZE ? SQLI_SBLOB_BUFSIZE : (uint16_t)rem;
        uint8_t ch_hdr[2] = {(uint8_t)(chlen >> 8), (uint8_t)(chlen & 0xFF)};
        if (sqli_tcp_send(fd, ch_hdr, 2) != 2 ||
            sqli_tcp_send(fd, cur, chlen) != (ssize_t)chlen) {
            set_error(conn, "failed to stream SQ_LODATA write chunk");
            return SQLI_IO_ERROR;
        }
        if (chlen & 1) {
            uint8_t pad = 0;
            if (sqli_tcp_send(fd, &pad, 1) != 1)
                return SQLI_IO_ERROR;
        }
        cur += chlen;
        rem -= chlen;
    }

    /* Send SQ_EOT (flip client -> server) */
    uint8_t eot[2] = {0, SQLI_SQ_EOT};
    if (sqli_tcp_send(fd, eot, 2) != 2) {
        set_error(conn, "failed to send SQ_EOT");
        return SQLI_IO_ERROR;
    }

    /* Receive write ack: opcode(2), optype(2), fileSize(4) */
    uint16_t resp_op = 0;
    while (1) {
        uint8_t op_buf[2];
        if (sqli_tcp_read(fd, op_buf, 2) != 2) {
            set_error(conn, "failed to read SQ_LODATA write response opcode");
            return SQLI_IO_ERROR;
        }
        resp_op = (uint16_t)((op_buf[0] << 8) | op_buf[1]);
        if (resp_op == SQLI_SQ_EOT)
            continue;
        break;
    }

    if (resp_op == SQLI_SQ_ERR) {
        sqli_result_t tmp_res;
        memset(&tmp_res, 0, sizeof(tmp_res));
        sqli_receive_error(conn, fd, &tmp_res);
        sqli_result_cleanup(&tmp_res);
        return SQLI_ERR;
    }

    if (resp_op != 97) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "unexpected opcode %u in smartblob write response", (unsigned)resp_op);
        set_error(conn, err_msg);
        return SQLI_PROTO_ERROR;
    }

    uint8_t body[6];
    if (sqli_tcp_read(fd, body, sizeof(body)) != (ssize_t)sizeof(body)) {
        set_error(conn, "failed to read SQ_LODATA write response body");
        return SQLI_IO_ERROR;
    }

    uint16_t optype = (uint16_t)((body[0] << 8) | body[1]);
    int32_t resp_size = (int32_t)(((uint32_t)body[2] << 24) | ((uint32_t)body[3] << 16) |
                                  ((uint32_t)body[4] << 8)  | (uint32_t)body[5]);

    /* Drain trailing SQ_DONE / SQ_EOT */
    while (1) {
        uint8_t tr_op[2];
        if (sqli_tcp_read(fd, tr_op, 2) != 2) break;
        uint16_t top = (uint16_t)((tr_op[0] << 8) | tr_op[1]);
        if (top == SQLI_SQ_EOT) break;
        if (top == SQLI_SQ_DONE) {
            uint8_t done_buf[10];
            if (sqli_tcp_read(fd, done_buf, 10) != 10) return SQLI_IO_ERROR;
        }
    }

    if (optype != 2 || resp_size < 0) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "smartblob write failed: optype=%u resp_size=%d",
                 (unsigned)optype, (int)resp_size);
        set_error(conn, err_msg);
        return SQLI_ERR;
    }

    *bytes_written = nbytes;
    clear_error(conn);
    return SQLI_OK;
}

sqli_status sqli_result_read_sblob(sqli_result_t *res, size_t col_index,
                                   void *buf, size_t nbytes, size_t *bytes_read)
{
    if (res == NULL || buf == NULL || bytes_read == NULL ||
        col_index >= (size_t)res->column_count)
        return SQLI_INVALID_STATE;

    *bytes_read = 0;
    if (sqli_result_is_null(res, col_index))
        return SQLI_OK;

    const char *locator = sqli_result_get_string(res, col_index);
    if (locator == NULL || strlen(locator) == 0)
        return SQLI_OK;

    sqli_conn_t *conn = res->owner_conn;
    if (conn == NULL)
        return SQLI_INVALID_STATE;

    uint8_t col_type = (uint8_t)res->columns[col_index].type;
    int lofd = -1;
    sqli_status rc;
    if (col_type == SQLI_TYPE_CLOB) {
        rc = sqli_sblob_open_clob(conn, locator, SQLI_LO_RDONLY, &lofd);
    } else {
        rc = sqli_sblob_open(conn, locator, SQLI_LO_RDONLY, &lofd);
    }
    if (rc != SQLI_OK)
        return rc;

    rc = sqli_sblob_read(conn, lofd, buf, nbytes, bytes_read);
    (void)sqli_sblob_close(conn, lofd);
    return rc;
}

static void write_long_sign_mag(uint8_t *dest, int64_t val)
{
    int16_t sign = 1;
    uint64_t mag;
    if (val < 0) {
        sign = -1;
        mag = (uint64_t)(-val);
    } else {
        mag = (uint64_t)val;
    }
    uint32_t low32 = (uint32_t)(mag & 0xFFFFFFFFu);
    uint32_t high32 = (uint32_t)(mag >> 32);
    dest[0] = (uint8_t)((low32 >> 24) & 0xFF);
    dest[1] = (uint8_t)((low32 >> 16) & 0xFF);
    dest[2] = (uint8_t)((low32 >> 8) & 0xFF);
    dest[3] = (uint8_t)(low32 & 0xFF);
    dest[4] = (uint8_t)((high32 >> 24) & 0xFF);
    dest[5] = (uint8_t)((high32 >> 16) & 0xFF);
    dest[6] = (uint8_t)((high32 >> 8) & 0xFF);
    dest[7] = (uint8_t)(high32 & 0xFF);
    uint16_t usign = (uint16_t)sign;
    dest[8] = (uint8_t)((usign >> 8) & 0xFF);
    dest[9] = (uint8_t)(usign & 0xFF);
    dest[10] = 0;
    dest[11] = 0;
}

static void sqli_safe_copy_str(char *dest, size_t dest_cap, const char *src)
{
    if (dest == NULL || dest_cap == 0)
        return;
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= dest_cap)
        len = dest_cap - 1;
    memcpy(dest, src, len);
    dest[len] = '\0';
}

static sqli_status get_lo_create_fphandle(sqli_conn_t *conn, int32_t *out_handle,
                                         char *out_dbname, size_t dbname_cap)
{
    if (conn->lo_create_fphandle > 0) {
        *out_handle = conn->lo_create_fphandle;
        if (out_dbname != NULL && dbname_cap > 0) {
            sqli_safe_copy_str(out_dbname, dbname_cap, conn->lo_create_dbname);
        }
        return SQLI_OK;
    }

    int fd = conn->socket_fd;
    const char *sig = "function informix.ifx_lo_create( ifx_lo_spec,integer,blob)";
    size_t sig_len = strlen(sig);

    uint8_t req[128];
    size_t pos = 0;
    req[pos++] = 0; req[pos++] = 101; /* SQ_GETROUTINE */
    req[pos++] = 0;                     /* flag = 0 */
    req[pos++] = (uint8_t)((sig_len >> 24) & 0xFF);
    req[pos++] = (uint8_t)((sig_len >> 16) & 0xFF);
    req[pos++] = (uint8_t)((sig_len >> 8) & 0xFF);
    req[pos++] = (uint8_t)(sig_len & 0xFF);
    memcpy(req + pos, sig, sig_len);
    pos += sig_len;
    if ((4 + sig_len) & 1u)
        req[pos++] = 0; /* padding */
    req[pos++] = 0; req[pos++] = 0;   /* req_fparam = 0 */
    req[pos++] = 0; req[pos++] = SQLI_SQ_EOT;

    if (sqli_tcp_send(fd, req, pos) != (ssize_t)pos) {
        set_error(conn, "failed to send SQ_GETROUTINE for ifx_lo_create");
        return SQLI_IO_ERROR;
    }

    uint16_t op = 0;
    while (1) {
        uint8_t op_buf[2];
        if (sqli_tcp_read(fd, op_buf, 2) != 2) {
            set_error(conn, "failed to read SQ_GETROUTINE opcode");
            return SQLI_IO_ERROR;
        }
        op = (uint16_t)((op_buf[0] << 8) | op_buf[1]);
        if (op == SQLI_SQ_DBOPEN_FLAGS) {
            uint8_t flags_buf[2];
            if (sqli_tcp_read(fd, flags_buf, 2) != 2) return SQLI_IO_ERROR;
            continue;
        }
        if (op == SQLI_SQ_EOT) {
            continue;
        }
        if (op == SQLI_SQ_XACTSTAT) {
            uint8_t xact_buf[6];
            if (sqli_tcp_read(fd, xact_buf, 6) != 6) return SQLI_IO_ERROR;
            continue;
        }
        break;
    }
    if (op == SQLI_SQ_ERR) {
        sqli_result_t tmp_res;
        memset(&tmp_res, 0, sizeof(tmp_res));
        sqli_receive_error(conn, fd, &tmp_res);
        sqli_result_cleanup(&tmp_res);
        return SQLI_ERR;
    }
    if (op != 101) {
        set_error(conn, "unexpected opcode in SQ_GETROUTINE response");
        return SQLI_PROTO_ERROR;
    }

    uint8_t dblen_buf[2];
    if (sqli_tcp_read(fd, dblen_buf, 2) != 2) {
        set_error(conn, "failed to read SQ_GETROUTINE dbName length");
        return SQLI_IO_ERROR;
    }
    uint16_t dblen = (uint16_t)((dblen_buf[0] << 8) | dblen_buf[1]);
    char dbname[128] = {0};
    if (dblen > 0) {
        size_t to_read = dblen < sizeof(dbname) - 1 ? dblen : sizeof(dbname) - 1;
        if (sqli_tcp_read(fd, (uint8_t *)dbname, to_read) != (ssize_t)to_read) {
            set_error(conn, "failed to read SQ_GETROUTINE dbName");
            return SQLI_IO_ERROR;
        }
        dbname[to_read] = '\0';
        if (dblen > to_read) {
            size_t excess = dblen - to_read;
            uint8_t dummy[128];
            while (excess > 0) {
                size_t d = excess > sizeof(dummy) ? sizeof(dummy) : excess;
                if (sqli_tcp_read(fd, dummy, d) != (ssize_t)d) return SQLI_IO_ERROR;
                excess -= d;
            }
        }
    }
    if ((2 + dblen) & 1u) {
        uint8_t pad;
        if (sqli_tcp_read(fd, &pad, 1) != 1) return SQLI_IO_ERROR;
    }

    uint8_t hbuf[4];
    if (sqli_tcp_read(fd, hbuf, 4) != 4) {
        set_error(conn, "failed to read SQ_GETROUTINE handle");
        return SQLI_IO_ERROR;
    }
    int32_t handle = (int32_t)(((uint32_t)hbuf[0] << 24) | ((uint32_t)hbuf[1] << 16) |
                               ((uint32_t)hbuf[2] << 8)  | (uint32_t)hbuf[3]);

    /* Drain trailing messages until SQ_EOT */
    while (1) {
        uint8_t tr_op[2];
        if (sqli_tcp_read(fd, tr_op, 2) != 2) break;
        uint16_t top = (uint16_t)((tr_op[0] << 8) | tr_op[1]);
        if (top == SQLI_SQ_EOT) break;
        if (top == SQLI_SQ_DONE) {
            uint8_t done_buf[10];
            if (sqli_tcp_read(fd, done_buf, 10) != 10) return SQLI_IO_ERROR;
        }
    }

    if (handle <= 0) {
        set_error(conn, "server returned invalid routine handle for ifx_lo_create");
        return SQLI_ERR;
    }

    conn->lo_create_fphandle = handle;
    sqli_safe_copy_str(conn->lo_create_dbname, sizeof(conn->lo_create_dbname), dbname);

    *out_handle = handle;
    if (out_dbname != NULL && dbname_cap > 0) {
        sqli_safe_copy_str(out_dbname, dbname_cap, dbname);
    }
    return SQLI_OK;
}

static sqli_status sblob_create_into(sqli_conn_t *conn, sqli_sblob_type type,
                              const sqli_sblob_options *options, sqli_sblob_t *out)
{
    if (conn == NULL || out == NULL)
        return SQLI_INVALID_STATE;

    clear_error(conn);

    memset(out, 0, sizeof(*out));
    out->lofd = -1;
    out->type = type;
    out->open = false;

    if (type != SQLI_SBLOB_BLOB && type != SQLI_SBLOB_CLOB) {
        set_error(conn, "invalid smart large object type");
        return SQLI_INVALID_STATE;
    }

    int mode = SQLI_LO_WRONLY;
    if (options != NULL) {
        if (options->open_mode < 0 || options->open_mode > 0xFFFF) {
            set_error(conn, "invalid open_mode in smartblob options");
            return SQLI_INVALID_STATE;
        }
        if (options->open_mode != 0)
            mode = options->open_mode;
        if (options->sbspace != NULL && strlen(options->sbspace) >= 128) {
            set_error(conn, "sbspace name exceeds maximum length (127)");
            return SQLI_INVALID_STATE;
        }
        if (options->estimated_bytes < -1 || options->maximum_bytes < -1 || options->extent_kib < -1) {
            set_error(conn, "smart large object size options must be >= -1");
            return SQLI_INVALID_STATE;
        }
    }

    if (conn->state != SQLI_CONN_READY || conn->socket_fd < 0) {
        set_error(conn, "connection is not ready");
        return SQLI_INVALID_STATE;
    }

    int32_t fp_handle = 0;
    char dbname[128] = {0};
    sqli_status rc = get_lo_create_fphandle(conn, &fp_handle, dbname, sizeof(dbname));
    if (rc != SQLI_OK)
        return rc;

    int fd = conn->socket_fd;

    /* Drain any pending tail packets from previous commands */
    for (int extra = 0; extra < 8; extra++) {
        if (!sqli_protocol_has_buffered_data(conn, fd)) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int prc = poll(&pfd, 1, 20);
            if (prc <= 0 || !(pfd.revents & POLLIN))
                break;
        }

        sqli_result_t tail;
        memset(&tail, 0, sizeof(tail));
        tail.owner_conn = conn;
        sqli_status drc = sqli_receive_dispatch(fd, &tail, conn);
        sqli_result_cleanup(&tail);
        if (drc != SQLI_OK)
            break;
    }

    /* Build 596-byte ifx_lo_spec */
    uint8_t spec[596];
    memset(spec, 0, sizeof(spec));
    spec[0] = 0xda; spec[1] = 0xda; spec[2] = 0xfe; spec[3] = 0xed; /* Magic 0xdadafeed */

    uint32_t create_flags = options ? options->create_flags : 0;
    spec[4] = (uint8_t)((create_flags >> 24) & 0xFF);
    spec[5] = (uint8_t)((create_flags >> 16) & 0xFF);
    spec[6] = (uint8_t)((create_flags >> 8) & 0xFF);
    spec[7] = (uint8_t)(create_flags & 0xFF);

    int64_t est_bytes = (options && options->estimated_bytes >= 0) ? options->estimated_bytes : -1;
    write_long_sign_mag(spec + 12, est_bytes);

    int64_t max_bytes = (options && options->maximum_bytes >= 0) ? options->maximum_bytes : -1;
    write_long_sign_mag(spec + 24, max_bytes);

    int32_t ext_size = (options && options->extent_kib >= 0) ? options->extent_kib : -1;
    uint32_t uext = (uint32_t)ext_size;
    spec[36] = (uint8_t)((uext >> 24) & 0xFF);
    spec[37] = (uint8_t)((uext >> 16) & 0xFF);
    spec[38] = (uint8_t)((uext >> 8) & 0xFF);
    spec[39] = (uint8_t)(uext & 0xFF);

    if (options && options->sbspace && options->sbspace[0] != '\0') {
        size_t slen = strlen(options->sbspace);
        if (slen > 127) slen = 127;
        memcpy(spec + 40, options->sbspace, slen);
    }

    /* rawColDescriptor (428 bytes starting at offset 168): offsets 404..411 are -1 */
    size_t raw_offset = 40 + 128; /* 168 */
    for (int k = 404; k <= 411; k++) {
        spec[raw_offset + k] = 0xFF;
    }

    /* Send SQ_EXFPROUTINE (102) + SQ_BIND (5) */
    uint16_t dblen = (uint16_t)strlen(dbname);
    uint8_t ex_buf[512];
    size_t pos = 0;

    ex_buf[pos++] = 0; ex_buf[pos++] = 102; /* SQ_EXFPROUTINE */
    ex_buf[pos++] = (uint8_t)((dblen >> 8) & 0xFF);
    ex_buf[pos++] = (uint8_t)(dblen & 0xFF);
    if (dblen > 0) {
        memcpy(ex_buf + pos, dbname, dblen);
        pos += dblen;
    }
    if ((2 + dblen) & 1u)
        ex_buf[pos++] = 0;

    ex_buf[pos++] = (uint8_t)((fp_handle >> 24) & 0xFF);
    ex_buf[pos++] = (uint8_t)((fp_handle >> 16) & 0xFF);
    ex_buf[pos++] = (uint8_t)((fp_handle >> 8) & 0xFF);
    ex_buf[pos++] = (uint8_t)(fp_handle & 0xFF);

    ex_buf[pos++] = 0; ex_buf[pos++] = 3; /* paramCount = 3 */
    ex_buf[pos++] = 0; ex_buf[pos++] = 0; /* req_fparam = 0 */

    ex_buf[pos++] = 0; ex_buf[pos++] = 5; /* SQ_BIND */
    ex_buf[pos++] = 0; ex_buf[pos++] = 3; /* paramCount = 3 */

    /* Param 1: ifx_lo_spec (UDT type 44) */
    ex_buf[pos++] = 0; ex_buf[pos++] = 44;
    ex_buf[pos++] = 0; ex_buf[pos++] = 0; /* owner len 0 */
    const char *spec_tname = "ifx_lo_spec";
    uint16_t spec_tnlen = (uint16_t)strlen(spec_tname);
    ex_buf[pos++] = (uint8_t)((spec_tnlen >> 8) & 0xFF);
    ex_buf[pos++] = (uint8_t)(spec_tnlen & 0xFF);
    memcpy(ex_buf + pos, spec_tname, spec_tnlen);
    pos += spec_tnlen;
    if ((2 + spec_tnlen) & 1u)
        ex_buf[pos++] = 0;
    ex_buf[pos++] = 0; ex_buf[pos++] = 0; /* null = 0 */
    ex_buf[pos++] = 0; ex_buf[pos++] = 0; /* enc_len = 0 */

    if (sqli_tcp_send(fd, ex_buf, pos) != (ssize_t)pos) {
        set_error(conn, "failed to send SQ_EXFPROUTINE header");
        return SQLI_IO_ERROR;
    }

    /* Payload of Param 1: 4-byte BE length (596) + 596 spec bytes */
    uint8_t p1_len[4] = {0, 0, (uint8_t)((sizeof(spec) >> 8) & 0xFF), (uint8_t)(sizeof(spec) & 0xFF)};
    if (sqli_tcp_send(fd, p1_len, 4) != 4 ||
        sqli_tcp_send(fd, spec, sizeof(spec)) != (ssize_t)sizeof(spec)) {
        set_error(conn, "failed to send ifx_lo_spec parameter");
        return SQLI_IO_ERROR;
    }

    /* Param 2: mode (type 2, 4-byte BE int) */
    uint8_t p2_buf[10];
    pos = 0;
    p2_buf[pos++] = 0; p2_buf[pos++] = 2; /* type 2 */
    p2_buf[pos++] = 0; p2_buf[pos++] = 0; /* null = 0 */
    p2_buf[pos++] = 0; p2_buf[pos++] = 0; /* enc_len = 0 */
    p2_buf[pos++] = (uint8_t)((mode >> 24) & 0xFF);
    p2_buf[pos++] = (uint8_t)((mode >> 16) & 0xFF);
    p2_buf[pos++] = (uint8_t)((mode >> 8) & 0xFF);
    p2_buf[pos++] = (uint8_t)(mode & 0xFF);
    if (sqli_tcp_send(fd, p2_buf, pos) != (ssize_t)pos) {
        set_error(conn, "failed to send mode parameter");
        return SQLI_IO_ERROR;
    }

    /* Param 3: blob (type 44, 72 zero bytes) */
    uint8_t p3_buf[32];
    pos = 0;
    p3_buf[pos++] = 0; p3_buf[pos++] = 44; /* type 44 */
    p3_buf[pos++] = 0; p3_buf[pos++] = 0;  /* owner len 0 */
    const char *blob_tname = "blob";
    uint16_t blob_tnlen = (uint16_t)strlen(blob_tname);
    p3_buf[pos++] = (uint8_t)((blob_tnlen >> 8) & 0xFF);
    p3_buf[pos++] = (uint8_t)(blob_tnlen & 0xFF);
    memcpy(p3_buf + pos, blob_tname, blob_tnlen);
    pos += blob_tnlen;
    if ((2 + blob_tnlen) & 1u)
        p3_buf[pos++] = 0;
    p3_buf[pos++] = 0; p3_buf[pos++] = 0; /* null = 0 */
    p3_buf[pos++] = 0; p3_buf[pos++] = 0; /* enc_len = 0 */
    p3_buf[pos++] = 0; p3_buf[pos++] = 0; p3_buf[pos++] = 0; p3_buf[pos++] = 72; /* 4-byte BE length */
    if (sqli_tcp_send(fd, p3_buf, pos) != (ssize_t)pos) {
        set_error(conn, "failed to send blob parameter header");
        return SQLI_IO_ERROR;
    }
    uint8_t zero_ptr[72] = {0};
    if (sqli_tcp_send(fd, zero_ptr, sizeof(zero_ptr)) != (ssize_t)sizeof(zero_ptr)) {
        set_error(conn, "failed to send blob parameter payload");
        return SQLI_IO_ERROR;
    }

    uint8_t eot[2] = {0, SQLI_SQ_EOT};
    if (sqli_tcp_send(fd, eot, 2) != 2) {
        set_error(conn, "failed to send SQ_EOT");
        return SQLI_IO_ERROR;
    }

    /* Read SQ_FPROUTINE response */
    uint16_t op = 0;
    while (1) {
        uint8_t op_buf[2];
        if (sqli_tcp_read(fd, op_buf, 2) != 2) {
            set_error(conn, "failed to read SQ_EXFPROUTINE response opcode");
            return SQLI_IO_ERROR;
        }
        op = (uint16_t)((op_buf[0] << 8) | op_buf[1]);
        if (op == SQLI_SQ_EOT)
            continue;
        break;
    }
    if (op == SQLI_SQ_ERR) {
        sqli_result_t tmp_res;
        memset(&tmp_res, 0, sizeof(tmp_res));
        sqli_receive_error(conn, fd, &tmp_res);
        sqli_result_cleanup(&tmp_res);
        return SQLI_ERR;
    }
    if (op != 103) { /* SQ_FPROUTINE */
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "unexpected opcode %u in fastpath response (expected SQ_FPROUTINE)", (unsigned)op);
        set_error(conn, err_msg);
        return SQLI_PROTO_ERROR;
    }

    uint8_t np_buf[2];
    if (sqli_tcp_read(fd, np_buf, 2) != 2) {
        set_error(conn, "failed to read output param count");
        return SQLI_IO_ERROR;
    }
    uint16_t num_params = (uint16_t)((np_buf[0] << 8) | np_buf[1]);

    int created_lofd = -1;
    uint8_t created_loc[SQLI_SBLOB_LOCATOR_MAX];
    size_t created_loc_len = 0;
    bool loc_truncated = false;

    for (uint16_t i = 0; i < num_params; i++) {
        uint8_t tp_buf[2];
        if (sqli_tcp_read(fd, tp_buf, 2) != 2) return SQLI_IO_ERROR;
        uint16_t raw_type = (uint16_t)((tp_buf[0] << 8) | tp_buf[1]);
        uint16_t sqltype = raw_type & 0xFF;
        bool is_distinct = (raw_type & 2048) != 0;

        if ((sqltype >= 18 && sqltype != 52 && sqltype != 53) || is_distinct) {
            uint8_t olen_buf[2];
            if (sqli_tcp_read(fd, olen_buf, 2) != 2) return SQLI_IO_ERROR;
            uint16_t olen = (uint16_t)((olen_buf[0] << 8) | olen_buf[1]);
            if (olen > 0) {
                uint8_t dump[128];
                while (olen > 0) {
                    size_t d = olen > sizeof(dump) ? sizeof(dump) : olen;
                    if (sqli_tcp_read(fd, dump, d) != (ssize_t)d) return SQLI_IO_ERROR;
                    olen -= d;
                }
            }
            if ((2 + olen) & 1u) { uint8_t pad; if (sqli_tcp_read(fd, &pad, 1) != 1) return SQLI_IO_ERROR; }

            uint8_t nlen_buf[2];
            if (sqli_tcp_read(fd, nlen_buf, 2) != 2) return SQLI_IO_ERROR;
            uint16_t nlen = (uint16_t)((nlen_buf[0] << 8) | nlen_buf[1]);
            if (nlen > 0) {
                uint8_t dump[128];
                while (nlen > 0) {
                    size_t d = nlen > sizeof(dump) ? sizeof(dump) : nlen;
                    if (sqli_tcp_read(fd, dump, d) != (ssize_t)d) return SQLI_IO_ERROR;
                    nlen -= d;
                }
            }
            if ((2 + nlen) & 1u) { uint8_t pad; if (sqli_tcp_read(fd, &pad, 1) != 1) return SQLI_IO_ERROR; }
        }

        uint8_t ind_prec[4];
        if (sqli_tcp_read(fd, ind_prec, 4) != 4) return SQLI_IO_ERROR;
        int16_t ind = (int16_t)((ind_prec[0] << 8) | ind_prec[1]);

        if (ind == -1) {
            continue;
        }

        if (sqltype == 40 || sqltype == 41 || sqltype == 44) {
            uint8_t udlen_buf[4];
            if (sqli_tcp_read(fd, udlen_buf, 4) != 4) return SQLI_IO_ERROR;
            uint32_t udlen = ((uint32_t)udlen_buf[0] << 24) | ((uint32_t)udlen_buf[1] << 16) |
                             ((uint32_t)udlen_buf[2] << 8)  | (uint32_t)udlen_buf[3];

            if (i == 0) {
                if (udlen > sizeof(created_loc)) {
                    loc_truncated = true;
                    uint8_t dump[128];
                    size_t rem = udlen;
                    while (rem > 0) {
                        size_t d = rem > sizeof(dump) ? sizeof(dump) : rem;
                        if (sqli_tcp_read(fd, dump, d) != (ssize_t)d) return SQLI_IO_ERROR;
                        rem -= d;
                    }
                } else {
                    if (sqli_tcp_read(fd, created_loc, udlen) != (ssize_t)udlen) return SQLI_IO_ERROR;
                    created_loc_len = udlen;
                }
            } else {
                uint8_t dump[128];
                size_t rem = udlen;
                while (rem > 0) {
                    size_t d = rem > sizeof(dump) ? sizeof(dump) : rem;
                    if (sqli_tcp_read(fd, dump, d) != (ssize_t)d) return SQLI_IO_ERROR;
                    rem -= d;
                }
            }
            if (udlen & 1u) {
                uint8_t pad;
                if (sqli_tcp_read(fd, &pad, 1) != 1) return SQLI_IO_ERROR;
            }
        } else if (sqltype == 2 || sqltype == 6) {
            uint8_t ibuf[4];
            if (sqli_tcp_read(fd, ibuf, 4) != 4) return SQLI_IO_ERROR;
            int32_t val = (int32_t)(((uint32_t)ibuf[0] << 24) | ((uint32_t)ibuf[1] << 16) |
                                    ((uint32_t)ibuf[2] << 8)  | (uint32_t)ibuf[3]);
            if (i == 1)
                created_lofd = val;
        } else {
            set_error(conn, "unexpected output parameter type in SQ_FPROUTINE");
            return SQLI_PROTO_ERROR;
        }
    }

    /* Drain trailing SQ_DONE / SQ_EOT */
    while (1) {
        uint8_t tr_op[2];
        if (sqli_tcp_read(fd, tr_op, 2) != 2) break;
        uint16_t top = (uint16_t)((tr_op[0] << 8) | tr_op[1]);
        if (top == SQLI_SQ_EOT) break;
        if (top == SQLI_SQ_DONE) {
            uint8_t done_buf[10];
            if (sqli_tcp_read(fd, done_buf, 10) != 10) return SQLI_IO_ERROR;
        }
    }

    if (loc_truncated) {
        if (created_lofd >= 0)
            (void)sqli_sblob_close(conn, created_lofd);
        set_error(conn, "smart large object locator exceeded maximum buffer length");
        return SQLI_ERR;
    }

    if (created_lofd < 0 || created_loc_len == 0) {
        if (created_lofd >= 0)
            (void)sqli_sblob_close(conn, created_lofd);
        set_error(conn, "failed to retrieve smart large object descriptor or locator");
        return SQLI_ERR;
    }

    out->lofd = created_lofd;
    out->type = type;
    memcpy(out->locator, created_loc, created_loc_len);
    out->locator_len = created_loc_len;
    out->open = true;

    clear_error(conn);
    return SQLI_OK;
}

sqli_status sqli_sblob_write_buffer(sqli_conn_t *conn, sqli_sblob_t *lob,
                                    const void *data, size_t length)
{
    if (conn == NULL || lob == NULL)
        return SQLI_INVALID_STATE;

    if (length > 0 && data == NULL)
        return SQLI_INVALID_STATE;

    if (!lob->open || lob->lofd < 0) {
        set_error(conn, "smart large object handle is not open");
        return SQLI_INVALID_STATE;
    }

    if (length == 0)
        return SQLI_OK;

    size_t written = 0;
    sqli_status rc = sqli_sblob_write(conn, lob->lofd, data, length, &written);
    if (rc != SQLI_OK)
        return rc;

    if (written != length) {
        set_error(conn, "short write to smart large object");
        return SQLI_ERR;
    }

    return SQLI_OK;
}

sqli_status sqli_sblob_write_stream(sqli_conn_t *conn, sqli_sblob_t *lob,
                                    sqli_sblob_reader reader, void *context,
                                    uint64_t *bytes_written)
{
    if (bytes_written != NULL)
        *bytes_written = 0;

    if (conn == NULL || lob == NULL || reader == NULL)
        return SQLI_INVALID_STATE;

    if (!lob->open || lob->lofd < 0) {
        set_error(conn, "smart large object handle is not open");
        return SQLI_INVALID_STATE;
    }

    uint8_t chunk[SQLI_SBLOB_BUFSIZE];
    uint64_t total = 0;

    while (1) {
        size_t nread = 0;
        sqli_status rc = reader(context, chunk, sizeof(chunk), &nread);
        if (rc != SQLI_OK) {
            set_error(conn, "smart large object stream reader callback failed");
            return rc;
        }

        if (nread == 0) {
            /* EOF reached */
            break;
        }

        if (nread > sizeof(chunk)) {
            set_error(conn, "smart large object stream reader exceeded buffer capacity");
            return SQLI_ERR;
        }

        if (nread > UINT64_MAX - total) {
            set_error(conn, "smart large object stream byte count exceeds supported range");
            return SQLI_LIMIT_EXCEEDED;
        }

        size_t written = 0;
        rc = sqli_sblob_write(conn, lob->lofd, chunk, nread, &written);
        /* Count only progress reported by the write operation, including a
         * confirmed partial write. Reader output alone is not confirmation. */
        if (written <= nread) {
            total += written;
            if (bytes_written != NULL)
                *bytes_written = total;
        } else {
            if (rc != SQLI_OK)
                return rc;
            set_error(conn, "smart large object write reported excessive progress");
            return SQLI_PROTO_ERROR;
        }
        if (rc != SQLI_OK)
            return rc;

        if (written != nread) {
            set_error(conn, "short write to smart large object");
            return SQLI_ERR;
        }
    }

    if (bytes_written != NULL)
        *bytes_written = total;

    return SQLI_OK;
}

sqli_status sqli_sblob_close_created(sqli_conn_t *conn, sqli_sblob_t *lob)
{
    if (conn == NULL || lob == NULL)
        return SQLI_INVALID_STATE;

    if (!lob->open || lob->lofd < 0) {
        /* Idempotent: already closed */
        return SQLI_OK;
    }

    int lofd = lob->lofd;
    lob->open = false;
    lob->lofd = -1;

    return sqli_sblob_close(conn, lofd);
}

sqli_status sqli_sblob_release(sqli_conn_t *conn, sqli_sblob_t *lob)
{
    if (conn == NULL || lob == NULL)
        return SQLI_INVALID_STATE;

    if (lob->locator_len == 0 || lob->locator_len > SQLI_SBLOB_LOCATOR_MAX) {
        set_error(conn, "smart large object locator length is invalid");
        return SQLI_INVALID_STATE;
    }

    /* Close descriptor if open */
    if (lob->open && lob->lofd >= 0) {
        (void)sqli_sblob_close_created(conn, lob);
    }

    char hex[SQLI_SBLOB_LOCATOR_MAX * 2 + 1];
    static const char hex_digits[] = "0123456789abcdef";
    for (size_t i = 0; i < lob->locator_len; i++) {
        hex[i * 2] = hex_digits[lob->locator[i] >> 4];
        hex[i * 2 + 1] = hex_digits[lob->locator[i] & 0x0f];
    }
    hex[lob->locator_len * 2] = '\0';

    char sql[512];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT ifx_lo_release('%s'::BLOB) FROM sysmaster:sysdual", hex);
    if (n < 0 || (size_t)n >= sizeof(sql)) {
        set_error(conn, "failed to format ifx_lo_release SQL");
        return SQLI_INVALID_STATE;
    }

    sqli_result_t *res = NULL;
    sqli_status rc = sqli_query(conn, sql, &res);
    if (rc != SQLI_OK) {
        if (res) sqli_result_destroy(res);
        return rc;
    }

    int ret_val = -1;
    if (res && sqli_result_next(res)) {
        ret_val = sqli_result_get_int(res, 0);
    }
    if (res) sqli_result_destroy(res);

    if (ret_val != 0) {
        set_error(conn, "ifx_lo_release rejected by server or object still referenced");
        return SQLI_ERR;
    }

    /* Invalidate locator on success */
    memset(lob->locator, 0, sizeof(lob->locator));
    lob->locator_len = 0;
    lob->open = false;
    lob->lofd = -1;

    return SQLI_OK;
}

sqli_status sqli_sblob_create(sqli_conn_t *conn, sqli_sblob_type type,
                              const sqli_sblob_options *options, sqli_sblob_t **out)
{
    if (conn == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_sblob_t *lob = calloc(1, sizeof(*lob));
    if (lob == NULL)
        return SQLI_ALLOC_FAIL;
    sqli_status status = sblob_create_into(conn, type, options, lob);
    if (status != SQLI_OK) {
        free(lob);
        return status;
    }
    *out = lob;
    return SQLI_OK;
}

void sqli_sblob_destroy(sqli_sblob_t *lob)
{
    free(lob);
}

sqli_status sqli_sblob_is_open(const sqli_sblob_t *lob, bool *out)
{
    if (lob == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    *out = lob->open;
    return SQLI_OK;
}

sqli_status sqli_sblob_get_type(const sqli_sblob_t *lob, sqli_sblob_type *out)
{
    if (lob == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    *out = lob->type;
    return SQLI_OK;
}
