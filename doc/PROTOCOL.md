# SQLI Protocol Overview (Public Reference)

This document describes the fundamentals of the Informix SQLI wire protocol as implemented in `libsqli`. The details here are derived from black-box tracing and network packet analysis of standard SQLI clients.

---

## 1. Session Layer (SL) Framing

For TCP-based connections (`onsoctcp` / `onsocssl`), every message is encapsulated in a 6-byte Session Layer (SL) header:

| Field | Size (Bytes) | Type | Description |
|---|---|---|---|
| `pdu_size` | 2 | Big-Endian uint16 | Total size of the packet (including this 6-byte header) |
| `sl_type` | 1 | uint8 | Message type (e.g., `1` for CONREQ, `2` for CONACC, `3` for CONREJ) |
| `sl_attr` | 1 | uint8 | Protocol identifier (always `60` for SQLI) |
| `sl_opts` | 2 | Big-Endian uint16 | Optional flags / options (usually `0`) |

---

## 2. Unix Domain Socket Handshake (`onipcstr`)

When connecting locally via Unix domain sockets, the handshake deviates from the standard TCP SL framing:

1. **Greeting:** The server immediately sends a 12-byte binary greeting.
2. **Preamble:** The client sends an IPC preamble string consisting of:
   * **Magic Header (10 bytes):** Character prefix `sq` followed by 8 Base64 characters. These 8 Base64 characters encode a 6-byte binary header containing:
     * `rest_len` (16-bit big-endian): Length of the rest of the preamble string (excluding `sq` and the length field itself).
     * `capabilities` (16-bit big-endian): Fixed to `0x013D`.
     * `padding` (16-bit big-endian): Fixed to `0x0000`.
   * **Text Header:** A string containing client environment variables (e.g., `CLIENT_LOCALE`, `DB_LOCALE`, `NODEFDAC`) and product information.
   * **Separator:** A single `:` character.
   * **Base64 Body:** Base64-encoded binary payload containing platform information (e.g., process ID, CWD/home directory, hostname).
3. **Response:** The server returns a frame consisting of a 2-byte big-endian length field followed by the raw CONACC/CONREJ message body.

---

## 3. SQLI Messages

After the connection handshake, all communications proceed via SQLI messages (using SL framing on TCP sockets). Important message structures include:

* **SQ_PROTOCOLS (126):** Capability exchange between client and server.
* **SQ_INFO (81):** Transmission of client-side configuration parameters.
* **SQ_DBOPEN (36) / SQ_DBCLOSE (37):** Commands to open and close databases.
* **SQ_PREPARE (2) / SQ_BIND (5) / SQ_EXECUTE (7):** Execution of dynamic statements and query parametrization.
* **SQ_NFETCH (9) / SQ_SFETCH (23):** Retrieval of result tuples.
* **SQ_DONE (15) / SQ_ERR (13):** Transaction execution boundaries and diagnostics reporting.
