#ifndef SQLI_SBLOB_H
#define SQLI_SBLOB_H

#include "sqli.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 * Smart Large Object (BLOB / CLOB) API
 * ---------------------------------------------------------------- */

#define SQLI_LO_APPEND       1
#define SQLI_LO_WRONLY       2
#define SQLI_LO_RDONLY       4
#define SQLI_LO_RDWR         8

#define SQLI_LO_SEEK_SET     0
#define SQLI_LO_SEEK_CUR     1
#define SQLI_LO_SEEK_END     2

typedef enum {
    SQLI_SBLOB_BLOB = 0,
    SQLI_SBLOB_CLOB = 1
} sqli_sblob_type;

typedef struct {
    const char *sbspace;      /* NULL: server/column default */
    int64_t estimated_bytes;  /* -1: unspecified */
    int64_t maximum_bytes;    /* -1: unspecified */
    int32_t extent_kib;       /* -1: unspecified */
    uint32_t create_flags;    /* 0: inherited defaults */
    int open_mode;            /* normally SQLI_LO_WRONLY or SQLI_LO_RDWR */
} sqli_sblob_options;

#define SQLI_SBLOB_OPTIONS_INIT { NULL, -1, -1, -1, 0, SQLI_LO_WRONLY }

typedef struct sqli_sblob sqli_sblob_t;

/** Free only client-side storage; NULL is allowed. Close an open descriptor
 * with close_created, or release the unreferenced server object, before destroy.
 * After a broken connection, terminate that connection before destroying its
 * handles. Destroy does not issue SQL, delete server data, or use a connection.
 * Synchronize all handle access and destruction externally.
 */
void sqli_sblob_destroy(sqli_sblob_t *lob);
/** Semantic inspection; invalid pointers fail without modifying outputs. */
sqli_status sqli_sblob_is_open(const sqli_sblob_t *lob, bool *out);
sqli_status sqli_sblob_get_type(const sqli_sblob_t *lob, sqli_sblob_type *out);

typedef sqli_status (*sqli_sblob_reader)(
    void *context,
    unsigned char *buffer,
    size_t capacity,
    size_t *bytes_read);

/**
 * @brief Create a new smart large object on the server and return an open handle.
 *
 * Invokes the server routine informix.ifx_lo_create to allocate a new Smart-LOB,
 * opens it with the requested mode (default SQLI_LO_WRONLY), and retrieves the
 * 72-byte locator.
 *
 * @param[in] conn Active connection.
 * @param[in] type SQLI_SBLOB_BLOB or SQLI_SBLOB_CLOB.
 * @param[in] options Optional creation parameters (can be NULL for server defaults).
 * @param[out] out Owned opaque handle; unchanged on failure. Destroy after use.
 * @return SQLI_OK on success.
 */
sqli_status sqli_sblob_create(sqli_conn_t *conn, sqli_sblob_type type,
                              const sqli_sblob_options *options, sqli_sblob_t **out);

/**
 * @brief Write an in-memory buffer to an open created Smart Large Object.
 *
 * @param[in] conn Active connection.
 * @param[in,out] lob Created Smart-LOB handle.
 * @param[in] data Buffer containing data to upload.
 * @param[in] length Number of bytes to write.
 * @return SQLI_OK on success.
 */
sqli_status sqli_sblob_write_buffer(sqli_conn_t *conn, sqli_sblob_t *lob,
                                    const void *data, size_t length);

/**
 * @brief Stream data to an open created Smart Large Object via reader callback.
 *
 * Synchronously invokes @p reader with library scratch buffers until @p reader
 * reports EOF (0 bytes read) or returns an error. Does not accept or open file paths.
 *
 * @param[in] conn Active connection.
 * @param[in,out] lob Created Smart-LOB handle.
 * @param[in] reader Reader callback function.
 * @param[in] context User context passed to callback.
 * @param[out] bytes_written Optional confirmed byte count, initialized to zero
 * before argument validation and updated after each write. Preserved on reader,
 * protocol and short-write errors. Includes any partial count confirmed by the
 * underlying write operation; excludes unacknowledged data and data returned by
 * a failing reader. Counts progress in the current transaction, not committed
 * durability. This function does not automatically close/release the LOB or
 * request a transaction rollback on failure.
 * @return SQLI_OK on success.
 */
sqli_status sqli_sblob_write_stream(sqli_conn_t *conn, sqli_sblob_t *lob,
                                    sqli_sblob_reader reader, void *context,
                                    uint64_t *bytes_written);

/**
 * @brief Close an open descriptor in a created Smart Large Object handle.
 *
 * Idempotent locally; subsequent calls return SQLI_OK. The locator remains
 * available in @p lob for statement binding.
 *
 * @param[in] conn Active connection.
 * @param[in,out] lob Created Smart-LOB handle.
 * @return SQLI_OK on success.
 */
sqli_status sqli_sblob_close_created(sqli_conn_t *conn, sqli_sblob_t *lob);

