# Native temporal result access and binding

The experimental API now integrates opaque DATETIME and INTERVAL values with
result rows and prepared statements. Include `<libsqli/sqli_temporal.h>`;
`<libsqli/sqli.h>` no longer declares domain-specific types or functions.
DECIMAL APIs have their own `<libsqli/sqli_decimal.h>`. Both domain headers are
self-contained and use the same library; no new dependency is introduced.

## Result getters

```c
sqli_status sqli_result_get_datetime(sqli_result_t *result, size_t index,
                                    sqli_datetime_t *out);
sqli_status sqli_result_get_interval(sqli_result_t *result, size_t index,
                                    sqli_interval_t *out);
```

Create the destination once and reuse it across rows. The getter reads directly
from the validated row cache through the native tuple codec. It allocates nothing,
performs no text conversion, and leaves the legacy `was_null` flag unchanged.
The owned value survives row advancement, result destruction and connection close.
Use `get_parts`, `is_null`, or the native formatters to inspect it.

SQL NULL succeeds and retains the field range. Wrong column types return
`SQLI_TYPE_MISMATCH`, even when NULL. Invalid position, stale cache, malformed
qualifiers/payloads and fetch failures are reported explicitly. All failures
leave the destination unchanged. Indices are zero-based.

The public `sqli_datetime_value` and `sqli_interval_value` structures are removed.
Their text-decoding implementations are removed as well. String and timestamp
conveniences now read native values. The SQL string getter retains a space between
date and time; `sqli_datetime_format_iso` supplies ISO `T` notation. Explicit native
formatting retains its existing buffer-size and NULL contracts.

## Native binding

```c
sqli_status sqli_bind_datetime(sqli_stmt_t *stmt, size_t param_index,
                               const sqli_datetime_t *value,
                               const sqli_temporal_range_t *target);
sqli_status sqli_bind_interval(sqli_stmt_t *stmt, size_t param_index,
                               const sqli_interval_t *value,
                               const sqli_temporal_range_t *target,
                               uint8_t leading_precision);
```

Parameter indices are zero-based `size_t`. The explicit range describes the source SQL
value transmitted by SQ_BIND. INTERVAL also requires leading precision 1..9;
FRACTION-only intervals require 0. Fractional precision on the wire is 1..5.
The server applies the SQL expression's actual target conversion. The API does
not infer placeholder targets from DESCRIBE output fields or issue catalog SQL.

The binder validates the range and exact representability, encodes the value into
a bounded private buffer, then replaces the old parameter atomically. Source and
transmitted integral ranges must match. Fractional rescaling is exact; nonzero
discarded digits return `SQLI_INEXACT`. Invalid ranges return
`SQLI_INVALID_ARGUMENT`; unsupported precision or excessive leading magnitude
returns `SQLI_OUT_OF_RANGE`. These failures preserve the previous binding.

Binding allocates nothing and retains a copy. The caller may change or destroy
the object immediately after binding. Batch snapshots copy the encoded value and
qualifier, including NULL state. NULL comes from the object, not a NULL C pointer;
the explicit range is still validated. An initially NULL object with unknown
range can be bound with an explicit range.

The parameter encoder writes the temporal type and qualifier, a length-prefixed
base-100 payload without unused trailing zero pairs, and even-byte transport
padding. NULL uses the SQ_BIND NULL indicator without payload. Source temporal
types are not overridden by legacy guessed BYTE/TEXT parameter metadata.

## Text conveniences and migration

Existing textual bind calls become `sqli_bind_datetime_string` and
`sqli_bind_interval_string`. They retain server-side textual conversion.
Timestamp and epoch conveniences remain available in the temporal header.
Native DATE and DECIMAL result getters move to their domain headers without
signature changes. Native DATE/DECIMAL binders and explicitly named `_string`
conveniences are now available; see [API consistency](API_CONSISTENCY.md).

Recompile consumers with explicit domain includes. No compatibility typedefs,
umbrella includes, or duplicate native getter names are introduced. The catalog
component remains deferred.

## Validation

The native temporal tests cover wrong types, typed NULL, validated-row state,
malformed calendar values and intervals, unchanged destinations on failure,
allocation-free reads/binds, object lifetime, batch copies, range mismatch,
precision overflow and inexact fractional rescaling. Existing result and codec
tests continue to cover framing and cursor boundaries.

The deterministic matrix exercises all 2,036 generated cases across 56 DATETIME
and 302 INTERVAL qualifiers. Its native insertion path uses the public binders;
the other paths use SQL literals and explicit string binding. Every retrieved
row is checked through the opaque getters, alongside text and following-column
sentinels. Fixed wire fixtures provide a separate independent payload oracle.

Validation on the configured test server passed all 2,036 live matrix cases and
36 fixed live reads plus 36 bind fixtures. All 18 sanitizer CTest suites passed;
Debug and Release builds include all tools. Focused Release tests and standalone
installed C11/C++ header checks passed as well. Live probes and native temporal
unit tests also ran with LeakSanitizer enabled. This evidence covers the tested
server configuration, not every server version or locale.
