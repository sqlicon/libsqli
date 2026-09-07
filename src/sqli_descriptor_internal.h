#ifndef SQLI_DESCRIPTOR_INTERNAL_H
#define SQLI_DESCRIPTOR_INTERNAL_H

#include "libsqli/sqli.h"
#include <stdatomic.h>

/* Mutable only while the receive path builds it; publish after full validation. */
struct sqli_descriptor {
    atomic_size_t references;
    sqli_descriptor_info_t info;
    sqli_descriptor_field_t *fields;
    uint8_t *names;
    size_t names_length;
    size_t retained_bytes;
};
sqli_status sqli_descriptor_create(const sqli_descriptor_info_t *info, sqli_descriptor_t **out);
sqli_status sqli_descriptor_alloc_bytes(sqli_descriptor_t *descriptor, size_t length, uint8_t **out);
sqli_status sqli_descriptor_assign_names(sqli_descriptor_t *descriptor);

#endif
