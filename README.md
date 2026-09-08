# libsqli

An experimental, lightweight C11 substitute for parts of the IBM Informix Client Software Development Kit (CSDK). This library is reverse-engineered from the proprietary Informix **SQLI** wire protocol, allowing native client applications to communicate directly with Informix database servers without depending on the heavy CSDK library stack.

> [!WARNING]
> This is an **experimental** project. While functional, it has been built via black-box network analysis and packet tracing. Use with caution in production environments.

---

## Features

- **Protocol Coverage:** Supports SQLI wire protocol framing and message flow (`onsoctcp`, `onsocssl`, and local Unix domain socket `onipcstr` transports).
- **Security:** TLS/SSL encryption support via OpenSSL.
- **Connection Multiplexing:** Built-in connection pool (`sqli_pool_t`).
- **Flexible Connection Modes:** Setup connections programmatically using parameters or connection URIs (e.g., `informix+onsoctcp://...`).
- **Parametrized Execution:** Full support for Prepared Statements (`sqli_stmt_t`) with parameter bindings and Callable Statements (`sqli_call_t`) for stored procedures.
- **Automatic Retries:** Intelligent classification of errors to determine retry-ability with recommended delays/backoffs.
- **No CSDK Dependency:** Pure C11 codebase targeting CMake-based builds.

---

## Getting Started

### Public headers

| Header | Public API |
| --- | --- |
| `<libsqli/sqli.h>` | Connections, statements, generic results and descriptor views |
| `<libsqli/sqli_decimal.h>` | DECIMAL/NUMERIC/MONEY values, result access, binding and decimal metadata |
| `<libsqli/sqli_temporal.h>` | DATE/DATETIME/INTERVAL values, result access, binding, temporal metadata and timestamp/epoch conveniences |
| `<libsqli/sqli_sblob.h>` | Smart-LOB handles, options, locator binding, streaming and low-level I/O |

Each domain header includes the core header and can be included alone. The core
header does not include the domain headers. Add explicit includes when migrating;
all functions remain in the same library. See the
[native temporal integration](doc/NATIVE_TEMPORAL_INTEGRATION.md) for the new
opaque DATETIME/INTERVAL getters and native binders. Text binding remains available
as `sqli_bind_date_string`, `sqli_bind_decimal_string`,
`sqli_bind_datetime_string` and `sqli_bind_interval_string`. See the
[API consistency migration](doc/API_CONSISTENCY.md) for native DATE/DECIMAL
binding, checked buffer contracts, statement fetch status and opaque Smart-LOB
handle ownership. The [checked query-output path](doc/CLI_VALUE_OUTPUT.md)
preserves long text and renders explicit LOB placeholders in every output mode.

### Prerequisites

To build and run `libsqli`, ensure the following packages are installed on your Linux system:
- **CMake** (>= 3.16)
- **OpenSSL** (development libraries)

For example, on Debian/Ubuntu:
```bash
sudo apt-get install cmake libssl-dev build-essential
```

### Building the Project

```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
```

Running unit tests:
```bash
ctest --output-on-failure
```

---

## Code Tutorial

Below is a quick guide on how to integrate `libsqli` into your C application.

### 1. Connecting to the Database

You can connect either programmatically using connection parameters or using a connection URI.

#### Programmatic Connection

```c
#include <stdio.h>
#include <libsqli/sqli.h>

int main() {
    sqli_conn_t *conn = NULL;
    sqli_status status = sqli_create(&conn);
    if (status != SQLI_OK) {
        fprintf(stderr, "Failed to create connection handle\n");
        return 1;
    }

    sqli_connect_params params = {
        .server = "ol_tli_tcp",            // INFORMIXSERVER name
        .hostname = "127.0.0.1",           // Host IP or name
        .service = "9088",                 // Port
        .database = "customers_db",        // Database name
        .username = "informix",            // Credentials
        .password = "my-secret-password",  // Credentials
        .client_locale = "en_US.UTF-8",
        .db_locale = "en_US.8859-1",
        .ssl_enable = false                // True to use TLS/SSL
    };

    status = sqli_connect(conn, &params);
    if (status != SQLI_OK) {
        fprintf(stderr, "Connection failed: %s\n", sqli_error(conn));
        sqli_destroy(conn);
        return 1;
    }

    printf("Successfully connected using parameters!\n");
    
    sqli_close(conn);
    sqli_destroy(conn);
    return 0;
}
```

#### URI-based Connection

```c
const char *uri = "informix+onsoctcp://127.0.0.1:9088/customers_db?INFORMIXSERVER=ol_tli_tcp";
status = sqli_connect_uri(conn, uri, "informix", "my-secret-password");
```

---

### 2. Executing Queries

Use status-returning fetch/get functions to distinguish values, SQL NULL and
errors. This example uses caller-owned text and a reusable native decimal;
include `<libsqli/sqli_decimal.h>` alongside `<libsqli/sqli.h>` and the standard
`<stdlib.h>`, `<stdio.h>` and `<inttypes.h>` headers.

