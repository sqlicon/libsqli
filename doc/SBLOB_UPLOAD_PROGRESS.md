# Smart-LOB upload progress on failure

Issue 21 from the Informix samples exposed a progress-reporting error in
`sqli_sblob_write_stream()`: after the reader supplied 37 bytes successfully and
then failed, the LOB contained those bytes but `bytes_written` still reported
zero. The total had only been published after successful EOF.

The stream now updates the caller's counter after each confirmed write, before
returning any later reader, protocol or short-write error. The optional output
is initialized to zero even for invalid arguments. A failed reader's bytes and
bytes merely sent without a confirmed write count are excluded. A partial count
reported by the underlying writer is included before the primary error is
returned. An impossible count larger than the supplied chunk is rejected, and
counter overflow is checked before issuing another write.

The existing low-level writer currently reports a full chunk only on success;
this fix does not reinterpret its raw server acknowledgment fields as a partial
byte count. The counter measures confirmed progress under that write contract,
not durable committed data. The stream does not automatically close or release
the handle or request transaction rollback on failure. Existing write errors
and their connection diagnostics are propagated without replacement by progress
handling.

Validation:

- Five socket-based regression tests cover the reported 37-byte reader failure,
  an invalid acknowledgment after prior progress, a lost acknowledgment, an
  oversized reader result, initial EOF/error, optional output and successful
  257-byte uploads in seven-byte chunks.
- All 16 CTest suites passed.
- The original samples `lob-upload` adapter was built in an isolated temporary
  directory against the corrected library. All 310 live checks passed for BLOB
  and ASCII CLOB, including independent readback, rollback, connection reuse,
  committed locator reopen and cleanup.
- Direct regression and live adapter runs used ASan, UBSan and LeakSanitizer.

The public output contract is documented with `sqli_sblob_write_stream()` in
`include/libsqli/sqli.h`. The samples repository was not modified.
