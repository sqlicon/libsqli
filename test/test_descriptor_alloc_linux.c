#include "libsqli/sqli.h"
#include "unity.h"

#include <stdlib.h>

#include "allocation_test.h"
#include "sqli_descriptor_internal.h"
#include "sqli_internal.h"

static sqli_descriptor_t *descriptor;
static sqli_descriptor_t *out;
static uint8_t *bytes;

void setUp(void)
{
    sqli_test_allow_allocations();
    const sqli_descriptor_info_t info = {.field_count = 1};
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_create(&info, &descriptor));
}
void tearDown(void)
{
    sqli_test_allow_allocations();
    sqli_descriptor_release(descriptor);
    sqli_descriptor_release(out);
    free(bytes);
    descriptor = out = NULL;
    bytes = NULL;
}
static void test_allocation_failures_preserve_outputs(void)
{
    const sqli_descriptor_info_t info = {.field_count = 1};
    sqli_descriptor_t *unchanged = descriptor;
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_descriptor_create(&info, &unchanged));
    TEST_ASSERT_EQUAL_PTR(descriptor, unchanged);
    size_t retained = descriptor->retained_bytes;
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_descriptor_alloc_bytes(descriptor, 1, &bytes));
    TEST_ASSERT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT(retained, descriptor->retained_bytes);
    TEST_ASSERT_EQUAL_INT(SQLI_LIMIT_EXCEEDED,
        sqli_descriptor_alloc_bytes(descriptor, SQLI_DESCRIPTOR_MAX_BYTES, &bytes));
    TEST_ASSERT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT(retained, descriptor->retained_bytes);
}
static void test_acquisition_and_views_do_not_allocate(void)
{
    sqli_result_t result = {.descriptor = descriptor}; /* Borrows this test's reference. */
    sqli_test_fail_next_allocation();
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_result_get_descriptor(&result, &out));
    sqli_descriptor_info_t info = {.field_count = 1};
    size_t count;
    const sqli_descriptor_field_t *field;
    sqli_descriptor_bytes_t names;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_get_field_count(out, &count));
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_get_field(out, 0, &field));
    TEST_ASSERT_EQUAL_INT(SQLI_METADATA_UNAVAILABLE, sqli_descriptor_field_get_name(field, &names));
    sqli_column_type type;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_descriptor_field_get_type(field, &type));
    sqli_descriptor_t *unchanged = descriptor;
    TEST_ASSERT_EQUAL_INT(SQLI_ALLOC_FAIL, sqli_descriptor_create(&info, &unchanged));
    TEST_ASSERT_EQUAL_PTR(descriptor, unchanged);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_allocation_failures_preserve_outputs);
    RUN_TEST(test_acquisition_and_views_do_not_allocate);
    return UNITY_END();
}
