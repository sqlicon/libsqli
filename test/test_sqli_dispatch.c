/*
 * Phase 3 unit tests for protocol dispatch & SQL execution.
 *
 * Tests DESCRIBE, TUPLE, DONE, ERROR parsing via local socketpairs.
 */

#include "unity.h"
#include "libsqli/sqli.h"
#include "sqli_internal.h"

#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/time.h>
#include <errno.h>

static int create_socket_pair(int *read_fd, int *write_fd)
{
    int sv[2];
    if (read_fd == NULL || write_fd == NULL)
        return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return -1;
    *read_fd = sv[0];
    *write_fd = sv[1];
    return 0;
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint64_t monotonic_ms(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
        return 0;
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
}

static int count_frame_opcode(const uint8_t *buf, ssize_t len, uint8_t opcode)
{
    int count = 0;
    if (buf == NULL || len < 8)
        return 0;
    for (ssize_t i = 0; i + 7 < len; i++) {
        if (buf[i] == 0 && buf[i + 1] == SQLI_SQ_ID &&
            buf[i + 4] == 0 && buf[i + 5] == opcode &&
            buf[i + 6] == 0 && buf[i + 7] == SQLI_SQ_EOT) {
            count++;
        }
    }
    return count;
}

static size_t build_describe_response(uint8_t *buf, uint16_t nfields)
{
    size_t p = 0;
    buf[p++] = 0; buf[p++] = SQLI_SQ_DESCRIBE;
    buf[p++] = 0; buf[p++] = 7;  /* stmt_type */
    buf[p++] = 0; buf[p++] = 1;  /* stmt_id */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;  /* cost */
    buf[p++] = 0; buf[p++] = (uint8_t)(nfields * 4);          /* tuple_size */
    buf[p++] = 0; buf[p++] = (uint8_t)nfields;                /* nfields */
    buf[p++] = 0; buf[p++] = 0;                               /* strtab size (short) */

    for (uint16_t i = 0; i < nfields; i++) {
        uint32_t pos = (uint32_t)(i * 4);
        buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = (uint8_t)i;        /* field index */
        buf[p++] = 0; buf[p++] = 0; buf[p++] = (uint8_t)(pos >> 8); buf[p++] = (uint8_t)pos;
        buf[p++] = 0; buf[p++] = (uint8_t)(SQLI_TYPE_INT | SQLI_BIT_NOTNULLABLE); /* type */
        buf[p++] = 0; buf[p++] = 4;                                                /* encoded len */
    }

    return p;
}

static size_t build_tuple_response_2int(uint8_t *buf, int col0, int col1)
{
    size_t p = 0;
    buf[p++] = 0; buf[p++] = SQLI_SQ_TUPLE;
    buf[p++] = 0; buf[p++] = 0;  /* warnings */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 8;  /* tuple_size */
    buf[p++] = (uint8_t)(col0 >> 24);
    buf[p++] = (uint8_t)(col0 >> 16);
    buf[p++] = (uint8_t)(col0 >> 8);
    buf[p++] = (uint8_t)col0;
    buf[p++] = (uint8_t)(col1 >> 24);
    buf[p++] = (uint8_t)(col1 >> 16);
    buf[p++] = (uint8_t)(col1 >> 8);
    buf[p++] = (uint8_t)col1;
    return p;
}

static size_t build_done_response_ex(uint8_t *buf, int64_t rows_affected, int32_t row_id, uint32_t errd1)
{
    size_t p = 0;
    buf[p++] = 0; buf[p++] = SQLI_SQ_DONE;
    buf[p++] = 0; buf[p++] = 0;  /* warnings */
    buf[p++] = (uint8_t)(rows_affected >> 24);
    buf[p++] = (uint8_t)(rows_affected >> 16);
    buf[p++] = (uint8_t)(rows_affected >> 8);
    buf[p++] = (uint8_t)rows_affected;
    buf[p++] = (uint8_t)(row_id >> 24);
    buf[p++] = (uint8_t)(row_id >> 16);
    buf[p++] = (uint8_t)(row_id >> 8);
    buf[p++] = (uint8_t)row_id;
    buf[p++] = (uint8_t)(errd1 >> 24);
    buf[p++] = (uint8_t)(errd1 >> 16);
    buf[p++] = (uint8_t)(errd1 >> 8);
    buf[p++] = (uint8_t)errd1;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;  /* errd2 */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;  /* errd3 */
    return p;
}

static size_t build_done_response(uint8_t *buf, int64_t rows_affected)
{
    return build_done_response_ex(buf, rows_affected, 0, 0);
}

static size_t build_insertdone_response(uint8_t *buf, int64_t serial8)
{
    size_t p = 0;
    uint64_t mag = (serial8 < 0) ? (uint64_t)(-serial8) : (uint64_t)serial8;
    int16_t sign = (serial8 < 0) ? -1 : (serial8 > 0 ? 1 : 0);
    uint32_t lo32 = (uint32_t)(mag & 0xFFFFFFFFu);
    uint32_t hi32 = (uint32_t)(mag >> 32);

    buf[p++] = 0; buf[p++] = SQLI_SQ_INSERTDONE;
    buf[p++] = (uint8_t)(sign >> 8); buf[p++] = (uint8_t)sign;
    buf[p++] = (uint8_t)(lo32 >> 24); buf[p++] = (uint8_t)(lo32 >> 16);
    buf[p++] = (uint8_t)(lo32 >> 8);  buf[p++] = (uint8_t)lo32;
    buf[p++] = (uint8_t)(hi32 >> 24); buf[p++] = (uint8_t)(hi32 >> 16);
    buf[p++] = (uint8_t)(hi32 >> 8);  buf[p++] = (uint8_t)hi32;
    return p;
}

static size_t build_error_response_with_message(uint8_t *buf, int16_t sqlcode,
                                                int16_t isamcode, const char *msg)
{
    size_t p = 0;
    uint16_t msg_len = (uint16_t)strlen(msg);

    buf[p++] = 0; buf[p++] = SQLI_SQ_ERR;
    buf[p++] = (uint8_t)(sqlcode >> 8); buf[p++] = (uint8_t)sqlcode;
    buf[p++] = (uint8_t)(isamcode >> 8); buf[p++] = (uint8_t)isamcode;
    buf[p++] = 0; buf[p++] = 0;  /* statementOffset (short mode) */
    buf[p++] = (uint8_t)(msg_len >> 8); buf[p++] = (uint8_t)msg_len;
    memcpy(buf + p, msg, msg_len);
    p += msg_len;
    if ((msg_len & 1u) != 0)
        buf[p++] = 0;

    return p;
}

static size_t build_error_response(uint8_t *buf, int16_t sqlcode, int16_t isamcode)
{
    return build_error_response_with_message(buf, sqlcode, isamcode, "test error");
}

static size_t build_blob_chunk_response(uint8_t *buf, const uint8_t *payload, uint16_t len)
{
    size_t p = 0;
    buf[p++] = 0; buf[p++] = SQLI_SQ_BLOB;
    buf[p++] = (uint8_t)(len >> 8); buf[p++] = (uint8_t)len;
    if (len > 0 && payload != NULL) {
        memcpy(buf + p, payload, len);
        p += len;
    }
    if ((len & 1u) != 0u)
        buf[p++] = 0;
    return p;
}

void test_dispatch_describe_zero_columns(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    uint8_t resp[128];
    size_t p = 0;
    p += build_describe_response(resp + p, 0);
    p += build_done_response(resp + p, 0);

    TEST_ASSERT_EQUAL_INT((int)p, (int)write(write_fd, resp, p));
    shutdown(write_fd, SHUT_WR);
    set_nonblocking(read_fd);

    sqli_result_t *result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_receive_dispatch(read_fd, result, NULL));
    TEST_ASSERT_EQUAL_INT(0, result->column_count);
    TEST_ASSERT_EQUAL_INT(0, result->rows_affected);
    TEST_ASSERT_EQUAL_INT(1, result->eof);

    sqli_result_destroy(result);
    close(read_fd);
    close(write_fd);
}

