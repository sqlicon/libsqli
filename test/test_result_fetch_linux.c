#include "libsqli/sqli.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "allocation_test.h"
#include "sqli_internal.h"
#include "sqli_result_internal.h"

enum { buffered_rows = 2, tuple_capacity = 32, native_column = 0, following_column = 1, column_count = 2 };
static sqli_result_t *result;
static sqli_conn_t *connection;
static int sockets[2];

void setUp(void)
{
    sqli_test_allow_allocations();
    sockets[0] = sockets[1] = -1;
}
void tearDown(void)
{
    sqli_test_allow_allocations();
    if (result != NULL) {
        result->owner_conn = NULL;
        sqli_result_destroy(result);
        result = NULL;
    }
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
static void make_result(sqli_column_type type, uint32_t descriptor,
                        const uint8_t *tuple, size_t length)
{
    TEST_ASSERT_NULL(result);
    TEST_ASSERT_TRUE(length <= tuple_capacity);
    result = calloc(1, sizeof(*result));
    TEST_ASSERT_NOT_NULL(result);
    result->columns = calloc(column_count, sizeof(*result->columns));
    result->rows = calloc(buffered_rows, sizeof(*result->rows));
    result->row_lens = calloc(buffered_rows, sizeof(*result->row_lens));
    TEST_ASSERT_NOT_NULL(result->columns);
    TEST_ASSERT_NOT_NULL(result->rows);
    TEST_ASSERT_NOT_NULL(result->row_lens);
    result->row_count = 1;
    result->row_capacity = buffered_rows;
    result->rows[0] = malloc(length != 0 ? length : 1);
    TEST_ASSERT_NOT_NULL(result->rows[0]);
    memcpy(result->rows[0], tuple, length);
    result->row_lens[0] = length;
    result->column_count = column_count;
    result->columns[native_column].type = type;
    result->columns[native_column].encoded_length = descriptor;
    result->columns[following_column].type = SQLI_TYPE_INT;
    result->columns[following_column].encoded_length = 4;
    result->cursor = result->current_row = result->cur_cache_row = -1;
    result->at_before_first = true;
    result->saw_done = true;
    result->eof = 1;
}
static void expect_failure(sqli_status expected)
{
    TEST_ASSERT_EQUAL_INT(expected, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(-1, result->current_row);
    TEST_ASSERT_NULL(result->tuple_buffer);
    TEST_ASSERT_EQUAL_INT(-1, result->cur_cache_row);
    TEST_ASSERT_EQUAL_INT(expected, sqli_result_fetch(result));
    TEST_ASSERT_FALSE(sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(expected, sqli_result_fetch(result));
}
static void test_native_layouts_and_every_short_prefix(void)
{
    static const struct {
        sqli_column_type type;
        uint32_t descriptor;
        size_t width;
    } cases[] = {
        {SQLI_TYPE_DECIMAL, 0x0402, 3}, {SQLI_TYPE_MONEY, 0x0402, 3},
        {SQLI_TYPE_DATE, 4, 4}, {SQLI_TYPE_DATETIME, 0x0e0a, 8},
        {SQLI_TYPE_INTERVAL, 0x05ac, 4}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t tuple[tuple_capacity] = {0}; /* Typed NULL except DATE epoch. */
        size_t length = cases[i].width + 4;
        tuple[length - 1] = 42;
        for (size_t cut = 0; cut < length; cut++) {
            make_result(cases[i].type, cases[i].descriptor, tuple, cut);
            expect_failure(SQLI_PROTO_ERROR);
            tearDown();
        }
        make_result(cases[i].type, cases[i].descriptor, tuple, length);
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
        TEST_ASSERT_EQUAL_INT(42, sqli_result_get_int(result, following_column));
        TEST_ASSERT_EQUAL_INT(SQLI_EOF, sqli_result_fetch(result));
        TEST_ASSERT_EQUAL_INT(SQLI_EOF, sqli_result_fetch(result));
        TEST_ASSERT_NULL(result->tuple_buffer);
        tearDown();
    }
}
static void test_later_row_failure_invalidates_cached_values(void)
{
    const uint8_t tuple[] = {1, 'x', 0, 0, 0, 42};
    make_result(SQLI_TYPE_VARCHAR, 8, tuple, sizeof(tuple));
    result->rows[1] = malloc(sizeof(tuple) - 1);
    TEST_ASSERT_NOT_NULL(result->rows[1]);
    memcpy(result->rows[1], tuple, sizeof(tuple) - 1);
    result->row_lens[1] = sizeof(tuple) - 1;
    result->row_count = buffered_rows;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(42, sqli_result_get_int(result, following_column));
    expect_failure(SQLI_PROTO_ERROR);
    TEST_ASSERT_EQUAL_INT(0, sqli_result_row_number(result));
}

static void test_row_number_overflow_is_explicit(void)
{
    const uint8_t tuple[] = {0, 0, 0, 0, 0, 0, 0, 42};
    make_result(SQLI_TYPE_INT, 4, tuple, sizeof(tuple));
    result->rows[1] = malloc(sizeof(tuple));
    TEST_ASSERT_NOT_NULL(result->rows[1]);
    memcpy(result->rows[1], tuple, sizeof(tuple));
    result->row_lens[1] = sizeof(tuple);
    result->row_count = buffered_rows;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    result->absolute_row_num = INT32_MAX;
    expect_failure(SQLI_OUT_OF_RANGE);
}

static void test_invalid_descriptors_are_not_prefixes(void)
{
    static const struct { sqli_column_type type; uint32_t descriptor; } cases[] = {
        {SQLI_TYPE_DECIMAL, 0}, {SQLI_TYPE_DECIMAL, 0x0405},
        {SQLI_TYPE_MONEY, 0x1000402}, {SQLI_TYPE_DATETIME, 0x0e0b},
        {SQLI_TYPE_INTERVAL, 0}, {SQLI_TYPE_INTERVAL, 0x10005ac}
    };
    const uint8_t tuple[tuple_capacity] = {0};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        make_result(cases[i].type, cases[i].descriptor, tuple, sizeof(tuple));
        size_t start = 7, length = 8, span = 9;
        TEST_ASSERT_EQUAL_INT(SQLI_PROTO_ERROR,
            sqli_tuple_locate_column(&result->columns[0], tuple, sizeof(tuple), &start, &length, &span));
        TEST_ASSERT_EQUAL_UINT(7, start);
        TEST_ASSERT_EQUAL_UINT(8, length);
        TEST_ASSERT_EQUAL_UINT(9, span);
        expect_failure(SQLI_PROTO_ERROR);
        tearDown();
    }
}
static void test_allocation_failure_and_reset(void)
{
    const uint8_t tuple[] = {0, 0, 0, 0, 0, 0, 0, 42};
    make_result(SQLI_TYPE_INT, 4, tuple, sizeof(tuple));
    sqli_test_fail_next_allocation();
    expect_failure(SQLI_ALLOC_FAIL);
    TEST_ASSERT_NULL(result->cur_col_data_start);
    TEST_ASSERT_NULL(result->cur_col_data_len);
    TEST_ASSERT_NULL(result->cur_col_is_null);
    sqli_result_clear_rows(result);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, result->fetch_status);
    TEST_ASSERT_EQUAL_INT(SQLI_EOF, sqli_result_fetch(result));
}
static void test_invalidated_cursor_and_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_ARGUMENT, sqli_result_fetch(NULL));
    const uint8_t tuple[] = {0, 0, 0, 0, 0, 0, 0, 42};
    make_result(SQLI_TYPE_INT, 4, tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&connection));
    result->owner_conn = connection;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    connection->rollback_epoch++;
    expect_failure(SQLI_INVALID_STATE);
    TEST_ASSERT_FALSE(connection->error_info.has_error);
}
static void prepare_refetch(const uint8_t *response, size_t length)
{
    const uint8_t tuple[] = {0, 0, 0, 0, 0, 0, 0, 42};
    make_result(SQLI_TYPE_INT, 4, tuple, sizeof(tuple));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_fetch(result));
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&connection));
    connection->socket_fd = sockets[0];
    connection->state = SQLI_CONN_READY;
    connection->strict_protocol = true;
    result->owner_conn = connection;
    result->cursor_type = SQLI_CURSOR_SCROLL_INSENSITIVE;
    result->statement_type = 2;
    result->stmt_id = 42;
    if (length != 0) {
        ssize_t written = write(sockets[1], response, length);
        TEST_ASSERT_TRUE(written >= 0);
        TEST_ASSERT_EQUAL_UINT(length, (size_t)written);
    }
    TEST_ASSERT_EQUAL_INT(0, shutdown(sockets[1], SHUT_WR));
}
static void test_refetch_transport_and_protocol_failures(void)
{
    prepare_refetch(NULL, 0);
    expect_failure(SQLI_IO_ERROR);
    tearDown();
    const uint8_t malformed_opcode[] = {0xff, 0xff};
    prepare_refetch(malformed_opcode, sizeof(malformed_opcode));
    expect_failure(SQLI_PROTO_ERROR);
}
static void test_refetch_server_error_preserves_diagnostics(void)
{
    /* SQ_ERR: SQLCODE -201, ISAMCODE -111, short offset, message and EOT. */
    const uint8_t response[] = {
        0, SQLI_SQ_ERR, 0xff, 0x37, 0xff, 0x91, 0, 0,
        0, 4, 't', 'e', 's', 't', 0, SQLI_SQ_EOT
    };
    prepare_refetch(response, sizeof(response));
    expect_failure(SQLI_PROTO_ERROR); /* Existing server-error status contract. */
    TEST_ASSERT_EQUAL_INT(-201, connection->error_info.sqlcode);
    TEST_ASSERT_EQUAL_INT(-111, connection->error_info.isamcode);
    TEST_ASSERT_EQUAL_STRING("test", connection->error_info.server_message);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_native_layouts_and_every_short_prefix);
    RUN_TEST(test_invalid_descriptors_are_not_prefixes);
    RUN_TEST(test_later_row_failure_invalidates_cached_values);
    RUN_TEST(test_row_number_overflow_is_explicit);
    RUN_TEST(test_allocation_failure_and_reset);
    RUN_TEST(test_invalidated_cursor_and_arguments);
    RUN_TEST(test_refetch_transport_and_protocol_failures);
    RUN_TEST(test_refetch_server_error_preserves_diagnostics);
    return UNITY_END();
}
