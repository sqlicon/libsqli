#include "decimal_alloc_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

/* Linux linker --wrap hooks. Production code has no allocator test hooks.
 * Atomic access also avoids a data race if a test runtime allocates on a thread;
 * fault-injection tests themselves must still run serially in this process. */
static atomic_bool fail_next;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);

void sqli_test_fail_next_allocation(void)
{
    atomic_store(&fail_next, true);
}

void sqli_test_allow_allocations(void)
{
    atomic_store(&fail_next, false);
}

void *__wrap_malloc(size_t size)
{
    if (atomic_exchange(&fail_next, false))
        return NULL;
    return __real_malloc(size);
}

/* Optimized builds may fold malloc followed by zeroing into calloc. */
void *__wrap_calloc(size_t count, size_t size)
{
    if (atomic_exchange(&fail_next, false))
        return NULL;
    return __real_calloc(count, size);
}
