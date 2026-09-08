#ifndef SQLI_DESCRIPTOR_INTERNAL_H
#define SQLI_DESCRIPTOR_INTERNAL_H

#include "libsqli/sqli.h"
#include <stdatomic.h>

enum { SQLI_DESCRIPTOR_MAX_BYTES = 16 * 1024 * 1024 };

typedef struct {
    uint16_t statement_type;
    uint16_t statement_id;
    uint32_t cost_raw;
    uint16_t tuple_size;
    size_t field_count;
    bool extended;
} sqli_descriptor_info_t;

/* Lossless receive representation, never part of the public ABI. */
struct sqli_descriptor_field {
    bool extended;
    uint32_t field_index;
    uint32_t tuple_offset;
    uint16_t type_raw;
    uint32_t encoded_length;
    uint32_t extended_info;
    uint16_t reference;
    uint16_t alignment;
    uint32_t source_type;
    sqli_descriptor_bytes_t name;
    sqli_descriptor_bytes_t type_owner;
    sqli_descriptor_bytes_t type_name;
};

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
