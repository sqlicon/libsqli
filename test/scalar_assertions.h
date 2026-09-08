#ifndef SQLI_SCALAR_ASSERTIONS_H
#define SQLI_SCALAR_ASSERTIONS_H
#include "libsqli/sqli.h"
#include "unity.h"

/* Test-only successful reads. NULL leaves the zero-initialized value untouched;
 * tests of NULL/error contracts use the public status functions directly. */
static inline int32_t test_read_int(sqli_result_t *result, size_t column)
{
    int32_t value = 0;
    bool is_null;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_int(result, column, &value, &is_null));
    return value;
}
static inline int64_t test_read_int64(sqli_result_t *result, size_t column)
{
    int64_t value = 0;
    bool is_null;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_int64(result, column, &value, &is_null));
    return value;
}
static inline double test_read_double(sqli_result_t *result, size_t column)
{
    double value = 0;
    bool is_null;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_double(result, column, &value, &is_null));
    return value;
}
static inline bool test_read_bool(sqli_result_t *result, size_t column)
{
    bool value = false;
    bool is_null;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_bool(result, column, &value, &is_null));
    return value;
}
#endif
