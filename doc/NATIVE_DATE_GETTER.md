# Native DATE result getter

Public declarations: `<libsqli/sqli_temporal.h>` (explicit include required).

The experimental DATE result API now uses the native calendar type directly:

```c
sqli_status sqli_result_get_date(sqli_result_t *result, size_t index,
                                sqli_date_t *out);
```

This replaces the former `int` index and `sqli_date_value` destination. The old
public structure, including `days_since_ifx_epoch`, has been removed. Rebuild
consumers and replace declarations with `sqli_date_t`; access `year`, `month`,
`day` and `is_null`. All in-tree callers, including the stress tool and DATE input
to the legacy timestamp convenience, have been migrated. No compatibility typedef
or alternate native DATE getter is introduced.

## Checked calendar reads

The zero-based getter requires a successfully positioned row and a DATE source
column. Wrong types return `SQLI_TYPE_MISMATCH`, including NULL values of other
types. Invalid indices return `SQLI_OUT_OF_RANGE`; NULL arguments return
`SQLI_INVALID_ARGUMENT`. Missing row state or transaction invalidation returns
`SQLI_INVALID_STATE`; a recorded fetch failure is propagated.

The getter validates the declared four-byte field width and cached span, then calls the
production DATE decoder. Non-NULL values must fit years 1..9999 in the proleptic
Gregorian calendar. Out-of-range day counts and malformed payloads return
`SQLI_PROTO_ERROR`. The wire epoch stays inside the codec. All failures preserve
the destination, and no connection diagnostic is fabricated.

SQL NULL returns `SQLI_OK` and a null calendar value with zeroed fields. The
native getter leaves the legacy `was_null` flag unchanged; use `out->is_null`.
DATE needs no creation/destruction or allocation. The copied value remains valid
after row advancement, EOF and result destruction. Synchronize result access and
destination mutation externally; destination storage must not overlap the result.

DATE and DECIMAL now share one internal current-row span validator. The decimal
value decoder and ownership contract remain unchanged.

## ISO and timestamp conveniences

The existing `sqli_result_get_date_string()` remains available. It now calls the
native getter and `sqli_date_format()`, producing exact `YYYY-MM-DD` text. Its
legacy thread-local return buffer and empty-string-on-NULL/error interface remain;
successful calls update the legacy NULL flag. It no longer falls back to displaying
an unchecked raw day count. For explicit status, NULL and buffer handling, call
the native getter followed by `sqli_date_format()`.

The DATE branch of `sqli_result_get_timestamp()` uses the new calendar value and
retains its existing midnight completion. The broader timestamp/string/epoch
convenience APIs remain transitional; this iteration does not change their full
conversion contracts. DATETIME and INTERVAL getter migration, native binding and
the final text convenience API migration remain pending. Catalog work is deferred.

## Verification

Five dedicated tests cover owned calendar values, ISO output, DATE-to-timestamp
convenience, NULL/type distinctions, values outside the supported calendar,
invalid descriptors/spans, failed fetches, rollback invalidation and unchanged
destinations. Fault injection confirms both structured reads and DATE text
formatting require no allocation after row positioning.

Four independent DATE fixtures extend the native wire probes: 0001-01-01,
9999-12-31, 2000-02-29 and 1900-03-01. Their fixed byte expectations are checked
against live server expressions and native bound payloads, alongside the existing
epoch, pre-epoch, Unix epoch and NULL fixtures. Structured and legacy ISO text
outputs are compared for every DATE fixture.

All 17 CTest suites and the focused DATE/DECIMAL Release tests passed. Debug and
Release builds include all enabled tools. All 36 live receive and 36 live bind
fixtures passed; direct Debug DATE tests and live probes used ASan, UBSan and
LeakSanitizer. The full decimal/temporal live matrices were not rerun because their
codecs are unchanged; this iteration adds targeted DATE integration evidence.
