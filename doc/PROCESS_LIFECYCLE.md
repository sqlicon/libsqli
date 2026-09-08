# Process lifecycle and inherited handles

The supported worker model creates each process before initializing libsqli in
that process. Each worker creates and owns its connections, pools, statements,
results and Smart-LOB handles. Never share a live database session between
processes by inheriting its handles.

## Current fork contract

After forking a process that has used libsqli, the child must not call libsqli
before exec. This includes getters, connection creation/reconnect, close/destroy,
statement/result cleanup, pool operations and Smart-LOB operations. Parent-side
ownership and use continue in the parent; child-side inherited handles are not
usable objects under the supported API contract.

This is a precondition, not a promise that inherited calls return a particular
status: the current implementation has no comprehensive process-ID guard and no
child-detach API. Standalone copied values do not establish a general post-fork
library reinitialization contract either.

The parent may keep its cursor and transaction only if the child performs no
protocol I/O on the inherited session. In particular, sqli_destroy() calls
sqli_close(), which can send a protocol EXIT. Pool destruction closes its
connections; TLS and charset cleanup can use locks and allocation. Ordinary
cleanup is therefore not a child-detach operation.

After a multithreaded fork, restrict the child to async-signal-safe operations
until exec, as required by [POSIX fork](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fork.html).
A PID comparison inside one API entry point would not make allocator, TLS or
mutex cleanup safe. Do not install handlers that call ordinary libsqli functions
from a POSIX signal handler or an at-fork child handler.

## Descriptor inheritance remains an application concern

The current POSIX TCP creation path does not explicitly set close-on-exec.
Therefore exec alone does not promise to release inherited session descriptors.
Use a process launcher that closes unwanted inherited descriptors before exec,
or create workers before opening database connections. Do not use shutdown on
an inherited socket: its underlying connection is shared with the parent.
If exec fails, terminate the child with _exit rather than running inherited
application cleanup handlers. No libsqli shutdown or destroy is required in
that child.

## Local detection and detach assessment

A future guard would need process ownership checks before locks, transport I/O
or mutable shared-state access in every connection-dependent entry point. It
must cover pools and borrowed statements/results, not just sqli_connect().
Void destroy functions and pointer/boolean conveniences need explicit rejection
semantics; silently introducing partial PID checks would imply protection they
do not provide. Pure value operations should have a separately defined scope.

A child-only detach, if later needed for a supported single-threaded fork model,
would invalidate dependent handles without protocol close, rollback, socket
shutdown or server release. It would need a dedicated transport cleanup path,
plus a tested policy for inherited TLS state, mutexes, pools and subsequent
reinitialization. It cannot bypass multithreaded-fork restrictions.

No detach implementation or inherited-handle runtime rejection is claimed in
this iteration. Tests for such a future feature must prove that a parent's
unread cursor and transaction survive, the child can use a fresh independent
connection under the supported model, and rejected inherited operations cause
no server activity. Until then the process-first ownership model is required.
