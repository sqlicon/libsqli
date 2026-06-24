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

### Prerequisites

To build and run `libsqli`, ensure the following packages are installed on your Linux system:
- **CMake** (>= 3.16)
- **OpenSSL** (development libraries)
- **liburiparser** (development libraries)

For example, on Debian/Ubuntu:
```bash
sudo apt-get install cmake libssl-dev liburiparser-dev build-essential
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

To run direct queries, use the `sqli_query` interface:

```c
sqli_result_t *result = NULL;
status = sqli_query(conn, "SELECT id, name, balance FROM customers", &result);
if (status != SQLI_OK) {
    fprintf(stderr, "Query failed: %s\n", sqli_error(conn));
    return;
}

// Iterate over results
while (sqli_result_next(result)) {
    int32_t id = sqli_result_get_int(result, 0);
    const char *name = sqli_result_get_string(result, 1);
    double balance = sqli_result_get_double(result, 2);

    printf("Customer #%d: %s | Balance: $%.2f\n", id, name, balance);
}

// Check for execution results / rows affected
printf("%lld rows fetched\n", (long long)sqli_result_rows_affected(result));

// Release resources
sqli_result_destroy(result);
```

---

### 3. Prepared Statements & Bindings

For security and efficiency, parameter bindings should be used:

```c
sqli_stmt_t *stmt = NULL;
int param_count = 0;
const char *sql = "INSERT INTO customers (name, balance, active) VALUES (?, ?, ?)";

status = sqli_prepare(conn, sql, &param_count, &stmt);
if (status != SQLI_OK) {
    fprintf(stderr, "Preparation failed: %s\n", sqli_error(conn));
    return;
}

// Bind positional parameters (1-indexed)
sqli_bind_string(stmt, 1, "Alice Cooper");
sqli_bind_double(stmt, 2, 250.75);
sqli_bind_bool(stmt, 3, true);

// Execute the statement
status = sqli_execute(stmt);
if (status != SQLI_OK) {
    fprintf(stderr, "Execution failed: %s\n", sqli_error(conn));
} else {
    printf("Insert completed successfully.\n");
    
    // For INSERT/UPDATE/DELETE queries:
    sqli_result_t *res = sqli_stmt_result(stmt);
    if (res && sqli_result_has_generated_serial(res)) {
         printf("Inserted SERIAL ID: %lld\n", (long long)sqli_result_generated_serial(res));
    }
}

// Clean up
sqli_stmt_destroy(stmt);
```

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
