# Native value API: wire contract and implementation gates

Status: first implementation iteration. This records executable wire probes and
API decisions for the native-value redesign. The new production value API and
binary binders are not implemented by this iteration. The catalog component
starts only after the remaining libsqli work and its tests are complete.

The subsequent standalone decimal implementation is documented in
[Exact Decimal Value API](NATIVE_DECIMAL_API.md). Binary result/bind integration
remains pending. Subsequent [temporal value objects](NATIVE_TEMPORAL_API.md)
and [checked temporal tuple codecs](NATIVE_TEMPORAL_CODECS.md) implement the
standalone temporal layers. The codec document records how the live native probe
now uses production temporal encoders; the evidence below records the initial
wire-audit iteration. The later [decimal codec iteration](NATIVE_DECIMAL_CODECS.md)
extends precision/scale and exponent evidence and switches the decimal native
probe to production encoding.

## Evidence and scope

`test/test_native_wire_contract.c` contains 20 fixed receive fixtures. Each fixture
has a typed SQL expression, native type, encoded length/qualifier, exact payload
and expected text/NULL state. Offline tests decode the fixed bytes. Live tests
compare independently evaluated server expressions against those same bytes,
including a following integer column to detect width/offset mistakes.

The live bind probe inserts native bytes into a session-local temporary table,
then verifies the stored value's descriptor, exact bytes, text, NULL state and row
count. It does not call the existing text binders or a production value encoder.
Table creation uses a typed expression to establish the target type; the initial
row is deleted before binding so an unexecuted insert cannot pass the test.

The temporal matrix adds broader coverage: 2,036 cases over 56 DATETIME and 302
INTERVAL qualifiers. Its independent field-to-pair model constructs expected
receive payloads. Every live case inserts a literal, a text-bound value and a
native-bound value. Each row is checked against generated semantic fields,
server-side equality to the literal, server-rendered text, exact receive bytes
and following sentinel columns. This distinguishes tests of the model from tests
of actual server behavior.

These are tests of the configured server and negotiated connection capabilities,
not proof of support across every server version, transport or descriptor mode.

## Validation recorded for this iteration (2026-09-07)

- Debug build with C11 warnings, AddressSanitizer and UndefinedBehaviorSanitizer.
- Unit suite: 371 registered tests, zero failures, six existing skipped cases.
- Fixed wire fixtures: 20/20 offline, 20/20 live receive, 20/20 live native bind.
- Four live PREPARE descriptor fixtures passed.
- Temporal matrix seed `0xc0ffee`: 2,036/2,036 offline field-derived fixtures and
  2,036/2,036 live cases, each with three insertion paths (6,108 checked rows).
- Fixed wire and descriptor probes also passed with LeakSanitizer enabled.
  The full matrix used ASan/UBSan with leak detection disabled, matching the
  existing test environment constraints.

Live success establishes the tested connection configuration. The remaining
cross-version, malformed-input and production-API gates below still apply.

## Receive representation

DATE is a four-byte big-endian signed day count. Fixed fixtures establish:

| Date | Payload |
|---|---|
| 1899-12-30 | `ff ff ff ff` |
| 1899-12-31 | `00 00 00 00` |
| 1970-01-01 | `00 00 63 e0` |
| SQL NULL | `80 00 00 00` |

The wire epoch stays inside the codec. The new public DATE type uses calendar
fields. Existing comments describing a different epoch are not authoritative;
the executable live fixtures resolve the discrepancy.

DECIMAL, MONEY, DATETIME and INTERVAL receive values use decimal pairs in base
100, with an exponent/sign byte followed by the coefficient and fixed-width
padding. They are not nibble-packed BCD. NUMERIC(p,s) shares the tested DECIMAL
identity. An all-zero payload represents NULL; numeric zero starts with `80` and
is followed by zeros. These must remain distinct.