void test_dispatch_multi_row_result(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    uint8_t resp[512];
    size_t p = 0;
    p += build_describe_response(resp + p, 2);
    p += build_tuple_response_2int(resp + p, 100, 200);
    p += build_tuple_response_2int(resp + p, 101, 201);
    p += build_tuple_response_2int(resp + p, 102, 202);
    p += build_done_response(resp + p, 3);

    TEST_ASSERT_EQUAL_INT((int)p, (int)write(write_fd, resp, p));
    shutdown(write_fd, SHUT_WR);
    set_nonblocking(read_fd);

    sqli_result_t *result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_receive_dispatch(read_fd, result, NULL));
    TEST_ASSERT_EQUAL_INT(2, result->column_count);
    TEST_ASSERT_EQUAL_INT(3, result->rows_affected);
    TEST_ASSERT_EQUAL_INT(3, result->row_count);

    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(100, sqli_result_get_int(result, 0));
    TEST_ASSERT_EQUAL_INT(200, sqli_result_get_int(result, 1));

    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(101, sqli_result_get_int(result, 0));
    TEST_ASSERT_EQUAL_INT(201, sqli_result_get_int(result, 1));

    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(102, sqli_result_get_int(result, 0));
    TEST_ASSERT_EQUAL_INT(202, sqli_result_get_int(result, 1));

    TEST_ASSERT_EQUAL_INT(0, sqli_result_next(result));

    sqli_result_destroy(result);
    close(read_fd);
    close(write_fd);
}

