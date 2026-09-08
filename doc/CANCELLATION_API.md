# Prepared DML cancellation

Include `<libsqli/sqli_cancel.h>` for the public POSIX TCP/TLS cancellation API.
This first version supports prepared INSERT, UPDATE and DELETE with scalar
bindings; positioned UPDATE/DELETE (`WHERE CURRENT OF`) are excluded.
PREPARE itself, SELECT, FETCH, stored procedures, LOB streaming, batches,
transaction commands and automatic BEGIN are not supported by this call. An
unsupported operation returns SQLI_UNSUPPORTED before registration or SQL I/O.
For manual transactions, call sqli_begin() explicitly before executing DML.

The operation is `sqli_execute_cancelable(statement, cancellation)`. Ordinary
`sqli_execute()` remains available and does not register cancellation. The
current SELECT execution path spans OPEN and repeated FETCH exchanges; it cannot
be wrapped safely as a single unqualified response wait. It remains a separate
integration gate. No public cancellation-aware fetch function is claimed here.

## Ownership and order

1. Acquire exclusive use of a connection and prepare/bind the statement normally.
2. Create a fresh `sqli_cancel_operation` with `sqli_cancel_operation_create()`.
3. Give a borrowed pointer to a requester thread. Only
   `sqli_cancel_operation_request()` may run concurrently with execution.
4. Call `sqli_execute_cancelable()` in the execution owner thread.
5. Join all requester threads, then inspect `sqli_cancel_operation_snapshot()`.
6. Destroy statement/result/reader handles while the connection object is alive.
7. Return the pool lease and check its status. Destroy the cancellation handle
   only after every requester has stopped using it.

The snapshot owns its diagnostics and may outlive the connection. The handle is
single-use: no resetting or rebinding to a later statement, fetch or pool lease.
Precondition rejection leaves a fresh handle unconsumed. A pre-start cancellation
consumes it, returns SQLI_CANCELED and invalidates the statement's result-valid
flag without sending SQL. Existing connection diagnostics are left untouched;
the terminal snapshot carries no fabricated server error for that local outcome.

## Request disposition versus execution outcome

A successful request call reports only the recorded disposition:

| Disposition | Meaning |
| --- | --- |
| SQLI_CANCEL_LATCHED | Accepted before start or during request transmission; no urgent byte sent yet |
| SQLI_CANCEL_SENT | One urgent byte sent; execution can still finish successfully |
| SQLI_CANCEL_SEND_FAILED | Send attempted unsuccessfully; disposal is still required |
| SQLI_CANCEL_ALREADY_REQUESTED | Duplicate; no further urgent byte |
| SQLI_CANCEL_COMPLETED | The call has finished; no connection access or network effect |

After execution, inspect the copied snapshot:

| Outcome | Interpretation |
| --- | --- |
| SQLI_CANCEL_NOT_EXECUTED | Pre-start cancellation; no DML sent |
| SQLI_CANCEL_EXECUTED | Server-reported execution success, including a race with cancellation |
| SQLI_CANCEL_INTERRUPTED | SQLCODE -213 confirmed statement interruption |
| SQLI_CANCEL_FAILED | Another local/server error; inspect operation_status and diagnostic |
| SQLI_CANCEL_UNKNOWN | Transport or malformed-response failure prevents determining the execution outcome |

`operation_status` and `disposal_status` are independent. The function returns
the operation status when disposal succeeds, or the disposal error otherwise.
For example, SQL can succeed while TLS-state disposal fails: the function returns
an error, but the snapshot still reports SQLI_OK and SQLI_CANCEL_EXECUTED for the
operation. Do not replay that DML. `send_status` is meaningful only when
`send_attempted` is true. `connection_discarded` means permanently unusable, not
necessarily that every cleanup action succeeded.

SQLI_CANCELED covers both local pre-start cancellation and server-confirmed
interruption; the outcome distinguishes them. None of these results claims that
an enclosing transaction rolled back or committed. A connection terminated after
an unknown outcome must not trigger automatic DML or COMMIT replay.

## Protocol boundary

Registration pins the connection while urgent sending is disarmed. A requester
arriving during ID/BIND/EXECUTE writes only latches its request. Once the entire
EXECUTE/EOT group has been sent, execution arms the response wait and sends any
pending interrupt under the same lifecycle mutex used by requesting and finishing.
No interrupt can be sent to a partially transmitted BIND group by this API.

