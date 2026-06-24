#include "unity.h"
#include "libsqli/sqli.h"
#include "sqli_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void test_stability_decimal_codec_loop(void)
{
    uint8_t digits[9] = {1,2,3,4,5,6,7,8,9};
    uint8_t buf[32];
    uint8_t out_digits[16];
    uint8_t scale = 0;
    int negative = 0;

    for (int i = 0; i < 10000; i++) {
        int neg = (i % 2);
        size_t n = sqli_encode_decimal(buf, sizeof(buf), 9, 4, neg, digits);
        TEST_ASSERT_GREATER_THAN(0, n);
        memset(out_digits, 0, sizeof(out_digits));
        TEST_ASSERT_EQUAL_INT(9, sqli_decode_decimal(buf, n, 9, &scale, &negative, out_digits));
        TEST_ASSERT_EQUAL_INT(4, (int)scale);
        TEST_ASSERT_EQUAL_INT(neg, negative);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(digits, out_digits, 9);
    }
}

void test_stability_bind_api_reuse(void)
{
    sqli_stmt_t stmt;
    memset(&stmt, 0, sizeof(stmt));
    stmt.socket_fd = -1;
    stmt.param_count = 1;
    stmt.params = calloc(1, sizeof(*stmt.params));
    TEST_ASSERT_NOT_NULL(stmt.params);

    uint8_t payload[64];
    for (int i = 0; i < (int)sizeof(payload); i++)
        payload[i] = (uint8_t)i;

    char txt[64];
    for (int i = 0; i < 2000; i++) {
        snprintf(txt, sizeof(txt), "v%d", i);
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_string(&stmt, 1, txt));
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_decimal(&stmt, 1, "12345.6789"));
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_date(&stmt, 1, "2026-06-20"));
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_datetime(&stmt, 1, "2026-06-20 12:34:56"));
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_interval(&stmt, 1, "1 02:03:04"));
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_bool(&stmt, 1, i & 1));
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_bytes(&stmt, 1, payload, sizeof(payload)));
        TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_bind_null(&stmt, 1));
    }

    sqli_stmt_close(&stmt);
}
