#ifndef SQLI_CANCEL_INTERNAL_H
#define SQLI_CANCEL_INTERNAL_H

#include "libsqli/sqli_cancel.h"

/* Begin/finish are private; only validated database operations may bind a handle.
 * begin is retained for the manual raw protocol probe and arms immediately.
 * prepare registers with sending disarmed; arm follows a complete EXECUTE write.
 */
sqli_status sqli_cancel_operation_begin(sqli_cancel_operation *operation,
                                      sqli_conn_t *conn, bool *execute);
sqli_status sqli_cancel_operation_prepare(sqli_cancel_operation *operation,
                                        sqli_conn_t *conn, bool *execute);
sqli_status sqli_cancel_operation_arm(sqli_cancel_operation *operation);
sqli_status sqli_cancel_operation_finish(sqli_cancel_operation *operation,
                                       sqli_status operation_status);
#endif