An interrupt can race a terminal response. Every urgent-send attempt therefore
marks the physical connection permanently unusable, regardless of send success
or SQL outcome. Finishing waits for the serialized send, snapshots diagnostics,
aborts TLS state without SSL_shutdown, shuts down/closes the transport and only
then drops the connection pin. A post-terminal request cannot touch a new lease,
even if the old socket number was reused. Transport send/read failures also
require disposal, even if cancellation was only latched and never sent. A local
response-processing failure after EXECUTE, including allocation failure, also
means an unknown outcome and requires disposal unless a server diagnostic
establishes the statement result.

Blocked writes are not interruptible through this interface. The operation must
return through its I/O timeout before cleanup; there is no absolute deadline or
hard bound on cancellation latency. The TLS registry mutex may also wait behind
an unrelated graceful TLS shutdown. These limitations are part of the contract.

## Pool cleanup errors

`sqli_pool_release()` rejects a pinned connection or a duplicate release. For a
discarded connection, cleanup must succeed before the slot becomes available.
A cleanup error retains the lease and the borrowed pointer; keep ownership and
retry cleanup after inspecting the error. Do not drop the pointer as though the
release had succeeded. A subsequent acquisition creates a new physical session.

`sqli_pool_destroy()` now returns sqli_status. NULL succeeds. Outstanding leases
return SQLI_INVALID_STATE and leave the pool usable. Once destruction starts, it
closes transports abortively without SQLI/TLS shutdown messages; a cleanup error
keeps the pool object alive but closed to new acquisitions. Retry destruction to
finish cleanup. This is not an implicit rollback acknowledgment. Stop all other
pool users before destruction; concurrent destroy/use is unsupported.

If pool creation fails and cleanup of already-created connections also fails,
`sqli_pool_create()` returns the cleanup error and a non-NULL pool handle solely
for retrying destruction. Normal creation failures leave the output NULL. This
keeps failed cleanup reachable instead of leaking the partially created pool.

This experimental signature change requires recompilation. Existing calls that
ignore the return value still compile, but applications should check it. No
separate compatibility wrapper is introduced.

## Public-only live demo

`test/cancel_demo_posix.c` includes only installed libsqli headers and POSIX
thread/time APIs. It uses a private test table, a locking holder connection, a
one-slot pool, scalar-bound prepared UPDATE and independent visibility checks.
It reads SQLI_TEST_HOST/PORT/SERVER/USER/PASS, locale variables, and
SQLI_TEST_LOGGING_DB (default `sqli_log_test`). TLS follows the library's normal
SQLI_SSL_ENABLE and certificate configuration. Credentials are never embedded.

Build with SQLI_ENABLE_LIVE_TESTS=ON, then run:

```sh
cmake --build build --target sqli_cancel_demo
build/sqli_cancel_demo --baseline
build/sqli_cancel_demo --interrupt
build/sqli_cancel_demo --prestart
build/sqli_cancel_demo --complete
```

SQLI_CANCEL_DEMO_MODE accepts the same mode strings, including the leading `--`;
an explicit command-line mode overrides it. Without either, interrupt is used.
The complete mode also exercises public prepared INSERT and DELETE successfully.
The demo checks pool release/destruction results and requests cancellation again
after reacquisition to verify that a terminal handle cannot affect the next lease.

The deterministic lifecycle suite additionally injects a request during writes,
a partial-write failure, a disposal error after successful SQL, failed pool
release and failed pool destruction. It verifies disposition/outcome separation,
lease retention and successful cleanup retry. These are controlled failure tests,
not claims that every server/transport failure has been simulated.

## Validation (2026-09-08)

All four public demo modes pass on the configured server over plain TCP and TLS.
Baseline reports -244 and retains the session; interrupt reports SQLI_CANCELED,
SQLI_CANCEL_INTERRUPTED and -213 with a new session afterward; pre-start reports
SQLI_CANCEL_NOT_EXECUTED with no send; normal completion reports SQLI_OK and
SQLI_CANCEL_EXECUTED. Independent visibility is 0 for the three non-success cases
and 2 for the successful UPDATE. INSERT and DELETE succeed through the same
public entry point. Every run removes its test table. TLS verification was
disabled for the self-signed test endpoint; trust/hostname verification is not
part of this evidence. No server authentication settings were changed.

All 20 selected local CTest suites pass, including 15 cancellation lifecycle/API
cases. The existing phase4 suite passes 97 tests and phase6 passes 33 tests,
including live transaction and Smart-LOB coverage. Debug builds use ASan/UBSan;
the explicit lifecycle, phase and live demo runs enable LeakSanitizer. All six
installed public headers compile independently as strict C11 and C++11. Doxygen
reports zero warnings. The unsupported-platform stub passes a C11 syntax check;
this is not a Windows build or live verification claim.
