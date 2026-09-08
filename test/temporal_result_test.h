#ifndef TEMPORAL_RESULT_TEST_H
#define TEMPORAL_RESULT_TEST_H

#include "libsqli/sqli_temporal.h"

/* Tests inspect semantic copies while exercising the public opaque API. */
static inline sqli_status test_datetime_parts(sqli_result_t *result, int column,
                                             sqli_datetime_parts_t *out)
{
    sqli_datetime_t *value = NULL;
    sqli_status status = sqli_datetime_create(&value);
    if (status == SQLI_OK)
        status = sqli_result_get_datetime(result, (size_t)column, value);
    if (status == SQLI_OK)
        status = sqli_datetime_get_parts(value, out);
    sqli_datetime_destroy(value);
    return status;
}

static inline sqli_status test_interval_parts(sqli_result_t *result, int column,
                                             sqli_interval_parts_t *out)
{
    sqli_interval_t *value = NULL;
    sqli_status status = sqli_interval_create(&value);
    if (status == SQLI_OK)
        status = sqli_result_get_interval(result, (size_t)column, value);
    if (status == SQLI_OK)
        status = sqli_interval_get_parts(value, out);
    sqli_interval_destroy(value);
    return status;
}

static inline uint32_t test_fraction(uint32_t nanosecond, uint8_t scale)
{
    for (unsigned i = scale; i < 9; i++)
        nanosecond /= 10;
    return nanosecond;
}

#endif