```c
/* conn is borrowed and remains open throughout this function. */
sqli_status print_customers(sqli_conn_t *conn)
{
    sqli_result_t *result = NULL;
    sqli_decimal_t *balance = NULL;
    char *name = NULL;
    sqli_status status = sqli_decimal_create(&balance);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_query(conn,
        "SELECT id, name, balance FROM customers", &result);
    if (status != SQLI_OK)
        goto cleanup;

    while ((status = sqli_result_fetch(result)) == SQLI_OK) {
        int32_t id = 0;
        bool id_null, name_null, balance_null;
        size_t required;
        status = sqli_result_get_int(result, 0, &id, &id_null);
        if (status != SQLI_OK)
            break;
        status = sqli_result_get_string_len(result, 1, NULL, 0,
                                             &required, &name_null);
        if (status != SQLI_OK)
            break;
        if (!name_null) {
            name = malloc(required);
            if (name == NULL) {
                status = SQLI_ALLOC_FAIL;
                break;
            }
            status = sqli_result_get_string_len(result, 1, name, required,
                                                 &required, &name_null);
            if (status != SQLI_OK)
                break;
        }
        status = sqli_result_get_decimal(result, 2, balance);
        if (status != SQLI_OK)
            break;
        /* A short buffer reports its required size without truncating. */
        char amount[128];
        status = sqli_decimal_format(balance, amount, sizeof(amount),
                                      &required, &balance_null);
        if (status != SQLI_OK)
            break;
        if (id_null)
            fputs("NULL", stdout);
        else
            printf("%" PRId32, id);
        printf(" | %s | %s\n", name_null ? "NULL" : name,
               balance_null ? "NULL" : amount);
        free(name);
        name = NULL;
    }
    if (status == SQLI_EOF)
        status = SQLI_OK;
    if (status == SQLI_OK && (fflush(stdout) != 0 || ferror(stdout)))
        status = SQLI_IO_ERROR;
cleanup:
    free(name);
    sqli_result_destroy(result);
    sqli_decimal_destroy(balance);
    return status;
}
```

The decimal remains exact until explicitly formatted or converted. Buffers and
native values belong to the caller. Legacy pointer-returning string getters
remain available, but their borrowed storage and NULL/error conventions make
them unsuitable as the default application interface.

---

### 3. Prepared Statements & Bindings

All parameter and result indices are **zero-based `size_t`**, including callable
statements. Bindings copy their input; owned native values may be destroyed after
binding. Explicit decimal targets describe the transmitted source type.

```c
sqli_status insert_customer(sqli_conn_t *conn)
{
    sqli_stmt_t *stmt = NULL;
    sqli_decimal_t *balance = NULL;
    const sqli_decimal_target_t target = {.precision = 10, .scale = 2};
    const size_t name_parameter = 0, balance_parameter = 1, active_parameter = 2;
    int parameter_count;
    sqli_status status = sqli_decimal_create(&balance);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_decimal_parse(balance, "250.75", 6, false);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_prepare(conn,
        "INSERT INTO customers (name, balance, active) VALUES (?, ?, ?)",
        &parameter_count, &stmt);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_bind_string(stmt, name_parameter, "Alice Cooper");
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_bind_decimal(stmt, balance_parameter, balance, &target);
    if (status != SQLI_OK)
        goto cleanup;
    status = sqli_bind_bool(stmt, active_parameter, true);
    if (status == SQLI_OK)
        status = sqli_execute(stmt);
cleanup:
    sqli_stmt_destroy(stmt);
    sqli_decimal_destroy(balance);
    return status;
}
```

Report local errors using the returned status, for example
`fprintf(stderr, "insert_customer: %s (%s)\n", sqli_status_name(status),
sqli_status_description(status));`. Connection SQL diagnostics provide additional
server context when present; local parse/get/bind failures need not update them.
See [Application API contracts and migration](doc/APPLICATION_API.md) for scalar
conversion rules, domain headers and Smart-LOB reader ownership.

---

### 4. Transactions

Transactions can be managed cleanly using explicit transaction functions:

```c
// Enable autocommit mode toggle or explicitly begin/commit
if (sqli_begin(conn) == SQLI_OK) {
    
    status = sqli_query(conn, "UPDATE accounts SET balance = balance - 100 WHERE id = 1", NULL);
    if (status != SQLI_OK) {
        sqli_rollback(conn);
        return;
    }
    
    status = sqli_query(conn, "UPDATE accounts SET balance = balance + 100 WHERE id = 2", NULL);
    if (status != SQLI_OK) {
        sqli_rollback(conn);
        return;
    }

    sqli_commit(conn);
    printf("Transaction committed successfully!\n");
}
```

---

## Protocol Internals

For detailed specifications of the message structures, handshake packets, and framing layers discovered during reverse-engineering, see the documentation in [doc/PROTOCOL.md](doc/PROTOCOL.md).

## License

This project is licensed under the terms of the license file included in the repository root. See [LICENSE](LICENSE) for details.
