# CSDK feature request decisions

Status after the application API commit `6bf8b86`, reviewed on 2026-09-08.
This tracks the decisions for FR-1 through FR-4 in the external sample feedback.
It does not introduce provisional function names into the public API.

| Request | Decision | Current deliverable / next gate |
| --- | --- | --- |
| FR-1: active-operation cancellation | Public prepared-DML cancellation implemented; SELECT/FETCH/LOB and deadlines pending | See [cancellation evidence and design gates](CANCELLATION_FEASIBILITY.md); see the [public API and demo](CANCELLATION_API.md) for the supported subset |
| FR-2: child detach after fork | Establish a strict process contract first | [Process lifecycle](PROCESS_LIFECYCLE.md); comprehensive PID rejection and detach require a separate implementation |
| FR-3: XA integration | Deferred until a concrete transaction-manager use case | Verify server/protocol support, recovery and branch states before defining an optional adapter |
| FR-4: application API consistency | Implemented in `6bf8b86` | Checked scalars, local status text, zero-based parameter indices, private legacy codecs, independent Smart-LOB readers and checked introductory examples |

FR-4 is covered by the [application API migration document](APPLICATION_API.md)
and its recorded validation. External examples still need to migrate old scalar
signatures and one-based bind/call indices before recompiling. Parameter and
column indices are zero-based size_t; legacy cursor row positions remain
one-based and are documented separately. Do not infer a new row-navigation API
from the index migration.

The next implementation gate is a bounded cancellation design backed by live
protocol evidence, including operation generations and connection reuse rules.
A timeout is not confirmation of cancellation or rollback. No shared-connection
thread safety is implied by the future cancel exception.

Smart-LOB size/stat and seek-origin operations remain potential smaller follow-up
features when required. Truncation needs its own write/transaction/cursor rules.
XA is not a wrapper around local COMMIT: it requires a complete branch lifecycle,
in-doubt outcomes and recovery responsibilities. ODBC stays outside libsqli.
Catalog work remains deferred until the preceding value/API work is complete
and tested; these decisions do not begin catalog implementation.

## Generated API reference

Run `doxygen Doxyfile` from the repository root. It produces HTML under
`build/doxygen/html` and XML under `build/doxygen/xml`. Doxygen is a documentation
tool dependency only; Graphviz is not required. Undocumented public declarations
and invalid Doxygen references make generation fail. Generated output is local
and excluded from Git.

Validation of this documentation/probe iteration: Doxygen 1.9.8 reports no
warnings with undocumented-symbol and enum-value checks enabled. All five
installed public headers compile independently as strict C11 and C++11. Public
function declarations are unchanged; the 14 public value structures retain their
sizes, alignments and field offsets. The Debug sanitizer build includes all tools;
all 19 selected local non-unit CTest suites pass. The manual baseline and interrupt
probes pass with ASan/UBSan and LeakSanitizer, including independent visibility
after explicit rollback. No public cancellation or detach behavior was added.

Cancellation follow-up: [handle lifetime and operation boundaries](CANCELLATION_LIFECYCLE.md)
now have private POSIX implementation and deterministic tests. TLS and plain TCP
live probes verify safe disposal, pool handoff and suppressed post-terminal sends.
Public prepared-DML integration is available. SELECT/FETCH/LOB phase support
and bounded cleanup deadlines remain implementation gates. See the lifecycle document for the precise scope and limitations.
