# libsqli Public Documentation

This directory contains the public reference documentation for the `libsqli` library.

## Contents

* [SQLI Wire Protocol Reference](PROTOCOL.md) - Details on SL framing, Unix domain socket preambles, and message formats derived from tracing the SQLI protocol.
* [Connection Performance & Transports](CONNECTION_PERFORMANCE.md) - Deep-dive comparison of `onsoctcp` vs `onipcstr`, Linux `perf` profiling data, handshake analysis, bottlenecks, and optimization strategies.

* [DATETIME / INTERVAL Qualifier Matrix](DATETIME_INTERVAL_QUALIFIER_MATRIX.md) - Deterministic generation, live coverage and reproduction of temporal boundary failures.
