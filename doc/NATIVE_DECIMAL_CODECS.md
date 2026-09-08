# Checked native decimal wire codecs

`src/sqli_decimal_codec.h` declares internal fixed-width tuple codecs for the
[exact decimal value type](NATIVE_DECIMAL_API.md). DECIMAL, NUMERIC and MONEY
share this representation; their SQL identities belong to metadata, not the
numeric object. No currency is inferred for MONEY. The implementation requires
no connection, text conversion, floating-point intermediate, locale or external
arithmetic library. It does not allocate after the caller creates value objects.

This codec iteration did not yet migrate public result getters or bind functions.
Subsequent [descriptor snapshots](DESCRIPTOR_SNAPSHOTS.md) and the
[native decimal getter](NATIVE_DECIMAL_GETTER.md) now provide owned metadata and
public binary decimal reads. Explicit parameter targets, binding snapshots and
legacy consumer migration remain separate work, followed by the catalog.
The codec itself remains an internal API.

## Descriptor and buffer contracts

The descriptor's high byte is precision 1..32. Its low byte is either fixed
scale 0..precision or `ff` for floating scale. Exactly 592 combinations are
supported. Reserved encodings, unknown precision, scale above precision and
precision above 32 return `SQLI_PROTO_ERROR`, including for NULL payloads.
`sqli_decimal_wire_size` validates the descriptor and returns integer division
`(precision + (scale & 1) + 3) / 2`, up to 18 bytes.

Decode requires exactly that tuple width. Encode takes an existing native value,
an explicit descriptor, an output buffer and its capacity, and a length output.
Success writes the complete fixed-width payload. Failures preserve every output,
including length. Short buffers return `SQLI_BUFFER_TOO_SMALL`; use the width
function separately when sizing. Missing pointers return `SQLI_INVALID_ARGUMENT`.
Buffers, values and output arguments must not overlap. All functions are
reentrant; callers synchronize mutation of shared values.

The decoder uses a bounded local coefficient and imports it only after all wire
and numeric checks succeed. Four base-10^9 limbs hold every supported result.
The encoder reads the source's borrowed limbs without copying or rescaling the
whole object. Large application coefficients with removable trailing zeros can
therefore fit a small server target without allocating a large intermediate.
The source's coefficient and scale remain unchanged on success and failure.

## Numeric interpretation

The first byte combines sign and a biased base-100 exponent; the remaining
bytes contain coefficient groups in 0..99 and trailing padding. Negative
coefficients use radix complement across the entire fixed-width group area.
Decimal and temporal codecs share that validated complement helper.

All-zero payloads are SQL NULL. Numerically zero values use `80` followed by
zero groups. Critically, `80` followed by nonzero groups is a positive value at
the minimum exponent, not zero. Likewise, `00` followed by nonzero groups can
be a negative value at the maximum exponent, not NULL.

The base-100 exponent spans -64..63. A group `g` immediately after the exponent
represents `g * 100^(exponent - 1)` before sign. Thus the minimum positive
magnitude is `1e-130`; `1e125` and `9.9e125` are representable. Values needing
an exponent outside this range fail with `SQLI_OUT_OF_RANGE`, including tiny
nonzero values that must not silently underflow to zero. Precision independently
limits the number of significant decimal digits. The tested server also limits
coefficients to 16 significant base-100 groups, even when the descriptor's tuple
width reserves an additional padding group.

Every received group is checked before complementing. Nonzero coefficients must
be normalized, without a leading zero group. Zero coefficients require the
canonical zero marker; invalid precision, unrepresentable fixed scale and
malformed/noncanonical data return `SQLI_PROTO_ERROR` without changing the
output. This deliberately accepts the representation established by the probes,
not every hypothetical noncanonical server encoding.

### Fixed scale

Decode preserves the descriptor scale, rebuilding required decimal trailing
zeros in the native coefficient. For example, a DECIMAL(8,4) payload for 123.45
becomes coefficient 1234500 with scale 4. Numeric zero retains that scale; NULL
has the native decimal object's ordinary NULL state.

Encode validates exact rescaling to the target: discarded nonzero digits return
`SQLI_INEXACT`, and a coefficient exceeding target precision returns
`SQLI_OUT_OF_RANGE`. Harmless trailing zeros can be removed. Neither rounding
nor an implicit text fallback is permitted. Scale 31 and 32 are supported.

### Floating scale

The wire does not retain the original application's BigDecimal scale. Decode
therefore chooses the shortest exact coefficient, removing all decimal trailing
zeros and adjusting scale accordingly. Zero uses scale 0. For example, 123.4500
becomes coefficient 12345 / scale 2, and 1000 becomes coefficient 1 / scale -3.
This is a documented reconstruction policy, not loss of a scale supplied by the
server. Standalone native objects still preserve their application scale.

