# Active-operation cancellation: feasibility and implementation gates

No public cancellation function is introduced by this investigation. A timeout,
a request to interrupt, an operation's terminal result and the transaction's
outcome are different facts and must remain distinguishable.

## Existing implementation

The POSIX transport uses socket read/write timeouts and TLS polling timeouts.
They limit individual waits, not an absolute operation deadline. A sequence of
successful partial transfers can extend total elapsed time. The pool-acquisition
timeout covers waiting for a lease, not completion of a query.

The current SQLI send/receive path has no public operation token or generation,
no cancellation state machine, and no verified cancellation handling across
prepare, execute, fetch and LOB transfers. Pool release currently makes a lease
available again; it is not a cancellation completion barrier. The normal close
path sends protocol shutdown and does not establish a rollback outcome when an
acknowledgment is missing.

## Evidence and limits

The documented [sqlbreak interface](https://www.ibm.com/docs/en/informix-servers/15.0.x?topic=library-sqlbreak-function)
establishes that the server supports interruption through a client facility.
Its documented busy-state check does not specify a complete wire implementation
or guarantee the terminal state of a canceled transaction.

A bounded local experiment on the configured logging test database demonstrates
a concrete non-TLS TCP path: sending the urgent byte 0x42 with MSG_OOB while an
UPDATE waits for another connection's row lock produces server SQLCODE -213
and ISAM code -157. The worker's explicit ROLLBACK then succeeds. The current
client surfaces that server response as SQLI_PROTO_ERROR, not a dedicated
cancellation status. This demonstrates a transport mechanism and a specific
terminal response, not a finished application API.

The experiment uses two independent connections and its own test table. A
10-second lock-wait limit bounds the blocked statement. The urgent request is
sent after a delay from a separate thread; the ordinary result reader alone
consumes the server response. No ordinary libsqli operation is concurrently
called on that connection by the interrupting thread.

The repeatable probe produced these results on 2026-09-08 with ASan/UBSan and
LeakSanitizer enabled:

| Mode | SQLCODE / ISAM | Elapsed blocked operation | After explicit rollback |
| --- | --- | --- | --- |
| Baseline, no urgent byte | -244 / -154 | 9.276 seconds | Fresh connection reads original amount 0 |
| Urgent interrupt after about one second | -213 / -157 | 1.002 seconds | Fresh connection reads original amount 0 |

Both runs cleaned up their own table. The independent read proves visibility
**after explicit rollback**, not an implicit transaction rollback by cancellation.
No debugger attachment was needed to demonstrate this path.

### Reproducing the probe

The manual `sqli_cancel_probe` target is built on Unix when
`SQLI_ENABLE_LIVE_TESTS=ON`. It is deliberately excluded from automatic CTest
execution. Configure the usual `SQLI_TEST_HOST`, `SQLI_TEST_PORT`,
`SQLI_TEST_SERVER`, `SQLI_TEST_USER`, `SQLI_TEST_PASS`,
`SQLI_TEST_CLIENT_LOCALE` and `SQLI_TEST_DB_LOCALE` environment variables.
`SQLI_TEST_LOGGING_DB` selects an existing logging-enabled test database and
otherwise defaults to `sqli_log_test`. Use an isolated test server; the account
needs permission to create and drop the probe's own table.

```sh
cmake --build build --target sqli_cancel_probe
SQLI_IO_TIMEOUT_SEC=15 LSAN_OPTIONS=detect_leaks=1 build/sqli_cancel_probe --baseline
SQLI_IO_TIMEOUT_SEC=15 LSAN_OPTIONS=detect_leaks=1 build/sqli_cancel_probe --interrupt
```

This is a private transport experiment using the connection's internal socket;
it is not an example of supported concurrent application use. The delay does not
provide a general operation-registration handshake. Do not turn it into a public
cancel implementation by copying the send call alone.

Still unverified: TLS behavior, server-version differences, a late interrupt
racing normal completion, FETCH and Smart-LOB behavior, repeated cancellation,
COMMIT interruption, and reuse after every possible terminal response. In
particular, a local generation number is not present in the demonstrated urgent
byte: preventing a delayed interrupt from reaching a subsequent operation needs
more than checking a token immediately before sending.

## Proposed contract, not public declarations

Use an opaque operation-bound cancellation capability. Only its cancel-request
operation is eligible for cross-thread use; execution, fetch, cleanup and
connection destruction remain serialized. Creating/arming the capability must
be atomic with associating the operation generation, so a request cannot fall
into a gap before the operation is registered.

Represent at least these facts independently:

- Request disposition: accepted, already completed, unsupported, or send failure.
- Terminal operation result: completed, server-confirmed interruption, or failure
  with unknown outcome. Preserve actual server SQLCODE/SQLSTATE when available.
- Connection usability: synchronized/reusable versus must be discarded.
- Transaction knowledge: known active/ended versus unknown; no invented rollback.

Successful request submission never means that the server has stopped. The
execution thread must consume the terminal response and restore synchronization
before the connection can be handed to another operation or pool lease. Define
how outstanding cancel references prevent close, descriptor reuse and use-after-
free. If the protocol cannot prove that an interrupt is consumed, discard the
connection rather than allowing a stale request to target the next operation.

A deadline should use one monotonic absolute time across the whole operation.
Deadline expiry may request cancellation; if terminal confirmation cannot be
obtained within a bounded cleanup interval, return an unknown outcome and discard
the connection. Never automatically replay DML or COMMIT after lost acknowledgment.
Do not call libsqli from signal handlers; notify a normal thread instead.

## Implementation sequence and acceptance gates

1. Completed for the configured server: a repeatable non-TLS cancellation probe
   with baseline lock timeout, SQLCODE capture, explicit rollback and independent
   visibility checks. Extend this evidence to the supported server matrix.
2. Verify TLS and terminal-response synchronization, then define supported phases.
   Unsupported transports/phases need an explicit contract; do not guess bytes.
3. Implement operation lifetime/generation management and its narrow concurrency
   exception, including pool handoff and connection-discard policy.
4. Integrate bounded deadlines only after request and terminal-result semantics
   are tested separately.

Acceptance must cover bounded SELECT and transactional DML; normal completion
racing cancellation; cancellation before start and after completion; transport
loss; prepare/fetch/LOB partial progress where supported; and pool handoff to a
new generation. Independently inspect database state after each transactional
case. COMMIT acknowledgment loss remains unknown even if the client stopped
waiting. A canceled statement is not evidence that its enclosing transaction
was rolled back.