void test_dispatch_done_exposes_generated_serial(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    uint8_t resp[128];
    size_t p = 0;
    p += build_done_response_ex(resp + p, 1, 42, 42);

    TEST_ASSERT_EQUAL_INT((int)p, (int)write(write_fd, resp, p));
    shutdown(write_fd, SHUT_WR);
    set_nonblocking(read_fd);

    sqli_result_t *result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_receive_dispatch(read_fd, result, NULL));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_has_generated_serial(result));
    TEST_ASSERT_EQUAL_INT64(42, sqli_result_generated_serial(result));

    sqli_result_destroy(result);
    close(read_fd);
    close(write_fd);
}

void test_dispatch_insertdone_exposes_generated_serial8(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    uint8_t resp[128];
    size_t p = 0;
    p += build_insertdone_response(resp + p, 1234567890123LL);
    p += build_done_response(resp + p, 1);

    TEST_ASSERT_EQUAL_INT((int)p, (int)write(write_fd, resp, p));
    shutdown(write_fd, SHUT_WR);
    set_nonblocking(read_fd);

    sqli_result_t *result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_receive_dispatch(read_fd, result, NULL));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_has_generated_serial8(result));
    TEST_ASSERT_EQUAL_INT64(1234567890123LL, sqli_result_generated_serial8(result));

    sqli_result_destroy(result);
    close(read_fd);
    close(write_fd);
}

void test_get_bytes_fetchblob_roundtrip(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    sqli_conn_t *conn = calloc(1, sizeof(*conn));
    sqli_result_t *result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_NOT_NULL(result);

    conn->state = SQLI_CONN_READY;
    conn->socket_fd = write_fd;
    conn->read_buf_cap = 4096;
    conn->read_buf = malloc(conn->read_buf_cap);
    TEST_ASSERT_NOT_NULL(conn->read_buf);

    result->owner_conn = conn;
    result->stmt_id = 7;
    result->column_count = 1;
    result->columns = calloc(1, sizeof(*result->columns));
    TEST_ASSERT_NOT_NULL(result->columns);
    result->columns[0].type = SQLI_TYPE_BLOB;
    result->columns[0].col_start_pos = 0;
    result->columns[0].encoded_length = 16;
    result->row_count = 1;
    result->cursor = 0;
    result->current_row = 0;
    result->tuple_len = 7;
    result->tuple_buffer = malloc(result->tuple_len);
    TEST_ASSERT_NOT_NULL(result->tuple_buffer);
    /* len16 + descriptor payload */
    result->tuple_buffer[0] = 0;
    result->tuple_buffer[1] = 5;
    result->tuple_buffer[2] = 1;
    result->tuple_buffer[3] = 2;
    result->tuple_buffer[4] = 3;
    result->tuple_buffer[5] = 4;
    result->tuple_buffer[6] = 5;

    uint8_t resp[128];
    size_t p = 0;
    const uint8_t payload[] = {'a', 'b', 'c'};
    p += build_blob_chunk_response(resp + p, payload, (uint16_t)sizeof(payload));
    p += build_done_response(resp + p, 0);
    resp[p++] = 0; resp[p++] = SQLI_SQ_EOT;
    TEST_ASSERT_EQUAL_INT((int)p, (int)write(read_fd, resp, p));

    uint8_t out[8] = {0};
    size_t out_len = sizeof(out);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_bytes(result, 0, out, &out_len));
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)out_len);
    TEST_ASSERT_EQUAL_UINT8('a', out[0]);
    TEST_ASSERT_EQUAL_UINT8('b', out[1]);
    TEST_ASSERT_EQUAL_UINT8('c', out[2]);

    uint8_t wire[32] = {0};
    ssize_t wn = read(read_fd, wire, sizeof(wire));
    TEST_ASSERT_TRUE(wn >= 11);
    TEST_ASSERT_EQUAL_UINT8(0, wire[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_ID, wire[1]);
    TEST_ASSERT_EQUAL_UINT8(0, wire[2]);
    TEST_ASSERT_EQUAL_UINT8(7, wire[3]);
    TEST_ASSERT_EQUAL_UINT8(0, wire[4]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_FETCHBLOB, wire[5]);

    sqli_result_destroy(result);
    free(conn->read_buf);
    free(conn);
    close(read_fd);
    close(write_fd);
}

void test_dispatch_error_response(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    uint8_t resp[256];
    size_t p = 0;
    p += build_error_response(resp + p, -100, 0);

    TEST_ASSERT_EQUAL_INT((int)p, (int)write(write_fd, resp, p));
    shutdown(write_fd, SHUT_WR);
    set_nonblocking(read_fd);

    sqli_result_t *result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_receive_dispatch(read_fd, result, NULL));
    TEST_ASSERT_EQUAL_INT(-100, result->error_code);
    TEST_ASSERT_EQUAL_INT(1, result->eof);

    sqli_result_destroy(result);
    close(read_fd);
    close(write_fd);
}