Encoding checks significant digits after ignoring coefficient trailing zeros,
then checks exponent range. The wire pair alignment can require a leading or
trailing decimal half-pair. Padding does not change the native value, but it
occupies wire storage: a 32-digit coefficient with odd alignment can require
17 groups and is rejected with `SQLI_OUT_OF_RANGE`. Thirty-one significant
digits fit at either alignment. All calculations use integer arithmetic,
including native int32 scale endpoints.

### Verified coefficient-capacity limitation

On the configured test server, casting the mathematical maximum of
DECIMAL(32,1), `10^31 - 0.1`, from either a numeric SQL literal or exponent-form
text discards its final fractional digit. Directly binding the exact 17-group
payload is rejected with SQLCODE -1226. The same alignment limitation is visible
with floating DECIMAL(32). This was established by separate receive and native
bind probes, not inferred solely from the legacy decoder.

The production encoder rejects values needing more than 16 groups instead of
reproducing that precision loss. Decoders reject nonzero groups beyond the
established capacity. The live matrix uses the largest exactly representable
aligned value for these targets, while local regression tests explicitly reject
the unrepresentable 32-digit maximum. A decoder cannot reconstruct a digit
already discarded by a server expression. Broader support requires new evidence
for the relevant server/connection mode.

## Parameter framing and consumer migration

These functions encode tuple payloads. They do not produce SQ_BIND headers,
source-scale attributes, parameter length words or transport padding. Tuple NULL
sentinels are not parameter payloads. The existing test-only native binder
provides parameter framing for probes; MONEY is sent as a decimal parameter to a
MONEY target. No independent MONEY parameter representation is claimed.

The fixed live probe now obtains decimal payloads from production codecs before
using that test framer, as it already does for temporal values. Ordinary public
binders still have their previous behavior. The existing decimal string getter
also has older formatting limits (including its scale handling); the new codec
probes decode decimal bytes directly so those limits cannot hide native codec
failures. Replacing those getters and preserving conveniences through the native
path is part of the upcoming API migration.

## Verification

Offline tests include:

- All 65,536 descriptor patterns, with exactly 592 valid encodings.
- 2,240 generated values covering every fixed precision/scale combination,
  checked against both fixed and floating targets: 2,224 exactly representable
  values round-trip and 16 pair-capacity boundary values are explicitly rejected.
- 25,344 independent single-group cases: every exponent, every nonzero radix
  group and both signs, checked against exact native coefficient/scale values.
- Fixed bytes for NULL, zero, signs, odd scales, scale 31/32 and exponent limits.
- Exact/inexact rescaling, target precision overflow, native scale endpoints,
  malformed/truncated/oversized payloads and unchanged outputs on failure.
- 10,496 deterministic byte mutations, requiring either exact canonical
  re-encoding or atomic protocol failure.
- Allocation-fault injection covering large source coefficients, replacement
  of heap-backed destinations and maximum-precision decoding without allocation.

The fixed wire probe has 32 fixtures, including temporal regression fixtures,
NUMERIC, MONEY and additional DECIMAL boundaries. The separate
`sqli_decimal_matrix` executable performs 10,432 read-only server checks: maximum
positive/negative wire-representable values, zero and NULL for every fixed descriptor, plus positive
and negative single-group values across all exponents and floating precisions.
It compares received descriptor, exact bytes, numeric value and following integer
sentinel. Generated server reference strings use integral coefficients and
exponents without a locale-dependent decimal separator. Fixed fractional
references use numeric SQL literals. The native path is locale-independent.

Normal CTests are offline. Live CTest registration requires
`SQLI_ENABLE_LIVE_TESTS=ON`; the matrix uses the existing private `SQLI_TEST_*`
configuration. Missing settings skip the live executable with status 77;
connection errors fail it. The native bind probe uses session-local temporary
tables and cleans them up. The decimal matrix only issues SELECT statements.

```sh
ctest --test-dir build -R 'decimal|temporal|native_wire_offline' --output-on-failure
./build/sqli_native_wire_contract --live
./build/sqli_native_wire_contract --live-bind
./build/sqli_decimal_matrix
```

Coverage establishes the tested server/connection mode. It does not establish
support across every server version, mode or unrelated parameter statement shape.

Validation recorded for this iteration (2026-09-08): all eleven Debug CTest
targets passed under ASan/UBSan. Targeted Release tests and explicit decimal
codec/allocation leak checks passed. All 32 fixed receive fixtures, 32 native
bind fixtures, four descriptor probes and 10,432 decimal matrix cases passed
against the configured test server with ASan, UBSan and leak detection enabled.
The coefficient-capacity boundary above is enforced by local rejection tests;
the matrix does not treat the server's truncation as an exact conversion.
