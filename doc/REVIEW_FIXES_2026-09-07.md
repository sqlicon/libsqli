# Code review finding resolutions — 2026-09-07

Source review: `../../docs/2026-09-06T223221Z_libsqli-code-review-findings.md`.
This follow-up records the current fixes; it does not replace the original observations.

| Finding | Resolution |
|---|---|
| 1. Pool reconnect storm and swallowed errors | Acquire attempts reconnection once and returns the actual failure with a NULL output handle. The slot is released and another waiter is signaled. Reconnect failures are logged before destroying the failed connection. |
| 2a. Truncated internal wire strings | The helper accepts the full 16-bit wire length, including strings beyond 4096 bytes, and rejects larger values before writing a prefix. Empty strings now return success after writing the zero length. |
| 2b. Truncated PAM password | Oversized PAM responses return an explicit error. Further inspection found an earlier truncation in connection-parameter copying: parameters, including passwords, now fail with a field-specific length error before network I/O. The existing connection password capacity remains 255 bytes plus NUL. |
| 3. Public smart-LOB locator length | Release rejects zero or oversized locator lengths before closing descriptors, reading the locator or building SQL. This includes SIZE_MAX. |
| 4. Ignored PAM sends and DESCRIBE padding reads | Both PAM response branches check complete sends. DESCRIBE consumes full padded strings through the existing checked helper, and string-table padding errors propagate with allocation cleanup. |
| 5. Forbidden string functions | The listed ASC/URI literal fallbacks use snprintf; smart-LOB hexadecimal encoding uses bounded nibble lookup instead of sprintf. |

## Pool behavior

A failed reconnect is returned rather than automatically retried. This is a deliberate alternative to the review's suggested exponential backoff: it both removes the internal retry storm and makes the failure visible to the caller. Callers that want retries must apply their own retry policy and delay.

The acquire timeout governs waiting for a free slot, not connection I/O. A free but disconnected slot may require connection I/O even for try-acquire. The public header now states this explicitly. Existing successful reconnect, busy timeout and waiter wakeup tests remain in place. Concurrent destruction/lifetime semantics were not redesigned in this change.

## Additional defects found while fixing DESCRIBE

The original review's assertion that length-prefixed reads always drain excess bytes did not hold for the two inline extended-name readers. Both read at most 256 bytes without consuming the remainder. Reusing the checked string reader fixes alignment for long owner/type names as well as checking padding reads.

The column-name table also had unchecked allocation failure and unbounded strlen/strncpy traversal over network data. It now checks allocation, uses bounded NUL searches and rejects unterminated names with SQLI_PROTO_ERROR. Raw type metadata preservation and broader ODBC metadata changes are separate work.

## Regression coverage

- Pool reconnect failure with zero, finite and unlimited acquire waits; the original connection is closed and the listener is shut down while retaining the reserved port.
- Complete internal wire strings at lengths 0, 4096, 4097 and 65535; 65536 rejected without buffer mutation.
- Oversized credentials rejected before socket creation with field-specific diagnostics.
- PAM informational ACK plus a 255-byte password response, including odd-length padding and subsequent server rejection.
- Caller-modified smart-LOB locator lengths, including SIZE_MAX, rejected without changing the open descriptor state.
- Extended DESCRIBE with 257-byte owner and type names, missing padding at each of three locations, and an unterminated names table.

The PAM send-error paths were checked in code; the mock verifies normal response transmission and server rejection, rather than injecting a short send at each individual call.

## Validation

- GCC build with `-Wall -Wextra -Werror -pedantic` and ASan/UBSan succeeded, including the workspace build.
- CTest: 365 unit/mock tests passed, zero failures or ignored tests; the offline temporal generator also passed (48.44 seconds total).
- Live Informix temporal matrix: cases 0 and 2035 passed with seed `0xc0ffee`, covering the changed connection and metadata path using literal/bound values. The full 2036-case live matrix was not rerun for this change.
- No ASan/UBSan findings in these runs; `git diff --check` passed.
