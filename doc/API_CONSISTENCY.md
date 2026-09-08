# Public API consistency and migration

The experimental API now provides native read/write symmetry for DATE,
DECIMAL/NUMERIC, DATETIME and INTERVAL. This change also standardizes result
indices and checked buffers, adds explicit statement fetch status, and hides
Smart-LOB implementation state. Recompile consumers and migrate the affected
calls; no compatibility aliases are retained for replaced signatures.

## Native DATE and decimal binding

```c
/* <libsqli/sqli_temporal.h> */
sqli_status sqli_bind_date(sqli_stmt_t *stmt, size_t param_index,
                           const sqli_date_t *value);

/* <libsqli/sqli_decimal.h> */
typedef struct {
    uint8_t precision;
    uint8_t scale;
    bool floating_scale;
} sqli_decimal_target_t;

sqli_status sqli_bind_decimal(sqli_stmt_t *stmt, size_t param_index,
                              const sqli_decimal_t *value,
                              const sqli_decimal_target_t *target);
```

DATE binds a checked calendar value directly to its binary representation.
Decimal binding uses the checked binary codec, with precision 1..32 and fixed
scale 0..precision, or floating scale when `floating_scale` is true (`scale` is
then ignored). These public fields express SQL semantics without exposing wire
qualifier bytes or decimal implementation storage. MONEY values use the same
native decimal representation; the server converts the transmitted DECIMAL to
the statement's actual target type.

The explicit decimal target describes the source parameter sent to the server,
not inferred placeholder metadata. Binding requires exact representability:
nonzero discarded digits return `SQLI_INEXACT`, and unsupported precision or
magnitude returns `SQLI_OUT_OF_RANGE`. DATE validates the calendar before
replacing the binding. No catalog queries or external decimal library are used.

Native binding allocates nothing, copies the encoded value, and preserves the
previous binding on failure. The caller can immediately mutate or destroy the
source. Batch snapshots retain independent copies, including SQL NULL. Use the
value's NULL state; a NULL C value pointer is invalid. Parameter and result indices now both use zero-based `size_t`.

Text input remains available under `sqli_bind_date_string`,
`sqli_bind_decimal_string`, `sqli_bind_datetime_string` and
`sqli_bind_interval_string`. DATE string binding validates ISO `YYYY-MM-DD`
locally and delegates to the native binder. The other three transmit text for
server conversion. Existing timestamp, epoch, ISO date and ISO timestamp
conveniences remain in the temporal header.

## Result indices and row iteration

All result column indices, including scalar/text access, column metadata,
timestamp/epoch conveniences and Smart-LOB streaming, now use zero-based
`size_t`. Generic descriptor access remains in `sqli.h`; include
`sqli_decimal.h` for precision/scale and `sqli_temporal.h` for temporal ranges.

Use `sqli_result_fetch()` or the new `sqli_stmt_fetch()` for iteration:
`SQLI_OK` positions a row, `SQLI_EOF` ends the result, and other statuses report
failure. The statement function delegates to the same result fetch protocol.
It rejects NULL statements and statements without an executed result.
`sqli_stmt_result()` exposes that result after execution, before the first fetch.
The boolean `sqli_result_next()` and `sqli_stmt_next()` remain conveniences that
return true exactly when fetch returns `SQLI_OK`; they cannot distinguish EOF
from failure. Prepared-statement probe tools now use the status protocol.

## Checked whole-value buffers

```c
sqli_status sqli_result_get_string_len(sqli_result_t *result, size_t column,
                                      char *out, size_t capacity,
                                      size_t *required, bool *is_null);
sqli_status sqli_result_get_bytes(sqli_result_t *result, size_t column,
                                 uint8_t *out, size_t capacity,
                                 size_t *required, bool *is_null);
```

Both getters require a validated current row and mandatory `required` and
`is_null` outputs. Passing NULL for `out` with zero capacity queries the size.
Output storage must not overlap other outputs or result storage.

| Result | Status and outputs |
|---|---|
| Non-NULL string | `required` includes the terminating NUL |
| Non-NULL bytes | `required` is the payload length, without a terminator |
| SQL NULL | `SQLI_OK`, `required=0`, `is_null=true`, buffer unchanged |
| Empty string / binary | Non-NULL, required size 1 / 0 |
| Insufficient capacity | `SQLI_BUFFER_TOO_SMALL`, size and NULL state reported, buffer unchanged |
| Other failure | Every output unchanged |

No partial copies or silent truncation occur. String output supports character
and legacy LOB columns plus native decimal and temporal formatting. Bytes expose
the column payload, with LOB materialization through the existing fetch path.
Materialization and character conversion may allocate or access the connection;
size queries can therefore fail too. These APIs do not modify `was_null`.
The SQL dump uses the required byte count to avoid its former 4 KiB limit and
reports read failures instead of substituting locator text.

## Checked scalars and legacy string conveniences

`get_int`, `get_int64`, `get_double` and `get_bool` now return `sqli_status` and
require value and NULL outputs. Failures preserve both outputs; SQL NULL leaves
the value unchanged. Integer conversion of fractional decimals returns
`SQLI_INEXACT`. See [the application API](APPLICATION_API.md) for the complete
conversion contract and migration examples.

Legacy string getters retain their thread-local NULL/error conventions. Prefer
checked whole-value buffers or native getters plus formatters. Synchronize
result/connection use and destination mutation; thread-local convenience buffers
are not independent owned values.

## Opaque Smart-LOB ownership

`sqli_sblob_t` is now an opaque handle. Its descriptor, locator bytes, locator
length and open state are private. Creation returns an owned pointer through
`sqli_sblob_t **out`, leaving it unchanged on failure. Query semantic state with
`sqli_sblob_is_open()` and `sqli_sblob_get_type()`. Creation options remain a
public value structure; the existing explicit low-level descriptor API remains.

Close an open created descriptor with `sqli_sblob_close_created()`, or release
an unreferenced server object with `sqli_sblob_release()`, before calling
`sqli_sblob_destroy()`. Destroy frees client storage only: it neither issues SQL
nor deletes server data. After a broken connection, terminate the connection
before destroying its handles. A NULL destroy argument is allowed. Synchronize
all access to a handle with mutation and destruction. Public tools and live tests
use this ownership model; only private white-box fixtures inspect the layout.

## Verification of the preceding native API iteration

The dedicated API consistency tests cover scalar overflow and NULL, buffer
atomicity and allocation failure, native bind copies and batch lifetime,
statement fetch errors/EOF, and opaque handle allocation/lifetime. Existing wire
fixtures now exercise the public binders for all four native value domains.
All 19 Debug sanitizer CTest suites pass; direct consistency tests also pass with
LeakSanitizer enabled. All 36 fixed live native bind fixtures pass.

Debug and Release builds include all enabled tools. All 18 non-unit Release
CTest suites pass, and installed public headers compile independently as C11
and C++11. The live integration section
passes all 33 tests without skips, including six server tests and Smart-LOB
creation, streaming, binding, readback and release. These results apply to the
configured test server; earlier large codec-matrix evidence remains recorded in
the respective domain documents. The catalog component remains deferred.
