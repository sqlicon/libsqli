#ifndef SQLI_CANCEL_INTERNAL_H
#define SQLI_CANCEL_INTERNAL_H

#include "libsqli/sqli.h"

/* Private POSIX experiment infrastructure; no installed cancellation API.
 * Owner calls create/begin/finish/snapshot/destroy. Only request is cross-thread.
 * The caller must stop and join requesters before destroying the single-use handle.
 */
typedef struct sqli_cancel_operation sqli_cancel_operation;

typedef enum {
    SQLI_CANCEL_LATCHED,
    SQLI_CANCEL_SENT,
    SQLI_CANCEL_SEND_FAILED,
    SQLI_CANCEL_ALREADY_REQUESTED,
    SQLI_CANCEL_COMPLETED
} sqli_cancel_disposition;

typedef struct {
    bool started;
    bool requested;
    bool send_attempted;
    sqli_status send_status;
    sqli_status operation_status;
    sqli_status disposal_status;
    sqli_error_info diagnostic;
} sqli_cancel_snapshot;

sqli_status sqli_cancel_operation_create(sqli_cancel_operation **out);
sqli_status sqli_cancel_operation_destroy(sqli_cancel_operation *operation);
/* Pins an exclusively owned TCP connection for one verified interruptible phase.
 * On pre-start cancellation, returns OK with execute=false and no connection I/O.
 * Production execute/fetch integration must verify phase boundaries separately.
 */
sqli_status sqli_cancel_operation_begin(sqli_cancel_operation *operation,
                                      sqli_conn_t *conn, bool *execute);
sqli_status sqli_cancel_operation_request(sqli_cancel_operation *operation,
                                        sqli_cancel_disposition *out);
/* After all operation I/O has stopped: joins the serialized send, snapshots
 * diagnostics and disposes a tainted transport before releasing the pin.
 */
sqli_status sqli_cancel_operation_finish(sqli_cancel_operation *operation,
                                       sqli_status operation_status);
sqli_status sqli_cancel_operation_snapshot(sqli_cancel_operation *operation,
                                         sqli_cancel_snapshot *out);

#endif
