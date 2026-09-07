#ifndef SQLI_NATIVE_WIRE_TEST_H
#define SQLI_NATIVE_WIRE_TEST_H

#include "libsqli/sqli.h"

/* Test-only wire probe. Payload is a fixed-width received value, with row
 * padding. The function borrows its arguments, performs one native bind and
 * execute, and consumes completion. The caller owns statement cleanup.
 * Use only on a dedicated, ready test connection, without concurrent access.
 */
sqli_status sqli_test_bind_wire(sqli_stmt_t *stmt, sqli_column_type type,
                                uint16_t encoded, const uint8_t *payload,
                                size_t payload_length, bool is_null);

#endif
