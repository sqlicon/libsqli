#include "sqli_sblob_internal.h"
#include "libsqli/sqli_sblob.h"
#include "libsqli/sqli.h"
#include "unity.h"
#include "sqli_internal.h"

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { reader_chunk = 7, failure_after = 37, complete_length = 257,
       ack_size = 10, response_capacity = 512 };
static sqli_conn_t *connection;
static int sockets[2];
static sqli_sblob_t lob;
struct reader_state { size_t supplied, limit; bool fail, overflow; };

void setUp(void)
{
    sockets[0] = sockets[1] = -1;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&connection));
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    connection->socket_fd = sockets[0];
    connection->state = SQLI_CONN_READY;
    lob = (sqli_sblob_t){.lofd = 42, .open = true, .type = SQLI_SBLOB_BLOB};
}
void tearDown(void)
{
    if (connection != NULL) {
        connection->socket_fd = -1;
        sqli_destroy(connection);
        connection = NULL;
    }
    for (size_t i = 0; i < 2; i++) {
        if (sockets[i] >= 0)
            close(sockets[i]);
        sockets[i] = -1;
    }
}
static sqli_status reader(void *context, unsigned char *buffer, size_t capacity, size_t *count)
{
    struct reader_state *state = context;
    if (state->supplied == state->limit) {
        if (state->overflow) {
            *count = capacity + 1;
            return SQLI_OK;
        }
        *count = state->fail ? 1 : 0; /* Bytes from a failing callback do not count. */
        return state->fail ? SQLI_ERR : SQLI_OK;
    }
    size_t length = state->limit - state->supplied;
    if (length > reader_chunk)
        length = reader_chunk;
    if (length > capacity)
        return SQLI_BUFFER_TOO_SMALL;
    for (size_t i = 0; i < length; i++)
        buffer[i] = (unsigned char)((state->supplied + i) & 0xff);
    state->supplied += length;
    *count = length;
    return SQLI_OK;
}
static void responses(size_t count, bool malformed_tail)
{
    uint8_t bytes[response_capacity];
    const uint8_t ack[ack_size] = {0, SQLI_SQ_LODATA, 0, 2, 0, 0, 0, 0, 0, SQLI_SQ_EOT};
    TEST_ASSERT_TRUE(count <= (sizeof(bytes) - 2) / sizeof(ack));
    size_t length = 0;
    for (size_t i = 0; i < count; i++) {
        memcpy(bytes + length, ack, sizeof(ack));
        length += sizeof(ack);
    }
    if (malformed_tail) {
        bytes[length++] = 0xff;
        bytes[length++] = 0xff;
    }
    if (length != 0) {
        ssize_t sent = write(sockets[1], bytes, length);
        TEST_ASSERT_TRUE(sent >= 0);
        TEST_ASSERT_EQUAL_UINT(length, (size_t)sent);
    }
}
static void test_reader_failure_retains_37_confirmed_bytes(void)
{
    responses((failure_after + reader_chunk - 1) / reader_chunk, false);
    struct reader_state state = {.limit = failure_after, .fail = true};
    uint64_t written = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT(SQLI_ERR, sqli_sblob_write_stream(connection, &lob, reader, &state, &written));
    TEST_ASSERT_EQUAL_UINT64(failure_after, written);
    TEST_ASSERT_TRUE(lob.open);
    TEST_ASSERT_EQUAL_INT(SQLI_CONN_READY, connection->state);
    TEST_ASSERT_EQUAL_STRING("smart large object stream reader callback failed", sqli_error(connection));
}
static void test_protocol_failure_keeps_only_prior_acknowledgments(void)
{
    responses(1, true);
    struct reader_state state = {.limit = complete_length};
    uint64_t written = 0;
    TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR, sqli_sblob_write_stream(connection, &lob, reader, &state, &written));
    TEST_ASSERT_EQUAL_UINT64(reader_chunk, written);
    TEST_ASSERT_EQUAL_UINT(reader_chunk * 2, state.supplied);
    TEST_ASSERT_NOT_NULL(strstr(sqli_error(connection), "unexpected opcode"));
}
static void test_lost_ack_keeps_prior_progress(void)
{
    responses(1, false);
    TEST_ASSERT_EQUAL_INT(0, shutdown(sockets[1], SHUT_WR));
    struct reader_state state = {.limit = complete_length};
    uint64_t written = 0;
    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR, sqli_sblob_write_stream(connection, &lob, reader, &state, &written));
    TEST_ASSERT_EQUAL_UINT64(reader_chunk, written);
}
static void test_reader_overflow_preserves_prior_progress(void)
{
    responses(1, false);
    struct reader_state state = {.limit = reader_chunk, .overflow = true};
    uint64_t written = 0;
    TEST_ASSERT_EQUAL_INT(SQLI_ERR, sqli_sblob_write_stream(connection, &lob, reader, &state, &written));
    TEST_ASSERT_EQUAL_UINT64(reader_chunk, written);
}
static void test_eof_initial_failure_and_success(void)
{
    struct reader_state state = {0};
    uint64_t written = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_sblob_write_stream(connection, &lob, reader, &state, &written));
    TEST_ASSERT_EQUAL_UINT64(0, written);
    state.fail = true;
    written = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT(SQLI_ERR, sqli_sblob_write_stream(connection, &lob, reader, &state, &written));
    TEST_ASSERT_EQUAL_UINT64(0, written);
    responses((complete_length + reader_chunk - 1) / reader_chunk, false);
    state = (struct reader_state){.limit = complete_length};
    lob.type = SQLI_SBLOB_CLOB;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_sblob_write_stream(connection, &lob, reader, &state, &written));
    TEST_ASSERT_EQUAL_UINT64(complete_length, written);
    state = (struct reader_state){0};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_sblob_write_stream(connection, &lob, reader, &state, NULL));
}
static void test_read_requires_complete_trailer(void)
{
    const uint8_t response[] = {0, SQLI_SQ_LODATA, 0, 0, 0, 0, 0, 2,
                               0, 2, 'a', 'b'};
    TEST_ASSERT_EQUAL_INT(sizeof(response), write(sockets[1], response, sizeof(response)));
    TEST_ASSERT_EQUAL_INT(0, shutdown(sockets[1], SHUT_WR));
    char buffer[2];
    size_t count = 42;
    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR,
                         sqli_sblob_read(connection, lob.lofd, buffer, sizeof(buffer), &count));
    TEST_ASSERT_EQUAL_UINT(0, count);
}

