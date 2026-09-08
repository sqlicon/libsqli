#ifndef SQLI_H
#define SQLI_H

/**
 * @file sqli.h
 * @brief Public C11 API for the libsqli Informix database client.
 *
 * This header exposes an experimental, application-facing interface for:
 * - connection lifecycle management
 * - statement execution (direct and prepared)
 * - result accessors and type helpers
 * - transaction control
 * - operational diagnostics and retry advice
 *
 * Internal transport and protocol details are intentionally abstracted away.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup sqli_status Status Codes
 *  @{ */

typedef enum {
    SQLI_OK = 0,
    SQLI_ERR,
    SQLI_EOF,
    SQLI_TIMEOUT,
    SQLI_AUTH_FAIL,
    SQLI_PROTO_ERROR,
    SQLI_IO_ERROR,
    SQLI_ALLOC_FAIL,
    SQLI_INVALID_STATE,
    SQLI_INVALID_ARGUMENT,
    SQLI_OUT_OF_RANGE,
    SQLI_INEXACT,
    SQLI_BUFFER_TOO_SMALL,
    SQLI_NULL_VALUE,
    SQLI_LIMIT_EXCEEDED,
    SQLI_METADATA_UNAVAILABLE,
    SQLI_TYPE_MISMATCH
} sqli_status;
/** @} */

/** Opaque connection handle. */

typedef struct sqli_conn sqli_conn_t;
typedef struct sqli_pool sqli_pool_t;

typedef enum {
    SQLI_CURSOR_FORWARD_ONLY = 1003,
    SQLI_CURSOR_SCROLL_INSENSITIVE = 1004
} sqli_cursor_type;

typedef enum {
    SQLI_CURSOR_HOLD_OVER_COMMIT = 1,
    SQLI_CURSOR_CLOSE_AT_COMMIT = 2
} sqli_cursor_holdability;

/* ----------------------------------------------------------------
 * Connection lifecycle
 * ---------------------------------------------------------------- */

/**
 * @brief Allocate a new connection handle.
 * @param[out] conn Destination pointer receiving the allocated handle.
 * @return SQLI_OK on success, SQLI_ALLOC_FAIL on memory exhaustion.
 */
sqli_status sqli_create(sqli_conn_t **conn);

/**
 * @brief Release a connection handle and all owned resources.
 * @param[in,out] conn Connection handle (NULL is allowed).
 */
void sqli_destroy(sqli_conn_t *conn);

/* ----------------------------------------------------------------
 * Error retrieval
 * ---------------------------------------------------------------- */

/**
 * @brief Return the last error message for a connection.
 * @param[in] conn Connection handle.
 * @return Pointer to an internal message buffer, or NULL if no error exists.
 * @note The returned pointer is invalidated by the next API call on @p conn.
 */
const char *sqli_error(sqli_conn_t *conn);

/**
 * @brief Return the numeric code of the last error.
 * @param[in] conn Connection handle.
 * @return Driver-specific numeric error code.
 */
int sqli_errno(sqli_conn_t *conn);

/* ----------------------------------------------------------------
 * Structured error info
 * ---------------------------------------------------------------- */

typedef struct {
    bool has_error;                 /* true if last operation ended with error */
    sqli_status status;             /* library status that triggered the error */
    int sqlcode;                    /* Informix SQLCODE, if available */
    int isamcode;                   /* Informix ISAM code, if available */
    char sqlstate[6];               /* 5-char SQLSTATE + NUL (if present) */
    uint16_t opcode;                /* SQLI opcode related to this error (if known) */
    char opcode_name[24];           /* human-readable opcode label */
    char context[48];               /* connection/query phase context */
    uint16_t unknown_opcode;        /* last unknown opcode observed in dispatch */
    uint32_t unknown_opcode_count;  /* number of unknown opcodes seen */
    char server_message[256];       /* server-provided message payload */
    char sql_message[384];          /* SQL message text resolved from catalog */
    char isam_message[384];         /* ISAM message text resolved from catalog */
    char message[512];              /* final composed message */
} sqli_error_info;

/**
 * @brief Retrieve structured error details for the last operation.
 * @param[in] conn Connection handle.
 * @param[out] out Output structure.
 * @return SQLI_OK on success.
 */
sqli_status sqli_error_get_info(sqli_conn_t *conn, sqli_error_info *out);

/* Upper bound, in bytes, on any single expanded message from the Informix catalog (excl. NUL). */
#define SQLI_ERRMSG_MAX_LEN 347

/**
 * @brief Look up an Informix SQLCODE or ISAM error message by numeric code.
 *
 * Resolves standard Informix SQL error codes (e.g. -206) and ISAM error codes
 * (e.g. 107 or -107) from the embedded 3,412-message Informix catalog.
 *
 * @param[in] code SQLCODE (e.g. -206) or ISAM error code (e.g. 107 or -107).
 * @param[out] out Caller-supplied buffer to receive the NUL-terminated message.
 * @param[in] outsz Size of the output buffer in bytes (recommended >= SQLI_ERRMSG_MAX_LEN + 1).
 * @return Number of bytes written excluding NUL on success,
 *         -1 if the code was not found in the catalog,
 *         -2 if the buffer was too small.
 */
