# DATETIME / INTERVAL qualifier matrix

The deterministic generator and live checks are implemented in
[`test/test_temporal_matrix.c`](../test/test_temporal_matrix.c), adapted from the
original `sqlicon/docs/test_temporal_matrix.c` sketch. The separate executable
`sqli_temporal_matrix` is built when `SQLI_BUILD_TESTS=ON`.

Current validation (2026-09-07): **2,036/2,036 live cases passed** with seed
`0xc0ffee` under ASan/UBSan and LeakSanitizer for literal, text binding and
production temporal encoding with test-only native parameter framing: 6,108
rows checked, including exact receive-byte comparisons. Offline native encoding
and semantic decoding of all 2,036 independent binary fixtures also pass.
See [native wire contract](NATIVE_VALUE_WIRE_CONTRACT.md) for its scope and the
remaining production API work. The historical failures below remain fixed.

## Coverage

Every qualifier below is generated explicitly. FRACTION scale is 1 through 5;
INTERVAL leading precision is 1 through 9. An omitted leading precision defaults
to 2 and is represented by the explicit precision-2 qualifier, not an additional
qualifier. YEAR/MONTH intervals never cross into DAY/time fields.

| Family | Qualifiers | Values per qualifier | Cases |
| --- | ---: | ---: | ---: |
| DATETIME: contiguous YEAR..SECOND spans | 21 | 4 | 84 |
| DATETIME: YEAR/MONTH/DAY/HOUR/MINUTE/SECOND to FRACTION(1..5) | 30 | 4 | 120 |
| DATETIME: FRACTION TO FRACTION(1..5) | 5 | 4 | 20 |
| INTERVAL: YEAR TO YEAR, YEAR TO MONTH, MONTH TO MONTH, each precision 1..9 | 27 | 6 | 162 |
| INTERVAL: contiguous DAY..SECOND spans, each precision 1..9 | 90 | 6 | 540 |
| INTERVAL: DAY/HOUR/MINUTE/SECOND to FRACTION(1..5), each precision 1..9 | 180 | 6 | 1,080 |
| INTERVAL: FRACTION TO FRACTION(1..5) | 5 | 6 | 30 |
| **Total** | **358** | | **2,036** |

DATETIME variants are minimum, maximum, seeded random and NULL. Maximum dates use
December 31; random days are limited to 1..28 to remain valid for any month.
INTERVAL variants are zero, positive maximum, negative maximum, positive random,
negative random and NULL. Leading values stay within `0..10^precision-1`;
subordinate month/hour/minute/second fields stay within their legal bounds.
Fractions use exactly the declared scale. Negative zero is normalized to zero.

This matrix covers valid values. Deliberately invalid dates, precision overflow,
timestamp/epoch convenience bindings and temporal arithmetic are separate tests.
The low-level fixed-format `sqli_encode_datetime()` helper is not used. The
existing public temporal binders still bind text. The additional test-only native
path now parses native values and uses the production temporal codecs before
sending binary SQ_BIND parameters through the test framer. The independent
field-to-pair model remains the expected-byte oracle. See
[checked temporal codecs](NATIVE_TEMPORAL_CODECS.md); public statement migration
remains pending.

## What each live case verifies

1. Create a session-local temporary table for the exact qualifier.
2. Insert a typed SQL literal as the server reference.
3. Insert the same value with a prepared statement and the public temporal bind
   API (or `sqli_bind_null()` for NULL).
4. Insert a third row using the test-only native wire probe, then have the server compare all three stored values against the independent literal.
5. Decode all three rows with the semantic getter and compare each present field,
   fraction scale, sign and qualifier against values generated before querying.
6. Compare the public temporal string getter with a server-side LVARCHAR cast.
   Only outer padding and an optional zero before a fraction-only decimal point
   are normalized; other digits and separators must agree.
7. Compare receive bytes and the descriptor qualifier against the independent
   field-derived wire model. Verify integer sentinels and a negative YEAR(3) TO MONTH interval after the
   tested value in a mixed projection, detecting tuple-width/offset mistakes.
8. Check the result row count, destroy handles and drop the temporary table.
   Session-local tables also disappear when the connection closes.

Comparing the literal and bound rows through the same decoder alone would miss a
shared decoding defect. The independent generated fields and server comparisons
make those errors observable. SQL failures, failed cleanup, incorrect values and
configured-but-unreachable servers fail the test; they are not converted to skips.

## Build and run

```sh
cmake -S . -B build -DSQLI_BUILD_TESTS=ON -DSQLI_ENABLE_LIVE_TESTS=ON
cmake --build build --target sqli_temporal_matrix -j4
ctest --test-dir build -R '^sqli_temporal_generator$' --output-on-failure
./build/sqli_temporal_matrix --list
```

The offline CTest checks deterministic generation, qualifier uniqueness, exact
coverage counts, value bounds, numeric option parsing and semantic decoding of
all 2,036 field-derived binary fixtures. It needs no database.

For live checks, set `SQLI_TEST_HOST`, `SQLI_TEST_PORT`, `SQLI_TEST_DB`,
`SQLI_TEST_USER` and `SQLI_TEST_PASS`. Optional settings are `SQLI_TEST_SERVER`,
`SQLI_CLIENT_LOCALE` and `SQLI_DB_LOCALE`. Use a scratch database with permission
to create temporary tables. Locale settings are taken from the environment,
not hard-coded to UTF-8 for a potentially different database locale.

