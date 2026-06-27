# Testing

This document describes the runtime requirements for the `libsqli` test suite,
especially the tests that are intentionally skipped when the environment does
not provide the required capabilities.

## Build And Run

Configure and build:

```bash
cmake -S . -B build-live
cmake --build build-live -j2
```

Run the unit and integration test binary:

```bash
./build-live/sqli_test
```

In some ASan-enabled environments, the test runner may require:

```bash
ASAN_OPTIONS=verify_asan_link_order=0 ./build-live/sqli_test
```

## Ignored Tests

The suite can complete successfully with ignored tests. An ignored test is not
a functional failure. It indicates that the current runtime does not satisfy
the prerequisites for that specific test.

There are two common skip categories.

### 1. Local Mock Server Tests

Several integration tests in `test/test_integration.c` start a local mock TCP
server on `127.0.0.1` using a kernel-assigned port.

These tests require:

- IPv4 loopback `127.0.0.1` to be available
- permission to create local TCP listener sockets
- permission to `bind()` to an ephemeral local port
- permission to `listen()` and `accept()` connections
- working local thread and socket support

If the runtime does not allow this, the tests are skipped with:

```text
local TCP listeners unavailable in this runtime
```

The affected tests are:

- `test_connect_success`
- `test_connect_reject`
- `test_connect_redirect`
- `test_connect_bad_done`
- `test_pool_create_acquire_release_destroy`
- `test_pool_acquire_timeout_when_busy`
- `test_pool_acquire_wakes_after_release`
- `test_pool_reconnect_after_borrower_closed_connection`
- `test_query_success_multi_row`
- `test_query_success_ddl`
- `test_query_error_response`
- `test_txn_begin_success`
- `test_txn_commit_success`
- `test_txn_rollback_success`
- `test_close_ready_sends_exit`
- `test_close_twice_safe`
- `test_result_destroy_after_query`
- `test_query_after_close`

### 2. Live Informix Test

The live integration test `test_query_live_systables` connects to a real
Informix server and is skipped unless all required environment variables are
set.

Required variables:

- `SQLI_TEST_HOST`
- `SQLI_TEST_PORT`
- `SQLI_TEST_DB`
- `SQLI_TEST_USER`
- `SQLI_TEST_PASS`

Example:

```bash
export SQLI_TEST_HOST=127.0.0.1
export SQLI_TEST_PORT=9088
export SQLI_TEST_DB=sysmaster
export SQLI_TEST_USER=informix
export SQLI_TEST_PASS=secret
```

Additional requirements:

- the Informix server must be reachable from the test environment
- the credentials must be valid
- the selected database must exist

If the variables are missing, the test is skipped with:

```text
SQLI_TEST_HOST/PORT/DB/USER/PASS environment variables not set - skipping live test
```

If the variables are present but the server cannot be reached, the test is also
skipped.

## Expected Outcome

A successful run may therefore look like:

```text
291 Tests 0 Failures 19 Ignored
OK
```

This means the available tests passed and only environment-dependent tests were
skipped.

## Maintainer Scripts

The repository also contains several maintainer-oriented shell scripts under
`tools/`. These are not part of the default `sqli_test` run. They are used for
live verification, benchmarking, and release-oriented report generation.

### `tools/generate_release_artifacts.sh`

Generates release-oriented verification reports in
`doc/status/artifacts/`.

Supported modes:

- `sanitizer`: configures a sanitizer-enabled build, runs `ctest`, and writes a
  sanitizer report
- `performance`: runs basic live performance measurements with `sqliconn` and
  `sqli_export_csv`, then writes baseline and gate reports
- `all`: runs both modes

Examples:

```bash
tools/generate_release_artifacts.sh sanitizer
tools/generate_release_artifacts.sh performance
tools/generate_release_artifacts.sh all
```

Required for `performance` mode:

- `SQLI_BENCH_HOST`
- `SQLI_BENCH_PORT`
- `SQLI_BENCH_DB`
- `SQLI_BENCH_SERVER`
- `SQLI_BENCH_USER`
- `SQLI_BENCH_PASS`

Optional:

- `SQLI_BUILD_DIR`
- `SQLI_SANITIZER_BUILD_DIR`
- `SQLI_CLIENT_LOCALE`
- `SQLI_DB_LOCALE`
- `SQLI_SLO_CQ_P95_MS`
- `SQLI_SLO_FETCH_P95_MS`
- `SQLI_SLO_FETCH_MIN_MIBPS`

### `tools/generate_stress_envelope_report.sh`

Runs live stress and fetch-throughput scenarios against a real Informix
instance and writes a stress envelope report plus raw logs under
`doc/status/artifacts/`.

It uses:

- `sqli_stress` for concurrent query load
- `sqli_export_csv` for repeated long-running fetch loops

Required variables:

- `SQLI_BENCH_HOST`
- `SQLI_BENCH_PORT`
- `SQLI_BENCH_DB`
- `SQLI_BENCH_SERVER`
- `SQLI_BENCH_USER`
- `SQLI_BENCH_PASS`

Optional:

- `SQLI_BUILD_DIR`
- `SQLI_CLIENT_LOCALE`
- `SQLI_DB_LOCALE`
- `SQLI_STRESS_LEVELS`
- `SQLI_STRESS_ITERATIONS`
- `SQLI_STRESS_SQL`
- `SQLI_FETCH_SECONDS`

Example:

```bash
tools/generate_stress_envelope_report.sh
```

### `tools/run_live_must_type_suite.sh`

Runs a live type-coverage and prepared-binding verification suite against a
real Informix database. The script creates a temporary table, inserts fixture
rows, validates query predicates across multiple SQL types, runs prepared probe
checks, and writes a Markdown report to `doc/status/artifacts/`.

It uses:

- `sqliconn`
- `sqli_prepared_probe`

Required variables:

- either `SQLI_LIVE_HOST`, `SQLI_LIVE_PORT`, `SQLI_LIVE_DB`,
  `SQLI_LIVE_SERVER`, `SQLI_LIVE_USER`, `SQLI_LIVE_PASS`
- or the equivalent `SQLI_BENCH_*` variables

Optional:

- `SQLI_BUILD_DIR`
- `SQLI_CLIENT_LOCALE`
- `SQLI_DB_LOCALE`

Example:

```bash
tools/run_live_must_type_suite.sh
```

### Notes

- These scripts expect a working local build tree, by default under `build/`.
- They connect to real databases and are therefore environment-dependent.
- They are intended for maintainer validation and benchmarking, not for the
  normal unit-test path.
