#ifndef SQLI_DECIMAL_ALLOC_TEST_H
#define SQLI_DECIMAL_ALLOC_TEST_H

/* Allocation fault injection for a serial test scope. Reset before leaving a
 * test, including assertion failure paths. These functions do not allocate. */
void sqli_test_fail_next_allocation(void);
void sqli_test_allow_allocations(void);

#endif