For a positive nonzero value, the high exponent-byte bit is set; the low seven
bits encode the base-100 exponent with bias 64. Negative values use a transformed
exponent and radix-complement coefficient. Leading zero pairs may be omitted with
an adjusted exponent; trailing receive padding does not imply semantic scale.

For DECIMAL/MONEY with precision p and descriptor scale byte s, the current receive
width is integer division `(p + (s & 1) + 3) / 2`. The descriptor scale byte `ff`
identifies floating scale. Fixed scale comes from the descriptor, not from the
number of padding pairs. Exhaustive floating-scale/exponent tests remain pending.

Temporal descriptors pack decimal digit count in the high byte and starting/
ending field codes in the low nibbles. Ordinary field codes advance by two:
YEAR=0, MONTH=2, DAY=4, HOUR=6, MINUTE=8 and SECOND=10. FRACTION starts at code 12;
its end code is `10 + fractional_precision`. Consequently, fraction-only values
must not be rejected merely because the raw start code exceeds the raw end code
(for example FRACTION TO FRACTION(1)). Public field enums must not inherit these
wire values or comparisons.

The temporal coefficient is aligned relative to the SECOND field. Normalized
leading pairs must be restored before interpreting calendar fields. DATETIME
receive width is `1 + (digit_count + 1) / 2`. INTERVAL adds a half-pair for an odd
leading precision before applying this formula. Declared leading precision is
therefore needed to calculate receive width even when the actual magnitude is
small. One sign applies to the entire interval.

## Parameter framing differs from tuple framing

The test probe sends:

```text
SQ_ID + statement ID
SQ_BIND + parameter count
    source type + null indicator + source encoding attribute
    value payload (if non-NULL)
SQ_EXECUTE + SQ_EOT
```

All header fields above are two-byte big-endian quantities in the tested mode.
For a native NULL, the indicator is -1, the encoding attribute is zero, and no
value bytes follow. Tuple NULL sentinels are not sent as parameter data.

DATE uses encoding attribute zero and a four-byte day count. DECIMAL parameters
carry source scale in the encoding attribute. Temporal parameters carry their
source qualifier. Decimal/temporal values have a two-byte payload length, the
exponent/coefficient bytes and even-byte transport padding; trailing tuple padding
can be removed. Therefore copying a fixed-width received value directly into
SQ_BIND without parameter framing is incorrect.

The probe sends MONEY values as native decimal parameters to a MONEY target. It
does not establish a separate MONEY parameter encoding. The test helper handles
one parameter in an INSERT; it is not a public generic binder and must not be
promoted to production without target checks, resource/state handling and broader
statement coverage.

## Observed PREPARE descriptor roles

Four live descriptor fixtures deliberately vary parameter and result counts:

| Statement | Placeholders | Described fields | Observed meaning |
|---|---:|---:|---|
| SELECT with one input and two projected expressions | 1 | 2 | Output columns |
| INSERT with two target columns and two inputs | 2 | 2 | Target columns in this statement form |
| UPDATE with one SET input and one WHERE input | 2 | 1 | SET target column; WHERE input is not described |
| SELECT with no inputs and one projection | 0 | 1 | Output column |

These observations rule out treating the current NDESCRIBE response as a universal
input descriptor. Preserve output descriptors separately. Even DML descriptors
must not be assigned to all placeholders by ordinal without a verified mapping.
The current statement retains its result descriptor, but its additional
`param_server_types` array copies types without preserving this distinction.

## API decisions for the next iterations

The public API and ABI are experimental and may be replaced without compatibility
wrappers. Migrate sqlicon, tests, examples and documentation together. Existing
string/ISO convenience functionality remains required, using the same native
values and codecs as structured get/bind operations.

- DECIMAL is opaque, owns a base-10^9 arbitrary-precision coefficient, signed
  32-bit scale and explicit NULL state. Preserve scale; do not automatically strip
  decimal trailing zeros. No libmpdec dependency is introduced.
- DATE is a small calendar value. Settle and document calendar/year conventions
  before accepting values outside the tested server calendar range.
