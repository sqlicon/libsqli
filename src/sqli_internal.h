#ifndef SQLI_H_INTERNAL
#define SQLI_H_INTERNAL

/*
 * SQLI internal connection and wire protocol definitions.
 *
 * NOT part of the public API. Include only from .c files.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <iconv.h>

/* Public API — must come before internal struct that references its types */
#include "libsqli/sqli.h"

/* ----------------------------------------------------------------
 * SL (Session Layer) constants
 * ---------------------------------------------------------------- */

#define SQLI_SL_HEADER_SIZE   ((size_t)6)
#define SQLI_SL_PROT_SQLI     60

#define SQLI_SLTYPE_CONREQ    1
#define SQLI_SLTYPE_CONACC    2
#define SQLI_SLTYPE_CONREJ    3
#define SQLI_SLTYPE_REDIRECT  13

/* ----------------------------------------------------------------
 * ASC-BINARY tag constants
 * ---------------------------------------------------------------- */

#define SQLI_TAG_ASC_BINARY        100
#define SQLI_TAG_VERSION_CAPS      101
#define SQLI_TAG_PRODUCT_TYPE      108
#define SQLI_TAG_STMT_OPTIONS      104
#define SQLI_TAG_ENV_VARS          106
#define SQLI_TAG_ENV_INFO          107
#define SQLI_TAG_APP_NAME          116
#define SQLI_TAG_END               127

/* ----------------------------------------------------------------
 * Environment variables
 * ---------------------------------------------------------------- */

#define ENV_PRIMARY_COUNT 12
#define ENV_SECONDARY_COUNT 2

extern const char *env_primary_list[ENV_PRIMARY_COUNT];
extern const char *env_secondary_list[ENV_SECONDARY_COUNT];

/* ----------------------------------------------------------------
 * SQLI protocol constants
 * ---------------------------------------------------------------- */

#define SQLI_PROTO_VERSION        61
#define SQLI_CLIENT_CAP_1         316

/* ----------------------------------------------------------------
 * SQLI message opcodes
 * ---------------------------------------------------------------- */

#define SQLI_SQ_COMMAND           1
#define SQLI_SQ_PREPARE           2
#define SQLI_SQ_ID                4
#define SQLI_SQ_BIND              5
#define SQLI_SQ_NFETCH            9
#define SQLI_SQ_CLOSE             10
#define SQLI_SQ_EXECUTE           7
#define SQLI_SQ_DESCRIBE          8
#define SQLI_SQ_RELEASE           11
#define SQLI_SQ_EOT               12
#define SQLI_SQ_CMMTWORK          19
#define SQLI_SQ_RBWORK            20
#define SQLI_SQ_SVPOINT           21
#define SQLI_SQ_NDESCRIBE         22
#define SQLI_SQ_SFETCH            23
#define SQLI_SQ_SCROLL            24
#define SQLI_SQ_BEGIN             35
#define SQLI_SQ_DBOPEN            36
#define SQLI_SQ_DBCLOSE           37
#define SQLI_SQ_FETCHBLOB         38
#define SQLI_SQ_BLOB              39
#define SQLI_SQ_DBOPEN_FLAGS      0
#define SQLI_SQ_HOLD              43
#define SQLI_SQ_ISOLEVEL          47
#define SQLI_SQ_LOCKWAIT          48
#define SQLI_SQ_WANTDONE          49
#define SQLI_SQ_COST              55
#define SQLI_SQ_VERSION           53
#define SQLI_SQ_EXIT              56
#define SQLI_SQ_INFO              81
#define SQLI_SQ_LODATA            97
#define SQLI_SQ_FILE              98
#define SQLI_SQ_INSERTDONE        94
#define SQLI_SQ_RET_TYPE          100
#define SQLI_SQ_XACTSTAT          99
#define SQLI_SQ_DONE              15
#define SQLI_SQ_TUPLE             14
#define SQLI_SQ_ERR               13
#define SQLI_SQ_ACK               128
#define SQLI_SQ_CHALLENGE         129
#define SQLI_SQ_RESPONSE          130
#define SQLI_SQ_ACCEPT            127
#define SQLI_SQ_PROTOCOLS         126
#define SQLI_SQ_VPUT              133
#define SQLI_SQ_SQLISETSVPT       137
#define SQLI_SQ_SQLIRELSVPT       138
#define SQLI_SQ_SQLIRBACKSVPT     139
#define SQLI_SQ_BATCHSTART        141
#define SQLI_SQ_BATCHEND          142

