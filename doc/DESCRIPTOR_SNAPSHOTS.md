# Immutable server descriptor snapshots

Status: implemented as the metadata foundation of the native-value API redesign.
This iteration preserves server DESCRIBE information independently of statement
and result lifetime. Native public get/bind integration and verified parameter
target association remain pending. The catalog component remains deferred.

## Public API and ownership

`include/libsqli/sqli.h` declares the opaque `sqli_descriptor_t` and these operations:

| Operation | Contract |
|---|---|
| `sqli_result_get_descriptor(result, &snapshot)` | Acquire an owned reference to the latest complete DESCRIBE received for this result. |
| `sqli_stmt_get_descriptor(stmt, &snapshot)` | Acquire the statement result's current server descriptor. |
| `sqli_descriptor_retain(snapshot)` | Acquire another reference without allocation; reference-count overflow returns `SQLI_LIMIT_EXCEEDED`. |
| `sqli_descriptor_release(snapshot)` | Release one reference; NULL is allowed. |
| `sqli_descriptor_get_field_count(snapshot, &count)` | Read the number of server-described fields. |
| `sqli_descriptor_get_field(snapshot, index, &field)` | Borrow an opaque const field view using a zero-based index. |
| `sqli_descriptor_field_get_name(field, &name)` | Borrow the complete field name. |
| `sqli_descriptor_field_get_type_owner/type_name(field, &bytes)` | Borrow the named type identity, when supplied. |
| `sqli_descriptor_field_get_type(field, &type)` | Read a supported semantic column type. |
| `sqli_descriptor_field_get_precision/scale(field, &value)` | Read validated DECIMAL/MONEY precision or fixed scale. |
| `sqli_descriptor_field_get_temporal_range(field, &range)` | Read the validated DATETIME/INTERVAL range and fractional precision. |

Successful acquisition requires a matching release. A reference remains valid
after result or statement close/destruction, connection destruction, and receipt
of a replacement descriptor. Replacement does not modify previously acquired
snapshots. Field views and byte spans borrow snapshot storage; no per-field allocation occurs.
Do not free these spans or use them after releasing the reference that keeps them
alive. A successful acquisition overwrites its output; release any previous owned
reference first or use a separate output variable.

Acquisition from a mutable statement/result requires external synchronization
with its mutation and destruction. Published snapshots are immutable. Their
getters and reference operations may run concurrently while an existing reference
keeps the object alive. Retain before handing ownership to another thread; retain
cannot resurrect a released object.

## Availability and failures

`SQLI_METADATA_UNAVAILABLE` means no complete DESCRIBE has been received for the
object. Legacy or locally synthesized column information does not manufacture a
server snapshot. A complete descriptor with zero fields is available and succeeds;
it is distinct from unavailable metadata. Metadata can also be available for a
SELECT result with no rows, before fetching its first row.

All public failures leave output arguments unchanged. Invalid pointers return
`SQLI_INVALID_ARGUMENT`; an invalid field index returns `SQLI_OUT_OF_RANGE`.
Acquisition and getters do not allocate. These statuses describe local outcomes;
they do not fabricate server SQL diagnostics.

Property getters also return `SQLI_METADATA_UNAVAILABLE` when the server did not
supply a property, its type is unknown or unsupported, or the property does not
apply to that type. Floating DECIMAL scale is unavailable, not 255. Unknown type
flags and extended identifiers are not silently discarded. Malformed known
DECIMAL or temporal qualifiers return `SQLI_PROTO_ERROR`; validation shares the
native wire codecs. No hidden SQL or text conversion is performed.

The experimental API deliberately removes `sqli_descriptor_get_info` and
`sqli_descriptor_get_names`, moves `sqli_descriptor_info_t` into the private
header, and makes `sqli_descriptor_field_t` incomplete in the public header.
Migrate field variables to `const sqli_descriptor_field_t *` and use property
getters instead of member access. Field pointers must never be freed separately.
Public DATE values, temporal ranges and native import/export parts remain plain
value structures. Legacy DATETIME/INTERVAL result structures remain transitional
until their getter migration; hiding them now would introduce a second handle API.

## Internally preserved information

| Structure | Preserved attributes |
|---|---|
| `sqli_descriptor_info_t` | Statement type and ID, raw cost bits, tuple size, field count and extended-descriptor mode. |
| `sqli_descriptor_field_t` | Raw field index, tuple offset, complete type word including flags and unknown codes, encoded length and field name. |
| Extended field attributes | Extended information word, complete type owner/name byte strings, reference, alignment and source type. |
| Original names table | Every received table byte, including terminators and unused trailing bytes. |

