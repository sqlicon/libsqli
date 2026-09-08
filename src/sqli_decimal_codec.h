#ifndef SQLI_DECIMAL_CODEC_H
#define SQLI_DECIMAL_CODEC_H

#include "libsqli/sqli_decimal.h"
#include "libsqli/sqli.h"

enum { SQLI_DECIMAL_WIRE_CAPACITY = 18 };

/* Internal fixed-width tuple codecs for DECIMAL, NUMERIC and MONEY. Descriptor
 * encoding: precision 1..32 in the high byte, scale 0..precision or 0xff
 * (floating) in the low byte. No SQ_BIND framing or connection diagnostics.
 * Reentrant; callers synchronize mutation. Outputs must not overlap inputs or
 * each other. Every failure leaves outputs, including length, unchanged.
 * At most 16 significant base-100 groups are representable; pair alignment
 * can therefore reduce 32-digit targets to 31 significant decimal digits.
 * Decode requires exact width; encode accepts at least width bytes.
 * Fixed scale is preserved; floating values use the shortest exact coefficient
 * (zero scale for zero), since original application scale is absent on the wire.
 * Unknown/malformed descriptors and payloads return SQLI_PROTO_ERROR.
 * Encode never rounds: target overflow returns SQLI_OUT_OF_RANGE, discarded
 * nonzero fractional digits SQLI_INEXACT. No allocation or text conversion.
 */
sqli_status sqli_decimal_wire_size(uint16_t descriptor, size_t *out);
sqli_status sqli_decimal_decode_wire(const uint8_t *bytes, size_t length,
                                      uint16_t descriptor, sqli_decimal_t *out);
sqli_status sqli_decimal_encode_wire(const sqli_decimal_t *value, uint16_t descriptor,
                                      uint8_t *bytes, size_t capacity, size_t *length);

#endif