static void test_seek_minimum_offset_encoding_and_trailer(void)
{
    const uint8_t response[] = {0, SQLI_SQ_LODATA, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(sizeof(response), write(sockets[1], response, sizeof(response)));
    TEST_ASSERT_EQUAL_INT(0, shutdown(sockets[1], SHUT_WR));
    char buffer[2];
    size_t count = 42;
    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR,
        sqli_sblob_read_seek(connection, lob.lofd, INT64_MIN, buffer, sizeof(buffer), &count));
    TEST_ASSERT_EQUAL_UINT(0, count);
    uint8_t request[26];
    TEST_ASSERT_EQUAL_INT(sizeof(request), read(sockets[1], request, sizeof(request)));
    const uint8_t minimum_offset[] = {0xff, 0xff, 0, 0, 0, 0, 0x80, 0, 0, 0};
    TEST_ASSERT_EQUAL_MEMORY(minimum_offset, request + 12, sizeof(minimum_offset));
    TEST_ASSERT_EQUAL_UINT8(1, request[23]); /* Relative seek. */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_read_requires_complete_trailer);
    RUN_TEST(test_seek_minimum_offset_encoding_and_trailer);
    RUN_TEST(test_reader_failure_retains_37_confirmed_bytes);
    RUN_TEST(test_protocol_failure_keeps_only_prior_acknowledgments);
    RUN_TEST(test_lost_ack_keeps_prior_progress);
    RUN_TEST(test_reader_overflow_preserves_prior_progress);
    RUN_TEST(test_eof_initial_failure_and_success);
    return UNITY_END();
}
