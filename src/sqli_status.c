#include "libsqli/sqli.h"

static const struct {
    const char *name;
    const char *description;
} status_text[] = {
    [SQLI_OK] = {"SQLI_OK", "Success"},
    [SQLI_ERR] = {"SQLI_ERR", "Operation failed"},
    [SQLI_EOF] = {"SQLI_EOF", "End of result"},
    [SQLI_TIMEOUT] = {"SQLI_TIMEOUT", "Operation timed out"},
    [SQLI_AUTH_FAIL] = {"SQLI_AUTH_FAIL", "Authentication failed"},
    [SQLI_PROTO_ERROR] = {"SQLI_PROTO_ERROR", "Invalid protocol data"},
    [SQLI_IO_ERROR] = {"SQLI_IO_ERROR", "Input/output failure"},
    [SQLI_ALLOC_FAIL] = {"SQLI_ALLOC_FAIL", "Memory allocation failed"},
    [SQLI_INVALID_STATE] = {"SQLI_INVALID_STATE", "Operation is invalid in the current state"},
    [SQLI_INVALID_ARGUMENT] = {"SQLI_INVALID_ARGUMENT", "Invalid argument"},
    [SQLI_OUT_OF_RANGE] = {"SQLI_OUT_OF_RANGE", "Value or index is outside the supported range"},
    [SQLI_INEXACT] = {"SQLI_INEXACT", "Conversion would discard nonzero digits"},
    [SQLI_BUFFER_TOO_SMALL] = {"SQLI_BUFFER_TOO_SMALL", "Output buffer is too small"},
    [SQLI_NULL_VALUE] = {"SQLI_NULL_VALUE", "Value is SQL NULL"},
    [SQLI_LIMIT_EXCEEDED] = {"SQLI_LIMIT_EXCEEDED", "Resource limit exceeded"},
    [SQLI_METADATA_UNAVAILABLE] = {"SQLI_METADATA_UNAVAILABLE", "Requested metadata is unavailable"},
    [SQLI_TYPE_MISMATCH] = {"SQLI_TYPE_MISMATCH", "Column type does not support this conversion"}
};

const char *sqli_status_name(sqli_status status)
{
    size_t index = (size_t)status;
    return index < sizeof(status_text) / sizeof(status_text[0]) ?
        status_text[index].name : "SQLI_UNKNOWN_STATUS";
}

const char *sqli_status_description(sqli_status status)
{
    size_t index = (size_t)status;
    return index < sizeof(status_text) / sizeof(status_text[0]) ?
        status_text[index].description : "Unknown library status";
}
