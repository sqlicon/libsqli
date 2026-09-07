#include "sqli_descriptor_internal.h"

#include <stdlib.h>
#include <string.h>

#include "sqli_internal.h"

sqli_status sqli_descriptor_create(const sqli_descriptor_info_t *info, sqli_descriptor_t **out)
{
    if (info == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (info->field_count > UINT16_MAX ||
        info->field_count > (SQLI_DESCRIPTOR_MAX_BYTES - sizeof(sqli_descriptor_t)) / sizeof(sqli_descriptor_field_t))
        return SQLI_LIMIT_EXCEEDED;
    sqli_descriptor_t *descriptor = calloc(1, sizeof(*descriptor));
    if (descriptor == NULL)
        return SQLI_ALLOC_FAIL;
    atomic_init(&descriptor->references, 1);
    descriptor->info = *info;
    if (info->field_count != 0) {
        descriptor->fields = calloc(info->field_count, sizeof(*descriptor->fields));
        if (descriptor->fields == NULL) {
            free(descriptor);
            return SQLI_ALLOC_FAIL;
        }
    }
    descriptor->retained_bytes = sizeof(*descriptor) + info->field_count * sizeof(*descriptor->fields);
    *out = descriptor;
    return SQLI_OK;
}

sqli_status sqli_descriptor_alloc_bytes(sqli_descriptor_t *descriptor, size_t length, uint8_t **out)
{
    if (descriptor == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (length > SQLI_DESCRIPTOR_MAX_BYTES - descriptor->retained_bytes)
        return SQLI_LIMIT_EXCEEDED;
    uint8_t *bytes = length != 0 ? malloc(length) : NULL;
    if (length != 0 && bytes == NULL)
        return SQLI_ALLOC_FAIL;
    descriptor->retained_bytes += length;
    *out = bytes;
    return SQLI_OK;
}

sqli_status sqli_descriptor_assign_names(sqli_descriptor_t *descriptor)
{
    if (descriptor == NULL)
        return SQLI_INVALID_ARGUMENT;
    size_t offset = 0;
    for (size_t i = 0; i < descriptor->info.field_count && offset < descriptor->names_length; i++) {
        const uint8_t *start = descriptor->names + offset;
        const uint8_t *end = memchr(start, 0, descriptor->names_length - offset);
        if (end == NULL)
            return SQLI_PROTO_ERROR;
        size_t length = (size_t)(end - start);
        descriptor->fields[i].name = (sqli_descriptor_bytes_t){start, length, true};
        offset += length + 1;
    }
    return SQLI_OK;
}

sqli_status sqli_descriptor_retain(sqli_descriptor_t *descriptor)
{
    if (descriptor == NULL)
        return SQLI_INVALID_ARGUMENT;
    size_t references = atomic_load_explicit(&descriptor->references, memory_order_relaxed);
    do {
        if (references == SIZE_MAX)
            return SQLI_LIMIT_EXCEEDED;
    } while (!atomic_compare_exchange_weak_explicit(&descriptor->references, &references,
                references + 1, memory_order_relaxed, memory_order_relaxed));
    return SQLI_OK;
}

void sqli_descriptor_release(sqli_descriptor_t *descriptor)
{
    if (descriptor == NULL)
        return;
    if (atomic_fetch_sub_explicit(&descriptor->references, 1, memory_order_acq_rel) != 1)
        return;
    for (size_t i = 0; i < descriptor->info.field_count; i++) {
        /* The builder owns these allocations; public views only borrow them. */
        free((void *)descriptor->fields[i].type_owner.data);
        free((void *)descriptor->fields[i].type_name.data);
    }
    free(descriptor->fields);
    free(descriptor->names);
    free(descriptor);
}

sqli_status sqli_result_get_descriptor(const sqli_result_t *result, sqli_descriptor_t **out)
{
    if (result == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (result->descriptor == NULL)
        return SQLI_METADATA_UNAVAILABLE;
    sqli_status status = sqli_descriptor_retain(result->descriptor);
    if (status == SQLI_OK)
        *out = result->descriptor;
    return status;
}

sqli_status sqli_stmt_get_descriptor(const sqli_stmt_t *stmt, sqli_descriptor_t **out)
{
    if (stmt == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    return sqli_result_get_descriptor(&stmt->result, out);
}

sqli_status sqli_descriptor_get_info(const sqli_descriptor_t *descriptor, sqli_descriptor_info_t *out)
{
    if (descriptor == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    *out = descriptor->info;
    return SQLI_OK;
}

sqli_status sqli_descriptor_get_field(const sqli_descriptor_t *descriptor, size_t index,
                                      sqli_descriptor_field_t *out)
{
    if (descriptor == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (index >= descriptor->info.field_count)
        return SQLI_OUT_OF_RANGE;
    *out = descriptor->fields[index];
    return SQLI_OK;
}

sqli_status sqli_descriptor_get_names(const sqli_descriptor_t *descriptor, sqli_descriptor_bytes_t *out)
{
    if (descriptor == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    *out = (sqli_descriptor_bytes_t){descriptor->names, descriptor->names_length, true};
    return SQLI_OK;
}
