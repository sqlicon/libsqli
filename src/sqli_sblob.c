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
    if (conn == NULL || locator_hex == NULL || out_lofd == NULL)
        return SQLI_INVALID_STATE;

    char sql[512];
    int n = snprintf(sql, sizeof(sql), "SELECT ifx_lo_open('%s'::CLOB, %d) FROM sysmaster:sysdual", locator_hex, mode);
    if (n < 0 || (size_t)n >= sizeof(sql))
        return SQLI_INVALID_STATE;

    return sqli_sblob_open_query(conn, sql, out_lofd);
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
    uint8_t resp[8];
    if (sqli_tcp_read(fd, resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        set_error(conn, "failed to read SQ_LODATA response header");
        return SQLI_IO_ERROR;
    }

    uint16_t resp_op = (uint16_t)((resp[0] << 8) | resp[1]);
    if (resp_op != 97) {
        sqli_log(SQLI_LOG_ERROR, "unexpected SQ_LODATA response opcode: %u", resp_op);
        set_error(conn, "unexpected opcode in SQ_LODATA read response");
        return SQLI_PROTO_ERROR;
    }

    int32_t file_size = (int32_t)(((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
                                  ((uint32_t)resp[6] << 8)  | (uint32_t)resp[7]);
    if (file_size <= 0) {
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

    *bytes_read = total_copied;
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

    uint8_t resp[8];
    if (sqli_tcp_read(fd, resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        set_error(conn, "failed to read SQ_LODATA seek response header");
        return SQLI_IO_ERROR;
    }

    uint16_t resp_op = (uint16_t)((resp[0] << 8) | resp[1]);
    if (resp_op != 97) {
        set_error(conn, "unexpected opcode in SQ_LODATA seek response");
        return SQLI_PROTO_ERROR;
    }

    int32_t remaining_size = (int32_t)(((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
                                       ((uint32_t)resp[6] << 8)  | (uint32_t)resp[7]);
    if (remaining_size <= 0) {
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

    *bytes_read = total_copied;
    return SQLI_OK;
}

sqli_status sqli_sblob_write(sqli_conn_t *conn, int lofd, const void *buf, size_t nbytes, size_t *bytes_written)
{
    if (conn == NULL || buf == NULL || bytes_written == NULL || lofd < 0)
        return SQLI_INVALID_STATE;

    *bytes_written = 0;
    if (nbytes == 0)
        return SQLI_OK;

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

    uint8_t eot[2] = {0, SQLI_SQ_EOT};
    if (sqli_tcp_send(fd, eot, sizeof(eot)) != (ssize_t)sizeof(eot)) {
        set_error(conn, "failed to send SQ_LODATA write terminator");
        return SQLI_IO_ERROR;
    }

    /* Receive write ack: opcode(2), optype(2), fileSize(4) */
    uint8_t resp[8];
    if (sqli_tcp_read(fd, resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        set_error(conn, "failed to read SQ_LODATA write response");
        return SQLI_IO_ERROR;
    }

    uint16_t resp_op = (uint16_t)((resp[0] << 8) | resp[1]);
    uint16_t optype = (uint16_t)((resp[2] << 8) | resp[3]);
    int32_t resp_size = (int32_t)(((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
                                  ((uint32_t)resp[6] << 8)  | (uint32_t)resp[7]);

    if (resp_op != 97 || optype != 2 || resp_size < 0) {
        set_error(conn, "smartblob write failed or was rejected by server");
        return SQLI_ERR;
    }

    *bytes_written = (size_t)resp_size;
    return SQLI_OK;
}

sqli_status sqli_result_read_sblob(sqli_result_t *res, int col_index,
                                   void *buf, size_t nbytes, size_t *bytes_read)
{
    if (res == NULL || buf == NULL || bytes_read == NULL ||
        col_index < 0 || col_index >= res->column_count)
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
