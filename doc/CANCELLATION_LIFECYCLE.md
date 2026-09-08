# Cancellation lifetime and operation boundaries

This is a first-version contract with private POSIX lifecycle infrastructure,
not an implemented public cancellation API. See implementation status below.
It refines [the feasibility investigation](CANCELLATION_FEASIBILITY.md) and
chooses conservative connection disposal whenever an interrupt send is attempted.
No server synchronization barrier has yet been established for urgent interrupts.

## Handle ownership

The application creates a fresh opaque operation handle in the execution thread
before starting an eligible operation. That thread owns the handle. It can give
borrowed access to a cancellation thread, but must keep the handle alive until
all requesting threads have stopped using it. Only the request operation is a
cross-thread exception; creation, result inspection and destruction remain with
the owner. No general concurrent connection, result or statement use is allowed.

A handle is single-use. It cannot be reset or rebound to a later operation or a
new pool lease. An application creates a new handle for each eligible call.
Its identity is not a socket number. A fresh handle is not yet bound to a
connection; entry into the blocking operation binds it and pins the connection
and lease atomically. The connection must already be exclusively owned by the
calling thread. A failed precondition check must not start network I/O.

After the operation has returned, the handle contains an owned terminal snapshot
with request disposition, operation status and available server diagnostics. It
no longer borrows connection storage. The snapshot can therefore outlive the
connection. Destruction requires both a terminal (or never-started) handle and
absence of outstanding requesting threads; it does not send cancellation or
perform implicit waits. A request racing destruction is an application lifetime
violation, not an additional supported concurrent operation.

## State transitions and repeated requests

| State at request serialization point | Request behavior | Network effect |
| --- | --- | --- |
| Fresh, operation not entered | Latch a pre-start cancellation | None |
| Pre-start cancellation already latched | Report already requested | None |
| Bound, between eligible network phases | Latch cancellation for the execution thread | None at that point |
| Bound, interruptible I/O active | Accept at most one interrupt attempt and mark the connection for disposal | At most one urgent byte |
| Request already accepted | Report already requested | No additional bytes |
| Terminal | Report already completed with no new side effects | None |
| Unsupported transport or operation | Report unsupported | None |

On entering an operation with a pre-start request, return a locally canceled
outcome without sending SQL. This is distinct from a server-confirmed canceled
operation and does not taint the connection. A pre-start request does not claim
that unsupported operations are supported: capability/precondition checks still
apply before execution is accepted.

The execution thread and cancel requester need one explicit serialization point
for completion versus the interrupt decision. If completion wins, a later request
cannot acquire a socket to send on. If cancellation wins, the connection is pinned
until the send attempt has finished and the execution thread has reached a
terminal state or exhausted its bounded cleanup interval. Never check a local
generation, drop its protection, and later send through a cached descriptor.

Do not hold a lock across the blocking database operation that the requester
needs in order to wake it. The implementation needs a small lifecycle lock or
atomic state protocol, plus protected ownership of the in-flight send. Closing
or reusing the descriptor while a sender still holds it is forbidden. This is
also necessary when the replacement connection receives the same descriptor
number from the operating system.

## Operation boundaries

A handle covers one explicitly cancel-enabled public blocking call, including
its internal protocol work and response consumption. It does not implicitly
cover the whole lifetime of a statement, result, transaction or Smart-LOB.
The initial implementation should expose support only for independently verified
operations; it must not retrofit a sticky cancel flag onto ordinary connection
use.