/**
 * @brief Release an unreferenced Smart Large Object on the server and invalidate the handle.
 *
 * Fails if the object is already referenced by a table column.
 *
 * @param[in] conn Active connection.
 * @param[in,out] lob Created Smart-LOB handle.
 * @return SQLI_OK on success.
 */
sqli_status sqli_sblob_release(sqli_conn_t *conn, sqli_sblob_t *lob);

/**
 * @brief Bind a Smart Large Object locator to a prepared statement parameter.
 *
 * @param[in] stmt Prepared statement handle.
 * @param[in] parameter_index 1-based parameter index.
 * @param[in] lob Created Smart-LOB handle with valid locator (or NULL for NULL).
 * @return SQLI_OK on success.
 */
sqli_status sqli_bind_sblob(sqli_stmt_t *stmt, int parameter_index, const sqli_sblob_t *lob);

/**
 * @brief Open a smart large object from its hexadecimal locator string (for BLOB).
 * @param[in] conn Active connection.
 * @param[in] locator_hex Hexadecimal locator string obtained from result set.
 * @param[in] mode Open mode (e.g. SQLI_LO_RDONLY, SQLI_LO_RDWR).
 * @param[out] out_lofd Output file descriptor handle.
 * @return SQLI_OK on success.
 */
sqli_status sqli_sblob_open(sqli_conn_t *conn, const char *locator_hex, int mode, int *out_lofd);

/**
 * @brief Open a smart large object from its hexadecimal locator string (for CLOB).
 * @param[in] conn Active connection.
 * @param[in] locator_hex Hexadecimal locator string obtained from result set.
 * @param[in] mode Open mode (e.g. SQLI_LO_RDONLY, SQLI_LO_RDWR).
 * @param[out] out_lofd Output file descriptor handle.
 * @return SQLI_OK on success.
 */
sqli_status sqli_sblob_open_clob(sqli_conn_t *conn, const char *locator_hex, int mode, int *out_lofd);

/**
 * @brief Open a smart large object using a custom query returning a file descriptor handle.
 * @param[in] conn Active connection.
 * @param[in] open_sql SQL query (e.g. "SELECT ifx_lo_open(b, 4) FROM tbl WHERE id=1").
 * @param[out] out_lofd Output file descriptor handle.
 * @return SQLI_OK on success.
 */
sqli_status sqli_sblob_open_query(sqli_conn_t *conn, const char *open_sql, int *out_lofd);

/**
 * @brief Close an open smart large object file descriptor handle.
 * @param[in] conn Active connection.
 * @param[in] lofd File descriptor handle to close.
 * @return SQLI_OK on success.
 */
sqli_status sqli_sblob_close(sqli_conn_t *conn, int lofd);

/**
 * @brief Read data from an open smart large object using SQ_LODATA (subCom = 0).
 * @param[in] conn Active connection.
 * @param[in] lofd Open file descriptor handle.
 * @param[out] buf Destination buffer.
 * @param[in] nbytes Maximum bytes to read.
 * @param[out] bytes_read Number of bytes actually read.
 * @return SQLI_OK on success.
 */
sqli_status sqli_sblob_read(sqli_conn_t *conn, int lofd, void *buf, size_t nbytes, size_t *bytes_read);

/**
 * @brief Seek and read data from an open smart large object using SQ_LODATA (subCom = 1).
 * @param[in] conn Active connection.
 * @param[in] lofd Open file descriptor handle.
 * @param[in] offset Byte offset to seek to.
 * @param[out] buf Destination buffer.
 * @param[in] nbytes Maximum bytes to read.
 * @param[out] bytes_read Number of bytes actually read.
 * @return SQLI_OK on success.
 */
sqli_status sqli_sblob_read_seek(sqli_conn_t *conn, int lofd, int64_t offset,
                                 void *buf, size_t nbytes, size_t *bytes_read);

/**
 * @brief Write data to an open smart large object using SQ_LODATA (subCom = 2).
 * @param[in] conn Active connection.
 * @param[in] lofd Open file descriptor handle.
 * @param[in] buf Source buffer to write.
 * @param[in] nbytes Number of bytes to write.
 * @param[out] bytes_written Number of bytes written confirmed by server.
 * @return SQLI_OK on success.
 */
sqli_status sqli_sblob_write(sqli_conn_t *conn, int lofd, const void *buf, size_t nbytes, size_t *bytes_written);

/**
 * @brief Convenience function to read a smart large object directly from a result column.
 * @param[in] res Result handle with active row.
 * @param[in] col_index 0-based column index of BLOB or CLOB.
 * @param[out] buf Destination buffer.
 * @param[in] nbytes Maximum bytes to read.
 * @param[out] bytes_read Number of bytes actually read.
 * @return SQLI_OK on success.
 */
sqli_status sqli_result_read_sblob(sqli_result_t *res, size_t col_index,
                                   void *buf, size_t nbytes, size_t *bytes_read);

#ifdef __cplusplus
}
#endif

#endif /* SQLI_SBLOB_H */