void test_dispatch_error_response_sets_error_info(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    uint8_t resp[256];
    size_t p = 0;
    p += build_error_response(resp + p, -908, 149);

    TEST_ASSERT_EQUAL_INT((int)p, (int)write(write_fd, resp, p));
    shutdown(write_fd, SHUT_WR);
    set_nonblocking(read_fd);

    sqli_result_t *result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);

    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    set_error_context(&conn, "query/execute_recv", SQLI_SQ_EXECUTE);

    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_receive_dispatch(read_fd, result, &conn));
    TEST_ASSERT_EQUAL_INT(-908, conn.error_info.sqlcode);
    TEST_ASSERT_EQUAL_INT(149, conn.error_info.isamcode);
    TEST_ASSERT_EQUAL_INT(SQLI_SQ_ERR, conn.error_info.opcode);
    TEST_ASSERT_EQUAL_STRING("SQ_ERR", conn.error_info.opcode_name);
    TEST_ASSERT_EQUAL_STRING("query/execute_recv", conn.error_info.context);
    TEST_ASSERT_EQUAL_INT(1, conn.error_info.has_error);
    TEST_ASSERT_NOT_NULL(strstr(conn.error_info.sql_message, "Socket connection to server"));
    TEST_ASSERT_NOT_NULL(strstr(conn.error_info.isam_message, "daemon is no longer running"));
    TEST_ASSERT_NOT_NULL(strstr(sqli_error(&conn), "SQLCODE -908"));

    sqli_result_destroy(result);
    close(read_fd);
    close(write_fd);
}

void test_dispatch_error_response_drains_truncated_string(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    char long_msg[700];
    memset(long_msg, 'x', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';

    uint8_t resp[1024];
    size_t p = 0;
    p += build_error_response_with_message(resp + p, -100, 0, long_msg);
    resp[p++] = 0;
    resp[p++] = SQLI_SQ_EOT;

    TEST_ASSERT_EQUAL_INT((int)p, (int)write(write_fd, resp, p));
    shutdown(write_fd, SHUT_WR);
    set_nonblocking(read_fd);

    sqli_result_t *result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_receive_dispatch(read_fd, result, NULL));

    uint8_t next[2] = {0};
    TEST_ASSERT_EQUAL_INT(2, (int)read(read_fd, next, sizeof(next)));
    TEST_ASSERT_EQUAL_UINT8(0, next[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_EOT, next[1]);

    sqli_result_destroy(result);
    close(read_fd);
    close(write_fd);
}

void test_dispatch_unknown_opcode_sets_diagnostics(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    uint8_t resp[] = {
        0x04, 0x71, /* unknown opcode 1137 */
        0x00, SQLI_SQ_EOT
    };
    TEST_ASSERT_EQUAL_INT((int)sizeof(resp), (int)write(write_fd, resp, sizeof(resp)));
    shutdown(write_fd, SHUT_WR);
    set_nonblocking(read_fd);

    sqli_result_t *result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);

    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    set_error_context(&conn, "query/execute_recv", SQLI_SQ_EXECUTE);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_receive_dispatch(read_fd, result, &conn));
    TEST_ASSERT_EQUAL_INT(1, conn.error_info.has_error);
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, conn.error_info.status);
    TEST_ASSERT_EQUAL_INT(1137, conn.error_info.unknown_opcode);
    TEST_ASSERT_EQUAL_INT(1, (int)conn.error_info.unknown_opcode_count);
    TEST_ASSERT_EQUAL_INT(1137, conn.error_info.opcode);
    TEST_ASSERT_NOT_NULL(strstr(conn.error_info.message, "Unknown SQLI opcode 1137"));

    sqli_result_destroy(result);
    close(read_fd);
    close(write_fd);
}

void test_dispatch_unknown_opcode_strict_mode_fails(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    uint8_t resp[] = {
        0x04, 0x71, /* unknown opcode 1137 */
        0x00, SQLI_SQ_EOT
    };
    TEST_ASSERT_EQUAL_INT((int)sizeof(resp), (int)write(write_fd, resp, sizeof(resp)));
    shutdown(write_fd, SHUT_WR);
    set_nonblocking(read_fd);

    sqli_result_t *result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);

    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.strict_protocol = 1;
    set_error_context(&conn, "query/execute_recv", SQLI_SQ_EXECUTE);

    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_receive_dispatch(read_fd, result, &conn));
    TEST_ASSERT_EQUAL_INT(1, conn.error_info.has_error);
    TEST_ASSERT_EQUAL_INT(1137, conn.error_info.unknown_opcode);

    sqli_result_destroy(result);
    close(read_fd);
    close(write_fd);
}

