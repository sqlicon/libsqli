# Checked query-result output in sqlicon

The SELECT/result renderer now reads character, DECIMAL/MONEY, DATE, DATETIME
and INTERVAL columns through `sqli_result_get_string_len()`. It queries the
required size and allocates an owned cell buffer. Decimal and temporal text is
produced through the native codecs behind that checked API. The former 4 KiB
convenience-buffer limit no longer applies to these query-result cells.

The shared collector serves aligned, CSV, line, JSON and Markdown output. It
preserves existing numeric formatting and SQL NULL presentation. Non-NULL BLOB,
CLOB and BYTE cells display `<BLOB>`, `<CLOB>` and `<BYTE>` respectively. These
placeholders do not fetch LOB contents, expose binary locators, or imply a known
length. SQL NULL remains distinct from a non-NULL LOB placeholder. TEXT continues
to use the checked legacy-LOB materialization path.

Row collection uses `sqli_result_fetch()` so EOF and failure remain distinct.
Initialization, allocation, row-capacity growth and cell conversion return explicit
statuses. Collection errors discard the buffered result before rendering; output
and flush errors cause command failure as well. Query output failures return the
existing SQL-error exit code rather than reporting a successful partial result.
The collector checks row-capacity multiplication and display-width narrowing.

The renderers currently consume C strings. A text value containing an embedded
NUL is rejected with `SQLI_PROTO_ERROR` rather than silently shortened. This is a
renderer limitation, not a claim that every such database value is malformed.
The collector still buffers the full result; this change does not impose a new
memory limit or provide streaming output.

## Scope and remaining legacy paths

The scalar integer/float branches now use checked getters and propagate local
conversion failures, with readable status names and descriptions. Numeric
formatting is unchanged. Legacy string conveniences remain available to other library
consumers. The separate schema-command and SQL-dump paths still have legacy text
calls; the checked SELECT renderer does not establish full-fidelity SQL export.

## Parameter metadata review

The existing send path is PREPARE plus NDESCRIBE, WANTDONE and EOT. The receive
path parses server-described fields into a result descriptor. There is no
separate verified input-parameter descriptor path in this implementation.

The live [descriptor fixtures](NATIVE_VALUE_WIRE_CONTRACT.md#observed-prepare-descriptor-roles)
were rerun: a SELECT with one placeholder describes two projected fields; an
UPDATE with SET and WHERE placeholders describes only the SET target; a SELECT
with no placeholders still describes its output field. These cases disprove a
universal ordinal mapping from the current descriptor to input parameters.
An INSERT where the counts happen to agree does not establish that mapping.

Consequently, exposing the existing `param_server_types` array as a public
parameter descriptor would advertise unverified information. Native decimal and
temporal binders continue to use explicit source-type contracts. This review
does not establish that no alternative protocol facility exists; a new facility
would require separate protocol evidence and tests. Legacy metadata inference
and the simplistic placeholder counter remain follow-up issues. Catalog work
remains deferred.

The reported pool retry loop is already fixed: a failed reconnect releases the
slot and returns its status. The existing reconnect-failure regression test is
part of the passing full suite; no new pool retry policy is introduced here.

## Validation of the preceding CLI output iteration

- All 20 Debug CTest suites pass with ASan/UBSan; the focused output tests also
  pass directly with LeakSanitizer enabled.
- Debug and Release builds include all tools. All three Release CLI suites pass.
- Output regressions cover 6,000-character LVARCHAR values in all five formats,
  BLOB/CLOB placeholders, SQL NULL, exact DATE output, invalid DATE conversion,
  fetch errors, allocation failure and embedded NUL rejection.
- All 36 live native read fixtures and the descriptor fixtures pass.
- A live CLI CSV result contains exactly 6,000 text characters, `12.3400` with
  its fixed scale, and `2000-02-29`, checked independently with a CSV reader.

Live evidence applies to the configured test server and locale combination.