```sh
ctest --test-dir build -R '^sqli_temporal_live$' --output-on-failure
# Or run directly, even when live CTest registration is disabled:
./build/sqli_temporal_matrix --live
```

The live CTest is registered only with `SQLI_ENABLE_LIVE_TESTS=ON`, has labels
`live;temporal`, and a 900-second timeout. Missing required settings return 77
(CTest: skipped). Other failures return 1. Invalid invocation/options return 2.
The existing `sqli_unit` executable does not run the matrix implicitly.

## Reproduce a failure

The default seed is `0xc0ffee`. Case IDs are zero-based and stable for this
matrix definition. Random values can change with the seed; case order and
qualifiers do not. Generate the entire sequence before selecting a case so that
selection does not change its random values.

```sh
SQLI_FUZZ_SEED=0xc0ffee SQLI_FUZZ_ONLY=0 ./build/sqli_temporal_matrix --live
SQLI_FUZZ_SEED=12345 ./build/sqli_temporal_matrix --live
```

`--list` prints the case ID, type, value and variant. The old sketch's
`SQLI_FUZZ_EDGE` switch is replaced by always including both boundary and random
variants in the same run. No legal combinations are silently truncated by an
undersized case array; a capacity/count mismatch is an error.

## Initial findings on 2ca4062

The semantic-only baseline tested all 2,036 cases: 2,025 passed and 11 failed.
All 11 failures were the minimum-year DATETIME variants starting with YEAR
(case IDs 0, 4, ..., 40). For example, YEAR TO YEAR decoded `0001` as `100`;
YEAR TO MONTH decoded the year in `0001-01` as `101`. The decoder loses leading base-100
positions when it ignores the packed value's exponent.

The final suite additionally checks string getters. A focused reproduction is
case 45, DATETIME MONTH TO MONTH: the server text is `12`, while the client
string getter returns an empty string. These findings were recorded before the decoder and formatter corrections
described below.


### Baseline live result with string checks

On 2026-09-07, the ASan/UBSan build against local Informix 14.10.FC13W10 with
seed `0xc0ffee` completed all 2,036 cases: **1,595 passed, 441 failed**. No
sanitizer diagnostics were reported. Failures remain ordinary nonzero test
results; no expected-failure inversion or assertion suppression is applied.

| Failure group | Failed cases | Example |
| --- | ---: | --- |
| Minimum DATETIME year loses leading positions | 11 | Case 0: `0001` becomes year 100 |
| DATETIME string getter requires YEAR as its starting field | 135 | Case 45: MONTH TO MONTH, server `12`, client empty |
| SECOND-leading INTERVAL string adds an absent minute field | 270 | Case 1803: SECOND(3) TO FRACTION(2), server `999.99`, client `0:999.99` |
| FRACTION-only INTERVAL string adds absent minute/second fields | 25 | Case 2034: server `-.03008`, client `-0:00.03008` |

The semantic-only baseline confirms that the latter three groups have correct
semantic values on this seed and are exposed by the added string checks. Both
insertion paths are tested for passing cases; a failing case stops at its first
mismatch and continues with the next case after cleanup.

At that baseline, the existing **357 unit/mock tests** and the new offline
generator CTest passed.
Live skip behavior without credentials and rejection of invalid case IDs were
also verified. Local diagnostic logs are `/tmp/libsqli-temporal-live.log`
(semantic baseline) and `/tmp/libsqli-temporal-final.log` (baseline with string checks).


## Decoder and formatter corrections

`sqli_result_get_datetime()` now reconstructs the fixed qualifier digit positions
using the packed base-100 exponent. Omitted leading zero pairs are restored,
including the leading year positions in `0001` and fractional positions in
`.00001`. Packed input length and digit values are checked before reconstruction.

Both temporal string getters use a shared formatter that iterates from the
starting qualifier to the ending qualifier. Separators and zero padding follow
the fields actually present. Partial DATETIME values no longer require a YEAR
field; SECOND-only and FRACTION-only intervals no longer acquire extra minute
or second fields. Fraction-only values are rendered as `.digits` (with a leading
minus for negative intervals), matching the server's qualified representation.

Offline regressions in `test/test_sqli_types.c` cover year 0001, a complete
YEAR TO FRACTION(5) value, partial calendar/time fields, very small fractions,
positive/negative intervals, zero and NULL. The **360 unit/mock tests** and the
offline generator test pass under ASan/UBSan with no ignored unit tests.


### Validation after corrections

The complete matrix against Informix 14.10.FC13W10 now passes **2,036/2,036
cases**, including both insertion paths and string/semantic checks, with seed
`0xc0ffee` under ASan/UBSan. All 441 previously failing cases pass. No sanitizer
diagnostics were reported. Log: `/tmp/libsqli-temporal-fixed.log`.

The **360 unit/mock tests** and offline generator CTest pass (48.45 seconds),
with zero ignored unit tests. The existing CSDK `temporal-edges` test additionally
passes all **38 checks** using the corrected sanitizer build; its report is
`/tmp/libsqli-temporal-fixed-adapter.json`. The normal workspace build also
compiles successfully with the project's warnings-as-errors settings.
