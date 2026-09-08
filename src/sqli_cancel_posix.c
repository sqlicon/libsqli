#define _POSIX_C_SOURCE 200809L
#include "sqli_cancel.h"
#include "sqli_internal.h"
#include "sqli_tcp.h"

#include <pthread.h>
#include <sys/socket.h>

struct sqli_cancel_operation {
    /* Serializes request versus finish; never held across database I/O.
     * Request holds it only for one nonblocking urgent send. The connection pin
     * stays set until finish has acquired this mutex and completed disposal.
     * Pool code reads the atomic pin and never acquires this mutex.
     */
    pthread_mutex_t mutex;
    enum { cancel_fresh, cancel_active, cancel_terminal } state;
    sqli_conn_t *connection;
    sqli_cancel_snapshot snapshot;
};

static sqli_status unlock_operation(sqli_cancel_operation *operation, sqli_status status)
{
    return pthread_mutex_unlock(&operation->mutex) == 0 ? status : SQLI_ERR;
}

sqli_status sqli_cancel_operation_create(sqli_cancel_operation **out)
{
    if (out == NULL)
        return SQLI_INVALID_ARGUMENT;
    *out = NULL;
    sqli_cancel_operation *operation = calloc(1, sizeof(*operation));
    if (operation == NULL)
        return SQLI_ALLOC_FAIL;
    if (pthread_mutex_init(&operation->mutex, NULL) != 0) {
        free(operation);
        return SQLI_ERR;
    }
    operation->snapshot.operation_status = SQLI_INVALID_STATE;
    *out = operation;
    return SQLI_OK;
}

sqli_status sqli_cancel_operation_destroy(sqli_cancel_operation *operation)
{
    if (operation == NULL)
        return SQLI_OK;
    /* Owner has already joined every potential requester. */
    if (operation->state == cancel_active)
        return SQLI_INVALID_STATE;
    if (pthread_mutex_destroy(&operation->mutex) != 0)
        return SQLI_ERR;
    free(operation);
    return SQLI_OK;
}

static bool is_tcp_connection(sqli_conn_t *conn)
{
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    return conn->state == SQLI_CONN_READY && conn->socket_fd >= 0 &&
        getsockname(conn->socket_fd, (struct sockaddr *)&address, &length) == 0 &&
        (address.ss_family == AF_INET || address.ss_family == AF_INET6);
}

sqli_status sqli_cancel_operation_begin(sqli_cancel_operation *operation,
                                      sqli_conn_t *conn, bool *execute)
{
    if (operation == NULL || conn == NULL || execute == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (pthread_mutex_lock(&operation->mutex) != 0)
        return SQLI_ERR;
    if (operation->state != cancel_fresh)
        return unlock_operation(operation, SQLI_INVALID_STATE);
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong(&conn->lifecycle, &expected, SQLI_CONN_PINNED))
        return unlock_operation(operation, SQLI_INVALID_STATE);
    if (!is_tcp_connection(conn)) {
        atomic_fetch_and(&conn->lifecycle, ~SQLI_CONN_PINNED);
        return unlock_operation(operation, SQLI_INVALID_STATE);
    }
    if (operation->snapshot.requested) {
        operation->state = cancel_terminal;
        atomic_fetch_and(&conn->lifecycle, ~SQLI_CONN_PINNED);
        *execute = false;
    } else {
        operation->state = cancel_active;
        operation->connection = conn;
        operation->snapshot.started = true;
        *execute = true;
    }
    return unlock_operation(operation, SQLI_OK);
}

sqli_status sqli_cancel_operation_request(sqli_cancel_operation *operation,
                                        sqli_cancel_disposition *out)
{
    if (operation == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (pthread_mutex_lock(&operation->mutex) != 0)
        return SQLI_ERR;
    if (operation->state == cancel_terminal) {
        *out = SQLI_CANCEL_COMPLETED;
    } else if (operation->snapshot.requested) {
        *out = SQLI_CANCEL_ALREADY_REQUESTED;
    } else {
        operation->snapshot.requested = true;
        if (operation->state == cancel_fresh) {
            *out = SQLI_CANCEL_LATCHED;
        } else {
            /* No ordinary connection fields are mutated by the requester. */
            sqli_conn_t *conn = operation->connection;
            atomic_fetch_or(&conn->lifecycle, SQLI_CONN_DISCARDED);
            operation->snapshot.send_attempted = true;
            operation->snapshot.send_status = sqli_tcp_interrupt(conn->socket_fd);
            *out = operation->snapshot.send_status == SQLI_OK ?
                SQLI_CANCEL_SENT : SQLI_CANCEL_SEND_FAILED;
        }
    }
    return unlock_operation(operation, SQLI_OK);
}

sqli_status sqli_cancel_operation_finish(sqli_cancel_operation *operation,
                                       sqli_status operation_status)
{
    if (operation == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (pthread_mutex_lock(&operation->mutex) != 0)
        return SQLI_ERR;
    if (operation->state != cancel_active)
        return unlock_operation(operation, SQLI_INVALID_STATE);
    sqli_conn_t *conn = operation->connection;
    operation->snapshot.operation_status = operation_status;
    operation->snapshot.diagnostic = conn->error_info;
    if (operation->snapshot.send_attempted)
        operation->snapshot.disposal_status = sqli_conn_discard(conn);
    operation->connection = NULL;
    operation->state = cancel_terminal;
    atomic_fetch_and(&conn->lifecycle, ~SQLI_CONN_PINNED);
    return unlock_operation(operation, operation->snapshot.disposal_status);
}

sqli_status sqli_cancel_operation_snapshot(sqli_cancel_operation *operation,
                                         sqli_cancel_snapshot *out)
{
    if (operation == NULL || out == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (pthread_mutex_lock(&operation->mutex) != 0)
        return SQLI_ERR;
    if (operation->state != cancel_terminal)
        return unlock_operation(operation, SQLI_INVALID_STATE);
    *out = operation->snapshot;
    return unlock_operation(operation, SQLI_OK);
}
