# Cancellation lifetime and operation boundaries

This is a proposed first-version contract, not an implemented public API.
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

The current pool does not enforce this policy. A new implementation needs an
explicit unusable/discard state and must make dependent statement, result and
LOB cleanup safe after transport disposal. Copied native values and retained
immutable descriptors keep their independent ownership. Calling the ordinary
close path, which can send SQLI EXIT, is not itself this discard primitive.
TLS disposal must free session state without writing close_notify to a shut-down
socket. The manual TLS experiment exposed SIGPIPE in the current detach path;
its probe-only signal policy must not become a library-wide signal disposition.

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