int sqli_error_message_lookup(int32_t code, char *out, size_t outsz);

typedef enum {
    SQLI_ERROR_CLASS_NONE = 0,
    SQLI_ERROR_CLASS_AUTH,
    SQLI_ERROR_CLASS_NETWORK,
    SQLI_ERROR_CLASS_PROTOCOL,
    SQLI_ERROR_CLASS_SERVER,
    SQLI_ERROR_CLASS_DATA,
    SQLI_ERROR_CLASS_UNKNOWN
} sqli_error_class;

/*
 * Classify the last error and decide if it is retryable.
 * retryable is true for transient failures, false otherwise.
 */
sqli_status sqli_error_classify(sqli_conn_t *conn, sqli_error_class *out_class,
                                bool *retryable);
bool sqli_error_is_retryable(sqli_conn_t *conn);
/*
 * Recommend retry behavior for the last error.
 * attempt is the 0-based retry attempt counter.
 * out_should_retry is true if a retry is recommended, false otherwise.
 * out_delay_ms is a suggested backoff delay in milliseconds.
 */
sqli_status sqli_retry_recommend(sqli_conn_t *conn, uint32_t attempt,
                                 bool *out_should_retry, uint32_t *out_delay_ms);

/**
 * @brief Enable or disable strict validation mode.
 * @param[in] conn Connection handle.
 * @param[in] enabled Non-zero enables strict mode.
 * @return SQLI_OK on success.
 */
sqli_status sqli_set_strict_protocol(sqli_conn_t *conn, bool enabled);
bool sqli_get_strict_protocol(sqli_conn_t *conn);

/**
 * @brief Enable or disable trailing-space trimming for CHAR/NCHAR string getters.
 * @param[in] conn Connection handle.
 * @param[in] enabled true trims trailing ASCII spaces, false preserves them.
 * @return SQLI_OK on success.
 *
 * Default is enabled.
 */
sqli_status sqli_set_trim_trailing_spaces(sqli_conn_t *conn, bool enabled);
bool sqli_get_trim_trailing_spaces(sqli_conn_t *conn);

sqli_status sqli_set_cursor_type(sqli_conn_t *conn, sqli_cursor_type type);
sqli_cursor_type sqli_get_cursor_type(sqli_conn_t *conn);
sqli_status sqli_set_cursor_holdability(sqli_conn_t *conn,
                                        sqli_cursor_holdability holdability);
sqli_cursor_holdability sqli_get_cursor_holdability(sqli_conn_t *conn);

/* ----------------------------------------------------------------
 * Configuration (set before connect)
 * ---------------------------------------------------------------- */

/**
 * @brief Connection configuration for @ref sqli_connect.
 *
 * All string pointers are caller-owned for the duration of the call.
 * The library copies relevant values internally.
 */
typedef struct {
    const char *server;     /* server name from sqlhosts (e.g. "ol_tli_tcp") */
    const char *hostname;   /* TCP hostname or IP */
    const char *service;    /* TCP port number (e.g. "9088") */
    const char *database;   /* database name to open */
    const char *username;   /* authentication username */
    const char *password;   /* authentication password (kept secret) */
    const char *client_locale; /* locale string (e.g. "en_US.UTF-8") */
    const char *db_locale;    /* database locale string */
    bool ssl_enable;        /* true enables TLS (OpenSSL) transport */
    bool ssl_verify_peer;   /* true enables certificate verification */
    const char *ssl_ca_file;/* optional CA bundle path for verification */
} sqli_connect_params;

/**
 * @brief Open a database connection.
 * @param[in,out] conn Connection handle created by @ref sqli_create.
 * @param[in] params Connection settings.
 * @return SQLI_OK on success; otherwise an error status.
 */
sqli_status sqli_connect(sqli_conn_t *conn, const sqli_connect_params *params);

/**
 * @brief Open a connection using a connection URI and explicit credentials.
 *
 * The URI specifies the protocol, host, port, and options (e.g., INFORMIXSERVER).
 * Credentials provided here override any user info embedded in the URI.
 *
 * Supported URI schemes:
 *   - informix+onsoctcp://[user:pass@]host:port/db?INFORMIXSERVER=srv
 *   - informix+onsocssl://[user:pass@]host:port/db?INFORMIXSERVER=srv&SSL_CA_FILE=...
 *   - informix+onipcstr:///db?INFORMIXSERVER=srv
 *
 * @param[in,out] conn Connection handle created by sqli_create.
 * @param[in] uri Connection string.
 * @param[in] username Explicit username (optional, can be NULL).
 * @param[in] password Explicit password (optional, can be NULL).
 * @return SQLI_OK on success; otherwise an error status.
 */
sqli_status sqli_connect_uri(sqli_conn_t *conn, const char *uri,
                             const char *username, const char *password);

/**
 * @brief Close a connection gracefully.
 * @param[in,out] conn Connection handle (NULL is allowed).
 */
void sqli_close(sqli_conn_t *conn);

/* ----------------------------------------------------------------
 * Connection pool (thread-safe, fixed-size)
 * ---------------------------------------------------------------- */