Numeric fields are decoded from their wire byte order without dropping bits.
`cost_raw`, unknown type codes and extended attributes remain uninterpreted.
Extended-only members are meaningful when `info.extended` is true. Legacy
encoded lengths are widened from 16 to 32 bits without changing their value.
The immutable tuple offset remains the server's original value even when the
legacy row decoder updates its separate column-location cache.

`sqli_descriptor_bytes_t` contains `data`, `length` and `available`. Bytes use
the server encoding; they are not guaranteed to be UTF-8 or NUL terminated.
Use explicit lengths. Extended owner/type strings preserve embedded NUL bytes.
A field name excludes its table terminator. A known empty name has
`available == true` and `length == 0`; it does not hide the following field's
name. Fields beyond the supplied names table have unavailable names. A zero-length
span need not have a non-NULL data pointer. Each consumed name must terminate
inside the received table; missing termination is a protocol error.

The existing `sqli_column_info` remains a transitional projection with its
127-byte display-name limit and existing type normalization. Full names are available through public snapshot views; raw
types remain available internally for future verified interpretation. An absent field name is not replaced with
an extended type owner in the snapshot, even where legacy display code uses that
fallback.

## Receive validation and resource bounds

The parser builds a private descriptor, reads and validates its fields and names,
and creates the legacy projection before publishing either. A truncated frame,
malformed name, allocation failure or storage-limit failure discards the partial
replacement and preserves the preceding descriptor and column projection.
This metadata guarantee does not promise connection reuse after a receive error
or redesign multiple-result handling.

The private `SQLI_DESCRIPTOR_MAX_BYTES` limits each snapshot's retained allocations to 16 MiB:
its object, field array, owner/type strings and original names table together.
Padding is consumed but not retained. The legacy column projection is a separate
allocation. Length arithmetic is checked before allocation; exceeding the budget
returns `SQLI_LIMIT_EXCEEDED`, and allocation failure returns `SQLI_ALLOC_FAIL`.
Holding multiple generations retains each generation's storage until its final
reference is released.

## Server fields are not input-parameter metadata

The [wire-contract probes](NATIVE_VALUE_WIRE_CONTRACT.md#observed-prepare-descriptor-roles)
show that PREPARE can describe SELECT outputs or DML target fields, with counts
that differ from the number of placeholders. This API deliberately exposes these
server fields without assigning them to input parameters by ordinal.

The legacy `param_server_types` inference and placeholder counting are unchanged
in this iteration. They must be replaced during the statement/native-bind
migration using verified target information or an explicit target contract.
Snapshot availability alone does not establish parameter metadata availability.
No ODBC types, functions or dependencies are introduced into libsqli.

## Validation

The POSIX descriptor tests exercise both descriptor layouts, every incomplete
prefix of an extended DESCRIBE, malformed and missing names, known empty names,
300-byte field names, 257-byte owner/type strings with embedded NULs, unknown type
codes, high-bit 32-bit attributes, unused table bytes, replacement and ownership
beyond result/connection lifetime. Four threads exercise concurrent reference
operations and getters. The high-bit fixtures also exposed and verified a fix
for signed-shift undefined behavior in the shared 32-bit receive helper.

Semantic getter tests cover fixed/floating decimals, invalid qualifiers, unknown
flags and extended types, temporal ranges, missing and empty names, and unchanged
outputs on failure.

Linux allocation-failure tests verify unchanged outputs and budgets, storage
limits, and allocation-free acquisition/getters. Debug tests use ASan/UBSan;
direct descriptor and allocation tests also run with LeakSanitizer enabled.
Release builds exercise the same descriptor and allocation tests without sanitizers.

Live probes retain snapshots across destruction for four PREPARE statement forms
and an empty SELECT with a 128-byte alias. The native wire executable additionally
covers 36 fixed receive and 36 native bind fixtures against the test server,
with ASan/UBSan and LeakSanitizer enabled. These probes verify the available server
configuration; they do not establish every server version or descriptor mode.

From an existing configured build with the test environment loaded:

```sh
ctest --test-dir build --output-on-failure
LSAN_OPTIONS=detect_leaks=1 ./build/sqli_descriptor_test
LSAN_OPTIONS=detect_leaks=1 ./build/sqli_descriptor_alloc_test
LSAN_OPTIONS=detect_leaks=1 ./build/sqli_native_wire_contract --live
LSAN_OPTIONS=detect_leaks=1 ./build/sqli_native_wire_contract --live-bind
```