/* ----------------------------------------------------------------
 * Connection state machine
 * ---------------------------------------------------------------- */

typedef enum {
    SQLI_CONN_CLOSED,       /* Created, not yet connected */
    SQLI_CONN_CONNECTING,   /* TCP socket opened */
    SQLI_CONN_HANDSHAKE,    /* SL/ASC-BINARY exchange */
    SQLI_CONN_VERSION,      /* SQ_VERSION exchange */
    SQLI_CONN_INFO,         /* SQ_INFO capability negotiation */
    SQLI_CONN_AUTH,         /* PAM/PrivateServer auth */
    SQLI_CONN_DBOPEN,       /* SQ_DBOPEN */
    SQLI_CONN_READY,        /* Connection established */
    SQLI_CONN_ERROR         /* Error occurred */
} sqli_conn_state;

/* ----------------------------------------------------------------
 * Server capabilities
 * ---------------------------------------------------------------- */

typedef struct {
    int32_t cap_1;
    int32_t cap_2;
    int32_t cap_3;
    int has_pam;
    int has_bigint;
    int has_savepoints;
    int extended_describe;  /* USVER9_0343: extended DESCRIBE layout */
    int large_tuple_mode;   /* USVER9_0349: 4-byte tupleSize */
    int long_row_id;        /* USVER9_0350: 8-byte rows_affected/rowId */
    int float_to_dec;
    int read_only;
    int remove_64k_limit;   /* USVER9_0342: 4-byte statementOffset */
} sqli_capabilities;

/* ----------------------------------------------------------------
 * Connection structure (opaque to callers)
 * ---------------------------------------------------------------- */

struct sqli_conn {
    /* --- Error handling --- */
    char errmsg[256];
    sqli_error_info error_info;
    char error_context[48];
    uint16_t error_opcode;

    /* --- Logging --- */
    sqli_log_level log_level;

    /* --- TCP socket --- */
    int socket_fd;

    /* --- State machine --- */
    sqli_conn_state state;

    /* --- Server capabilities (set during handshake) --- */
    sqli_capabilities caps;

    /* --- Protocol state --- */
    bool database_open;
    bool strict_protocol;
    bool trim_trailing_spaces;
    sqli_cursor_type cursor_type;
    sqli_cursor_holdability holdability;

    /* --- Transaction state --- */
    bool in_transaction;       /* true if a transaction is active */
    bool in_batch;             /* true if batch protocol block is active */
    bool autocommit;           /* true = autocommit on, false = manual tx */
    sqli_isolation_level isolation; /* current transaction isolation level */

    /* --- I/O buffers --- */
    uint8_t *read_buf;
    size_t read_buf_pos;
    size_t read_buf_len;
    size_t read_buf_cap;

    uint8_t *write_buf;
    size_t write_buf_len;
    size_t write_buf_cap;

    /* --- Owned strings (copied from connect_params) --- */
    char *server;
    char *hostname;
    char *service;
    char *database;
    char *username;
    char *password;
    char *client_locale;
    char *db_locale;
    uint32_t fetch_buf_size;
    iconv_t decode_cd;
    bool decode_cd_ready;
    bool decode_cp1252_utf8;
    bool decode_locale_checked;

    /* --- Environment variables --- */
    char **env_vars;
    size_t env_var_count;
    size_t env_var_cap;
};

/* ----------------------------------------------------------------
 * Buffer helpers (sqli_conn.c)
 * ---------------------------------------------------------------- */

/* Grow a buffer to at least `min_size` bytes. */
sqli_status sqli_conn_grow_buf(sqli_conn_t *c, uint8_t **buf, size_t *len,
                               size_t *cap, size_t min_size);

/* Write helpers — append to conn->write_buf */
sqli_status sqli_conn_write_buf(sqli_conn_t *c, const uint8_t *data, size_t len);
sqli_status sqli_conn_write_byte(sqli_conn_t *c, uint8_t val);
sqli_status sqli_conn_write_be16(sqli_conn_t *c, uint16_t val);
sqli_status sqli_conn_write_be32(sqli_conn_t *c, uint32_t val);
sqli_status sqli_conn_write_str(sqli_conn_t *c, const char *s);
sqli_status sqli_conn_encode_client_to_db(sqli_conn_t *c, const char *s,
                                          uint8_t **out, size_t *out_len);