/**
 * @brief Create a fixed-size connection pool.
 *
 * The pool owns internal copies of @p params and opens @p pool_size
 * connections eagerly.
 *
 * @param[out] pool Destination pointer receiving the allocated pool.
 * @param[in] params Connection settings used for all pool connections.
 * @param[in] pool_size Number of pooled connections (> 0).
 * @return SQLI_OK on success.
 */
sqli_status sqli_pool_create(sqli_pool_t **pool,
                             const sqli_connect_params *params,
                             size_t pool_size);

/**
 * @brief Acquire one pooled connection (blocking).
 *
 * Blocks until a slot becomes available. A dead connection is reconnected once;
 * a failed reconnect returns its error status with *conn == NULL.
 *
 * @param[in] pool Pool handle.
 * @param[out] conn Borrowed connection handle.
 * @return SQLI_OK on success.
 */
sqli_status sqli_pool_acquire(sqli_pool_t *pool, sqli_conn_t **conn);

/**
 * @brief Acquire one pooled connection without waiting.
 *
 * Returns SQLI_TIMEOUT when no slot is currently available.
 * An available dead connection is reconnected once; connection I/O may block
 * and its error status is returned on failure.
 *
 * @param[in] pool Pool handle.
 * @param[out] conn Borrowed connection handle.
 * @return SQLI_OK on success, SQLI_TIMEOUT if none available.
 */
sqli_status sqli_pool_try_acquire(sqli_pool_t *pool, sqli_conn_t **conn);

/**
 * @brief Acquire one pooled connection with timeout.
 *
 * Waits up to @p timeout_ms for an available connection.
 * Use timeout_ms == 0 to avoid waiting for a slot. This timeout does not bound
 * connection I/O. Reconnection is attempted once and its error is propagated;
 * retries and any retry delay are the caller's responsibility.
 *
 * @param[in] pool Pool handle.
 * @param[out] conn Borrowed connection handle.
 * @param[in] timeout_ms Maximum wait in milliseconds.
 * @return SQLI_OK on success, SQLI_TIMEOUT on timeout.
 */
sqli_status sqli_pool_acquire_timeout(sqli_pool_t *pool, sqli_conn_t **conn,
                                      uint32_t timeout_ms);

/**
 * @brief Return a borrowed connection to the pool.
 *
 * @param[in] pool Pool handle.
 * @param[in] conn Connection previously returned by @ref sqli_pool_acquire.
 * @return SQLI_OK on success.
 */
sqli_status sqli_pool_release(sqli_pool_t *pool, sqli_conn_t *conn);

/**
 * @brief Destroy a pool and close all managed connections.
 *
 * Connections still in use by other threads become invalid after destroy.
 *
 * @param[in,out] pool Pool handle (NULL is allowed).
 */
void sqli_pool_destroy(sqli_pool_t *pool);

/* ----------------------------------------------------------------
 * SQL execution & results
 * ---------------------------------------------------------------- */

/** Opaque result handle. */
typedef struct sqli_result sqli_result_t;

typedef struct {
    sqli_cursor_type cursor_type;
    sqli_cursor_holdability holdability;
} sqli_query_options;

/**
 * @brief Execute a direct SQL statement.
 * @param[in] conn Connection handle.
 * @param[in] sql SQL text.
 * @param[out] result Result handle on success; NULL on failure.
 * @return SQLI_OK on success; otherwise an error status.
 */
sqli_status sqli_query(sqli_conn_t *conn, const char *sql, sqli_result_t **result);
sqli_status sqli_query_ex(sqli_conn_t *conn, const char *sql,
                          const sqli_query_options *options,
                          sqli_result_t **result);
/*
 * Execute SQL with retry handling based on sqli_retry_recommend().
 * max_retries is the number of retries after the first attempt.
 */
sqli_status sqli_query_with_retry(sqli_conn_t *conn, const char *sql,
                                  uint32_t max_retries, sqli_result_t **result);

/*
 * Row callback for streaming query execution.
 * Return 0 to continue, non-zero to abort streaming.
 */
typedef int (*sqli_row_callback)(sqli_result_t *row_result, void *ctx);

/*
 * Execute SQL and stream rows to callback without retaining full result sets.
 * out_rows (optional) receives the number of delivered rows. On callback abort
 * or error, it receives the count of rows delivered before the failure.
 */
sqli_status sqli_query_stream(sqli_conn_t *conn, const char *sql,
                              sqli_row_callback on_row, void *ctx,
                              int64_t *out_rows);

/*
 * Advance to the next row. Returns true if a row is available,
 * false at end of result set or on error.
 */
/** Advance with an explicit outcome: SQLI_OK means a validated row layout,
 * SQLI_EOF means clean exhaustion, otherwise an error status. Failed fetches
 * invalidate the current row and remain errors on subsequent fetch calls until
 * the result is reset/re-executed. Payload conversion remains the getter's task.
 * Synchronize all operations on the same result externally.
 */
sqli_status sqli_result_fetch(sqli_result_t *result);
/** Compatibility convenience: true exactly when fetch returns SQLI_OK. */
bool sqli_result_next(sqli_result_t *result);
bool sqli_result_previous(sqli_result_t *result);
bool sqli_result_first(sqli_result_t *result);
bool sqli_result_last(sqli_result_t *result);
bool sqli_result_absolute(sqli_result_t *result, int row_1based);
bool sqli_result_relative(sqli_result_t *result, int offset);
int sqli_result_row_number(sqli_result_t *result);

