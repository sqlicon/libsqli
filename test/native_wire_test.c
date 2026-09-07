#include "native_wire_test.h"
#include "sqli_internal.h"
#include "sqli_tcp.h"

#include <string.h>

enum {
    bind_payload_capacity = 32,
    bind_frame_capacity = 64,
    bind_header_width = 14,
    bind_completion_limit = 8,
    date_wire_width = 4
};

_Static_assert(bind_frame_capacity >= bind_header_width + 2 +
               bind_payload_capacity + 1 + 4, "Native test frame capacity");

/* Test-only parameter framing from fixed bytes. No public native binder is
 * claimed by this probe. In particular, row padding is removed before adding
 * the parameter length prefix and even-byte transport padding. */
sqli_status sqli_test_bind_wire(sqli_stmt_t *stmt, sqli_column_type source_type,
                                uint16_t encoded, const uint8_t *payload,
                                size_t payload_length, bool is_null)
{
    if (stmt == NULL || stmt->conn == NULL || payload == NULL ||
        stmt->stmt_id < 0 || stmt->stmt_id > UINT16_MAX ||
        payload_length == 0 || payload_length > bind_payload_capacity)
        return SQLI_INVALID_STATE;
    if (source_type != SQLI_TYPE_DECIMAL && source_type != SQLI_TYPE_MONEY &&
        source_type != SQLI_TYPE_DATE && source_type != SQLI_TYPE_DATETIME &&
        source_type != SQLI_TYPE_INTERVAL)
        return SQLI_INVALID_STATE;
    if (source_type == SQLI_TYPE_DATE && payload_length != date_wire_width)
        return SQLI_INVALID_STATE;
    uint16_t statement_id = (uint16_t)stmt->stmt_id;
    uint16_t type = (uint16_t)source_type;
    if (source_type == SQLI_TYPE_DATE)
        encoded = 0;
    else if (source_type == SQLI_TYPE_DECIMAL || source_type == SQLI_TYPE_MONEY) {
        /* MONEY shares the decimal value representation; the table provides
         * its target identity. The parameter descriptor carries source scale. */
        type = SQLI_TYPE_DECIMAL;
        encoded &= 0xff;
    }
    uint8_t frame[bind_frame_capacity] = {
        0, SQLI_SQ_ID, (uint8_t)(statement_id >> 8), (uint8_t)statement_id,
        0, SQLI_SQ_BIND, 0, 1,
        (uint8_t)(type >> 8), (uint8_t)type,
        0, 0, (uint8_t)(encoded >> 8), (uint8_t)encoded
    };
    size_t length = bind_header_width;
    if (is_null) {
        /* Null indicator and encoded length occupy the last four header bytes. */
        frame[bind_header_width - 4] = 0xff;
        frame[bind_header_width - 3] = 0xff;
        frame[bind_header_width - 2] = 0;
        frame[bind_header_width - 1] = 0;
    } else if (source_type == SQLI_TYPE_DATE) {
        memcpy(frame + length, payload, payload_length);
        length += payload_length;
    } else {
        size_t significant_length = payload_length;
        while (significant_length > 1 && payload[significant_length - 1] == 0)
            significant_length--;
        frame[length++] = 0;
        frame[length++] = (uint8_t)significant_length;
        memcpy(frame + length, payload, significant_length);
        length += significant_length;
        if ((length & 1u) != 0)
            frame[length++] = 0;
    }
    frame[length++] = 0;
    frame[length++] = SQLI_SQ_EXECUTE;
    frame[length++] = 0;
    frame[length++] = SQLI_SQ_EOT;
    ssize_t sent = sqli_tcp_send(stmt->socket_fd, frame, length);
    if (sent < 0 || (size_t)sent != length)
        return SQLI_IO_ERROR;
    sqli_result_t response = {0};
    response.owner_conn = stmt->conn;
    sqli_status status = SQLI_OK;
    for (unsigned turn = 0; turn < bind_completion_limit; turn++) {
        status = sqli_receive_dispatch(stmt->socket_fd, &response, stmt->conn);
        if (status != SQLI_OK || response.saw_done)
            break;
    }
    if (status == SQLI_OK && !response.saw_done)
        status = SQLI_PROTO_ERROR;
    sqli_result_cleanup(&response);
    return status;
}