| Call category | Proposed boundary | Initial implementation gate |
| --- | --- | --- |
| Prepared execute | One execute through its terminal response or bounded failure cleanup | Verify execute interruption and disposal |
| Result/statement fetch | One fetch call; a subsequent fetch needs a fresh handle | Verify server FETCH; a cached row may complete locally without sending |
| Direct query / prepare | The entire explicit call, including its internal phases | Verify PREPARE and each compound transition before offering cancellation |
| Row-streaming callback API | Whole synchronous call would span all fetches and callbacks | Defer; define callback cancellation and non-reentrancy first |
| Native formatting/scalar conversion | Local value operation | No network cancellation capability |
| Getter that materializes a LOB | Potential hidden I/O | Not implicitly covered by a previous execute/fetch handle |
| Smart-LOB read or seek/read | One transfer call; neither reader lifetime nor subsequent calls | Verify LOB framing, terminal response and partial-buffer semantics |
| Smart-LOB streaming upload | Whole synchronous upload if explicitly enabled | Defer pending partial-progress and callback-boundary tests |
| COMMIT/ROLLBACK and retry wrappers | Separate transactional/compound operation | Exclude initially; no automatic replay or invented transaction outcome |

Cancellation cannot forcibly interrupt an application callback executing arbitrary
code. For a future upload/row callback API, a request must be latched and observed
at an explicit boundary; executing more server I/O afterward requires a new
state decision. Successfully acknowledged upload bytes remain a progress report,
not evidence of transaction commit. An earlier execute's handle has no authority
to cancel a later fetch, implicit LOB read or new pool lease.

## Connection disposal and pool handoff

For the proposed first version, any attempt to send the urgent interrupt marks
the physical connection non-reusable, even if the operation completes normally,
returns SQLCODE -213, or a diagnostic query appears to work afterward. Available
experiments do not establish a protocol barrier proving that no delayed interrupt
can affect future work. Purely local pre-start cancellation and requests observed
after terminal completion do not trigger disposal because they send nothing.

The connection remains exclusively pinned while either the execution thread or
an accepted cancel sender can access its transport. Pool release during that
period must fail locally without making the slot available. Once both are done,
terminate the transport without issuing new SQL, detach the old connection from
the slot, and only then allow a new physical connection to satisfy another lease.
A broken/expired operation may require transport shutdown to wake blocked I/O;
shutdown must target the pinned session, never a recycled descriptor.

The pool now enforces the pin for internally registered operations and refuses
duplicate releases. Discarded connections cannot be leased again: reacquisition
creates a fresh connection object. Statement cleanup and reader cleanup suppress
server operations after disposal; new execute/fetch/reader calls reject disposal. Copied native values and retained
immutable descriptors keep their independent ownership. Calling the ordinary
close path, which can send SQLI EXIT, is not itself this discard primitive.
TLS disposal must free session state without writing close_notify to a shut-down
socket. The original manual TLS experiment exposed SIGPIPE in graceful detach after
socket shutdown. The new abortive path omits SSL_shutdown entirely and the race
probe no longer changes SIGPIPE disposition. Graceful close is a separate path.

Transaction outcome remains a separate result. Successful SQL before the
interrupt can already have committed. Server-confirmed statement cancellation
does not prove that an enclosing transaction ended. If the connection is lost,
do not report a successful rollback without evidence. A fresh pool connection
does not resume the old transaction.

## Remaining acceptance work

Before public declarations: verify the chosen transports and phases; test
pre-start, repeated and post-completion requests; test the completion/send
serialization point and in-flight descriptor pin; reject premature pool release;
and prove fresh-session handoff after an interrupt attempt. Deadline cleanup
uses an absolute monotonic deadline and a separately bounded termination phase.
A passing timing sample alone is not proof that connection reuse is safe.

## Implemented private infrastructure

`src/sqli_cancel.h` is not installed. Its opaque, single-use operation records
fresh/active/terminal state under a lifecycle mutex. Only requesting cancellation
may run concurrently with the execution owner. Request holds the mutex for one
nonblocking urgent send; finish obtains the same mutex after operation I/O ends.
The connection's atomic pin remains set throughout both activities, including
transport disposal. Pool release and close/destroy reject a pinned connection.
Pool destruction rejects outstanding leases and leaves the pool intact.