/*
 * Return the number of rows affected/returned by the query.
 * Valid only after sqli_result_next() returns false (end of set).
 */
int64_t sqli_result_rows_affected(sqli_result_t *result);

/*
 * Return whether a generated SERIAL value was provided for this result.
 */
bool sqli_result_has_generated_serial(sqli_result_t *result);

/*
 * Return the generated SERIAL value from DONE sqlca.sqlerrd1 (0 if unavailable).
 */
int64_t sqli_result_generated_serial(sqli_result_t *result);

/*
 * Return whether a generated SERIAL8 value was provided for this result.
 */
bool sqli_result_has_generated_serial8(sqli_result_t *result);

/*
 * Return the generated SERIAL8 value from SQ_INSERTDONE (0 if unavailable).
 */
int64_t sqli_result_generated_serial8(sqli_result_t *result);

/*
 * Return the number of columns in the result set.
 */
int sqli_result_columns(sqli_result_t *result);

/*
 * Destroy a result object and release all resources.
 * Safe to call with NULL (no-op).
 */
void sqli_result_destroy(sqli_result_t *result);

/* ----------------------------------------------------------------
 * SQLI column type constants (returned by sqli_result_column_type)
 * ---------------------------------------------------------------- */

typedef enum {
    SQLI_TYPE_CHAR            = 0,
    SQLI_TYPE_SMALLINT        = 1,
    SQLI_TYPE_INT             = 2,
    SQLI_TYPE_FLOAT           = 3,
    SQLI_TYPE_SMFLOAT         = 4,
    SQLI_TYPE_DECIMAL         = 5,
    SQLI_TYPE_SERIAL          = 6,
    SQLI_TYPE_DATE            = 7,
    SQLI_TYPE_MONEY           = 8,
    SQLI_TYPE_NULL            = 9,
    SQLI_TYPE_DATETIME        = 10,
    SQLI_TYPE_BYTE            = 11,
    SQLI_TYPE_TEXT            = 12,
    SQLI_TYPE_VARCHAR         = 13,
    SQLI_TYPE_INTERVAL        = 14,
    SQLI_TYPE_NCHAR           = 15,
    SQLI_TYPE_NVCHAR          = 16,
    SQLI_TYPE_INT8            = 17,
    SQLI_TYPE_SERIAL8         = 18,
    SQLI_TYPE_BIGINT          = 52,
    SQLI_TYPE_BIGSERIAL       = 53,
    SQLI_TYPE_LVARCHAR        = 40,
    SQLI_TYPE_BOOL            = 41,
    SQLI_TYPE_DBOOLEAN        = 45,
    SQLI_TYPE_CLOB            = 101,
    SQLI_TYPE_BLOB            = 102,
} sqli_column_type;

/* SQLI type flags */
#define SQLI_BIT_NOTNULLABLE      0x0100
#define SQLI_BIT_DISTINCT         0x0800
#define SQLI_BIT_NAMEDROW         0x1000
#define SQLI_BIT_DBOOLEAN         0x4000

/* ----------------------------------------------------------------
 * Prepared statements
 * ---------------------------------------------------------------- */

/* Opaque prepared statement handle */
typedef struct sqli_stmt sqli_stmt_t;
typedef struct sqli_batch_result sqli_batch_result_t;


/*
 * Prepare an SQL statement with ? parameter markers.
 *
 * Sends the SQL to the server for parsing, receives a statement ID
 * and parameter count. The caller owns the statement and must call
 * sqli_stmt_close() when done.
 *
 * Returns SQLI_OK on success. On error, *stmt is set to NULL
 * and sqli_error(conn) contains the reason.
 */
sqli_status sqli_prepare(sqli_conn_t *conn, const char *sql,
                         int *param_count, sqli_stmt_t **stmt);
/*
 * Prepare a statement with retry handling based on
 * sqli_retry_recommend(). max_retries is the number of retries
 * after the first attempt.
 */
sqli_status sqli_prepare_with_retry(sqli_conn_t *conn, const char *sql,
                                    uint32_t max_retries, int *param_count,
                                    sqli_stmt_t **stmt);

/*
 * Bind an int32 value to a positional parameter (1-indexed).
 */
sqli_status sqli_bind_int(sqli_stmt_t *stmt, int param_index, int32_t value);

/*
 * Bind an int64 value to a positional parameter (1-indexed).
 */
sqli_status sqli_bind_int64(sqli_stmt_t *stmt, int param_index, int64_t value);

/*
 * Bind a double value to a positional parameter (1-indexed).
 */
sqli_status sqli_bind_double(sqli_stmt_t *stmt, int param_index, double value);

/*
 * Bind a UTF-8 string value to a positional parameter (1-indexed).
 */
sqli_status sqli_bind_string(sqli_stmt_t *stmt, int param_index, const char *value);

/*
 * Bind a boolean value.
 */
sqli_status sqli_bind_bool(sqli_stmt_t *stmt, int param_index, bool value);

/*
 * Bind raw bytes (BYTE/BLOB style payload).
 */
sqli_status sqli_bind_bytes(sqli_stmt_t *stmt, int param_index,
                            const uint8_t *value, size_t len);

