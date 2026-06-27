/*
 * Phase 1 unit tests for sqli infrastructure.
 *
 * Tests: connection lifecycle, error handling, padding logic.
 * All tests use public API only — no internal struct access.
 */

#include "libsqli/sqli.h"
#include "sqli_internal.h"

#include "unity.h"

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

/* ----------------------------------------------------------------
 * test_sqli_create_destroy
 * ---------------------------------------------------------------- */

void test_sqli_create_destroy(void)
{
    sqli_conn_t *conn = NULL;

    /* Create succeeds and returns valid handle */
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_NOT_NULL(conn);

    /* No error reported after creation */
    TEST_ASSERT_NULL(sqli_error(conn));

    /* Destroy does not crash */
    sqli_destroy(conn);
    conn = NULL;
}

void test_sqli_create_null_out_ptr(void)
{
    /* NULL output pointer must not crash, should return error */
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_create(NULL));
}

void test_sqli_destroy_null(void)
{
    /* Destroy with NULL must be a no-op, never crash */
    sqli_destroy(NULL);
}

void test_sqli_create_multiple(void)
{
    /* Multiple creates produce independent objects */
    sqli_conn_t *a = NULL;
    sqli_conn_t *b = NULL;

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&a));
    TEST_ASSERT_NOT_NULL(a);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&b));
    TEST_ASSERT_NOT_NULL(b);

    sqli_destroy(a);
    sqli_destroy(b);
}

void test_sqli_destroy_closes_socket(void)
{
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        TEST_IGNORE_MESSAGE("socketpair unavailable");

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_NOT_NULL(conn);

    conn->socket_fd = sv[0];
    conn->state = SQLI_CONN_READY;
    conn->database_open = false;

    sqli_destroy(conn);

    TEST_ASSERT_EQUAL_INT(-1, fcntl(sv[0], F_GETFD));
    close(sv[1]);
}

/* ----------------------------------------------------------------
 * test_sqli_error_null_safe
 * ---------------------------------------------------------------- */

void test_sqli_error_null_safe(void)
{
    /* Error functions with NULL conn must not crash */
    TEST_ASSERT_NULL(sqli_error(NULL));
    TEST_ASSERT_EQUAL_INT(-1, sqli_errno(NULL));
}

void test_sqli_errno_no_error(void)
{
    sqli_conn_t *conn = NULL;

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_EQUAL_INT(0, sqli_errno(conn));

    sqli_destroy(conn);
}

void test_sqli_error_info_no_error(void)
{
    sqli_conn_t *conn = NULL;
    sqli_error_info info;

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_error_get_info(conn, &info));
    TEST_ASSERT_EQUAL_INT(0, info.has_error);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, info.status);
    TEST_ASSERT_EQUAL_INT(0, info.sqlcode);
    TEST_ASSERT_EQUAL_INT(0, info.isamcode);
    TEST_ASSERT_EQUAL_STRING("", info.message);

    sqli_destroy(conn);
}

void test_sqli_strict_protocol_toggle(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_EQUAL_INT(0, sqli_get_strict_protocol(conn));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_set_strict_protocol(conn, 1));
    TEST_ASSERT_EQUAL_INT(1, sqli_get_strict_protocol(conn));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_set_strict_protocol(conn, 0));
    TEST_ASSERT_EQUAL_INT(0, sqli_get_strict_protocol(conn));
    sqli_destroy(conn);
}

void test_sqli_trim_trailing_spaces_toggle(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_EQUAL_INT(1, sqli_get_trim_trailing_spaces(conn));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_set_trim_trailing_spaces(conn, 0));
    TEST_ASSERT_EQUAL_INT(0, sqli_get_trim_trailing_spaces(conn));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_set_trim_trailing_spaces(conn, 1));
    TEST_ASSERT_EQUAL_INT(1, sqli_get_trim_trailing_spaces(conn));
    sqli_destroy(conn);
}

void test_sqli_query_with_retry_null_params(void)
{
    sqli_result_t *result = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE,
                          sqli_query_with_retry(NULL, "SELECT 1", 1, &result));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE,
                          sqli_query_with_retry(NULL, NULL, 1, NULL));
}

/* ----------------------------------------------------------------
 * test_sqli_pad_even
 * ---------------------------------------------------------------- */

void test_sqli_pad_even_zero(void)
{
    TEST_ASSERT_EQUAL_UINT(0, sqli_pad_even(0));
}

void test_sqli_pad_even_even(void)
{
    TEST_ASSERT_EQUAL_UINT(4, sqli_pad_even(4));
    TEST_ASSERT_EQUAL_UINT(8, sqli_pad_even(8));
}

void test_sqli_pad_even_odd(void)
{
    TEST_ASSERT_EQUAL_UINT(2, sqli_pad_even(1));
    TEST_ASSERT_EQUAL_UINT(4, sqli_pad_even(3));
    TEST_ASSERT_EQUAL_UINT(6, sqli_pad_even(5));
}

void test_sqli_pad_even_large(void)
{
    TEST_ASSERT_EQUAL_UINT(1024, sqli_pad_even(1024));
    TEST_ASSERT_EQUAL_UINT(1026, sqli_pad_even(1025));
}