/* ----------------------------------------------------------------
 * SL header (sqli_wire.c)
 * ---------------------------------------------------------------- */

/* Encode SL header into buf. Returns bytes written, 0 on error. */
size_t sqli_wire_encode_sl_header(uint8_t *buf, size_t buf_size,
                                  uint16_t pdu_size, uint8_t sl_type,
                                  uint8_t sl_attr, uint16_t sl_opts);

/* Decode SL header from buf. Returns SQLI_OK on success. */
sqli_status sqli_wire_decode_sl_header(const uint8_t *buf, size_t buf_size,
                                       uint16_t *pdu_size, uint8_t *sl_type,
                                       uint8_t *sl_attr, uint16_t *sl_opts);

/* ----------------------------------------------------------------
 * ASC-BINARY encoding (sqli_asc.c)
 * ---------------------------------------------------------------- */

/* Encode a CONREQ body into buf. Returns bytes written, 0 on error. */
size_t sqli_asc_encode_conreq(sqli_conn_t *c, uint8_t *buf, size_t buf_size,
                              const char *transport);

/* Encode an IPC "sqAZ" preamble for onipcstr connections.
 * Returns bytes written, 0 on error. */
size_t sqli_asc_encode_ipc_preamble(sqli_conn_t *c, uint8_t *buf, size_t buf_size,
                                    const char *transport);

/* ----------------------------------------------------------------
 * TCP socket (sqli_tcp.c)
 * ---------------------------------------------------------------- */

int sqli_tcp_connect(const char *hostname, const char *service);
void sqli_tcp_close(int fd);
ssize_t sqli_tcp_read(int fd, uint8_t *buf, size_t count);
ssize_t sqli_tcp_send(int fd, const uint8_t *buf, size_t count);

/* ----------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------- */

static inline void clear_error(sqli_conn_t *conn)
{
    if (conn == NULL)
        return;
    conn->errmsg[0] = '\0';
    memset(&conn->error_info, 0, sizeof(conn->error_info));
    conn->error_context[0] = '\0';
    conn->error_opcode = 0;
}

static inline void set_error_context(sqli_conn_t *conn, const char *context, uint16_t opcode)
{
    if (conn == NULL)
        return;
    if (context != NULL && context[0] != '\0') {
        size_t n = strlen(context);
        if (n >= sizeof(conn->error_context))
            n = sizeof(conn->error_context) - 1;
        memcpy(conn->error_context, context, n);
        conn->error_context[n] = '\0';
    } else {
        conn->error_context[0] = '\0';
    }
    conn->error_opcode = opcode;
}

static inline void set_error(sqli_conn_t *conn, const char *msg)
{
    if (conn == NULL || msg == NULL)
        return;
    size_t len = (size_t)strlen(msg);
    if (len >= sizeof(conn->errmsg))
        len = sizeof(conn->errmsg) - 1;
    memcpy(conn->errmsg, msg, len);
    conn->errmsg[len] = '\0';
    conn->error_info.has_error = true;
    conn->error_info.status = SQLI_ERR;
    conn->error_info.sqlcode = 0;
    conn->error_info.isamcode = 0;
    conn->error_info.sqlstate[0] = '\0';
    conn->error_info.server_message[0] = '\0';
    conn->error_info.sql_message[0] = '\0';
    conn->error_info.isam_message[0] = '\0';
    if (conn->error_context[0] != '\0') {
        size_t c = strlen(conn->error_context);
        if (c >= sizeof(conn->error_info.context))
            c = sizeof(conn->error_info.context) - 1;
        memcpy(conn->error_info.context, conn->error_context, c);
        conn->error_info.context[c] = '\0';
    } else {
        conn->error_info.context[0] = '\0';
    }
    conn->error_info.opcode = conn->error_opcode;
    conn->error_info.opcode_name[0] = '\0';
    conn->error_info.unknown_opcode = 0;
    size_t ml = strlen(conn->errmsg);
    if (ml >= sizeof(conn->error_info.message))
        ml = sizeof(conn->error_info.message) - 1;
    memcpy(conn->error_info.message, conn->errmsg, ml);
    conn->error_info.message[ml] = '\0';
}

