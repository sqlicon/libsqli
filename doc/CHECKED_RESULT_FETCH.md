# Checked result fetching

Status: implemented as the row-access foundation for the native get/bind migration.

## API and outcomes

```c
sqli_status sqli_result_fetch(sqli_result_t *result);
```

The operation advances using the existing result cursor behavior. `SQLI_OK` means
that a row is positioned and its computed field spans fit inside the received
tuple. `SQLI_EOF` means normal exhaustion of the current result or buffered batch.
Other statuses report failure. Forward-only results continue to traverse their
buffered rows; server-scrollable results retain the existing refetch behavior.
This API does not introduce a new paging or multi-result protocol.

On failure the current-row view and cache marker are invalidated. A malformed row
is not delivered as a row containing NULL fields, and the next fetch does not skip
it or turn the failure into EOF. The same error remains until row storage is reset
as part of re-execution or an explicit cursor operation that refetches from the
server. Allocation failures follow the same rule. Normal EOF is repeatable.
Passing NULL returns `SQLI_INVALID_ARGUMENT`. Commit/rollback invalidation returns
`SQLI_INVALID_STATE`; advancing beyond the supported row-number range returns
`SQLI_OUT_OF_RANGE` before signed arithmetic can overflow.

Result operations are not concurrently safe; callers synchronize access to a
shared result. `sqli_result_next()` now returns exactly whether
`sqli_result_fetch()` returns `SQLI_OK`. It remains available during migration,
but its boolean cannot distinguish EOF from error. Legacy getters remain valid
only while positioned on a successfully fetched row.

A minimal traversal pattern is:

```c
sqli_status status;
while ((status = sqli_result_fetch(result)) == SQLI_OK) {
    /* Read the current row using the appropriate checked getters. */
}
if (status != SQLI_EOF) {
    /* Handle the status and consult existing connection diagnostics if present. */
}
```

## Row-cache validation

The row-cache builder now returns a status and publishes its cache marker only
when every described field has a valid computed span. Partial cache contents are
unpublished. Allocation size arithmetic and field bounds are checked, including
narrowing of tuple offsets. Partial allocation is cleaned up; no new allocations
are needed for cache reuse on later rows of the same result shape.

DECIMAL/MONEY precision and scale, and DATETIME/INTERVAL qualifiers, are validated
by the production native wire-size functions. Descriptor values exceeding 16 bits
are rejected instead of truncated. A malformed descriptor or a native field that
does not fit returns `SQLI_PROTO_ERROR`. These fields never fall back to treating
the same bytes as an unrelated length prefix. DATE uses its fixed four-byte span.
The immutable descriptor snapshot is independent of the mutable row-location
cache and remains unchanged.

Fetch checks layout and native descriptor validity. It does not decode every
value or validate every numeric digit/calendar field; the checked native getters
must use the production decoders for that work. Existing CHAR and opaque/LOB
layout heuristics and trailing tuple-byte handling are unchanged. Broader payload
and type-specific layout migration remains separate work.

## Error propagation and diagnostics

The scroll path retains send/receive failures for the public fetch call.
`sqli_query_stream()` now propagates row-validation errors and invokes callbacks
only for successful rows. Its delivered-row count excludes the malformed row.

The existing protocol diagnostic contract remains in effect. In particular,
server SQ_ERR responses still return `SQLI_PROTO_ERROR`, accompanied by their
SQLCODE, ISAMCODE and actual server message. A fetch status alone does not yet
distinguish a server SQL error from malformed protocol input; the connection's
structured diagnostics retain that distinction. Transport statuses are propagated
as currently produced by the transport. Local row-validation/allocation failures
do not fabricate SQLCODEs or overwrite connection diagnostics.

## Tests

`test/test_result_fetch_linux.c` covers:

- Every incomplete prefix of two-column rows containing DECIMAL, MONEY, DATE,
  DATETIME or INTERVAL followed by an integer, including zero-byte rows.
- Successful typed-NULL layouts, the following column and repeatable clean EOF.
- Invalid native descriptors and unchanged outputs from failed direct locators.
- A malformed later row after a successfully cached row with a variable-width
  first column; stale data cannot remain the current-row view.
- Allocation failure, cleanup, persistent errors and row-storage reset.
- Cursor invalidation after rollback and row-number overflow.
- Refetch I/O failure, strict-protocol rejection and preservation of SQLCODE,
  ISAMCODE and server text from a server error response.

The integration suite additionally sends a complete tuple frame whose second row
is shorter than its two-column descriptor. Streaming returns `SQLI_PROTO_ERROR`,
invokes the callback once and reports exactly one delivered row.

The native wire probes and decimal/temporal matrix runners now require explicit
`SQLI_OK` and `SQLI_EOF` outcomes. Validation for this iteration includes all 14
CTest suites, the focused Release test, and the 32 live receive plus 32 live native
bind fixtures. Direct Debug fetch tests and live probes also passed with ASan,
UBSan and LeakSanitizer enabled. The full live decimal/temporal matrices were not
rerun in this iteration; their earlier codec evidence remains documented in the
respective codec documents.

From a configured build, with test-server settings loaded for live commands:

```sh
ctest --test-dir build --output-on-failure
LSAN_OPTIONS=detect_leaks=1 ./build/sqli_result_fetch_test
LSAN_OPTIONS=detect_leaks=1 ./build/sqli_native_wire_contract --live
LSAN_OPTIONS=detect_leaks=1 ./build/sqli_native_wire_contract --live-bind
```

Native public getters, binding snapshots and explicit parameter-target contracts
remain pending. The catalog component stays deferred until the native API work
and its acceptance tests are complete.