/*
 * Bind a NULL string value to a positional parameter (1-indexed).
 */
sqli_status sqli_bind_null(sqli_stmt_t *stmt, int param_index);

/*
 * Bind a NULL int32 value to a positional parameter (1-indexed).
 */
sqli_status sqli_bind_null_int(sqli_stmt_t *stmt, int param_index);

/*
 * Bind a NULL int64 value to a positional parameter (1-indexed).
 */
sqli_status sqli_bind_null_int64(sqli_stmt_t *stmt, int param_index);

/*
 * Bind a NULL double value to a positional parameter (1-indexed).
 */
sqli_status sqli_bind_null_double(sqli_stmt_t *stmt, int param_index);

/*
 * Snapshot the currently bound parameter set as one batch row.
 */
sqli_status sqli_stmt_batch_add(sqli_stmt_t *stmt);

/*
 * Remove all queued batch rows from a prepared statement.
 */
void sqli_stmt_batch_clear(sqli_stmt_t *stmt);

/*
 * Return the number of queued batch rows.
 */
size_t sqli_stmt_batch_size(const sqli_stmt_t *stmt);

/*
 * Execute queued batch rows for a prepared DML statement.
 *
 * The batch is cleared after a successful execution.
 */
sqli_status sqli_stmt_batch_execute(sqli_stmt_t *stmt,
                                    sqli_batch_result_t **out_batch);

/*
 * Execute a prepared statement.
 *
 * Sends the EXECUTE command to the server. The result is available
 * via sqli_stmt_result(stmt), including before the first fetch.
 *
 * Returns SQLI_OK on success.
 */
sqli_status sqli_execute(sqli_stmt_t *stmt);
/*
 * Execute a prepared statement with retry handling based on
 * sqli_retry_recommend(). max_retries is the number of retries
 * after the first attempt.
 */
sqli_status sqli_execute_with_retry(sqli_stmt_t *stmt, uint32_t max_retries);

/** Advance a prepared result using the same SQLI_OK / SQLI_EOF / error
 * contract as sqli_result_fetch. Requires successful execution; NULL statement
 * returns SQLI_INVALID_ARGUMENT, unavailable result SQLI_INVALID_STATE.
 * Synchronize access with statement/result mutation and destruction.
 */
sqli_status sqli_stmt_fetch(sqli_stmt_t *stmt);
/** Boolean convenience: true exactly when sqli_stmt_fetch returns SQLI_OK.
 * Use sqli_stmt_fetch to distinguish end of data from errors. */
bool sqli_stmt_next(sqli_stmt_t *stmt);

/*
 * Get the result object from a prepared statement execution.
 * Available after successful execution, even before fetching the first row.
 * Returns NULL when no result is available or after sqli_stmt_close().
 */
sqli_result_t *sqli_stmt_result(sqli_stmt_t *stmt);

/*
 * Close a prepared statement on the server and free local resources.
 * Safe to call with NULL (no-op).
 */
void sqli_stmt_close(sqli_stmt_t *stmt);

/*
 * Destroy a prepared statement (close if not already closed).
 * Safe to call with NULL (no-op).
 */
void sqli_stmt_destroy(sqli_stmt_t *stmt);

/* ----------------------------------------------------------------
 * Immutable server descriptor snapshots
 * Type-specific field getters live in sqli_decimal.h and sqli_temporal.h.
 * ---------------------------------------------------------------- */
typedef struct sqli_descriptor sqli_descriptor_t;

/** Borrowed bytes in server encoding, not necessarily UTF-8 or NUL terminated.
 * available distinguishes missing information from a known empty byte span. */
typedef struct {
    const uint8_t *data;
    size_t length;
    bool available;
} sqli_descriptor_bytes_t;

typedef struct sqli_descriptor_field sqli_descriptor_field_t;

/** Acquisition returns an owned reference, usable after result/statement close
 * or destruction. SQLI_METADATA_UNAVAILABLE means no complete DESCRIBE has
 * arrived; a known zero-field descriptor is a successful snapshot. These APIs
 * expose server DESCRIBE fields, not inferred parameter metadata.
 * Acquisition requires external synchronization with result/statement mutation.
 * Published snapshots are immutable: getters and reference operations may run
 * concurrently while callers hold a reference. Retained byte views remain valid
 * until the owning reference is released. All failures preserve outputs.
 */
sqli_status sqli_result_get_descriptor(const sqli_result_t *result, sqli_descriptor_t **out);
sqli_status sqli_stmt_get_descriptor(const sqli_stmt_t *stmt, sqli_descriptor_t **out);
sqli_status sqli_descriptor_retain(sqli_descriptor_t *descriptor);
void sqli_descriptor_release(sqli_descriptor_t *descriptor);
sqli_status sqli_descriptor_get_field_count(const sqli_descriptor_t *descriptor, size_t *out);
/** Zero-based index; borrows a field from the snapshot without allocating.
 * The view and its byte spans remain valid while a snapshot reference is held.
 * Property getters return SQLI_METADATA_UNAVAILABLE for missing or unsupported
 * information (including unknown types), SQLI_PROTO_ERROR for malformed known
 * qualifiers. All failures preserve outputs. No SQL or text conversion occurs.
 */
