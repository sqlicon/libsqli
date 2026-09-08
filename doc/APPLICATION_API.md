# Application API contracts and migration

The experimental API replaces scalar fallback returns and unifies indexing.
Recompile consumers: there are no compatibility aliases for the old signatures.
Catalog work remains deferred; no ODBC or external decimal dependency is added.

## Headers and owned values

| Header | Application interface |
| --- | --- |
| `libsqli/sqli.h` | Connections, statements, descriptors, checked scalars and buffers, status text |
| `libsqli/sqli_decimal.h` | Opaque decimal values, semantic parts, parsing, formatting, native read/bind |
| `libsqli/sqli_temporal.h` | Calendar DATE, opaque DATETIME/INTERVAL, semantic parts, native read/bind and text conveniences |
| `libsqli/sqli_sblob.h` | Opaque upload handles and independent read cursors, explicit descriptor operations |

Create reusable decimal/datetime/interval objects, fill them through checked
getters, then inspect, format or bind them. Their values survive subsequent
fetches and result destruction. Destroy owned objects explicitly. Native binders
copy their encoded input and preserve the previous binding on failure. Semantic
DATE fields, decimal import/export parts and temporal ranges stay public values.

Historical `sqli_encode_date`, `sqli_decode_date`, `sqli_encode_datetime`,
`sqli_decode_datetime` and `sqli_encode_decimal` declarations have moved into a
private source header. Applications should use the native semantic interfaces;
wire epochs, packed qualifiers and unchecked raw buffers are not public contracts.

## Checked scalars

```c
sqli_status sqli_result_get_int(sqli_result_t *, size_t, int32_t *, bool *);
sqli_status sqli_result_get_int64(sqli_result_t *, size_t, int64_t *, bool *);
sqli_status sqli_result_get_double(sqli_result_t *, size_t, double *, bool *);
sqli_status sqli_result_get_bool(sqli_result_t *, size_t, bool *, bool *);
```

The output pointers are mandatory and must not overlap. A fetched current row
is required. These functions do not change `sqli_result_was_null()`.

| Outcome | Status | Output changes |
| --- | --- | --- |
| Value | `SQLI_OK` | Value assigned, `is_null=false` |
| SQL NULL of an accepted type | `SQLI_OK` | Value unchanged, `is_null=true` |
| Invalid access or conversion | Specific failure | Both unchanged |

Integer getters accept integer/boolean types and exact integral DECIMAL/MONEY.
Fractional decimals return `SQLI_INEXACT`; overflow returns `SQLI_OUT_OF_RANGE`.
DATE epochs and text are not implicitly converted. Boolean reads accept boolean
columns only. Double reads accept integers, decimals and floating types, with
ordinary floating-point rounding; use native decimal for exact decimal values.
Nonfinite floating values and overflow return `SQLI_OUT_OF_RANGE`. Unsupported
types return `SQLI_TYPE_MISMATCH`, even when the source is SQL NULL. Decimal
conversion may allocate. Synchronize result access and output mutation.

Replace `value = sqli_result_get_int(result, column)` with a status check followed
by an explicit NULL decision. Use `sqli_result_fetch()` / `sqli_stmt_fetch()`:
`SQLI_EOF` ends iteration, while other non-OK statuses indicate failure.

## Local diagnostics

`sqli_status_name(status)` and `sqli_status_description(status)` return immutable
static strings. They are reentrant, allocate nothing and do not access connection
diagnostics. Unknown values return `SQLI_UNKNOWN_STATUS` and `Unknown library
status`. Log the operation and returned status; add connection diagnostics for
server failures when available. A local parse/get/bind error does not imply that
`sqli_error(conn)` contains a new message.

## One index convention

Every public parameter and result index uses zero-based `size_t`, including
callable parameter modes and output access. Parameter counts remain counts.
The first parameter is 0, the last is count minus one; count and `SIZE_MAX` are
out of range. Migrate literal and computed indices together, including loops.
SQL placeholders and the wire protocol are unchanged.

## Checked text and binary buffers

The [README](../README.md#2-executing-queries) leads with checked buffers and
native decimal values. `sqli_result_get_string_len()` and native formatters
report required capacity including the terminating NUL. Byte getters report
payload length without a terminator. A NULL buffer with zero capacity queries
size. SQL NULL succeeds with required size zero and an explicit NULL flag.
A short buffer reports `SQLI_BUFFER_TOO_SMALL`, updates required size/NULL state
and leaves the buffer unchanged. Other failures leave all outputs unchanged.

The caller owns allocated buffers and frees them with the corresponding
allocator. No allocating convenience with ambiguous ownership is introduced.
Legacy pointer-returning string functions remain conveniences with their
documented borrowed/thread-local storage and NULL/error conventions. Copy their
results before any operation that invalidates that storage; prefer checked APIs
when correctness depends on distinguishing empty text, SQL NULL and failure.

## Independent Smart-LOB readers

`sqli_sblob_reader_open(conn, source, &reader)` opens an owned opaque
`sqli_sblob_read_cursor_t` directly from a created/uploaded BLOB or CLOB handle.
The output pointer is unchanged on failure. The reader has an independent server
descriptor and position; it neither moves nor closes the upload descriptor.
Multiple readers may inspect the same object. This also supports inspecting the
confirmed prefix after an upload callback aborts.

- `sqli_sblob_reader_read()` advances the reader's position and reports bytes read.
- `sqli_sblob_reader_read_seek()` first moves by a signed relative byte offset,
  then reads. Negative offsets allow rereading preceding bytes.
- A successful zero-byte read with nonzero capacity indicates EOF. Zero capacity
  is a no-op, including for seek/read. Capacity greater than `INT32_MAX` is rejected.
- Read failures preserve the byte-count output, but may have modified the buffer
  or server position. Discard the connection after transport/protocol failure.
- `sqli_sblob_reader_close()` closes its server descriptor and is repeatable.
  `sqli_sblob_reader_destroy()` frees client memory only and permits NULL.

The reader borrows the connection, which must outlive its I/O and close. It does
not retain the source handle pointer. Close readers before releasing the server
object, and close a reader before destroying it. If the connection breaks,
terminate the connection before destroying outstanding handles. Synchronize all
operations sharing a connection, even when cursor positions are independent.
EOF does not close the reader automatically.

## Regression coverage

Tests cover checked scalar value/NULL/error separation, exact decimal conversion,
overflow, malformed booleans, allocation failure, unchanged outputs and parameter
boundaries. Existing consumers, callable fixtures and native wire bind fixtures
use the new signatures and indices. Live Smart-LOB tests exercise BLOB/CLOB,
independent cursors, backward relative seek, closed cursors and readback after
both successful and aborted uploads. Historical validation counts in the domain
documents describe their respective iterations.

Validation for this iteration: all 24 Debug ASan/UBSan CTest suites including
live decimal and temporal matrices pass. All 20 Release suites pass, with all
tools enabled in both builds. The live integration section passes all 33 tests
with LeakSanitizer enabled; the expanded seven Smart-LOB socket regressions also
pass directly with LeakSanitizer. The five installed public headers compile
independently as C11 and C++11, and both introductory README functions compile
as C11 with strict warnings. Live evidence applies to the configured server.