void test_error_classify_network_retryable_sqlcode(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.error_info.has_error = 1;
    conn.error_info.status = SQLI_PROTO_ERROR;
    conn.error_info.sqlcode = -908;

    sqli_error_class klass = SQLI_ERROR_CLASS_NONE;
    bool retryable = false;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_error_classify(&conn, &klass, &retryable));
    TEST_ASSERT_EQUAL_INT(SQLI_ERROR_CLASS_NETWORK, klass);
    TEST_ASSERT_EQUAL_INT(1, retryable);
    TEST_ASSERT_EQUAL_INT(1, sqli_error_is_retryable(&conn));
}

void test_error_classify_auth_not_retryable(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.error_info.has_error = 1;
    conn.error_info.status = SQLI_AUTH_FAIL;
    conn.error_info.sqlcode = -23101;

    sqli_error_class klass = SQLI_ERROR_CLASS_NONE;
    bool retryable = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_error_classify(&conn, &klass, &retryable));
    TEST_ASSERT_EQUAL_INT(SQLI_ERROR_CLASS_AUTH, klass);
    TEST_ASSERT_EQUAL_INT(0, retryable);
    TEST_ASSERT_EQUAL_INT(0, sqli_error_is_retryable(&conn));
}

void test_error_classify_protocol_unknown_opcode_not_retryable(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.error_info.has_error = 1;
    conn.error_info.status = SQLI_PROTO_ERROR;
    conn.error_info.unknown_opcode = 1137;

    sqli_error_class klass = SQLI_ERROR_CLASS_NONE;
    bool retryable = true;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_error_classify(&conn, &klass, &retryable));
    TEST_ASSERT_EQUAL_INT(SQLI_ERROR_CLASS_PROTOCOL, klass);
    TEST_ASSERT_EQUAL_INT(0, retryable);
}

void test_error_classify_generic_transport_message_retryable(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.error_info.has_error = 1;
    conn.error_info.status = SQLI_ERR;
    strncpy(conn.error_info.context, "connect/tcp", sizeof(conn.error_info.context) - 1);
    strncpy(conn.error_info.message, "TCP connection failed", sizeof(conn.error_info.message) - 1);

    sqli_error_class klass = SQLI_ERROR_CLASS_NONE;
    bool retryable = false;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_error_classify(&conn, &klass, &retryable));
    TEST_ASSERT_EQUAL_INT(SQLI_ERROR_CLASS_NETWORK, klass);
    TEST_ASSERT_EQUAL_INT(1, retryable);
    TEST_ASSERT_EQUAL_INT(1, sqli_error_is_retryable(&conn));
}

void test_retry_recommend_network_backoff(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.error_info.has_error = 1;
    conn.error_info.status = SQLI_IO_ERROR;

    bool should_retry = false;
    uint32_t delay_ms = 0;

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_retry_recommend(&conn, 0, &should_retry, &delay_ms));
    TEST_ASSERT_EQUAL_INT(1, should_retry);
    TEST_ASSERT_EQUAL_UINT32(250, delay_ms);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_retry_recommend(&conn, 3, &should_retry, &delay_ms));
    TEST_ASSERT_EQUAL_INT(1, should_retry);
    TEST_ASSERT_EQUAL_UINT32(2000, delay_ms);
}

void test_retry_recommend_nonretryable_auth(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.error_info.has_error = 1;
    conn.error_info.status = SQLI_AUTH_FAIL;

    bool should_retry = true;
    uint32_t delay_ms = 999;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_retry_recommend(&conn, 0, &should_retry, &delay_ms));
    TEST_ASSERT_EQUAL_INT(0, should_retry);
    TEST_ASSERT_EQUAL_UINT32(0, delay_ms);
}

void test_query_with_retry_retries_on_retryable_error(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.state = SQLI_CONN_READY;
    conn.socket_fd = -1; /* force send failure -> retryable transport message */

    sqli_result_t *result = (sqli_result_t *)0x1;
    uint64_t t0 = monotonic_ms();
    sqli_status rc = sqli_query_with_retry(&conn, "SELECT 1", 1, &result);
    uint64_t t1 = monotonic_ms();

    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR, rc);
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(200, t1 - t0);
}

void test_query_with_retry_nonreadonly_does_not_retry(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.state = SQLI_CONN_READY;
    conn.socket_fd = -1; /* force send failure */

    sqli_result_t *result = NULL;
    uint64_t t0 = monotonic_ms();
    sqli_status rc = sqli_query_with_retry(&conn, "INSERT INTO t VALUES (1)", 3, &result);
    uint64_t t1 = monotonic_ms();

    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR, rc);
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(150, t1 - t0);
}