static inline void set_error_fmt(sqli_conn_t *conn, const char *fmt, ...)
{
    if (conn == NULL || fmt == NULL)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(conn->errmsg, sizeof(conn->errmsg), fmt, ap);
    va_end(ap);
    set_error(conn, conn->errmsg);
}

/* ----------------------------------------------------------------
 * Result structure (opaque to callers)
 * ---------------------------------------------------------------- */

/* SQLI type flags (also defined in sqli.h) */
#define SQLI_BIT_NOTNULLABLE   0x0100
#define SQLI_BIT_DISTINCT      0x0800
#define SQLI_BIT_NAMEDROW      0x1000
#define SQLI_BIT_DBOOLEAN      0x4000

typedef struct {
    char name[128];         /* column name */
    sqli_column_type type;  /* base type (without flags) */
    uint16_t flags;         /* type flags */
    uint32_t encoded_length; /* (precision << 8) | scale */
    uint32_t col_start_pos; /* offset within tuple */
    int32_t field_index;    /* field index */
} sqli_column_info;

struct sqli_result {
    sqli_conn_t *owner_conn; /* connection context for locale-aware decoding */
    /* --- Metadata --- */
    sqli_column_info *columns;
    int column_count;
    int64_t rows_affected;
    int64_t generated_serial;
    int64_t generated_serial8;
    bool has_generated_serial;
    bool has_generated_serial8;
    uint16_t statement_type;
    uint16_t warnings;
    int32_t stmt_id;        /* statement ID from PREPARE/DESCRIBE */
    bool ret_type_sent;     /* SQ_RET_TYPE already sent for this result/statement */
    sqli_cursor_type cursor_type;
    sqli_cursor_holdability holdability;

    /* --- Row storage --- */
    uint8_t **rows;         /* array of per-row data buffers */
    size_t *row_lens;       /* corresponding row data lengths */
    uint8_t *rows_arena;    /* contiguous storage for tuple payloads */
    size_t rows_arena_len;
    size_t rows_arena_cap;
    bool rows_use_arena;
    int row_count;          /* number of rows received */
    int row_capacity;       /* allocated capacity of rows/row_lens arrays */
    int cursor;             /* current position: -1 = before first row */
    uint64_t adaptive_tuple_bytes_total; /* observed tuple payload bytes */
    uint32_t adaptive_tuple_count;       /* observed tuple payload count */
    uint32_t adaptive_fetch_buf_size;    /* last selected fetch buffer size */

    /* --- Current-row view (set by sqli_result_next) --- */
    uint8_t *tuple_buffer;  /* points into rows[cursor]; NULL if not positioned */
    size_t tuple_capacity;  /* unused; kept for layout compatibility */
    size_t tuple_len;
    int current_row;        /* same as cursor; checked by typed accessors */
    int eof;                /* true after DONE received */
    int error_code;         /* non-zero if last row had error */
    bool last_was_null;     /* set by typed getters */
    bool saw_done;          /* set when SQ_DONE was observed */
    bool saw_error;         /* set when SQ_ERR was observed */
    size_t *cur_col_data_start; /* per-column data start for current row */
    size_t *cur_col_data_len;   /* per-column data len for current row */
    uint8_t *cur_col_is_null;   /* per-column null markers for current row */
    int cur_cache_row;          /* row index cached in arrays, -1 if invalid */
};

/* ----------------------------------------------------------------
 * Protocol dispatch (sqli_dispatch.c)
 * ---------------------------------------------------------------- */

sqli_status sqli_send_eot(int fd);
sqli_status sqli_send_prepare(sqli_conn_t *conn, const char *sql);
sqli_status sqli_send_execute(int fd, int stmt_id);
sqli_status sqli_send_open(int fd, int stmt_id, sqli_cursor_type cursor_type,
                           sqli_cursor_holdability holdability);
sqli_status sqli_send_fetch(int fd, int stmt_id, sqli_result_t *result);
sqli_status sqli_send_scroll_fetch(int fd, int stmt_id, sqli_result_t *result,
                                   uint16_t scroll_type, int32_t index);
sqli_status sqli_receive_dispatch(int fd, sqli_result_t *result, sqli_conn_t *conn);