sqli_status sqli_descriptor_get_field(const sqli_descriptor_t *descriptor, size_t index,
                                      const sqli_descriptor_field_t **out);
sqli_status sqli_descriptor_field_get_name(const sqli_descriptor_field_t *field,
                                           sqli_descriptor_bytes_t *out);
sqli_status sqli_descriptor_field_get_type_owner(const sqli_descriptor_field_t *field,
                                                 sqli_descriptor_bytes_t *out);
sqli_status sqli_descriptor_field_get_type_name(const sqli_descriptor_field_t *field,
                                                sqli_descriptor_bytes_t *out);
sqli_status sqli_descriptor_field_get_type(const sqli_descriptor_field_t *field,
                                           sqli_column_type *out);
/* ----------------------------------------------------------------
 * Callable statements (stored procedures/functions)
 * ---------------------------------------------------------------- */

typedef struct sqli_call sqli_call_t;

typedef enum {
    SQLI_CALL_PARAM_IN = 0,
    SQLI_CALL_PARAM_OUT = 1,
    SQLI_CALL_PARAM_INOUT = 2
} sqli_call_param_mode;

/*
 * Prepare a callable statement.
 * The SQL text should be a CALL/EXECUTE FUNCTION style statement with
 * positional markers.
 */
sqli_status sqli_call_prepare(sqli_conn_t *conn, const char *sql,
                              int *param_count, sqli_call_t **call);

/*
 * Access underlying prepared statement handle to reuse sqli_bind_* APIs.
 */
sqli_stmt_t *sqli_call_stmt(sqli_call_t *call);

/*
 * Set parameter direction mode (1-indexed).
 */
sqli_status sqli_call_set_param_mode(sqli_call_t *call, int param_index,
                                     sqli_call_param_mode mode);

/*
 * Execute callable statement and capture OUT/INOUT values from first result row.
 */
sqli_status sqli_call_execute(sqli_call_t *call);

/*
 * Read OUT/INOUT values by parameter index.
 */
sqli_status sqli_call_get_int64(sqli_call_t *call, int param_index,
                                int64_t *out, bool *is_null);
sqli_status sqli_call_get_double(sqli_call_t *call, int param_index,
                                 double *out, bool *is_null);
sqli_status sqli_call_get_string(sqli_call_t *call, int param_index,
                                 const char **out, bool *is_null);

/*
 * Destroy callable statement and owned resources.
 */
void sqli_call_destroy(sqli_call_t *call);

/* ----------------------------------------------------------------
 * Result column accessors
 * ---------------------------------------------------------------- */

/*
 * Get the name of a column by 0-based index.
 * Returns NULL if index is out of range.
 */
const char *sqli_result_column_name(sqli_result_t *result, size_t col_index);

/*
 * Get the type of a column by 0-based index.
 * Returns -1 if index is out of range.
 * Type values match the SQLI column type constants.
 */
int sqli_result_column_type(sqli_result_t *result, size_t col_index);

/* ----------------------------------------------------------------
 * Row data extractors (only valid between sqli_result_next() == 1 calls)
 * ---------------------------------------------------------------- */

/** Legacy scalar conveniences; all column indices are zero-based size_t.
 * NULL and invalid index/state/type, malformed values and overflow return
 * 0 / 0.0 / false. Successful NULL detection sets was_null=true; invalid
 * arguments/state clear it when a result exists. These fallbacks cannot
 * distinguish errors from a real zero. Integer getters accept integer, BOOL,
 * DATE-epoch and decimal columns; decimal fractions are truncated toward zero.
 * Narrowing overflow returns zero. Double accepts integer, decimal and IEEE
 * floating types; nonfinite values return zero. BOOL is get_int()!=0.
 * Use native value getters for exact conversion and explicit error statuses.
 * Synchronize access with result mutation, advancement and destruction.
 */
int32_t sqli_result_get_int(sqli_result_t *result, size_t col_index);

/* Extract an int64 value from column col_index (0-based). */
int64_t sqli_result_get_int64(sqli_result_t *result, size_t col_index);

/* Extract a double value from column col_index (0-based). */
double sqli_result_get_double(sqli_result_t *result, size_t col_index);

/*
 * Extract a string value from column col_index (0-based).
 * Returns a pointer into the result's internal tuple buffer.
 * Valid until the next sqli_result_next() call or result destruction.
 * NOTE: Truncates at 4096 bytes. For full-length strings, use
 * sqli_result_get_string_len().
 */
const char *sqli_result_get_string(sqli_result_t *result, size_t col_index);

/** Checked whole-value buffers with separate capacity and required size.
 * NULL output with capacity 0 queries size. required and is_null are mandatory.
 * String required includes NUL; byte required does not. SQL NULL succeeds with
 * required=0, is_null=true and leaves the buffer untouched. Empty text needs one
 * byte, empty binary needs none. A short buffer returns SQLI_BUFFER_TOO_SMALL,
 * updates required/is_null and leaves the buffer untouched. Other errors preserve
 * every output. No silent truncation. Outputs must not overlap one another or
 * result storage. These calls leave was_null unchanged and require a validated
 * current row; invalid indices return SQLI_OUT_OF_RANGE. LOB reads/conversion
 * may allocate or access the connection. Synchronize all result/connection use.
 * String output supports character, legacy LOB, native decimal and temporal
 * columns; bytes expose the column payload (LOB contents are materialized).
 */
