#include "sqli_cancel.h"

sqli_status sqli_cancel_operation_create(sqli_cancel_operation **out)
{
    if (out == NULL)
        return SQLI_INVALID_ARGUMENT;
    *out = NULL;
    return SQLI_UNSUPPORTED;
}

sqli_status sqli_cancel_operation_destroy(sqli_cancel_operation *operation)
{
    return operation == NULL ? SQLI_OK : SQLI_UNSUPPORTED;
}

sqli_status sqli_cancel_operation_request(sqli_cancel_operation *operation,
                                        sqli_cancel_disposition *out)
{
    if (operation == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    return SQLI_UNSUPPORTED;
}

sqli_status sqli_cancel_operation_snapshot(sqli_cancel_operation *operation,
                                         sqli_cancel_snapshot *out)
{
    if (operation == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    return SQLI_UNSUPPORTED;
}

sqli_status sqli_cancel_operation_prepare(sqli_cancel_operation *operation,
                                        sqli_conn_t *conn, bool *execute)
{
    if (operation == NULL || conn == NULL || execute == NULL)
        return SQLI_INVALID_ARGUMENT;
    return SQLI_UNSUPPORTED;
}

sqli_status sqli_cancel_operation_begin(sqli_cancel_operation *operation,
                                      sqli_conn_t *conn, bool *execute)
{
    return sqli_cancel_operation_prepare(operation, conn, execute);
}

sqli_status sqli_cancel_operation_arm(sqli_cancel_operation *operation)
{
    return operation == NULL ? SQLI_INVALID_ARGUMENT : SQLI_UNSUPPORTED;
}

sqli_status sqli_cancel_operation_finish(sqli_cancel_operation *operation,
                                       sqli_status operation_status)
{
    (void)operation_status;
    return operation == NULL ? SQLI_INVALID_ARGUMENT : SQLI_UNSUPPORTED;
}
