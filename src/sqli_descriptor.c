#include "libsqli/sqli_temporal.h"
#include "libsqli/sqli_decimal.h"
#include "sqli_descriptor_internal.h"

#include <stdlib.h>
#include <string.h>

#include "sqli_internal.h"
#include "sqli_decimal_codec.h"
#include "sqli_temporal_codec.h"

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
    for (size_t i = 0; i < info->field_count; i++)
        descriptor->fields[i].extended = info->extended;
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

sqli_status sqli_descriptor_get_field_count(const sqli_descriptor_t *descriptor, size_t *out)
{
    if (descriptor == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    *out = descriptor->info.field_count;
    return SQLI_OK;
}

sqli_status sqli_descriptor_get_field(const sqli_descriptor_t *descriptor, size_t index,
                                      const sqli_descriptor_field_t **out)
{
    if (descriptor == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (index >= descriptor->info.field_count)
        return SQLI_OUT_OF_RANGE;
    *out = &descriptor->fields[index];
    return SQLI_OK;
}

static sqli_status get_bytes(const sqli_descriptor_bytes_t *bytes, sqli_descriptor_bytes_t *out)
{
    if (bytes == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (!bytes->available)
        return SQLI_METADATA_UNAVAILABLE;
    *out = *bytes;
    return SQLI_OK;
}

sqli_status sqli_descriptor_field_get_name(const sqli_descriptor_field_t *field,
                                           sqli_descriptor_bytes_t *out)
{
    return get_bytes(field != NULL ? &field->name : NULL, out);
}

sqli_status sqli_descriptor_field_get_type_owner(const sqli_descriptor_field_t *field,
                                                 sqli_descriptor_bytes_t *out)
{
    return get_bytes(field != NULL ? &field->type_owner : NULL, out);
}

sqli_status sqli_descriptor_field_get_type_name(const sqli_descriptor_field_t *field,
                                                sqli_descriptor_bytes_t *out)
{
    return get_bytes(field != NULL ? &field->type_name : NULL, out);
}

sqli_status sqli_descriptor_field_get_type(const sqli_descriptor_field_t *field,
                                           sqli_column_type *out)
{
    enum { extended_blob = 10, extended_clob = 11, extended_lvarchar = 1, extended_bool = 5 };
    if (field == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    /* Never report an unknown or distinct type as its apparent base type. */
    if ((field->type_raw & ~(0xffu | SQLI_BIT_NOTNULLABLE)) != 0)
        return SQLI_METADATA_UNAVAILABLE;
    sqli_column_type type;
    if (field->extended && field->extended_info != 0) {
        switch (field->extended_info) {
        case extended_blob: type = SQLI_TYPE_BLOB; break;
        case extended_clob: type = SQLI_TYPE_CLOB; break;
        case extended_lvarchar: type = SQLI_TYPE_LVARCHAR; break;
        case extended_bool: type = SQLI_TYPE_BOOL; break;
        default: return SQLI_METADATA_UNAVAILABLE;
        }
    } else {
        switch (field->type_raw & 0xff) {
        case SQLI_TYPE_CHAR: case SQLI_TYPE_SMALLINT: case SQLI_TYPE_INT:
        case SQLI_TYPE_FLOAT: case SQLI_TYPE_SMFLOAT: case SQLI_TYPE_DECIMAL:
        case SQLI_TYPE_SERIAL: case SQLI_TYPE_DATE: case SQLI_TYPE_MONEY:
        case SQLI_TYPE_NULL: case SQLI_TYPE_DATETIME: case SQLI_TYPE_BYTE:
        case SQLI_TYPE_TEXT: case SQLI_TYPE_VARCHAR: case SQLI_TYPE_INTERVAL:
        case SQLI_TYPE_NCHAR: case SQLI_TYPE_NVCHAR: case SQLI_TYPE_INT8:
        case SQLI_TYPE_SERIAL8: case SQLI_TYPE_BIGINT: case SQLI_TYPE_BIGSERIAL:
        case SQLI_TYPE_LVARCHAR: case SQLI_TYPE_BOOL: case SQLI_TYPE_DBOOLEAN:
        case SQLI_TYPE_CLOB: case SQLI_TYPE_BLOB:
            type = (sqli_column_type)(field->type_raw & 0xff);
            break;
        default: return SQLI_METADATA_UNAVAILABLE;
        }
    }
    *out = type;
    return SQLI_OK;
}

static sqli_status decimal_qualifier(const sqli_descriptor_field_t *field)
{
    sqli_column_type type;
    sqli_status status = sqli_descriptor_field_get_type(field, &type);
    if (status != SQLI_OK)
        return status;
    if (type != SQLI_TYPE_DECIMAL && type != SQLI_TYPE_MONEY)
        return SQLI_METADATA_UNAVAILABLE;
    if (field->encoded_length > UINT16_MAX)
        return SQLI_PROTO_ERROR;
    size_t width;
    return sqli_decimal_wire_size((uint16_t)field->encoded_length, &width);
}

sqli_status sqli_descriptor_field_get_precision(const sqli_descriptor_field_t *field, uint8_t *out)
{
    if (out == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_status status = decimal_qualifier(field);
    if (status == SQLI_OK)
        *out = (uint8_t)(field->encoded_length >> 8);
    return status;
}

sqli_status sqli_descriptor_field_get_scale(const sqli_descriptor_field_t *field, uint8_t *out)
{
    if (out == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_status status = decimal_qualifier(field);
    if (status != SQLI_OK)
        return status;
    if ((field->encoded_length & 0xff) == 0xff)
        return SQLI_METADATA_UNAVAILABLE;
    *out = (uint8_t)(field->encoded_length & 0xff);
    return SQLI_OK;
}

sqli_status sqli_descriptor_field_get_temporal_range(const sqli_descriptor_field_t *field,
                                                     sqli_temporal_range_t *out)
{
    if (out == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_column_type type;
    sqli_status status = sqli_descriptor_field_get_type(field, &type);
    if (status != SQLI_OK)
        return status;
    if (type != SQLI_TYPE_DATETIME && type != SQLI_TYPE_INTERVAL)
        return SQLI_METADATA_UNAVAILABLE;
    if (field->encoded_length > UINT16_MAX)
        return SQLI_PROTO_ERROR;
    return sqli_temporal_decode_range((uint16_t)field->encoded_length, type == SQLI_TYPE_INTERVAL, out);
}