sqli_status sqli_result_get_string_len(sqli_result_t *result, size_t col_index,
                                       char *out, size_t capacity, size_t *required, bool *is_null);
sqli_status sqli_result_get_bytes(sqli_result_t *result, size_t col_index,
                                  uint8_t *out, size_t capacity, size_t *required, bool *is_null);

/*
 * Return true if column is NULL in current row, else false.
 */
bool sqli_result_is_null(sqli_result_t *result, size_t col_index);

/*
 * Return the legacy NULL-state last set by a convenience getter or is_null.
 * Native value getters and checked whole-value buffers leave it unchanged.
 */
bool sqli_result_was_null(sqli_result_t *result);

/*
 * Extract boolean value (true/false).
 */
bool sqli_result_get_bool(sqli_result_t *result, size_t col_index);

/* Chunk callback for streaming column bytes. */
typedef int (*sqli_stream_chunk_cb)(const uint8_t *chunk, size_t len, void *ctx);

/*
 * Stream column bytes in chunks to callback.
 * Returns SQLI_OK on success.
 */
sqli_status sqli_result_stream_bytes(sqli_result_t *result, size_t col_index,
                                     size_t chunk_size, sqli_stream_chunk_cb cb,
                                     void *ctx);

/* ----------------------------------------------------------------
 * Protocol utilities
 * ---------------------------------------------------------------- */

/*
 * Round up n to the next even number.
 * SQLI protocol requires all variable-length payloads to be
 * padded to an even byte boundary.
 */
size_t sqli_pad_even(size_t n);

/* ----------------------------------------------------------------
 * Transactions
 * ---------------------------------------------------------------- */

/*
 * Transaction isolation levels.
 */
typedef enum {
    SQLI_TXN_ISOLATION_COMMITTED = 0,  /* READ COMMITTED */
    SQLI_TXN_ISOLATION_REPEATABLE_READ = 1,
    SQLI_TXN_ISOLATION_SERIALIZABLE = 3,
    SQLI_TXN_ISOLATION_CURSOR = 4,
} sqli_isolation_level;

/*
 * Begin a new transaction.
 *
 * Sends BEGIN WORK to the server. The connection enters transaction mode;
 * subsequent queries are part of the transaction until commit or rollback.
 *
 * Returns SQLI_OK on success, or an error status (e.g. SQLI_IO_ERROR,
 * SQLI_PROTO_ERROR, SQLI_TIMEOUT) if sending or receiving the response fails.
 */
sqli_status sqli_begin(sqli_conn_t *conn);

/*
 * Commit the current transaction.
 *
 * Sends COMMIT WORK to the server. The configured autocommit mode is unchanged.
 *
 * Returns SQLI_OK on success, or an error status (e.g. SQLI_IO_ERROR,
 * SQLI_PROTO_ERROR, SQLI_TIMEOUT) if the commit request could not be sent
 * or the server's acknowledgement was not received.
 *
 * NOTE: If a network or I/O error occurs while waiting for the commit response,
 * the transaction outcome is uncertain: the server may have already committed
 * the work before the connection was lost. Applications should inspect state
 * or handle reconnection rather than blindly retrying non-idempotent operations.
 */
sqli_status sqli_commit(sqli_conn_t *conn);

/*
 * Roll back the current transaction.
 *
 * Sends ROLLBACK WORK to the server. The configured autocommit mode is unchanged.
 *
 * Returns SQLI_OK on success, or an error status (e.g. SQLI_IO_ERROR,
 * SQLI_PROTO_ERROR, SQLI_TIMEOUT) if sending or receiving the response fails.
 */
sqli_status sqli_rollback(sqli_conn_t *conn);

/*
 * Set autocommit mode.
 *
 * When autocommit is enabled (on == true), each individual SQL statement
 * is automatically committed after execution. When disabled, the
 * application must explicitly call sqli_commit() or sqli_rollback().
 * New connections default to enabled. In disabled mode, executing SELECT,
 * DML or a routine starts a transaction lazily if none is active; preparing
 * alone, session settings and DDL do not start one. Explicit BEGIN remains
 * supported in either mode. COMMIT/ROLLBACK leave the configured mode intact.
 * Repeating a mode or disabling it preserves any active transaction.
 * Enabling it from disabled mode during a transaction returns
 * SQLI_INVALID_STATE; commit or roll back first.
 *
 * Returns SQLI_OK on success.
 */
sqli_status sqli_set_autocommit(sqli_conn_t *conn, bool on);

/*
 * Get the current autocommit state.
 *
 * Returns true if autocommit is enabled, false otherwise.
 */
bool sqli_get_autocommit(sqli_conn_t *conn);

/*
 * Set transaction isolation level.
 *
 * Must be called before BEGIN WORK to take effect.
 *
 * Returns SQLI_OK on success.
 */
sqli_status sqli_set_isolation_level(sqli_conn_t *conn, sqli_isolation_level level);

/*
 * Get the current transaction isolation level.
 */
sqli_isolation_level sqli_get_isolation_level(sqli_conn_t *conn);

/*
 * Set lock wait behavior.
 *
 * seconds >= 0 configures WAIT seconds.
 * seconds < 0 configures NOT WAIT.
 */
