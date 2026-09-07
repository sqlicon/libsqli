#ifndef SQLI_TEMPORAL_CODEC_H
#define SQLI_TEMPORAL_CODEC_H

#include "libsqli/sqli.h"

enum { SQLI_DATE_WIRE_SIZE = 4, SQLI_TEMPORAL_WIRE_CAPACITY = 12 };

/* Internal fixed-width tuple codecs, not SQ_BIND framing. No allocation or
 * connection access. Reentrant; callers synchronize destination mutation.
 * Malformed qualifiers/payloads return SQLI_PROTO_ERROR. Encoding validates the
 * target and exact representability. All failures preserve outputs, including
 * length; buffers and output objects must not overlap. Decode requires exactly
 * the descriptor width. Encode accepts at least that capacity and returns width.
 * Qualifiers support server fractions 1..5 and interval leading precision 1..9.
 */
sqli_status sqli_temporal_wire_size(uint16_t qualifier, bool interval, size_t *out);
sqli_status sqli_date_decode_wire(const uint8_t *bytes, size_t length, sqli_date_t *out);
sqli_status sqli_date_encode_wire(const sqli_date_t *value, uint8_t *bytes,
                                   size_t capacity, size_t *length);
sqli_status sqli_datetime_decode_wire(const uint8_t *bytes, size_t length,
                                       uint16_t qualifier, sqli_datetime_t *out);
sqli_status sqli_datetime_encode_wire(const sqli_datetime_t *value, uint16_t qualifier,
                                       uint8_t *bytes, size_t capacity, size_t *length);
sqli_status sqli_interval_decode_wire(const uint8_t *bytes, size_t length,
                                       uint16_t qualifier, sqli_interval_t *out);
sqli_status sqli_interval_encode_wire(const sqli_interval_t *value, uint16_t qualifier,
                                       uint8_t *bytes, size_t capacity, size_t *length);

#endif
