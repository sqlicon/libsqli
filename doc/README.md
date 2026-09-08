# libsqli Public Documentation

This directory contains the public reference documentation for the `libsqli` library.

## Contents

* [SQLI Wire Protocol Reference](PROTOCOL.md) - Details on SL framing, Unix domain socket preambles, and message formats derived from tracing the SQLI protocol.
* [Connection Performance & Transports](CONNECTION_PERFORMANCE.md) - Deep-dive comparison of `onsoctcp` vs `onipcstr`, Linux `perf` profiling data, handshake analysis, bottlenecks, and optimization strategies.

* [DATETIME / INTERVAL Qualifier Matrix](DATETIME_INTERVAL_QUALIFIER_MATRIX.md) - Deterministic generation, live coverage and reproduction of temporal boundary failures.

* [Native Value Wire Contract](NATIVE_VALUE_WIRE_CONTRACT.md) - Verified native payloads, descriptor roles and implementation gates.
* [Exact Decimal Value API](NATIVE_DECIMAL_API.md) - Owned decimal values, scale-preserving text, exact conversion and resource limits.
* [Native Temporal Value API](NATIVE_TEMPORAL_API.md) - Validated dates, opaque qualified timestamps and intervals, exact fractions and text convenience functions.
* [Checked Temporal Wire Codecs](NATIVE_TEMPORAL_CODECS.md) - Exact binary DATE/DATETIME/INTERVAL conversion, qualifier validation and malformed-payload tests.
* [Checked Decimal Wire Codecs](NATIVE_DECIMAL_CODECS.md) - Exact DECIMAL/NUMERIC/MONEY tuple conversion, fixed/floating scales and exponent-boundary validation.
* [Immutable Descriptor Snapshots](DESCRIPTOR_SNAPSHOTS.md) - Complete raw server metadata, explicit availability and ownership beyond statement/result lifetime.
* [Checked Result Fetching](CHECKED_RESULT_FETCH.md) - Explicit row/EOF/error outcomes, native layout validation and streaming failure propagation.
* [Native Decimal Result Getter](NATIVE_DECIMAL_GETTER.md) - Exact owned decimal reads, explicit type/NULL/error handling and public-API live matrix coverage.
* [Native DATE Result Getter](NATIVE_DATE_GETTER.md) - Calendar-only DATE reads, removal of the old epoch-bearing structure and preserved ISO conveniences.
