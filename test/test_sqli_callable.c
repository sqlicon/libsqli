#include "unity.h"
#include "libsqli/sqli.h"
#include "sqli_internal.h"

#include <string.h>

void test_call_prepare_null_params(void)
{
    sqli_call_t *call = NULL;
    int nparams = 0;
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.state = SQLI_CONN_READY;

    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_call_prepare(NULL, "call p(?)", &nparams, &call));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_call_prepare(&conn, NULL, &nparams, &call));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_call_prepare(&conn, "call p(?)", &nparams, NULL));
}

void test_call_prepare_not_ready_conn(void)
{
    sqli_call_t *call = NULL;
    int nparams = 0;
    sqli_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.state = SQLI_CONN_CLOSED;
    conn.socket_fd = -1;

    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE,
                          sqli_call_prepare(&conn, "call p(?)", &nparams, &call));
    TEST_ASSERT_NULL(call);
}

void test_call_api_null_safe(void)
{
    int64_t i64 = 0;
    double d = 0.0;
    const char *s = NULL;
    bool is_null = false;

    TEST_ASSERT_NULL(sqli_call_stmt(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE,
                          sqli_call_set_param_mode(NULL, 1, SQLI_CALL_PARAM_IN));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_call_execute(NULL));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_call_get_int64(NULL, 1, &i64, &is_null));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_call_get_double(NULL, 1, &d, &is_null));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_call_get_string(NULL, 1, &s, &is_null));

    sqli_call_destroy(NULL);
}
