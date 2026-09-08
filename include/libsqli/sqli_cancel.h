#ifndef LIBSQLI_CANCEL_H
#define LIBSQLI_CANCEL_H

/** @file
 * @brief Single-use cancellation for prepared INSERT, UPDATE and DELETE on POSIX TCP/TLS.
 * Only request may run in another thread. All other calls belong to the execution
 * owner. Keep the handle alive until all requester threads have stopped and joined.
 * This API does not make connection/statement/result use generally thread-safe.
 */
#include "libsqli/sqli.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque capability for exactly one cancel-enabled call, never reset or rebound. */
typedef struct sqli_cancel_operation sqli_cancel_operation;

/** Request disposition, independent of execution and transaction outcome. */
typedef enum {
    SQLI_CANCEL_LATCHED, /**< Recorded before execution or during request transmission; no urgent send yet. */
    SQLI_CANCEL_SENT, /**< One urgent byte sent; execution may still succeed. */
    SQLI_CANCEL_SEND_FAILED, /**< Urgent send attempted unsuccessfully; connection still must be discarded. */
    SQLI_CANCEL_ALREADY_REQUESTED, /**< Duplicate request; no additional urgent byte. */
    SQLI_CANCEL_COMPLETED /**< Execution already terminal; connection is not accessed. */
} sqli_cancel_disposition;

/** Terminal execution interpretation; never a transaction-commit/rollback claim. */
typedef enum {
    SQLI_CANCEL_NOT_EXECUTED, /**< Canceled locally before sending the DML request. */
    SQLI_CANCEL_EXECUTED, /**< Server reported successful execution, including when cancel raced completion. */
    SQLI_CANCEL_INTERRUPTED, /**< Server reported SQLCODE -213. */
    SQLI_CANCEL_FAILED, /**< Local or server error; inspect operation_status and diagnostic. */
    SQLI_CANCEL_UNKNOWN /**< Transport/response failure prevents determining the execution outcome. */
} sqli_cancel_outcome;

/** Owned terminal snapshot; does not borrow connection or handle storage. */
typedef struct {
    bool started; /**< The cancel-enabled call passed registration; not proof SQL was sent. */
    bool requested; /**< At least one cancellation request was accepted. */
    bool send_attempted; /**< An urgent send was attempted, successfully or otherwise. */
    sqli_status send_status; /**< Urgent-send status; meaningful only when send_attempted is true. */
    sqli_status operation_status; /**< Execution result, independent of disposal errors. */
    sqli_status disposal_status; /**< Abortive transport cleanup status; never a rollback result. */
    sqli_error_info diagnostic; /**< Copy of available server diagnostics for this operation. */
    sqli_cancel_outcome outcome; /**< Interpretation of the execution result. */
    bool connection_discarded; /**< Connection is permanently unusable, even if cleanup failed. */
} sqli_cancel_snapshot;

/** Allocate a fresh capability; out is required and set to NULL on failure.
 * Returns SQLI_UNSUPPORTED on platforms without cancellation support.
 */
sqli_status sqli_cancel_operation_create(sqli_cancel_operation **out);
/** Free a fresh or terminal capability after joining all requesters; NULL succeeds.
 * Active destruction returns SQLI_INVALID_STATE and keeps the handle alive.
 * Concurrent destruction/request is an application lifetime violation.
 */
sqli_status sqli_cancel_operation_destroy(sqli_cancel_operation *operation);
/** Request cancellation. out is required; SQLI_OK means disposition was recorded,
 * not that execution stopped. Repeated requests send no further urgent bytes.
 * This is the only cross-thread call; it is not signal-handler safe.
 */
sqli_status sqli_cancel_operation_request(sqli_cancel_operation *operation,
                                        sqli_cancel_disposition *out);
/** Copy terminal outcome; before completion returns SQLI_INVALID_STATE.
 * Both arguments are required; on failure out remains unchanged.
 */
sqli_status sqli_cancel_operation_snapshot(sqli_cancel_operation *operation,
                                         sqli_cancel_snapshot *out);
/** Execute prepared INSERT/UPDATE/DELETE with a fresh cancellation capability.
 * Supports scalar bindings on POSIX TCP/TLS; WHERE CURRENT OF is excluded.
 * SELECT, FETCH, LOB streaming,
 * procedures, transaction commands, batches and implicit BEGIN are unsupported.
 * For manual transactions, call sqli_begin() explicitly beforehand.
 * Unsupported/precondition failures occur before registration and send no SQL.
 * PREPARE is a separate, ordinary call and is not covered by this capability.
 *
 * Requests during request transmission are latched; urgent sending is enabled
 * only after the complete EXECUTE group is sent. A blocked write must return
 * through its I/O timeout; no bounded cancellation latency is promised.
 * After an urgent-send attempt the connection is discarded, even on SQLI_OK.
 * Destroy dependent handles before returning the lease or destroying the connection.
 *
 * Returns the operation status unless disposal fails, in which case that error
 * is returned. Always inspect the terminal snapshot to distinguish both results.
 * SQLI_CANCELED means local cancellation or server-confirmed -213; outcome tells
 * which. Cancellation is not evidence of transaction rollback. Never replay DML
 * automatically after an unknown result. A failed pool release retains the lease.
 */
sqli_status sqli_execute_cancelable(sqli_stmt_t *stmt, sqli_cancel_operation *operation);

#ifdef __cplusplus
}
#endif
#endif
