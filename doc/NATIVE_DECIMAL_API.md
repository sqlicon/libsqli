# Exact decimal value objects

This iteration implements the standalone native DECIMAL value object. DECIMAL,
NUMERIC and MONEY share its numeric representation; their SQL type identities
remain separate. It does not yet change result getters, parameter encoding or
the existing string binders. Temporal objects, descriptor migration and the
catalog component are separate subsequent work.

## Value and ownership contract

`sqli_decimal_t` is opaque and owns its coefficient. A newly created object is
SQL NULL. A non-NULL value consists of a sign, an unsigned arbitrary-precision
coefficient and an `int32_t` scale:

```text
value = sign * coefficient * 10^(-scale)
```

The coefficient uses base-1,000,000,000 limbs, least significant first. Public
import/export uses `sqli_decimal_parts_t`. Import validates and copies; export
borrows storage until the next successful mutation or destruction. Leading zero
limbs are removed, but decimal trailing zeros and scale remain intact. Zero has
no negative sign and preserves its scale. A C null pointer is an invalid argument,
not SQL NULL; NULL must be explicit.

Four coefficient limbs fit inside the object (up to 36 decimal digits). Larger
values use dynamically allocated limbs. Objects are reusable, but operations on
large coefficients may allocate temporary storage to guarantee atomic failure.
No allocation-free promise applies to those operations. Copy is deep; a copied
value remains valid after the source is changed or destroyed. Self-copy and
import from the destination's own borrowed coefficient are supported.

All operations are reentrant. Read-only operations on a shared object may run
concurrently. A shared object needs external synchronization when any thread
modifies it. No connection, mutable global arithmetic context, locale setting,
server conversion or external decimal library is involved.

## API families

| Operations | Contract |
|---|---|
| `create`, `destroy` | Create a SQL-NULL object; destroy releases all owned memory and accepts NULL |
| `copy`, `set_parts`, `get_parts` | Deep copy, validated coefficient import and borrowed export |
| `is_null`, `set_null` | Explicit NULL state; setting NULL clears numeric meaning |
| `set_i64` | Set coefficient and scale exactly, including INT64_MIN |
| `to_i64` | Convert the numeric value exactly, with integrality and range checks |
| `compare` | Numeric ordering, independent of scale differences, without rescaling operands |
| `rescale_exact` | Change scale without changing value or rounding |
| `parse`, `format` | Length-aware ASCII input and canonical scale-preserving output |

All names have the prefix `sqli_decimal_`. Public declarations and detailed
preconditions are in `include/libsqli/sqli.h`.

Fallible operations return `sqli_status`. A failed operation leaves the destination
value unchanged. New local statuses are:

| Status | Meaning |
|---|---|
| `SQLI_INVALID_ARGUMENT` | Missing required pointer, malformed decimal text or invalid coefficient limb |
| `SQLI_OUT_OF_RANGE` | Scale outside int32 range or integral value outside int64 range |
| `SQLI_INEXACT` | Integer conversion or scale reduction would discard nonzero digits |
| `SQLI_BUFFER_TOO_SMALL` | Format buffer cannot hold the text and its terminating NUL |
| `SQLI_NULL_VALUE` | A comparison, integer conversion or rescale requires a number but received SQL NULL |
| `SQLI_LIMIT_EXCEEDED` | Coefficient or input would exceed the explicit resource ceiling |
| `SQLI_ALLOC_FAIL` | Required allocation failed |

These do not synthesize SQLCODE, ISAMCODE or server diagnoses. NULL formatting,
copying and inspection succeed. Comparisons involving NULL do not invent an
ordering. Fractional integer conversions return INEXACT; integral overflow returns
OUT_OF_RANGE. Output arguments remain unchanged on error except for format's
explicit short-buffer metadata contract.

## Strings and scale

Parse accepts an optional sign, ASCII mantissa with at least one digit, optional
decimal point, and optional `E`/`e` exponent. Whitespace, grouping, embedded NUL,
NaN and infinity are rejected. Input need not be NUL-terminated. Scale is the
number of fractional mantissa digits minus the exponent, with checked arithmetic.

