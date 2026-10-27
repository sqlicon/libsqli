#include "unity.h"
#include "sqli_internal.h"
#include "sqli_protocol_internal.h"

#include <sys/socket.h>
#include <unistd.h>

enum { reader_end = 0, writer_end = 1, opcode_size = 2, integer_size = 4,
       cost_payload_size = 2 * integer_size,
       frame_size = opcode_size + cost_payload_size + opcode_size };
static sqli_conn_t *connection;
static int sockets[2];
static sqli_result_t result;

void setUp(void)
{
    connection = NULL;
    sockets[reader_end] = sockets[writer_end] = -1;
    memset(&result, 0, sizeof(result));
}

void tearDown(void)
{
    sqli_result_cleanup(&result);
    sqli_destroy(connection);
    connection = NULL;
    for (size_t i = 0; i < sizeof(sockets) / sizeof(sockets[0]); i++) {
        if (sockets[i] >= 0) {
            int status = close(sockets[i]);
            sockets[i] = -1;
            TEST_ASSERT_EQUAL_INT(0, status);
        }
    }
}

static void put_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void make_frame(uint8_t frame[frame_size], uint32_t rows, uint32_t cost)
{
    frame[0] = 0;
    frame[1] = SQLI_SQ_COST;
    put_be32(frame + opcode_size, rows);
    put_be32(frame + opcode_size + integer_size, cost);
    frame[opcode_size + cost_payload_size] = 0;
    frame[frame_size - 1] = SQLI_SQ_EOT;
}

static void receive_frame(const uint8_t *frame, size_t length, size_t buffered,
                          sqli_status expected)
{
    TEST_ASSERT_TRUE(buffered <= length);
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&connection));
    connection->socket_fd = sockets[reader_end];
    sockets[reader_end] = -1; /* Connection owns this descriptor. */
    connection->strict_protocol = true;
    TEST_ASSERT_TRUE(buffered <= connection->read_buf_cap);
    memcpy(connection->read_buf, frame, buffered);
    connection->read_buf_len = buffered;
    size_t sent = buffered;
    while (sent < length) {
        ssize_t count = write(sockets[writer_end], frame + sent, length - sent);
        TEST_ASSERT_TRUE(count > 0);
        sent += (size_t)count;
    }
    TEST_ASSERT_EQUAL_INT(0, shutdown(sockets[writer_end], SHUT_WR));
    TEST_ASSERT_EQUAL_INT(expected,
        sqli_receive_dispatch(connection->socket_fd, &result, connection));
    if (expected == SQLI_OK) {
        TEST_ASSERT_TRUE(result.eof);
        TEST_ASSERT_EQUAL_INT(0, result.row_count);
    }
    tearDown();
    setUp();
}

static void test_cost_numeric_values_are_not_opcodes(void)
{
    const uint32_t values[] = {
        0, SQLI_SQ_EOT, SQLI_SQ_DONE, SQLI_SQ_ERR, SQLI_SQ_TUPLE,
        SQLI_SQ_DESCRIBE, SQLI_SQ_INFO, SQLI_SQ_INSERTDONE,
        SQLI_SQ_XACTSTAT, SQLI_SQ_EXIT,
        (uint32_t)SQLI_SQ_DONE << 16, 1000000, UINT32_MAX
    };
    uint8_t frame[frame_size];
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
            make_frame(frame, values[i], values[j]);
            receive_frame(frame, sizeof(frame), 0, SQLI_OK);
        }
    }
}

static void test_cost_crosses_every_buffer_boundary(void)
{
    uint8_t frame[frame_size];
    make_frame(frame, SQLI_SQ_EOT, SQLI_SQ_DONE);
    for (size_t split = 1; split <= sizeof(frame); split++)
        receive_frame(frame, sizeof(frame), split, SQLI_OK);
}

static void test_cost_rejects_every_truncated_payload(void)
{
    uint8_t frame[frame_size];
    make_frame(frame, SQLI_SQ_DONE, SQLI_SQ_EOT);
    for (size_t payload = 0; payload < cost_payload_size; payload++)
        receive_frame(frame, opcode_size + payload, 0, SQLI_IO_ERROR);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cost_numeric_values_are_not_opcodes);
    RUN_TEST(test_cost_crosses_every_buffer_boundary);
    RUN_TEST(test_cost_rejects_every_truncated_payload);
    return UNITY_END();
}
