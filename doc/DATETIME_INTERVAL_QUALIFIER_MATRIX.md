# Informix DATETIME / INTERVAL — Complete Qualifier Matrix

Reference for the type generator. Goal: hit every legal combination exactly once,
with values at the boundaries. The "tested in libsqli?" column reflects the current
state of the test/ suite (only 3 combinations present).

## Field levels (Informix TU codes)

Informix numbers the time fields internally. A type's qualifier is a pair
(start, end) drawn from these levels. FRACTION additionally carries a scale of 1..5.

| Field    | TU code | Share in wire BCD |
|----------|---------|-------------------|
| YEAR     | 0       | 4 digits (first) or free |
| MONTH    | 2       | 2 digits          |
| DAY      | 4       | 2 digits (or first-field-width for INTERVAL) |
| HOUR     | 6       | 2 digits          |
| MINUTE   | 8       | 2 digits          |
| SECOND   | 10      | 2 digits          |
| FRACTION | 11..15  | scale = code-10, i.e. FRACTION(1)=11 … FRACTION(5)=15 |

The qualifier byte in the wire protocol is conventionally `(start<<4) | end`, or in
Informix `(first_field_width_code, end_code)` — the library reads it via
`sqli_qual_start/end/length`. For the generator the SQL level is enough: you write
the type as text and the server assigns the qualifier.

---

## DATETIME — all legal start→end (start ≤ end, contiguous)

DATETIME must cover a contiguous range from a coarser to a finer (or equal) field,
YEAR..FRACTION.

| # | Type                             | tested in libsqli? | bug risk |
|---|----------------------------------|--------------------|----------|
| 1 | DATETIME YEAR TO YEAR            | no                 | medium   |
| 2 | DATETIME YEAR TO MONTH           | no                 | medium   |
| 3 | DATETIME YEAR TO DAY             | no                 | medium   |
| 4 | DATETIME YEAR TO HOUR            | no                 | medium   |
| 5 | DATETIME YEAR TO MINUTE          | no                 | medium   |
| 6 | DATETIME YEAR TO SECOND          | **yes**            | low      |
| 7 | DATETIME YEAR TO FRACTION(1)     | no                 | **high** |
| 8 | DATETIME YEAR TO FRACTION(2)     | no                 | **high** |
| 9 | DATETIME YEAR TO FRACTION(3)     | no                 | **high** |
|10 | DATETIME YEAR TO FRACTION(4)     | no                 | **high** |
|11 | DATETIME YEAR TO FRACTION(5)     | no                 | **high** |
|12 | DATETIME MONTH TO MONTH          | no                 | medium   |
|13 | DATETIME MONTH TO DAY            | no                 | medium   |
|14 | DATETIME MONTH TO HOUR           | no                 | medium   |
|15 | DATETIME MONTH TO MINUTE         | no                 | medium   |
|16 | DATETIME MONTH TO SECOND         | no                 | medium   |
|17 | DATETIME DAY TO DAY              | no                 | medium   |
|18 | DATETIME DAY TO HOUR             | no                 | medium   |
|19 | DATETIME DAY TO MINUTE           | no                 | medium   |
|20 | DATETIME DAY TO SECOND           | no                 | medium   |
|21 | DATETIME DAY TO FRACTION(3)      | no                 | **high** |
|22 | DATETIME HOUR TO HOUR            | no                 | medium   |
|23 | DATETIME HOUR TO MINUTE          | no                 | medium   |
|24 | DATETIME HOUR TO SECOND          | no                 | medium   |
|25 | DATETIME HOUR TO FRACTION(5)     | no                 | **high** |
|26 | DATETIME MINUTE TO MINUTE        | no                 | medium   |
|27 | DATETIME MINUTE TO SECOND        | no                 | medium   |
|28 | DATETIME MINUTE TO FRACTION(3)   | no                 | **high** |
|29 | DATETIME SECOND TO SECOND        | no                 | medium   |
|30 | DATETIME SECOND TO FRACTION(5)   | no                 | **high** |
|31 | DATETIME FRACTION TO FRACTION    | no                 | **high** |
|32 | DATETIME FRACTION(1) TO FRACTION(5) | no              | **high** |

Note: `sqli_encode_datetime()` writes a fixed 14 digits (YYYYMMDDHHMMSS) per its
header and states "frac reserved". The **binary bind path** therefore likely does
NOT cover #7–#11, #21, #25, #28, #30–#32. When binding as a string
(`sqli_bind_datetime`) it may work — precisely this discrepancy is a test target.

---

## INTERVAL — two separate classes (must not overlap)

INTERVAL allows only two ranges. YEAR-MONTH and DAY-FRACTION cannot be mixed
(no `MONTH TO DAY` for INTERVAL). The first field carries a precision (digit count),
default 2.

### Class A: YEAR-MONTH
| # | Type                          | tested in libsqli? | bug risk |
|---|-------------------------------|--------------------|----------|
|33 | INTERVAL YEAR(p) TO YEAR       | no                 | medium   |
|34 | INTERVAL YEAR(p) TO MONTH      | partial (3-2)      | low      |
|35 | INTERVAL MONTH(p) TO MONTH     | no                 | medium   |

### Class B: DAY-FRACTION
| # | Type                             | tested in libsqli? | bug risk |
|---|----------------------------------|--------------------|----------|
|36 | INTERVAL DAY(p) TO DAY            | no                 | medium   |
|37 | INTERVAL DAY(p) TO HOUR           | no                 | medium   |
|38 | INTERVAL DAY(p) TO MINUTE         | no                 | medium   |
|39 | INTERVAL DAY(p) TO SECOND         | **yes**            | low      |
|40 | INTERVAL DAY(p) TO FRACTION(1..5) | no                | **high** |
|41 | INTERVAL HOUR(p) TO HOUR          | no                 | medium   |
|42 | INTERVAL HOUR(p) TO MINUTE        | no                 | medium   |
|43 | INTERVAL HOUR(p) TO SECOND        | no                 | medium   |
|44 | INTERVAL HOUR(p) TO FRACTION(3)   | no                 | **high** |
|45 | INTERVAL MINUTE(p) TO MINUTE      | no                 | medium   |
|46 | INTERVAL MINUTE(p) TO SECOND      | no                 | medium   |
|47 | INTERVAL MINUTE(p) TO FRACTION(5) | no                | **high** |
|48 | INTERVAL SECOND(p) TO SECOND      | no                 | medium   |
|49 | INTERVAL SECOND(p) TO FRACTION(1..5) | no             | **high** |
|50 | INTERVAL FRACTION TO FRACTION     | no                 | **high** |
|51 | INTERVAL FRACTION(1..5)           | no                 | **high** |

First-field precision p: for DAY/HOUR/... typically 1..9. Test boundaries:
p=1 (overflows easily), p=9 (max), default (omit).

---

## Boundary values per field (for value generation)

- YEAR:     1, 9999  (Informix DATETIME year 1..9999)
- MONTH:    1, 12
- DAY:      1, 28/29/30/31 depending on month  → generator uses 28 as safe, 31 as edge
- HOUR:     0, 23
- MINUTE:   0, 59
- SECOND:   0, 59  (Informix has no leap-second 60)
- FRACTION: 0, and the maximum per scale (F1=9, F2=99, F3=999, F4=9999, F5=99999)
- INTERVAL first-field width: 0, and 10^p - 1 (overflow candidate when p is too small)
- Sign: INTERVAL can be negative → always test negative values as well
- NULL:  each combination additionally as NULL (separate insert row)
