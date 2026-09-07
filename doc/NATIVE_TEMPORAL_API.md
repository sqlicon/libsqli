# Native DATE, DATETIME and INTERVAL values

This iteration adds standalone value types to `<libsqli/sqli.h>`. It follows the
[wire contract audit](NATIVE_VALUE_WIRE_CONTRACT.md) and the
[exact decimal value API](NATIVE_DECIMAL_API.md). Values require no connection,
locale, timezone database or additional dependency. They expose semantic fields,
not server epochs, packed qualifiers or decimal-pair encoding.

[Checked temporal tuple codecs](NATIVE_TEMPORAL_CODECS.md) are implemented in
the subsequent iteration. Statement getters/binders and descriptor migration
remain subsequent steps. Existing statement accessors still use their previous
types during this transition. The new values are not yet accepted by statement bind functions.
The experimental API may be changed or removed as that migration proceeds;
there is no compatibility-wrapper requirement. Catalog implementation remains
deferred until the native core is implemented and tested.

## Representation and ownership

| Type | Representation | Lifetime |
| --- | --- | --- |
| `sqli_date_t` | Public `year`, `month`, `day`, `is_null` | Caller-owned value; ordinary assignment copies it |
| `sqli_datetime_t` | Opaque, qualified calendar/time fields | `sqli_datetime_create` / `sqli_datetime_destroy` |
| `sqli_interval_t` | Opaque, qualified magnitudes and one overall sign | `sqli_interval_create` / `sqli_interval_destroy` |

The opaque objects start as SQL NULL with unknown range. Their `copy` functions
copy into an existing destination. Imports validate and copy all fields;
`get_parts` exports an independent struct copy, without borrowed storage.
`destroy(NULL)` is permitted. Creation is the only allocating operation. DATE
can be initialized as `sqli_date_t date = {.is_null = true};`; a zero-initialized
non-NULL date is invalid.

All functions are reentrant. Concurrent read-only access is supported; callers
must synchronize mutation of a shared value, including destruction. Failed
operations preserve the destination and output arguments, except the documented
short-buffer size/NULL outputs. Callers must supply valid objects and storage;
format buffers and output pointers must not overlap each other or the source.

An invalid C pointer argument is not SQL NULL. NULL is represented explicitly.
`set_null` clears numeric fields but retains a known DATETIME/INTERVAL range.
Importing NULL parts ignores numeric fields and clears them, but still validates
the supplied range. Unknown range means both endpoints `SQLI_FIELD_UNKNOWN` and
zero fractional digits; it is valid only for NULL.

## Range and validation

`sqli_temporal_range_t` contains `first`, `last` and `fractional_digits`. The
`SQLI_FIELD_*` enum names YEAR, MONTH, DAY, HOUR, MINUTE, SECOND and FRACTION;
these are independent of wire qualifier codes. Endpoints include every field
between them. Fractional precision is 1..9 when the last field is FRACTION,
and zero otherwise. A fraction-only range has FRACTION at both endpoints.

DATE and DATETIME use the proleptic Gregorian calendar, years 1..9999. Calendar
hours are 0..23, minutes and seconds 0..59. Leap seconds and hour 24 are rejected.
Partial values contain only the declared fields: missing fields must be zero on
non-NULL import and are never completed implicitly. MONTH TO DAY accepts February
29 without inventing a year; February 30 is rejected. DAY alone permits 1..31.
Calendar checks use the year whenever present.

INTERVAL stores unsigned magnitudes (`years`, `months`, `days`, `hours`,
`minutes`, `seconds`) and a single `negative` flag. YEAR/MONTH and DAY/time
families cannot be mixed. The leading integral field accepts the entire
`uint64_t` range; subordinate months are 0..11, hours 0..23, minutes/seconds
0..59. Absent fields must be zero. Negative zero becomes positive zero without
losing its range or fractional precision. Leading SQL precision belongs to the
target descriptor, not the standalone value. There is no implicit carrying
between fields or conversion from months to days.

Both opaque types store `nanosecond` in 0..999999999 and retain the declared
fractional precision, including trailing zeros. The nanosecond value must be an
exact multiple of `10^(9 - fractional_digits)`; otherwise import returns
`SQLI_INEXACT`. A nonzero fraction outside a FRACTION range is invalid.

The application representation supports nine fractional digits and wide leading
interval fields. This does **not** expand the server's supported domain. The
wire audit covers server fractional precision 1..5 and interval leading
precision 1..9. Future codecs must check actual target qualifiers and exact
representability, without silent truncation or hidden text conversion.