The formatter uses plain notation when scale is nonnegative and the adjusted
exponent (`coefficient_digits - 1 - scale`) is at least -6. Otherwise it uses
scientific notation. This retains both the exact value and scale without expanding
large exponents into enormous strings.

| Input | Output | Retained scale |
|---|---|---:|
| `+00123.4500` | `123.4500` | 4 |
| `-0.00` | `0.00` | 2 |
| `1e3` | `1E+3` | -3 |
| `1.2300e2` | `123.00` | 2 |
| `0.000001` | `0.000001` | 6 |
| `0.0000001` | `1E-7` | 7 |
| `1E+2147483648` | `1E+2147483648` | INT32_MIN |

Format takes a caller buffer, capacity, required-size output and NULL output.
`required` includes the terminating NUL for numeric values. `buffer=NULL` with
zero capacity is a successful size query. A short buffer is unchanged, but
required size and NULL state are reported. SQL NULL succeeds with `required=0`,
`is_null=true` and no write to the buffer. Empty text and the string `NULL` do not
represent SQL NULL. Passing `is_null=true` to parse explicitly sets NULL and ignores
the numeric input span.

## Resource ceilings

The coefficient is not limited to Informix precision or 128 bits. This initial
implementation documents a resource ceiling of one million coefficient digits
(`SQLI_DECIMAL_MAX_DIGITS`), excluding leading zero digits in the mantissa. Input text
is limited to that count plus 32 bytes (`SQLI_DECIMAL_MAX_TEXT`). Imported limb
arrays also have a bounded count, including leading zero limbs.

The limit prevents an input with a compact exponent from forcing uncontrolled
memory growth during exact rescaling. Scale itself still spans all of int32.
Comparisons do not materialize powers of ten. Zero can change to any scale without
coefficient growth. A nonzero coefficient requiring more digits fails explicitly;
no clipping or rounding is performed. The ceilings are independent of, and do not
claim, server capabilities.

## Checked example

```c
#include "libsqli/sqli.h"

sqli_status format_amount(char *buffer, size_t capacity, size_t *required,
                          bool *is_null)
{
    sqli_decimal_t *amount = NULL;
    sqli_status status = sqli_decimal_create(&amount);
    if (status != SQLI_OK)
        return status;
    status = sqli_decimal_parse(amount, "123.4500", sizeof("123.4500") - 1, false);
    if (status == SQLI_OK)
        status = sqli_decimal_format(amount, buffer, capacity, required, is_null);
    sqli_decimal_destroy(amount);
    return status;
}
```

## Validation

The standalone CTest `sqli_decimal_values` covers coefficient ownership, aliasing,
NULL, malformed text, bounded non-terminated input, scale endpoints, output-buffer
contracts, int64 limits, comparisons, limb-boundary rescaling, heap/inline
transitions and resource limits. The Linux allocation-failure target additionally
checks create, parse, copy, import, growth and shrink failures without production
allocator hooks. Its linker wrapping is selected by CMake and confined to tests.

```sh
ctest --test-dir build -R '^sqli_decimal_' --output-on-failure
```

No database is needed for these tests. Existing wire fixtures remain the separate
baseline for integration. Subsequent [checked decimal tuple codecs](NATIVE_DECIMAL_CODECS.md)
implement binary conversion. The [native result getter](NATIVE_DECIMAL_GETTER.md)
now reads owned decimal values; legacy getter and public bind migration remain
pending.

Validation recorded for this iteration (2026-09-07): Debug and Release builds
passed with the project warning flags. All seven Debug CTest entries passed,
including the existing unit, wire, temporal-generator and CLI suites. The final
Decimal suites contain 11 value tests and three allocation-failure tests and pass
in both build modes. Decimal tests also passed with ASan, UBSan and LeakSanitizer.
An additional deterministic 5,000-case differential check against Python Decimal
and integer arithmetic confirmed coefficients/scales, canonical-text roundtrips,
comparison, integer conversion and exact rescaling.