void test_query_select_closes_once_and_destroy_sends_nothing(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    set_nonblocking(read_fd);
    set_nonblocking(write_fd); /* prevent blocking reads when socket drains */

    /* Set a receive timeout on write_fd so reads don't block forever */
    struct timeval tv = {5, 0}; /* 5s */
    setsockopt(write_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t resp[512];
    size_t p = 0;

    /* PREPARE response */
    p += build_describe_response(resp + p, 1);
    /* Force SELECT statement_type for query-ex fetch path coverage. */
    resp[2] = 0;
    resp[3] = 2;
    p += build_done_response(resp + p, 0);
    /* OPEN ack — EOT marks end of OPEN response group */
    resp[p++] = 0; resp[p++] = SQLI_SQ_EOT;
    /* FETCH response (first row) */
    p += build_tuple_response_2int(resp + p, 42, 0);
    p += build_done_response(resp + p, 1);
    /* Second FETCH response (no more rows) */
    p += build_done_response(resp + p, 1);
    /* SQ_CLOSE control ack — EOT terminates this response group */
    resp[p++] = 0; resp[p++] = SQLI_SQ_CLOSE;
    resp[p++] = 0; resp[p++] = SQLI_SQ_EOT;

    /* Write close/release responses after sqli_query sends each command,
     * but since we're pre-batching, only provide close ack here.
     * SQ_RELEASE ack is not needed — the dispatch exits after CLOSE EOT
     * when no more data is peekable. */

    TEST_ASSERT_EQUAL_INT((int)p, (int)write(read_fd, resp, p));

    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.state = SQLI_CONN_READY;
    conn.socket_fd = write_fd;
    conn.cursor_type = SQLI_CURSOR_FORWARD_ONLY;
    conn.holdability = SQLI_CURSOR_CLOSE_AT_COMMIT;
    sqli_result_t *result = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_query(&conn, "SELECT 42", &result));
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(2, result->statement_type);
    TEST_ASSERT_EQUAL_INT(1, result->row_count);
    TEST_ASSERT_EQUAL_INT(-1, result->stmt_id);

    uint8_t wire[4096];
    ssize_t n = recv(read_fd, wire, sizeof(wire), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_INT(1, count_frame_opcode(wire, n, SQLI_SQ_CLOSE));
    TEST_ASSERT_EQUAL_INT(1, count_frame_opcode(wire, n, SQLI_SQ_RELEASE));

    sqli_result_destroy(result);

    uint8_t tail[64];
    ssize_t after = recv(read_fd, tail, sizeof(tail), MSG_DONTWAIT);
    TEST_ASSERT_TRUE(after == 0 || (after < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)));

    close(read_fd);
    close(write_fd);
}

void test_send_eot(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_send_eot(write_fd));

    uint8_t buf[2] = {0};
    TEST_ASSERT_EQUAL_INT(2, (int)recv(read_fd, buf, 2, 0));
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_EOT, buf[1]);

    close(read_fd);
    close(write_fd);
}

void test_send_command_sql(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.socket_fd = write_fd;

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_send_prepare(&conn, "SELECT 1"));

    uint8_t buf[64] = {0};
    int n = (int)recv(read_fd, buf, sizeof(buf), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_PREPARE, buf[1]);

    close(read_fd);
    close(write_fd);
}

void test_send_command_null(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_send_prepare(NULL, "SELECT 1"));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_send_prepare(NULL, NULL));
}

void test_send_fetch_bool_does_not_send_ret_type(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    sqli_result_t result;
    memset(&result, 0, sizeof(result));
    result.column_count = 1;
    result.columns = calloc(1, sizeof(*result.columns));
    TEST_ASSERT_NOT_NULL(result.columns);
    result.columns[0].type = SQLI_TYPE_BOOL;
    result.columns[0].encoded_length = 1;

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_send_fetch(write_fd, 0x1234, &result));

    uint8_t buf[64];
    int n = (int)recv(read_fd, buf, sizeof(buf), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_INT(14, n);
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_ID, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x12, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x34, buf[3]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_NFETCH, buf[5]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[12]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_EOT, buf[13]);

    free(result.columns);
    close(read_fd);
    close(write_fd);
}

void test_send_fetch_varchar_sends_ret_type(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    sqli_result_t result;
    memset(&result, 0, sizeof(result));
    result.column_count = 1;
    result.columns = calloc(1, sizeof(*result.columns));
    TEST_ASSERT_NOT_NULL(result.columns);
    result.columns[0].type = SQLI_TYPE_VARCHAR;
    result.columns[0].encoded_length = 64;

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_send_fetch(write_fd, 0x1234, &result));

    uint8_t buf[96];
    int n = (int)recv(read_fd, buf, sizeof(buf), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_INT(26, n);
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_ID, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x12, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x34, buf[3]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_RET_TYPE, buf[5]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[10]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_TYPE_VARCHAR, buf[11]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[22]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_EOT, buf[25]);

    free(result.columns);
    close(read_fd);
    close(write_fd);
}

