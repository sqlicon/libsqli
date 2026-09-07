# Checked native temporal wire codecs

The internal `src/sqli_temporal_codec.h` API converts fixed-width SQLI tuple
payloads to and from the [native temporal values](NATIVE_TEMPORAL_API.md).
The implementation lives in `src/sqli_temporal_codec.c` and has no allocation,
text conversion, connection access or external dependencies. Public statement
getters and binders still use their previous paths; their migration and the
DECIMAL codec are separate steps. Catalog implementation remains deferred.

## Codec boundary

The date encode/decode functions operate on `sqli_date_t`. The datetime and
interval functions use existing opaque destination objects and a raw 16-bit
qualifier. `sqli_temporal_wire_size` validates that qualifier before returning
its tuple width. All functions are reentrant; shared destination mutation
requires caller synchronization. Buffers and output arguments must not overlap.

Decoders require exactly the declared tuple width. They validate before updating
the caller's destination. Encoders require a concrete target qualifier, validate
exact representability, build a temporary payload and copy it only on success.
Every failure preserves outputs, including the encoded length. Unlike public
text formatters, this internal binary API does not provide size queries or
update the required length on short-buffer errors: callers can query the width
separately. DATE has the fixed width `SQLI_DATE_WIRE_SIZE` (four bytes).

Malformed qualifiers and payloads return `SQLI_PROTO_ERROR`; invalid C arguments
return `SQLI_INVALID_ARGUMENT`. Outgoing leading-magnitude overflow returns
`SQLI_OUT_OF_RANGE`, lost fractional precision returns `SQLI_INEXACT`, and
insufficient output capacity returns `SQLI_BUFFER_TOO_SMALL`. No connection
error or synthetic server diagnostic is created by these standalone functions.

These are **tuple payload codecs**, not SQ_BIND frame encoders. Encoded NULL is
the tuple sentinel, and encoded non-NULL values retain tuple padding. A future
production binder must supply the parameter type, NULL indicator, encoding
attribute, payload length and transport padding as described by the
[wire contract](NATIVE_VALUE_WIRE_CONTRACT.md). It must not copy a tuple NULL
sentinel into parameter data. The existing test-only binder performs that
framing for the live probes.

## DATE

The four-byte big-endian signed day count uses 1899-12-31 as day zero. The
`80 00 00 00` sentinel is SQL NULL, distinct from that epoch date. Signed decoding
uses a wider integer and explicit subtraction; it does not depend on an
out-of-range unsigned-to-signed cast.

Calendar conversion uses Gregorian day counts and years 1..9999, matching the
native DATE value contract. A wire day outside that application domain is
rejected rather than narrowed or reported as NULL. The epoch and day-count
representation remain internal. Date conversion does not introduce a time,
timezone or local-midnight assumption.

## Temporal qualifiers and numeric groups

Only the established qualifier domain is accepted: 56 DATETIME qualifiers and
302 INTERVAL qualifiers, with fractional precision 1..5 and interval leading
precision 1..9. The high byte must match the field span and leading precision;
reserved field codes, inconsistent lengths and mixed YEAR/MONTH with DAY/time
intervals are rejected. Fraction-only qualifiers are checked semantically:
FRACTION TO FRACTION(1) remains valid although its raw start code exceeds its
raw end code. Valid tuple widths fit `SQLI_TEMPORAL_WIRE_CAPACITY` (12 bytes).

Payloads contain a sign/exponent byte and base-100 coefficient groups. Decoding
validates every group, reverses the negative radix complement when necessary,
restores omitted leading groups by exponent alignment, and extracts numeric
fields arithmetically. It then imports through the native value validation, so
invalid leap days, hour 24, subordinate-field overflow and negative calendar
values fail atomically. There is no intermediate string or decimal-digit text
buffer.

The accepted representation is normalized: nonzero coefficients begin with a
nonzero group; all-zero coefficient data is accepted only with the canonical
numeric-zero marker `80`. All-zero payloads are SQL NULL. Nonzero groups outside
the declared field span and nonzero unused fractional half-pairs are rejected.
Right-hand zero padding carries no additional precision. Negative complementing
includes the fixed-width coefficient area, preserving its padding behavior.
This policy is verified against the configured server and rejects unverified
noncanonical forms; it is not a claim about every server generation or mode.

For encoding, source and target must have identical first/last semantic fields.
There is no implicit extension, truncation or conversion between units. Different
fractional precisions are permitted only when the nanosecond value is exactly
representable at the target precision. A value with nine application digits can
therefore encode to five server digits if the final four digits are zero; the
source object is unchanged. Leading interval magnitude must fit the declared
precision. Unknown-range NULL may use the target range; a known NULL range must
match the target endpoints. Decoding NULL retains the descriptor's range.

## Verification

The following automated checks exercise the production codecs:

- Fixed temporal/date receive fixtures are decoded, formatted and re-encoded
  against independently recorded bytes. The existing 20-fixture executable
  continues to include decimal fixtures on their previous path.
- All 2,036 generated temporal cases compare native encoding to the independent
  field-to-wire model, and decode that model into independently expected fields,
  signs, precision, NULL state and canonical text.
- All 65,536 qualifier bit patterns are classified for each temporal family;
  only the independently enumerated 56/302 qualifiers succeed.
- A complete 146,097-day Gregorian cycle verifies calendar fields and consecutive
  wire days, with separate fixed epoch, year-one, year-9999 and NULL fixtures.
- Target overflow, exact/inexact fractional narrowing, range mismatch, buffer
  limits, truncated/oversized payloads and malformed calendar/radix/exponent
  inputs are checked explicitly.
- 7,168 deterministic one-byte mutations must either decode into an exactly
  re-encodable value or return a protocol error without changing the destination.
- Allocation fault injection remains pending throughout non-NULL codec calls,
  proving that they allocate no storage after value creation.

The live probes now send production-encoded temporal/date payloads through the
test-only parameter framer. The matrix's third insertion path uses native value
parsing and production encoding; expected receive bytes are still generated
independently. All three rows (literal, existing text binder, native codec) are
compared to the server literal, expected fields, raw bytes and following sentinel
columns. This establishes the codec path without claiming production statement
binding, target-descriptor discovery or binding snapshots are complete.

Validation recorded for this iteration (2026-09-07): all ten Debug CTest
targets passed under ASan/UBSan; targeted Release tests and explicit codec/
allocation leak checks passed. Live receive and native-bind fixtures passed
20/20 each, all four descriptor probes passed, and the complete matrix passed
2,036/2,036 cases (6,108 rows). Live probes used ASan, UBSan and LeakSanitizer
with leak detection enabled. These results apply to the configured test server.

Reproduction after a Debug build with tests enabled:

```sh
ctest --test-dir build -R 'temporal|native_wire_offline' --output-on-failure
./build/sqli_native_wire_contract --live
./build/sqli_native_wire_contract --live-bind
./build/sqli_temporal_matrix --live
```

Live tests use the existing private `SQLI_TEST_*` connection configuration and
session-local temporary tables. Missing configuration skips the live probes;
configured connection failures fail them. Normal CTests require no database.
