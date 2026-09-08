#ifndef SQLI_SCALAR_EXPECTATIONS_H
#define SQLI_SCALAR_EXPECTATIONS_H
#include "libsqli/sqli.h"
static inline bool test_integer_equals(sqli_result_t *result, size_t column, int32_t expected)
{
    int32_t actual;
    bool is_null;
    return sqli_result_get_int(result, column, &actual, &is_null) == SQLI_OK &&
        !is_null && actual == expected;
}
#endif