void test_send_open_forward_cursor(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    TEST_ASSERT_EQUAL_INT(SQLI_OK,
                          sqli_send_open(write_fd, 0x1234,
                                         SQLI_CURSOR_FORWARD_ONLY,
                                         SQLI_CURSOR_CLOSE_AT_COMMIT));

    uint8_t buf[64];
    int n = (int)recv(read_fd, buf, sizeof(buf), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_ID, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[n - 2]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_EOT, buf[n - 1]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[n - 4]);
    TEST_ASSERT_EQUAL_UINT8(6, buf[n - 3]); /* SQ_OPEN */

    close(read_fd);
    close(write_fd);
}

void test_send_open_scroll_hold_cursor(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    TEST_ASSERT_EQUAL_INT(SQLI_OK,
                          sqli_send_open(write_fd, 0x1234,
                                         SQLI_CURSOR_SCROLL_INSENSITIVE,
                                         SQLI_CURSOR_HOLD_OVER_COMMIT));

    uint8_t buf[96];
    int n = (int)recv(read_fd, buf, sizeof(buf), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_GREATER_THAN_INT(28, n);

    /* Tail should contain SQ_SCROLL, SQ_HOLD, SQ_OPEN, SQ_EOT in order. */
    TEST_ASSERT_EQUAL_UINT8(0, buf[n - 8]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_SCROLL, buf[n - 7]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[n - 6]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_HOLD, buf[n - 5]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[n - 4]);
    TEST_ASSERT_EQUAL_UINT8(6, buf[n - 3]); /* SQ_OPEN */
    TEST_ASSERT_EQUAL_UINT8(0, buf[n - 2]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_EOT, buf[n - 1]);

    close(read_fd);
    close(write_fd);
}

void test_send_scroll_fetch_varchar_sends_sfetch(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    sqli_result_t result;
    memset(&result, 0, sizeof(result));
    result.column_count = 1;
    result.columns = calloc(1, sizeof(*result.columns));
    TEST_ASSERT_NOT_NULL(result.columns);
    result.columns[0].type = SQLI_TYPE_VARCHAR;
    result.columns[0].encoded_length = 64;

    TEST_ASSERT_EQUAL_INT(SQLI_OK,
                          sqli_send_scroll_fetch(write_fd, 0x1234, &result, 1, 7));

    uint8_t buf[128];
    int n = (int)recv(read_fd, buf, sizeof(buf), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_ID, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_RET_TYPE, buf[5]);

    TEST_ASSERT_EQUAL_UINT8(0, buf[16]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_SFETCH, buf[17]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[18]);  /* scroll_type hi */
    TEST_ASSERT_EQUAL_UINT8(1, buf[19]);  /* scroll_type lo */
    TEST_ASSERT_EQUAL_UINT8(0, buf[20]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[21]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[22]);
    TEST_ASSERT_EQUAL_UINT8(7, buf[23]);  /* index */
    TEST_ASSERT_EQUAL_UINT8(0, buf[n - 2]);
    TEST_ASSERT_EQUAL_UINT8(SQLI_SQ_EOT, buf[n - 1]);

    free(result.columns);
    close(read_fd);
    close(write_fd);
}

void test_result_destroy_null(void)
{
    sqli_result_destroy(NULL);
}

void test_result_columns_null(void)
{
    TEST_ASSERT_EQUAL_INT(0, sqli_result_columns(NULL));
}

void test_result_rows_null(void)
{
    TEST_ASSERT_EQUAL_INT64(0, sqli_result_rows_affected(NULL));
}

void test_result_next_null(void)
{
    TEST_ASSERT_EQUAL_INT(0, sqli_result_next(NULL));
}

void test_result_scroll_navigation(void)
{
    sqli_result_t result;
    memset(&result, 0, sizeof(result));

    uint8_t row0[] = {1, 'a'};
    uint8_t row1[] = {1, 'b'};
    uint8_t row2[] = {1, 'c'};
    uint8_t *rows[] = {row0, row1, row2};
    size_t lens[] = {sizeof(row0), sizeof(row1), sizeof(row2)};

    result.rows = rows;
    result.row_lens = lens;
    result.row_count = 3;
    result.column_count = 1;
    result.columns = calloc(1, sizeof(*result.columns));
    TEST_ASSERT_NOT_NULL(result.columns);
    result.columns[0].type = SQLI_TYPE_VARCHAR;
    result.columns[0].col_start_pos = 0;
    result.columns[0].encoded_length = 8;
    result.cursor = -1;
    result.current_row = -1;

    TEST_ASSERT_TRUE(sqli_result_first(&result));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_row_number(&result));

    TEST_ASSERT_TRUE(sqli_result_last(&result));
    TEST_ASSERT_EQUAL_INT(3, sqli_result_row_number(&result));

    TEST_ASSERT_TRUE(sqli_result_previous(&result));
    TEST_ASSERT_EQUAL_INT(2, sqli_result_row_number(&result));

    TEST_ASSERT_TRUE(sqli_result_absolute(&result, 1));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_row_number(&result));

    TEST_ASSERT_TRUE(sqli_result_relative(&result, 2));
    TEST_ASSERT_EQUAL_INT(3, sqli_result_row_number(&result));

    TEST_ASSERT_FALSE(sqli_result_absolute(&result, 0));
    TEST_ASSERT_FALSE(sqli_result_relative(&result, 1));

    free(result.columns);
}