The pin and returned-lease marker share one atomic state: operation registration
and pool release cannot both acquire ownership. The requester changes no ordinary
connection fields or connection diagnostics. Before a send attempt, it marks the
connection discarded; send failure also requires disposal. Finish copies the
operation status and diagnostic into handle-owned storage and frees TLS session
state without close_notify before shutting down and closing the socket. Only
then does it drop the pin. A terminal request never dereferences the connection,
even after the pool has replaced it or its socket number has been reused.

The implementation uses TCP MSG_OOB with per-call MSG_DONTWAIT and MSG_NOSIGNAL;
it does not alter signal handlers. No retry loop runs in the requester. A failed
or would-block send is recorded separately from the operation's result. TLS
session disposal uses SSL_free, not SSL_shutdown: the latter can initiate further
network writes, as documented by [OpenSSL](https://docs.openssl.org/3.1/man3/SSL_shutdown/).
Transport shutdown/close still performs operating-system connection termination;
"no I/O" here means no additional SQLI or TLS reads/writes or protocol draining.

The private snapshot distinguishes whether execution started, a request was
latched, sending was attempted, the send status, the operation result and disposal
status. Pre-start cancellation reports `execute=false`; it does not invent a
server cancellation diagnostic. No public canceled-status enum is introduced.
Transaction flags/epochs and existing diagnostics are not rewritten to claim a
rollback after disposal. A discarded connection object cannot be reconnected;
create a fresh object, or let the pool replace it after release.

Dependent statements, results and readers must still be cleaned up while their
connection object is alive and before returning its lease. This iteration does
not add reference counting for arbitrary dependent objects. Copied native values
and retained descriptors keep their existing independent ownership.

## Verification and next gates

The local lifecycle suite checks pre-start/repeated/post-terminal requests,
rejected active-handle destruction, failed-send disposal, successful operation
completion despite an interrupt attempt, and snapshots after connection destruction.
A controlled sender pause proves that pool release, connection destruction and
pool destruction cannot recycle its connection. Finishing serializes behind the
sender. Another test reuses the discarded socket number and verifies that stale
statement destruction sends nothing. A wrapped SSL_shutdown asserts zero calls
on abortive disposal; a real send to a shut-down socket exercises MSG_NOSIGNAL.
The tests also verify that SIGPIPE disposition remains unchanged.

Live `--race-discard` now uses the private lifecycle and makes a post-terminal
request after pool reacquisition. Plain TCP and TLS each pass the 90/100/110/300 ms
cases: at 90/100 ms the first operation is interrupted, at 110 ms it succeeds but
an accepted send still forces a new session, and at 300 ms completion wins, no
send occurs and the session remains reusable. The next locked UPDATE times out
normally in all eight samples. Session IDs establish physical session identity.
These timings are observations on the configured server, not scheduling promises.
TLS certificate verification remains disabled for the self-signed test endpoint.

Public execute/fetch integration, between-phase request latching, PREPARE/FETCH/
LOB phase verification, interrupted streaming progress and bounded deadlines
remain open. Begin/finish currently surround the manual probe's operation; they
are not automatically invoked by ordinary database calls. An active operation
must return through its I/O timeout before finish can run: no deadline-driven
concurrent shutdown is implemented. The existing global TLS registry mutex can
also delay disposal behind an unrelated graceful TLS shutdown. Hard cleanup deadlines require
further transport work; this iteration does not promise bounded total latency.
The discard primitive is also supplied for the existing Windows transport, but
cancel registration/sending and the live evidence in this iteration are POSIX-only;
no Windows build or live verification is claimed.

Validation: all 20 selected local non-unit CTest suites pass with ASan/UBSan;
the existing phase4 suite passes 97 tests and phase6 passes 33 tests including
live database, transaction and Smart-LOB cases. LeakSanitizer is enabled for
these explicit phase and live probe runs. Doxygen reports no warnings.
