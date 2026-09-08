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

The follow-up below covers TLS lock waits and controlled completion races.
Still unverified: server-version differences, all completion/send interleavings,
FETCH and Smart-LOB behavior, repeated cancellation,
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

1. Completed for the configured server: repeatable plain TCP and TLS cancellation probes
   with baseline lock timeout, SQLCODE capture, explicit rollback and independent
   visibility checks. Extend this evidence to the supported server matrix.
2. Establish terminal-response synchronization or enforce disposal, then define supported phases.
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

## Lifetime and reuse refinement

The [operation lifecycle proposal](CANCELLATION_LIFECYCLE.md) defines ownership,
pre-start and repeated requests, terminal races, per-call operation boundaries,
and a conservative first-version policy: dispose of the physical connection
after any urgent-send attempt. A generation alone is not a wire barrier.
The pool must wait for both execution and the accepted sender before recycling
a slot, including when descriptor numbers can be reused by the operating system.

### Completion and pool experiments

The manual `sqli_cancel_race_probe` uses a one-slot pool and two independently
locked table rows. `--late-reuse` deliberately releases a completed operation's
lease and delays its cached-socket interrupt until the next lease is blocked.
The first operation returns SQLI_OK; the second is interrupted with SQLCODE -213
on the same server session. This is a controlled negative example of failing to
serialize completion and sending; it is not a supported application pattern.

`--race-reuse` releases the first lock after 100 ms and sends the interrupt after
90, 100 or 110 ms. The execution and sender threads are joined before pool
handoff. `--race-discard` uses the same timings, then terminates the old transport
and makes the pool reconnect. Server session IDs distinguish physical reuse
from reconnect; comparing client pointer/descriptor values would be insufficient.

The next lease performs normal setup/session-ID queries and then an UPDATE that
is deliberately held by a second row lock with a two-second wait limit. These
intervening queries are part of the experiment and are not asserted to form a
protocol barrier. In the observed plain TCP and TLS samples, reuse and discard both reached
the normal lock timeout (-244/-154) for that UPDATE. Only discard consistently
used a new session. These finite samples do not prove safe reuse after a sent
interrupt; the late-reuse negative case demonstrates why lifecycle fencing is
mandatory even when earlier reuse samples pass.

All three modes use their own tables and remove them on successful completion.
The table changes are isolated test data. The original cancellation probe tests
independent visibility after explicit rollback; the race probe instead measures
which operation receives the interrupt and which server session the pool leases.

```sh
cmake --build build --target sqli_cancel_race_probe
SQLI_IO_TIMEOUT_SEC=10 LSAN_OPTIONS=detect_leaks=1 build/sqli_cancel_race_probe --late-reuse
SQLI_IO_TIMEOUT_SEC=10 LSAN_OPTIONS=detect_leaks=1 build/sqli_cancel_race_probe --race-reuse
SQLI_IO_TIMEOUT_SEC=10 LSAN_OPTIONS=detect_leaks=1 build/sqli_cancel_race_probe --race-discard
```

### TLS follow-up (2026-09-08)

The configured server's TLS listener accepted the native test account. The
separate PAM listener and the TLS listener have different authentication
configuration; resolving a username does not establish that native shadow
password authentication can validate it. No server authentication settings were
changed for these tests. Credentials are not part of this document or the probe.

With `SQLI_SSL_ENABLE=1` and the test environment selecting the TLS listener,
the sanitizer build produced the following results. Certificate verification was
disabled for the self-signed test endpoint; these tests do not validate trust or
hostname verification. The urgent byte is sent using TCP MSG_OOB, outside TLS
records, while SQLI requests and responses use the TLS transport.

| Probe | Observed result |
| --- | --- |
| Baseline lock wait | -244/-154 after 9.124 s; independent visibility check after explicit rollback passed |
| Interrupt after one second | -213/-157 after 1.001 s; independent visibility check after explicit rollback passed |
| Late reuse | First operation succeeded; next lease on the same session received -213 |
| Completion race, reuse, 90/100/110 ms | First operation returned -213/-213/success; next operation returned -244 in all three samples; same session |
| Completion race, discard, 90/100/110 ms | First operation returned -213/-213/success; next operation returned -244 in all three samples; new session each time |

These measurements establish a working TLS lock-wait interruption path on this
server, not a synchronization barrier or general cancellation support.

The initial TLS discard experiment terminated with SIGPIPE (exit status 141).
The existing TLS detach implementation calls `SSL_shutdown`, which can write
close_notify even after the probe has shut down the socket. The manual race
probe now ignores SIGPIPE explicitly for its own process so it can complete the
experiment. This is a probe-only policy, not a library fix or a recommendation
for applications. Production disposal needs a no-I/O TLS teardown, bounded
transport termination and safe signal handling without changing application-wide
signal disposition. This remains an implementation gate. The table left by the
terminated experiment was removed explicitly; successful reruns cleaned their
own tables.

The final TLS probes pass with ASan/UBSan and LeakSanitizer. All 19 selected local
non-unit CTest suites pass in the Debug sanitizer build. The public API remains
unchanged; this iteration adds evidence and the proposed lifecycle contract.