## Text convenience functions

All parsers consume an explicit byte length; they do not require a terminating
NUL. No surrounding whitespace, embedded NUL, zone suffix or locale-dependent
text is accepted. Explicit NULL input ignores the text pointer and length.

| Function | Input / output |
| --- | --- |
| `sqli_date_parse`, `sqli_date_format` | Exact `YYYY-MM-DD` |
| `sqli_datetime_parse` | Qualified text with an explicit range |
| `sqli_datetime_format` | Qualified text; full timestamps use `T` |
| `sqli_datetime_parse_iso` | Strict `YYYY-MM-DDThh:mm:ss[.fraction]`, inferring precision |
| `sqli_datetime_format_iso` | Requires YEAR TO SECOND or YEAR TO FRACTION for non-NULL values |
| `sqli_interval_parse`, `sqli_interval_format` | Qualified duration text with one overall sign |

Qualified calendar years have four digits. Other calendar fields and subordinate
interval fields have two digits. Leading integral interval fields accept 1..20
digits, optionally preceded by `+` or `-`; formatting omits leading padding and
positive signs. Separators before MONTH/DAY, HOUR, MINUTE/SECOND and FRACTION
are respectively `-`, space, `:` and `.`. Qualified DATETIME parsing also accepts
`T` between DAY and HOUR. Formatting uses `T` when the value spans YEAR through
at least SECOND, otherwise the qualified space separator.

Fractions must contain exactly the declared number of digits. Fraction-only
text accepts `.1200` or `0.1200` and formats as `.1200`; the negative INTERVAL
form is `-.1200`. Examples of other partial values are `02-29`, `31 23:59`,
`23:59:58.1200` and `-2-11` (INTERVAL YEAR TO MONTH). Parsing and formatting
preserve fractional precision; they perform no rounding or timezone conversion.

Qualified parsers require a known range for non-NULL input. For NULL input, a
supplied range is validated and retained; a NULL range pointer retains the
object's existing range. ISO NULL parsing also retains the existing range.
`format_iso` returns `SQLI_INVALID_STATE` for a non-NULL partial timestamp,
leaving all outputs unchanged. NULL formatting succeeds even with unknown range.

`SQLI_TEMPORAL_MAX_TEXT` limits temporal text input to 64 bytes, excluding NUL.
DATE additionally requires exactly ten bytes; ISO grammar imposes its narrower
length constraints. Canonical output always fits within 64 bytes plus NUL.
Every formatter follows the decimal buffer contract:

- `required` includes the terminating NUL for a non-NULL value.
- `buffer = NULL, capacity = 0` queries the required size and NULL state.
- A short non-NULL buffer returns `SQLI_BUFFER_TOO_SMALL`, updates `required`
  and `is_null`, and leaves every buffer byte unchanged.
- SQL NULL returns `SQLI_OK`, `required = 0`, `is_null = true`, and leaves the
  buffer unchanged; it does not produce an empty string or the word `NULL`.
- All metadata output pointers are mandatory. Other failures leave them unchanged.

## Error categories

`SQLI_INVALID_ARGUMENT` covers invalid pointers, text syntax, absent-field data,
unknown non-NULL ranges and inconsistent range/precision combinations.
`SQLI_OUT_OF_RANGE` covers invalid calendar dates, field bounds and magnitude
overflow. `SQLI_INEXACT` means that nanoseconds cannot be represented at the
requested fractional precision. `SQLI_LIMIT_EXCEEDED` rejects oversized temporal
input; `SQLI_ALLOC_FAIL` reports object creation failure. These local operations
return statuses directly and do not change connection diagnostics.

## Verification

`test/test_sqli_temporal.c` covers calendar limits and leap years, partial dates,
NULL and copy semantics, exact nanosecond precision 1..9, ISO restrictions,
fraction-only values, signed intervals through `UINT64_MAX`, invalid ranges,
malformed and explicitly sized text, atomic errors, and buffer contracts.

The existing deterministic qualifier matrix additionally checks native imports,
exported fields, re-imports and canonical formatting against its independently
generated expectations for all 2,036 cases: 56 DATETIME and 302 INTERVAL
qualifiers, including 358 NULL cases. Its previous offline wire checks remain.

Linux allocation tests inject creation failures and verify that value operations
need no allocations after creation. The test-only allocator wrapper is shared
with decimal tests; production code has no fault-injection hooks. Debug tests
run with AddressSanitizer and UndefinedBehaviorSanitizer. Release tests verify
optimized builds, including allocator-call folding. No new live-server evidence
is claimed by this standalone value iteration.
