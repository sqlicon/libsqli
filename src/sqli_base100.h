#ifndef SQLI_BASE100_H
#define SQLI_BASE100_H

#include <stddef.h>
#include <stdint.h>

/* Internal radix complement over validated groups in 0..99. The caller owns
 * the writable span; trailing zero groups remain padding. No shared state. */
static inline void sqli_base100_radix_complement(uint8_t *groups, size_t count)
{
    enum { radix = 100 };
    unsigned carry = 1;
    while (count != 0) {
        unsigned digit = radix - 1u - groups[--count] + carry;
        groups[count] = (uint8_t)(digit % radix);
        carry = digit / radix;
    }
}

#endif
