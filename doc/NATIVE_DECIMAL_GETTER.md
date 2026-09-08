# Native decimal result getter

Public declarations: `<libsqli/sqli_decimal.h>` (explicit include required).

The public `sqli_result_get_decimal()` connects the checked result-row path to
the production decimal decoder. It reads DECIMAL, NUMERIC and MONEY into an
existing, application-owned `sqli_decimal_t` without formatting, parsing text or
passing through floating point. NUMERIC has the DECIMAL wire type; MONEY keeps
its distinct SQL type in the descriptor while sharing the exact value model.

```c
sqli_status sqli_result_get_decimal(sqli_result_t *result, size_t index,
                                    sqli_decimal_t *out);
```

## Value, ownership and NULL

Create the destination once with `sqli_decimal_create()` and reuse it across
rows. The getter owns no reference to the result or its row buffer: the imported
coefficient belongs to the destination. It remains valid after fetching another
row, reaching EOF, destroying the result or closing the connection. Destroy the
value with `sqli_decimal_destroy()` when finished.

Fixed-scale values retain the descriptor scale, including trailing decimal
zeros. Floating-scale values use the shortest exact coefficient and adjusted
scale, as specified by the [decimal codec](NATIVE_DECIMAL_CODECS.md). For example,
a floating value of 2000 becomes coefficient 2 with scale -3; the shared formatter
renders it as `2E+3`. This is exact and does not preserve a text spelling that is
absent from the wire.

SQL NULL succeeds with `SQLI_OK` and sets the object's NULL state. Query that state
with `sqli_decimal_is_null()`. The new getter does not modify the legacy
`last_was_null` flag. NULL does not bypass type checking: a NULL INTEGER or VARCHAR
still returns `SQLI_TYPE_MISMATCH`.

Every supported server decimal fits the object's inline coefficient storage.
The getter performs no allocation, including when replacing a previously larger
application value. Creation and application imports can still allocate; this is
not a general allocation-free guarantee for arbitrary decimal operations.

## Preconditions and failures

The column index is zero-based. The result must be positioned on a successfully
validated row, normally through `sqli_result_fetch()`. Existing cursor navigation
that successfully prepares the same row cache also works. Reading before the
first row, after EOF or after transaction invalidation returns
`SQLI_INVALID_STATE`. A recorded fetch failure is propagated.

| Condition | Status |
|---|---|
| NULL result or destination pointer | `SQLI_INVALID_ARGUMENT` |
| Index outside the result's columns | `SQLI_OUT_OF_RANGE` |
| No valid current row/cache, or commit/rollback invalidation | `SQLI_INVALID_STATE` |
| Other column type, including its NULL value | `SQLI_TYPE_MISMATCH` |
| Malformed descriptor, payload or cached span | `SQLI_PROTO_ERROR` |

All failures leave the destination unchanged. The getter checks cached bounds
without overflowing size arithmetic and requires the exact codec width. It
validates raw payload bytes independently of legacy NULL markers. No numeric
coercion, rounding, text fallback or local fabricated SQL diagnostic occurs.

The result and destination require external synchronization against concurrent
mutation or destruction. Destination storage must not overlap result storage.

## Text convenience

Use `sqli_decimal_format()` on the fetched object for checked text output. This
uses the same explicit NULL, capacity and required-length contract as standalone
decimal values: required size includes the NUL terminator; NULL returns required
size zero and leaves the buffer untouched; a short buffer is unchanged while
required size and NULL state are reported. A NULL buffer with zero capacity can
query the required size.

A typical application creates one decimal object, fetches a row, calls the native
getter, and then either accesses coefficient/scale with `sqli_decimal_get_parts()`
or formats the owned value. No second server conversion is involved.

The existing `sqli_result_get_decimal_string()` remains a transitional
thread-local convenience with an empty-string NULL/error fallback. Subsequent
[API consistency work](API_CONSISTENCY.md) adds public native decimal binding,
explicit parameter targets and checked whole-value buffers, and corrects scalar
overflow handling. The checked string buffer getter uses the native decoder.
The catalog stays deferred.

## Validation

The dedicated Linux test covers fixed scale, MONEY, floating scale, typed NULL,
wrong-type NULL, malformed bytes/descriptors/cache spans, unavailable row state,
transaction invalidation, propagated fetch failures, independent ownership and
unchanged destinations on failure. Decimal columns follow a variable-width field
and precede an integer sentinel, so locating column zero accidentally cannot pass.
An injected pending allocation failure remains pending across repeated getter
calls, including replacing a 100-digit destination, and is consumed only by a
later explicit object creation.

The fixed native wire probes now call the public getter and compare its formatted
value with independent expectations. The live decimal matrix also uses the public
getter for all 10,432 fixed/floating precision, scale, sign and exponent cases,
while retaining its independent comparison of server bytes with encoded values.

All 15 CTest suites, the focused Release test, 32 live receive and 32 live bind
fixtures, and all 10,432 live matrix cases passed. Direct Debug getter tests and
live probes ran with ASan, UBSan and LeakSanitizer enabled.

Validation commands from a configured build, with test-server settings loaded
for the live commands:

```sh
ctest --test-dir build --output-on-failure
LSAN_OPTIONS=detect_leaks=1 ./build/sqli_native_decimal_getter_test
LSAN_OPTIONS=detect_leaks=1 ./build/sqli_native_wire_contract --live
LSAN_OPTIONS=detect_leaks=1 ./build/sqli_native_wire_contract --live-bind
LSAN_OPTIONS=detect_leaks=1 ./build/sqli_decimal_matrix
```