/* Type codecs (sqli_types.c) */
int sqli_decode_decimal(const uint8_t *buf, size_t buf_size,
                        uint8_t precision, uint8_t *scale,
                        int *negative, uint8_t *digits);

/* Release heap members of a stack-allocated result (does NOT free the result itself) */
static inline void sqli_result_cleanup(sqli_result_t *r)
{
    if (r == NULL) return;
    free(r->columns);
    r->columns = NULL;
    if (r->rows != NULL) {
        if (!r->rows_use_arena) {
            for (int i = 0; i < r->row_count; i++)
                free(r->rows[i]);
        }
        free(r->rows);
        r->rows = NULL;
    }
    free(r->rows_arena);
    r->rows_arena = NULL;
    r->rows_arena_len = 0;
    r->rows_arena_cap = 0;
    r->rows_use_arena = false;
    free(r->row_lens);
    r->row_lens = NULL;
    r->row_count = 0;
    r->row_capacity = 0;
    r->cursor = -1;
    r->adaptive_tuple_bytes_total = 0;
    r->adaptive_tuple_count = 0;
    r->adaptive_fetch_buf_size = 0;
    r->current_row = -1;
    r->tuple_buffer = NULL;  /* pointed into rows[], already freed above */
    r->tuple_len = 0;
    r->last_was_null = false;
    free(r->cur_col_data_start);
    free(r->cur_col_data_len);
    free(r->cur_col_is_null);
    r->cur_col_data_start = NULL;
    r->cur_col_data_len = NULL;
    r->cur_col_is_null = NULL;
    r->cur_cache_row = -1;
    r->ret_type_sent = false;
}

/* ----------------------------------------------------------------
 * Prepared statements (sqli_prepare.c)
 * ---------------------------------------------------------------- */

/* Bind parameter type codes for SQ_BIND message */
typedef enum {
    SQLI_BIND_INT       = 2,    /* SQLI_TYPE_INT */
    SQLI_BIND_BIGINT    = 52,   /* SQLI_TYPE_BIGINT */
    SQLI_BIND_FLOAT     = 3,    /* SQLI_TYPE_FLOAT */
    SQLI_BIND_DECIMAL   = 5,    /* SQLI_TYPE_DECIMAL */
    SQLI_BIND_DATE      = 7,    /* SQLI_TYPE_DATE */
    SQLI_BIND_STRING    = 13,   /* SQLI_TYPE_VARCHAR */
    SQLI_BIND_BYTES     = 11,   /* SQLI_TYPE_BYTE */
    SQLI_BIND_NULL      = -1,   /* NULL marker */
} sqli_bind_type;

/* Bound parameter */
typedef struct {
    sqli_bind_type type;
    union {
        int32_t ival;
        int64_t ival64;
        double dval;
    } value;
    char *sval;       /* for SQLI_BIND_STRING */
    uint8_t *bval;    /* for SQLI_BIND_BYTES */
    size_t blen;      /* for SQLI_BIND_BYTES */
    bool is_null;
} sqli_bound_param;

/* Prepared statement */
struct sqli_stmt {
    int socket_fd;            /* connected socket fd */
    sqli_conn_t *conn;        /* owning connection (for capabilities) */
    int stmt_id;              /* server-assigned statement ID */
    int param_count;          /* number of parameters */
    bool read_only;           /* true for SELECT/WITH-style statements */
    sqli_bound_param *params; /* bound parameters (1-indexed, index 0 unused) */
    int param_cap;
    uint8_t *param_server_types; /* server-described types per parameter */
    int param_server_type_count; /* number of entries in param_server_types */
    bool executed;            /* true after sqli_execute() */

    /* Result from execution */
    sqli_result_t result;     /* result from last execution */
    bool result_valid;        /* true if result has valid data */
};

/* Extract a value from the current tuple buffer at the given column offset.
 * col_info points to the column metadata from DESCRIBE.
 * tuple_buf and tuple_len point to the current row data.
 * Returns SQLI_OK on success. */
sqli_status extract_value_from_tuple(const sqli_column_info *col_info,
                                     const uint8_t *tuple_buf, size_t tuple_len,
                                     uint8_t *out, size_t *out_len);

#endif /* SQLI_H_INTERNAL */