sqli_status sqli_set_lock_wait(sqli_conn_t *conn, int seconds);

/*
 * Create a savepoint in the current transaction.
 *
 * If unique_name is true, the server may enforce uniqueness semantics.
 */
sqli_status sqli_savepoint_set(sqli_conn_t *conn, const char *name, bool unique_name);

/*
 * Release a savepoint in the current transaction.
 */
sqli_status sqli_savepoint_release(sqli_conn_t *conn, const char *name);

/*
 * Roll back to a savepoint in the current transaction.
 */
sqli_status sqli_savepoint_rollback(sqli_conn_t *conn, const char *name);

/*
 * Check if the connection is currently in a transaction.
 *
 * Returns true if a transaction is active, false otherwise.
 */
bool sqli_in_transaction(sqli_conn_t *conn);

/* ----------------------------------------------------------------
 * Batch execution framing
 * ---------------------------------------------------------------- */

/**
 * @brief Begin a batch execution block.
 * @param[in] conn Connection handle.
 * @return SQLI_OK on success.
 */
sqli_status sqli_batch_begin(sqli_conn_t *conn);

/**
 * @brief End an active batch execution block.
 * @param[in] conn Connection handle.
 * @return SQLI_OK on success.
 */
sqli_status sqli_batch_end(sqli_conn_t *conn);

/*
 * Return true if a batch block is currently active, otherwise false.
 */
bool sqli_in_batch(sqli_conn_t *conn);

/* ----------------------------------------------------------------
 * Batch result model
 * ---------------------------------------------------------------- */

typedef struct {
    sqli_status status;      /* per-statement status */
    int64_t rows_affected;   /* valid when status == SQLI_OK */
    int sqlcode;             /* Informix SQLCODE if available */
    int isamcode;            /* Informix ISAM code if available */
    uint16_t opcode;         /* related opcode if available */
    char message[512];       /* composed diagnostic message */
} sqli_batch_item_result;

/**
 * @brief Execute multiple SQL statements as one logical batch.
 *
 * Return value semantics:
 * - SQLI_OK: batch framing completed; inspect per-item statuses in @p out_batch.
 * - non-OK: batch framing failed (precheck/start/end); @p out_batch is NULL.
 *
 * @param[in] conn Connection handle.
 * @param[in] sql_list Array of SQL strings.
 * @param[in] sql_count Number of entries in @p sql_list.
 * @param[out] out_batch Batch result container.
 * @return SQLI_OK on success.
 */
sqli_status sqli_batch_execute(sqli_conn_t *conn, const char **sql_list,
                               size_t sql_count, sqli_batch_result_t **out_batch);

/* Number of statements recorded in a batch result. */
size_t sqli_batch_result_count(const sqli_batch_result_t *batch);

/* Number of successful statements in a batch result. */
size_t sqli_batch_result_success_count(const sqli_batch_result_t *batch);

/* Number of failed statements in a batch result. */
size_t sqli_batch_result_error_count(const sqli_batch_result_t *batch);

/* Copy one item by index. Returns SQLI_INVALID_STATE on invalid input/index. */
sqli_status sqli_batch_result_item(const sqli_batch_result_t *batch, size_t index,
                                   sqli_batch_item_result *out_item);

/* Release a batch result object. Safe with NULL. */
void sqli_batch_result_destroy(sqli_batch_result_t *batch);

/* ----------------------------------------------------------------
 * sqlhosts file parser
 * ---------------------------------------------------------------- */

/*
 * A single entry from the sqlhosts file.
 */
typedef struct {
    char server_name[256];   /* server name as defined in sqlhosts */
    char protocol[64];       /* protocol (e.g. "onsoctcp", "tliv2") */
    char hostname[256];      /* server hostname or IP */
    char service[64];        /* port number or service name */
    char options[256];       /* additional options */
} sqli_sqlhosts_entry;

/*
 * Parse the sqlhosts file and return an array of entries.
 *
 * If filepath is NULL, uses the default path $INFORMIXDIR/etc/sqlhosts
 * or /etc/sqlhosts.
 *
 * On success, *entries is set to a malloc'd array of *count entries.
 * The caller must free(*entries) when done.
 *
 * Returns SQLI_OK on success.
 */
sqli_status sqli_parse_sqlhosts(const char *filepath,
                                sqli_sqlhosts_entry **entries,
                                int *count);

/*
 * Look up a server name in the sqlhosts entries.
 *
 * Returns a pointer to the matching entry, or NULL if not found.
 * The returned pointer is valid until *entries is freed.
 */
const sqli_sqlhosts_entry *sqli_find_sqlhosts_entry(
    const sqli_sqlhosts_entry *entries, int count,
    const char *server_name);

/* ----------------------------------------------------------------
 * Logging levels (for configuration)
 * ---------------------------------------------------------------- */

typedef enum {
    SQLI_LOG_NONE = 0,
    SQLI_LOG_ERROR,
    SQLI_LOG_WARN,
    SQLI_LOG_INFO,
    SQLI_LOG_DEBUG
} sqli_log_level;

void sqli_log_set_level(sqli_log_level level);

#ifdef __cplusplus
}
#endif

#endif /* SQLI_H */