- DATETIME and INTERVAL are opaque. Validate field ranges and precision on import;
  provide component export and reusable destinations. Public import/export views
  also have ABI contracts and need documented lifetime/versioning decisions.
- A newly created temporal NULL may have unknown field range. Setting an existing
  typed value to NULL retains its known range. A target descriptor can supply
  missing information; otherwise an operation requiring it fails explicitly.
- Getters and imports leave destinations unchanged on failure. Binders own a copy
  of the value. Applications may then modify or release their input objects.
- Bind validates all currently known target constraints. Validate the complete
  target before sending any bytes. No text fallback, silent rounding or implicit
  completion is permitted.
- Re-prepare invalidates bindings and explicit parameter-type overrides. Changing
  a parameter type validates any existing binding atomically before taking effect.
- Use consistently zero-based result-column and parameter indices in the redesigned
  API. Catalog ordinals, when eventually implemented, keep their own semantics.
- Fetch returns status: row, clean EOF or error. Invalid/truncated rows must not
  become successful NULL values or an apparently clean end of stream.
- Retain immutable descriptor snapshots independently of statement/result lifetime.
  Availability is explicit; unknown parameter metadata is not zero parameters.
- Reuse the existing structured connection diagnostics. Preserve SQLCODE, ISAMCODE
  and actual server text; keep local message lookup separate. Distinguish a valid
  server error response from a malformed protocol response. Standalone value
  operations use explicit native statuses and do not fabricate server diagnostics.

String convenience inputs have explicit length and NULL state. Outputs have
capacity, required-length reporting and explicit NULL state; define one consistent
terminator convention. Decimal parsing is locale-independent and exact. Complete
DATETIME values may format as local ISO timestamps; partial values retain their
field range. Do not invent a timezone, discard an offset, or turn an incomplete
value into a complete timestamp. Empty text and the literal `NULL` are not SQL NULL.

## Remaining implementation gates

1. Verify input versus output descriptor identity for INSERT, UPDATE, SELECT with
   additional parameter/expression shapes and routines beyond the four probes above.
   Current PREPARE code counts question
   marks lexically and retains only type bytes; neither establishes a complete
   parameter type contract. Preserve necessary raw fields and names first.
2. Extend decimal evidence to precision extremes, floating scale, exponent bounds,
   odd scales, rescaling failures and additional server modes.
3. Test invalid temporal descriptors and payloads, narrowing/overflow, NULL metadata,
   and malformed row handling. Existing getters still have permissive validation
   and output mutation on failure; the new API must not inherit those behaviors.
4. Implement independently validated production codecs, binding snapshots, batch/
   repeated execution and source-to-target representability checks. Test all public
   convenience paths through those codecs.
5. Migrate consumers and remove superseded APIs. Complete native-value, descriptor,
   integration and sanitizer tests before starting the catalog component.

## Running the probes

Build with `SQLI_BUILD_TESTS=ON`. Register live CTests explicitly with
`SQLI_ENABLE_LIVE_TESTS=ON`; normal builds do not require a database. Configure
`SQLI_TEST_HOST`, `SQLI_TEST_PORT`, `SQLI_TEST_DB`, `SQLI_TEST_USER` and
`SQLI_TEST_PASS` through the existing private test environment. Locale and server
settings use the same variables as the temporal matrix. Never enable credential
byte logging for these runs.

```sh
ctest --test-dir build -R 'sqli_native_wire_offline|sqli_temporal_generator' --output-on-failure
./build/sqli_native_wire_contract --live
./build/sqli_native_wire_contract --live-bind
./build/sqli_temporal_matrix --live
```

Missing connection settings skip a live test with status 77. A configured but
unreachable server fails. Connection credentials are not printed by the probes.
The full temporal test uses session-local temporary tables and checks their
cleanup. The receive fixtures use SELECT statements; the descriptor fixtures additionally
create and drop a session-local temporary table, and prepare INSERT/UPDATE
statements without executing them.