void test_result_first_scroll_refetches_with_sfetch_absolute(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    /* Preload server response for SQ_SFETCH: one VARCHAR row "z" + DONE. */
    uint8_t resp[] = {
        0, SQLI_SQ_TUPLE,
        0, 0,              /* warnings */
        0, 0, 0, 2,        /* tuple_size */
        1, 'z',            /* VARCHAR(1) */
        0, SQLI_SQ_DONE,
        0, 0,              /* warnings */
        0, 0, 0, 1,        /* rows_affected */
        0, 0, 0, 1,        /* rowid */
        0, 0, 0, 0         /* sqlerrd1 */
    };
    TEST_ASSERT_EQUAL_INT((int)sizeof(resp),
                          (int)write(read_fd, resp, sizeof(resp)));

    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.socket_fd = write_fd;
    conn.state = SQLI_CONN_READY;

    sqli_result_t result;
    memset(&result, 0, sizeof(result));
    result.owner_conn = &conn;
    result.cursor_type = SQLI_CURSOR_SCROLL_INSENSITIVE;
    result.statement_type = 2; /* SELECT */
    result.stmt_id = 0x1234;
    result.column_count = 1;
    result.columns = calloc(1, sizeof(*result.columns));
    TEST_ASSERT_NOT_NULL(result.columns);
    result.columns[0].type = SQLI_TYPE_VARCHAR;
    result.columns[0].encoded_length = 8;
    result.columns[0].col_start_pos = 0;

    TEST_ASSERT_TRUE(sqli_result_first(&result));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_row_number(&result));
    TEST_ASSERT_EQUAL_STRING("z", sqli_result_get_string(&result, 0));

    uint8_t sent[128];
    int n = (int)recv(read_fd, sent, sizeof(sent), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    int found_sfetch = 0;
    for (int i = 0; i + 7 < n; i++) {
        if (sent[i] == 0 && sent[i + 1] == SQLI_SQ_SFETCH &&
            sent[i + 2] == 0 && sent[i + 3] == 6 &&
            sent[i + 4] == 0 && sent[i + 5] == 0 &&
            sent[i + 6] == 0 && sent[i + 7] == 1) {
            found_sfetch = 1;
            break;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, found_sfetch);

    sqli_result_cleanup(&result);
    close(read_fd);
    close(write_fd);
}

void test_result_get_string_cp1252_to_utf8_iconv(void)
{
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.client_locale = "en_US.utf8";
    conn.db_locale = "de_DE.CP1252";

    sqli_result_t result;
    memset(&result, 0, sizeof(result));
    result.owner_conn = &conn;
    result.column_count = 1;
    result.current_row = 0;

    result.columns = calloc(1, sizeof(*result.columns));
    TEST_ASSERT_NOT_NULL(result.columns);
    result.columns[0].type = SQLI_TYPE_VARCHAR;

    static const uint8_t tuple[] = {
        18,                          /* VARCHAR length */
        'Z', 0xE4, 'h', 'l', 'l', 'i', 's', 't', 'e', 'n',
        ' ', 'u', 'p', 'd', 'a', 't', 'e', 'n'
    };
    result.tuple_buffer = (uint8_t *)tuple;
    result.tuple_len = sizeof(tuple);

    const char *out = sqli_result_get_string(&result, 0);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("Z\xC3\xA4hllisten updaten", out);

    free(result.columns);
}

void test_result_destroy_skips_close_when_stmt_invalid(void)
{
    int read_fd = -1, write_fd = -1;
    if (create_socket_pair(&read_fd, &write_fd) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable for dispatch test");

    sqli_conn_t *conn = calloc(1, sizeof(*conn));
    sqli_result_t *result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_NOT_NULL(result);

    conn->state = SQLI_CONN_READY;
    conn->socket_fd = write_fd;

    result->owner_conn = conn;
    result->statement_type = 2; /* SELECT */
    result->stmt_id = -1;       /* already invalidated => no close/release */

    sqli_result_destroy(result);

    uint8_t sent[16];
    ssize_t n = recv(read_fd, sent, sizeof(sent), MSG_DONTWAIT);
    TEST_ASSERT_TRUE(n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)));

    free(conn);
    close(read_fd);
    close(write_fd);
}
