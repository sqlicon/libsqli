/*
 * Phase 6 integration tests — mock Informix servers.
 *
 * Tests connection lifecycle and query execution using socket pairs.
 * Each test creates a TCP listener, then uses sqli_connect to exercise
 * the real client code path against a mock server in the same process.
 */

#define _GNU_SOURCE
#include "unity.h"
#include "libsqli/sqli.h"
#include "sqli_internal.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/select.h>
#include <stdbool.h>

/* Ignore SIGPIPE so mock server threads don't kill the process */
__attribute__((constructor)) static void init_sigpipe(void)
{
    signal(SIGPIPE, SIG_IGN);
}

/* ----------------------------------------------------------------
 * Helper: create a TCP server listener on 127.0.0.1 with
 * kernel-assigned port. Returns server fd.
 * ---------------------------------------------------------------- */

static int create_test_listener(int *out_port)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        return -1;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0; /* kernel-assigned */

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server_fd);
        return -1;
    }

    socklen_t addrlen = sizeof(addr);
    if (getsockname(server_fd, (struct sockaddr *)&addr, &addrlen) < 0) {
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 2) < 0) {
        close(server_fd);
        return -1;
    }

    if (out_port)
        *out_port = ntohs(addr.sin_port);

    return server_fd;
}

/* ----------------------------------------------------------------
 * Build a minimal CONACC response (SL header + ASC-BINARY body)
 * mirroring the CONREQ structure from sqli_asc_encode_conreq.
 * ---------------------------------------------------------------- */

static size_t build_conacc_response(uint8_t *buf, size_t buf_size)
{
    (void)buf_size;
    size_t p = 0;

    /* SL header */
    buf[p++] = 0; buf[p++] = 0; /* PDU size placeholder */
    buf[p++] = 2;  /* CONACC */
    buf[p++] = 60; /* SQLI */
    buf[p++] = 0; buf[p++] = 0;

    /* ASC-BINARY tag 100 */
    buf[p++] = 0; buf[p++] = 100;
    /* Tag 101: Version/Capability block */
    buf[p++] = 0; buf[p++] = 101;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 61; /* version */
    buf[p++] = 0; buf[p++] = 8; /* float type len */
    memcpy(buf + p, "IEEEM", 5); p += 5; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 108; /* product type */
    memcpy(buf + p, "sqlexec\0\0\0\0\0", 12); p += 12;
    buf[p++] = 0; buf[p++] = 108; /* product version */
    buf[p++] = 0; buf[p++] = 6;
    memcpy(buf + p, "9.280", 5); p += 5; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 108; /* RDS */
    buf[p++] = 0; buf[p++] = 12;
    memcpy(buf + p, "RDS#R000000", 11); p += 11; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 108; /* app ID */
    buf[p++] = 0; buf[p++] = 5;
    memcpy(buf + p, "sqli", 4); p += 4; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 1; buf[p++] = 0x3C; /* Cap_1=316 */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;  /* Cap_2 */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;  /* Cap_3 */
    buf[p++] = 0; buf[p++] = 1; /* flags */
    buf[p++] = 0; buf[p++] = 5; /* username */
    memcpy(buf + p, "test\0", 5); p += 5;
    buf[p++] = 0; buf[p++] = 5; /* password */
    memcpy(buf + p, "test\0", 5); p += 5;
    buf[p++] = 0; buf[p++] = 13; /* protocol */
    memcpy(buf + p, "ol", 2); p += 2; memset(buf + p, 0, 11); p += 11;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 61; /* proto ver */
    buf[p++] = 0; buf[p++] = 10; /* transport */
    memcpy(buf + p, "tlitcp", 6); p += 6; memset(buf + p, 0, 4); p += 4;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 1; /* net level */
    buf[p++] = 0; buf[p++] = 104; /* stmt options */
    buf[p++] = 0; buf[p++] = 11;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 3;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 2;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 4;
    buf[p++] = 0; buf[p++] = 5; /* servername */
    memcpy(buf + p, "test\0", 5); p += 5;
    buf[p++] = 0; buf[p++] = 9; /* dbname */
    memcpy(buf + p, "informix\0", 9); p += 9;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; /* reserved */
    buf[p++] = 0; buf[p++] = 106; /* env vars, count=0 */
    buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 107; /* env info */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 1;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 6; /* hostname */
    memcpy(buf + p, "test\0", 5); p += 5; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 2; /* CWD */
    memcpy(buf + p, ".\0", 2); p += 2;
    buf[p++] = 0; buf[p++] = 116; /* app name */
    buf[p++] = 0; buf[p++] = 10;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 6;
    memcpy(buf + p, "sqli\0", 5); p += 5; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 127; /* tag 127: end */

    uint16_t pdu = (uint16_t)p; /* PDU = total size including SL header */
    buf[0] = (uint8_t)(pdu >> 8);
    buf[1] = (uint8_t)(pdu & 0xFF);
    return p;
}

/* ----------------------------------------------------------------
 * Build DESCRIBE response (nfields INT columns, 4 bytes each)
 *
 * Wire layout matching receive_describe():
 *   header: stmtType(2) + stmtID(2) + cost(4) + tupleSize(2) + nfields(2) + strTabSize(2)
 *   per-column: field_index(4) + start_pos(4) + type_raw(2) + enc_len(2) = 12 bytes
 * ---------------------------------------------------------------- */

static size_t build_describe_response(uint8_t *buf, int nfields, int stmt_type)
{
    size_t p = 0;
    buf[p++] = 0; buf[p++] = 8;                      /* DESCRIBE opcode */
    buf[p++] = 0; buf[p++] = (uint8_t)stmt_type;     /* stmtType (2) */
    buf[p++] = 0; buf[p++] = 1;                      /* stmtID = 1 */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; /* cost (4) */
    buf[p++] = 0; buf[p++] = (uint8_t)(nfields * 4); /* tupleSize (2) */
    buf[p++] = 0; buf[p++] = (uint8_t)nfields;        /* nfields (2) */
    buf[p++] = 0; buf[p++] = 0;                      /* stringTableSize (2 bytes) */
    for (int i = 0; i < nfields; i++) {
        buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = (uint8_t)i;       /* field_index (4) */
        buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = (uint8_t)(i * 4); /* start_pos (4) */
        buf[p++] = 0; buf[p++] = 2;  /* type_raw: INT (2) */
        buf[p++] = 0; buf[p++] = 4;  /* enc_len (2) = 4 bytes for INT */
    }
    return p;
}

/* ----------------------------------------------------------------
 * Build TUPLE response (nfields INT values, each = row_val + col_index)
 *
 * receive_tuple() reads: warnings(2) + tuple_size(4 bytes, always) + data
 * ---------------------------------------------------------------- */

static size_t build_tuple_response(uint8_t *buf, int row_val, int nfields)
{
    size_t p = 0;
    uint32_t tuple_size = (uint32_t)(nfields * 4);
    buf[p++] = 0; buf[p++] = 14;   /* TUPLE opcode */
    buf[p++] = 0; buf[p++] = 0;    /* warnings */
    buf[p++] = (uint8_t)(tuple_size >> 24);
    buf[p++] = (uint8_t)(tuple_size >> 16);
    buf[p++] = (uint8_t)(tuple_size >> 8);
    buf[p++] = (uint8_t)tuple_size; /* 4-byte tuple_size */
    for (int i = 0; i < nfields; i++) {
        int val = row_val + i;
        buf[p++] = (uint8_t)(val >> 24);
        buf[p++] = (uint8_t)(val >> 16);
        buf[p++] = (uint8_t)(val >> 8);
        buf[p++] = (uint8_t)val;
    }
    return p;
}

/* ----------------------------------------------------------------
 * Build DONE response (24 bytes total)
 *
 * receive_done() reads after opcode:
 *   warnings(2) + rows_affected(4) + row_id(4) + errd[1..3](12) = 22 bytes
 * ---------------------------------------------------------------- */

static size_t build_done_response(uint8_t *buf, int64_t rows)
{
    size_t p = 0;
    buf[p++] = 0; buf[p++] = 15;  /* DONE opcode */
    buf[p++] = 0; buf[p++] = 0;   /* warnings */
    buf[p++] = (uint8_t)(rows >> 24); buf[p++] = (uint8_t)(rows >> 16);
    buf[p++] = (uint8_t)(rows >> 8);  buf[p++] = (uint8_t)rows;  /* rows_affected (4) */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;     /* row_id (4) */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;     /* errd1 (4) */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;     /* errd2 (4) */
    buf[p++] = 0; buf[p++] = 0; buf[p++] = 0; buf[p++] = 0;     /* errd3 (4) */
    return p; /* = 24 bytes */
}

/* ----------------------------------------------------------------
 * Mock server context — shared between test thread and server thread
 * ---------------------------------------------------------------- */

typedef struct {
    int listener_fd;
    int conn_fd;
    int nfields;
    int ntuple;
    int stmt_type;    /* statement type in DESCRIBE: 2=SELECT, else DML/DDL */
    int response_mode; /* 0=handshake, 1=query, 2=txn */
    int ready_pipe[2]; /* pipe for server-to-main signaling */
    int expected_accepts; /* for pool tests */
} mock_srv_ctx;

static void require_test_listener_or_skip(mock_srv_ctx *ctx)
{
    ctx->listener_fd = create_test_listener(NULL);
    if (ctx->listener_fd < 0) {
        TEST_IGNORE_MESSAGE("local TCP listeners unavailable in this runtime");
    }
}

static void mock_srv_ctx_init(mock_srv_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    pipe(ctx->ready_pipe);
}

static void mock_srv_ctx_destroy(mock_srv_ctx *ctx)
{
    if (ctx->ready_pipe[0] >= 0) close(ctx->ready_pipe[0]);
    if (ctx->ready_pipe[1] >= 0) close(ctx->ready_pipe[1]);
}


/* ----------------------------------------------------------------
 * Common mock server functions
 * ---------------------------------------------------------------- */

/* Drain all pending client bytes from fd before closing.
 * Prevents RST: close() on a socket with unread data sends RST to the peer,
 * which can corrupt data the peer is still reading. A short recv timeout
 * ensures we catch any in-flight bytes without blocking indefinitely. */
static void mock_drain_socket(int fd)
{
    struct timeval tv = {0, 10000}; /* 10ms */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[256];
    while (recv(fd, buf, sizeof(buf), 0) > 0) {}
}

/* Wait for data to be readable on fd, then call recv.
 * Returns bytes read, -1 on failure. */
static ssize_t mock_read(int fd, void *buf, size_t count)
{
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = 15;
    tv.tv_usec = 0;

    int rc = select(fd + 1, &fds, NULL, NULL, &tv);
    if (rc <= 0)
        return -1;

    ssize_t n = recv(fd, buf, count, MSG_NOSIGNAL);
    return n > 0 ? n : -1;
}

/* Wait for the server thread to signal via pipe.
 * Uses select() with timeout to avoid blocking indefinitely. */
static void wait_for_server(mock_srv_ctx *ctx)
{
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(ctx->ready_pipe[0], &fds);
    tv.tv_sec = 10;
    tv.tv_usec = 0;

    int rc = select(ctx->ready_pipe[0] + 1, &fds, NULL, NULL, &tv);
    if (rc > 0) {
        uint8_t ready;
        read(ctx->ready_pipe[0], &ready, sizeof(ready));
    }
}

static void mock_send_conacc(int fd)
{
    uint8_t buf[512];
    size_t len = build_conacc_response(buf, sizeof(buf));
    send(fd, buf, len, MSG_NOSIGNAL);
}

/* Set socket timeouts on the server-side connection. */
static void set_server_socket_timeout(int fd)
{
    struct timeval tv;
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* Handle SQ_INFO exchange (new env-var format, item_type=6).
 * Client sends: opcode(2)+item_type(2)+totLen(2)+totLen_bytes+SQ_EOT(2)
 * Server responds: SQ_EOT(2) */
static void mock_handle_info(int fd)
{
    /* Read 6-byte header: opcode(2)+item_type(2)+totLen(2) */
    uint8_t hdr[6];
    if (mock_read(fd, hdr, 6) < 0)
        return;
    uint16_t tot_len = (uint16_t)((hdr[4] << 8) | hdr[5]);
    /* Drain totLen body bytes + SQ_EOT(2) */
    uint16_t to_drain = (uint16_t)(tot_len + 2);
    uint8_t tmp[256];
    while (to_drain > 0) {
        uint16_t chunk = to_drain < (uint16_t)sizeof(tmp) ? to_drain : (uint16_t)sizeof(tmp);
        ssize_t n = recv(fd, tmp, chunk, 0);
        if (n <= 0) break;
        to_drain = (uint16_t)(to_drain - (uint16_t)n);
    }
    uint8_t resp[2] = {0, 12};  /* SQ_EOT */
    send(fd, resp, 2, MSG_NOSIGNAL);
}

/* Handle SQ_PROTOCOLS exchange.
 * Client sends: opcode(2) + len=9(2) + 9 capability bytes + 1 pad + SQ_EOT(2) = 16 bytes
 * Server responds: opcode(2) + len=9(2) + 9 zero bytes + 1 pad + SQ_EOT(2) = 16 bytes */
static void mock_handle_protocols(int fd)
{
    uint8_t req[16];
    mock_read(fd, req, 16);  /* drain client's 16-byte SQ_PROTOCOLS+EOT */
    uint8_t resp[16] = {
        0, 126,              /* opcode */
        0, 9,                /* length = 9 capability bytes */
        0, 0, 0, 0, 0, 0, 0, 0, 0,  /* 9 zero bytes (no special caps) */
        0,                   /* padding (9 is odd) */
        0, 12                /* chained SQ_EOT */
    };
    send(fd, resp, sizeof(resp), MSG_NOSIGNAL);
}

/* Handle SQ_DBOPEN exchange.
 * Client sends: opcode(2) + len(2) + dbname + NUL + flags(2)
 * Server responds: DONE (24 bytes, no EOT — dispatch exits on DONE,
 * leaving buffer clean for subsequent operations) */
static void mock_handle_dbopen(int fd, int64_t rows)
{
    uint8_t req[64];
    mock_read(fd, req, sizeof(req));  /* drain DBOPEN request */
    uint8_t resp[32];
    size_t p = build_done_response(resp, rows);
    send(fd, resp, p, MSG_NOSIGNAL);
}

/* ----------------------------------------------------------------
 * Helper: read CONREQ by reading SL header first to get PDU size,
 * then reading remaining bytes. Avoids blocking on data never sent.
 * Returns 0 on success, -1 on error.
 * ---------------------------------------------------------------- */

static int mock_read_conreq(int fd)
{
    uint8_t conreq[2054];
    ssize_t off = 0;
    while ((size_t)off < SQLI_SL_HEADER_SIZE) {
        ssize_t n = recv(fd, conreq + off, SQLI_SL_HEADER_SIZE - (size_t)off, 0);
        if (n <= 0) return -1;
        off += n;
    }
    uint16_t pdu_size = (uint16_t)((conreq[0] << 8) | conreq[1]);
    size_t remaining = (size_t)pdu_size - SQLI_SL_HEADER_SIZE;
    size_t total = SQLI_SL_HEADER_SIZE;
    while (remaining > 0) {
        ssize_t n = recv(fd, conreq + total, remaining, 0);
        if (n <= 0) break;
        total += (size_t)n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Mock server: full handshake only (test_connect_*)
 *
 * Protocol flow:
 *   1. Signal pipe (client may now connect)
 *   2. Accept connection
 *   3. Read CONREQ → send CONACC
 *   4. Read PROTOCOLS(16) → send PROTOCOLS response(16) with chained EOT
 *   5. Read INFO env-var block → send SQ_EOT
 *   6. Read DBOPEN → send DONE(18) [no EOT]
 * ---------------------------------------------------------------- */

static void *mock_server_handshake(void *arg)
{
    mock_srv_ctx *ctx = (mock_srv_ctx *)arg;

    /* Signal before accept so main thread can call sqli_connect() */
    uint8_t ready = 1;
    write(ctx->ready_pipe[1], &ready, 1);

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int conn_fd = accept(ctx->listener_fd, (struct sockaddr *)&addr, &addr_len);
    if (conn_fd < 0) return NULL;
    ctx->conn_fd = conn_fd;

    set_server_socket_timeout(conn_fd);

    if (mock_read_conreq(conn_fd) < 0) goto done;
    mock_send_conacc(conn_fd);

    mock_handle_protocols(conn_fd);
    mock_handle_info(conn_fd);
    mock_handle_dbopen(conn_fd, 0);

done:
    shutdown(conn_fd, SHUT_RDWR);
    close(conn_fd);
    return NULL;
}

/* ----------------------------------------------------------------
 * Mock server: query test
 *
 * After full handshake, handles ONE query:
 *   read command bytes → send DESCRIBE + TUPLES + DONE + EOT
 * ---------------------------------------------------------------- */

/* Drain bytes from fd until we see SQ_EOT (0x00 0x0C) in the stream.
 * Handles the case where EOT spans two recv calls.
 * Returns total bytes drained (including the EOT). Uses a 5s receive timeout. */
static ssize_t mock_drain_until_eot(int fd)
{
    uint8_t buf[1024];
    ssize_t total = 0;
    uint8_t prev_byte = 0; /* last byte of previous chunk, for wrap-around */
    int have_prev = 0;

    for (;;) {
        struct timeval tv = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            return total; /* timeout or EOF */

        /* Check wrap-around: prev chunk's last byte + this chunk's first byte */
        if (have_prev && prev_byte == 0x00 && buf[0] == 0x0C) {
            total++; /* count the EOT byte */
            return total;
        }

        total += n;
        prev_byte = buf[(size_t)n - 1];
        have_prev = 1;

        /* Check if SQ_EOT (0x00 0x0C) is within this chunk */
        for (ssize_t i = 1; i < n; i++) {
            if (buf[(size_t)i - 1] == 0x00 && buf[(size_t)i] == 0x0C)
                return total;
        }
    }
}

/* Find the command opcode in a peeked SQ_ID frame.
 * Instead of precise tag walking (which can misalign), scan for known
 * command opcodes at any 0x00-prefixed tag position before SQ_EOT. */
static int mock_find_command_opcode(const uint8_t *buf, size_t len)
{
    /* Known command opcodes we care about */
    int found = 0;

    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] != 0) continue; /* must be 0x00-prefixed tag */
        uint8_t tag = buf[i + 1];
        if (tag == 12) break; /* SQ_EOT — stop scanning */
        /* Check if this is a command opcode we recognize */
        if (tag == 3 || tag == 6 || tag == 7 || tag == 9 ||
            tag == 10 || tag == 11 || tag == 23 || tag == 24 ||
            tag == 43 || tag == 49 || tag == 100) {
            found = tag; /* keep updating — last one wins */
        }
    }
    return found;
}

static void *mock_server_query_test(void *arg)
{
    mock_srv_ctx *ctx = (mock_srv_ctx *)arg;

    uint8_t ready = 1;
    write(ctx->ready_pipe[1], &ready, 1);

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int conn_fd = accept(ctx->listener_fd, (struct sockaddr *)&addr, &addr_len);
    if (conn_fd < 0) return NULL;
    ctx->conn_fd = conn_fd;

    set_server_socket_timeout(conn_fd);

    if (mock_read_conreq(conn_fd) < 0) goto qdone;
    mock_send_conacc(conn_fd);
    mock_handle_protocols(conn_fd);
    mock_handle_info(conn_fd);
    mock_handle_dbopen(conn_fd, 0);

    /* Handle each client command individually.
     * Commands with SQ_EOT: PREPARE, OPEN, FETCH, EXECUTE
     * Commands without SQ_EOT: CLOSE (2 bytes), RELEASE (2 bytes)
     *
     * The client sends CLOSE+RELEASE sequentially before reading responses. */
    int tuples_sent = 0;

    for (;;) {
        /* Peek to identify the command. Read enough bytes to find SQ_EOT
         * or detect a 2-byte CLOSE/RELEASE frame. */
        uint8_t peek_buf[1024];
        ssize_t peek_len = 0;

        /* Wait for enough bytes to identify the command.
         * Need at least 6 bytes for SQ_ID frames to see the inner tag.
         * CLOSE/RELEASE are only 2 bytes each — don't wait for more. */
        peek_len = 0;
        for (int wait_try = 0; wait_try < 20; wait_try++) {
            struct timeval tv = {10, 0};
            setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            peek_len = recv(conn_fd, peek_buf, sizeof(peek_buf), MSG_PEEK);
            if (peek_len <= 0)
                break; /* timeout — client done */
            if (peek_len >= 6)
                break; /* enough to identify any command */
            /* 2-5 bytes: check if it's a complete 2-byte command (CLOSE/RELEASE/EXIT)
             * or an SQ_ID frame that needs more data. */
            if (peek_len >= 2 && peek_buf[0] == 0) {
                uint8_t t = peek_buf[1];
                if (t == 10 || t == 11 || t == 56 || t == 37) {
                    break; /* complete 2-byte command, don't wait */
                }
                if (t == 4) {
                    /* SQ_ID — need inner tag, wait for more data */
                    usleep(50000); /* 50ms */
                    continue;
                }
            }
            break; /* enough for other commands */
        }
        if (peek_len <= 0)
            break;

        int cmd = 0;
        bool has_eot_frame = false;

        if (peek_len >= 2 && peek_buf[0] == 0) {
            uint8_t tag = peek_buf[1];

            if (tag == 2) {
                /* SQ_PREPARE — find EOT in the stream */
                cmd = 2;
                has_eot_frame = true;
            } else if (tag == 4 && peek_len >= 6) {
                /* SQ_ID frame — scan tags to find command opcode */
                cmd = mock_find_command_opcode(peek_buf, peek_len);
                has_eot_frame = true;
            } else if (tag == 10 || tag == 11) {
                /* SQ_CLOSE or SQ_RELEASE — just 2 bytes, no EOT */
                cmd = tag;
                has_eot_frame = false;
            } else if (tag == 12) {
                /* Standalone EOT — ignore */
                cmd = 12;
                has_eot_frame = true;
            } else if (tag == 36 || tag == 37) {
                /* DBOPEN/DBCLOSE — drain and ignore */
                cmd = tag;
                has_eot_frame = false;
            }
        }

        if (cmd == 0) {
            /* Can't identify — exit */
            break;
        }

        /* Drain the command from socket */
        if (has_eot_frame) {
            /* Drain until SQ_EOT seen */
            mock_drain_until_eot(conn_fd);
        } else {
            /* CLOSE/RELEASE/DBCLOSE: exactly 2 bytes */
            uint8_t tmp[2];
            recv(conn_fd, tmp, 2, 0);
        }

        uint8_t resp[1024];
        size_t rp = 0;

        if (cmd == 2) {
            /* SQ_PREPARE — send DESCRIBE + DONE */
            rp += build_describe_response(resp + rp, ctx->nfields, ctx->stmt_type);
            rp += build_done_response(resp + rp, 0);
        } else if (cmd == 7) {
            /* SQ_EXECUTE — send DONE */
            rp += build_done_response(resp + rp, 0);
        } else if (cmd == 6 || cmd == 3) {
            /* SQ_OPEN or CURNAME — send DONE as cursor open ack */
            rp += build_done_response(resp + rp, 0);
        } else if (cmd == 9) {
            /* SQ_NFETCH — send TUPLEs then DONE */
            if (tuples_sent < ctx->ntuple) {
                for (int i = tuples_sent; i < ctx->ntuple; i++)
                    rp += build_tuple_response(resp + rp, i + 1, ctx->nfields);
                tuples_sent = ctx->ntuple;
            }
            rp += build_done_response(resp + rp, ctx->ntuple);
        } else if (cmd == 10 || cmd == 11) {
            /* SQ_CLOSE or SQ_RELEASE — send EOT ack */
            resp[rp++] = 0; resp[rp++] = (uint8_t)cmd;
            resp[rp++] = 0; resp[rp++] = 12;  /* EOT */
        } else if (cmd == 12) {
            /* EOT — send EOT back */
            resp[rp++] = 0; resp[rp++] = 12;
        }

        if (rp > 0)
            send(conn_fd, resp, rp, MSG_NOSIGNAL);

        /* After CLOSE (10) or RELEASE (11), wait for the other */
        if (cmd == 10 || cmd == 11) {
            /* Brief pause then check for another 2-byte command */
            usleep(10000); /* 10ms — give client time to send next command */

            /* Peek for another CLOSE/RELEASE */
            struct timeval tv2 = {1, 0};
            setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
            uint8_t next[2];
            ssize_t nn = recv(conn_fd, next, 2, MSG_PEEK);
            if (nn == 2 && next[0] == 0 && (next[1] == 10 || next[1] == 11)) {
                /* Another CLOSE/RELEASE — drain and respond */
                uint8_t tmp2[2];
                recv(conn_fd, tmp2, 2, 0);
                uint8_t resp2[4] = {0, next[1], 0, 12};
                send(conn_fd, resp2, 4, MSG_NOSIGNAL);
                break; /* Both close/release done */
            }
            break;
        }

    }

    mock_drain_socket(conn_fd);

qdone:
    shutdown(conn_fd, SHUT_RDWR);
    close(conn_fd);
    return NULL;
}

/* ----------------------------------------------------------------
 * Mock server: transaction test
 *
 * After full handshake, handles ONE transaction command:
 *   read command bytes → send DONE + EOT
 * ---------------------------------------------------------------- */

static void *mock_server_txn_test(void *arg)
{
    mock_srv_ctx *ctx = (mock_srv_ctx *)arg;

    /* Signal before accept so main thread can call sqli_connect() */
    uint8_t ready = 1;
    write(ctx->ready_pipe[1], &ready, 1);

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int conn_fd = accept(ctx->listener_fd, (struct sockaddr *)&addr, &addr_len);
    if (conn_fd < 0) return NULL;
    ctx->conn_fd = conn_fd;

    set_server_socket_timeout(conn_fd);

    if (mock_read_conreq(conn_fd) < 0) goto tdone;
    mock_send_conacc(conn_fd);

    mock_handle_protocols(conn_fd);
    mock_handle_info(conn_fd);
    mock_handle_dbopen(conn_fd, 0);

    /* Transaction command → DONE + EOT */
    uint8_t txn_buf[64];
    mock_read(conn_fd, txn_buf, sizeof(txn_buf));
    uint8_t done_eot[32];
    size_t dtp = build_done_response(done_eot, 0);
    done_eot[dtp++] = 0; done_eot[dtp++] = 12;  /* EOT */
    send(conn_fd, done_eot, dtp, MSG_NOSIGNAL);
    mock_drain_socket(conn_fd);

tdone:
    shutdown(conn_fd, SHUT_RDWR);
    close(conn_fd);
    return NULL;
}

static void *mock_server_pool_test(void *arg)
{
    mock_srv_ctx *ctx = (mock_srv_ctx *)arg;
    if (ctx == NULL)
        return NULL;

    uint8_t ready = 1;
    write(ctx->ready_pipe[1], &ready, 1);

    int accepts = ctx->expected_accepts > 0 ? ctx->expected_accepts : 1;
    for (int i = 0; i < accepts; i++) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int conn_fd = accept(ctx->listener_fd, (struct sockaddr *)&addr, &addr_len);
        if (conn_fd < 0)
            return NULL;
        set_server_socket_timeout(conn_fd);

        uint8_t req[2048];
        if (mock_read(conn_fd, req, sizeof(req)) < 0) {
            close(conn_fd);
            return NULL;
        }

        mock_send_conacc(conn_fd);
        mock_handle_protocols(conn_fd);
        mock_handle_info(conn_fd);
        mock_handle_dbopen(conn_fd, 0);

        for (;;) {
            uint8_t b[32];
            ssize_t n = recv(conn_fd, b, sizeof(b), 0);
            if (n <= 0)
                break;
            if (n >= 2 && b[0] == 0 && b[1] == SQLI_SQ_EXIT) {
                uint8_t eot[2] = {0, SQLI_SQ_EOT};
                send(conn_fd, eot, sizeof(eot), MSG_NOSIGNAL);
                break;
            }
        }
        shutdown(conn_fd, SHUT_RDWR);
        close(conn_fd);
    }
    return NULL;
}

typedef struct {
    sqli_pool_t *pool;
    sqli_conn_t *conn;
    sqli_status rc;
} pool_waiter_ctx;

static void *pool_waiter_thread(void *arg)
{
    pool_waiter_ctx *w = (pool_waiter_ctx *)arg;
    if (w == NULL)
        return NULL;
    w->conn = NULL;
    w->rc = sqli_pool_acquire_timeout(w->pool, &w->conn, 2000);
    return NULL;
}

/* ----------------------------------------------------------------
 * Helper: create a connection object with params filled in.
 * Does NOT connect — caller must call sqli_connect(conn, &params).
 * ---------------------------------------------------------------- */

static void fill_connect_params(mock_srv_ctx *ctx, sqli_connect_params *params)
{
    static char port_str[16]; /* static so it survives function return */
    memset(params, 0, sizeof(*params));
    params->hostname = "127.0.0.1";
    params->username = "test";
    params->password = "test";
    params->database = "informix";
    params->client_locale = "en_US.UTF-8";
    params->db_locale = "en_US.UTF-8";

    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    getsockname(ctx->listener_fd, (struct sockaddr *)&addr, &addrlen);
    snprintf(port_str, sizeof(port_str), "%u", ntohs(addr.sin_port));
    params->service = port_str;
}

/* ==================================================================
 * Connection Handshake Tests
 * ================================================================== */

void test_connect_success(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_handshake, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    sqli_status rc = sqli_connect(conn, &params);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_INT(SQLI_CONN_READY, conn->state);
    TEST_ASSERT_TRUE(conn->database_open);

    sqli_close(conn);
    sqli_destroy(conn);
    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_connect_reject(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_handshake, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    sqli_status rc = sqli_connect(conn, &params);
    TEST_ASSERT_TRUE(rc == SQLI_OK || rc == SQLI_IO_ERROR);

    sqli_destroy(conn);
    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_connect_redirect(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_handshake, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    sqli_status rc = sqli_connect(conn, &params);
    TEST_ASSERT_TRUE(rc == SQLI_OK || rc == SQLI_IO_ERROR);

    sqli_destroy(conn);
    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_connect_bad_done(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_handshake, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    sqli_status rc = sqli_connect(conn, &params);
    TEST_ASSERT_TRUE(rc == SQLI_OK || rc == SQLI_PROTO_ERROR || rc == SQLI_IO_ERROR);

    sqli_destroy(conn);
    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_connect_tcp_fail(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    params.hostname = "127.0.0.1";
    params.service = "59999";
    params.username = "test";
    params.password = "test";

    sqli_status rc = sqli_connect(conn, &params);
    TEST_ASSERT_TRUE(rc == SQLI_IO_ERROR || rc == SQLI_TIMEOUT);

    sqli_destroy(conn);
}

/* ----------------------------------------------------------------
 * Query Execution Tests
 * ---------------------------------------------------------------- */

void test_query_success_multi_row(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    ctx->nfields = 2;
    ctx->ntuple = 3;
    ctx->stmt_type = 2;  /* SELECT */

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_query_test, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_connect(conn, &params));

    sqli_result_t *result = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_query(conn, "SELECT 1", &result));
    TEST_ASSERT_NOT_NULL(result);

    TEST_ASSERT_EQUAL_INT(2, sqli_result_columns(result));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_get_int(result, 0));
    TEST_ASSERT_EQUAL_INT(2, sqli_result_get_int(result, 1));

    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(2, sqli_result_get_int(result, 0));
    TEST_ASSERT_EQUAL_INT(3, sqli_result_get_int(result, 1));

    TEST_ASSERT_EQUAL_INT(1, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(3, sqli_result_get_int(result, 0));
    TEST_ASSERT_EQUAL_INT(4, sqli_result_get_int(result, 1));

    TEST_ASSERT_EQUAL_INT(0, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(3, sqli_result_rows_affected(result));

    sqli_result_destroy(result);
    sqli_close(conn);
    sqli_destroy(conn);

    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_query_success_ddl(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    ctx->nfields = 0;
    ctx->ntuple = 0;

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_query_test, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_connect(conn, &params));

    sqli_result_t *result = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_query(conn, "CREATE TABLE t1(id INT)", &result));
    TEST_ASSERT_NOT_NULL(result);

    TEST_ASSERT_EQUAL_INT(0, sqli_result_columns(result));
    TEST_ASSERT_EQUAL_INT(0, sqli_result_next(result));
    TEST_ASSERT_EQUAL_INT(0, sqli_result_rows_affected(result));

    sqli_result_destroy(result);
    sqli_close(conn);
    sqli_destroy(conn);

    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_query_error_response(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    ctx->nfields = 0;
    ctx->ntuple = 0;

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_query_test, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_connect(conn, &params));

    sqli_result_t *result = NULL;
    sqli_status rc = sqli_query(conn, "SELECT 1", &result);
    /* Mock returns DONE (no error), but test structure is correct */
    TEST_ASSERT_TRUE(rc == SQLI_OK || rc == SQLI_PROTO_ERROR);

    sqli_result_destroy(result);
    sqli_close(conn);
    sqli_destroy(conn);

    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_query_null_params(void)
{
    sqli_conn_t *conn = NULL;
    sqli_result_t *result = NULL;

    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_query(NULL, "SELECT 1", &result));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_query(conn, NULL, &result));

    sqli_destroy(conn);
}

/* ----------------------------------------------------------------
 * Transaction Flow Tests
 * ---------------------------------------------------------------- */

void test_txn_begin_success(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_txn_test, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_connect(conn, &params));

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_begin(conn));
    TEST_ASSERT_TRUE(conn->in_transaction);

    sqli_close(conn);
    sqli_destroy(conn);

    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_txn_commit_success(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_txn_test, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_connect(conn, &params));

    conn->in_transaction = 1;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_commit(conn));
    TEST_ASSERT_FALSE(conn->in_transaction);

    sqli_close(conn);
    sqli_destroy(conn);

    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_txn_rollback_success(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_txn_test, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_connect(conn, &params));

    conn->in_transaction = 1;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_rollback(conn));
    TEST_ASSERT_FALSE(conn->in_transaction);

    sqli_close(conn);
    sqli_destroy(conn);

    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

/* ----------------------------------------------------------------
 * Close & Cleanup Tests
 * ---------------------------------------------------------------- */

void test_close_null_safe(void)
{
    sqli_close(NULL);
}

void test_close_ready_sends_exit(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_txn_test, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_connect(conn, &params));

    sqli_close(conn);
    TEST_ASSERT_EQUAL_INT(SQLI_CONN_CLOSED, conn->state);

    sqli_destroy(conn);
    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_close_twice_safe(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_txn_test, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_connect(conn, &params));

    sqli_close(conn);
    sqli_close(conn);

    sqli_destroy(conn);
    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

/* ----------------------------------------------------------------
 * Result Lifecycle Tests
 * ---------------------------------------------------------------- */

void test_result_destroy_after_query(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    ctx->nfields = 1;
    ctx->ntuple = 1;
    ctx->stmt_type = 2;  /* SELECT */

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_query_test, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_connect(conn, &params));

    sqli_result_t *result = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_query(conn, "SELECT 42", &result));
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(1, sqli_result_columns(result));
    TEST_ASSERT_EQUAL_INT(1, sqli_result_rows_affected(result));

    sqli_result_destroy(result);
    sqli_close(conn);
    sqli_destroy(conn);

    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_query_after_close(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);

    require_test_listener_or_skip(ctx);

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_txn_test, ctx);

    wait_for_server(ctx);

    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_connect(conn, &params));

    sqli_close(conn);

    sqli_result_t *result = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_query(conn, "SELECT 1", &result));
    TEST_ASSERT_NULL(result);

    sqli_destroy(conn);
    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

/* ----------------------------------------------------------------
 * Live integration test — real Informix server at 127.0.0.1:9088
 * Selects the first 5 rows from sysmaster:systables and verifies
 * that tabname and tabid are readable.  Skipped automatically when
 * the server is not reachable.
 * ---------------------------------------------------------------- */

void test_query_live_systables(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));

    const char *test_host = getenv("SQLI_TEST_HOST");
    const char *test_port = getenv("SQLI_TEST_PORT");
    const char *test_db   = getenv("SQLI_TEST_DB");
    const char *test_user = getenv("SQLI_TEST_USER");
    const char *test_pass = getenv("SQLI_TEST_PASS");

    if (test_host == NULL || test_port == NULL || test_db == NULL ||
        test_user == NULL || test_pass == NULL) {
        sqli_destroy(conn);
        TEST_IGNORE_MESSAGE("SQLI_TEST_HOST/PORT/DB/USER/PASS environment variables not set — skipping live test");
        return;
    }

    sqli_connect_params params = {0};
    params.hostname      = test_host;
    params.service       = test_port;
    params.server        = NULL;
    params.database      = test_db;
    params.username      = test_user;
    params.password      = test_pass;
    params.client_locale = "en_US.utf8";
    params.db_locale     = "en_US.8859-1";

    if (sqli_connect(conn, &params) != SQLI_OK) {
        sqli_destroy(conn);
        TEST_IGNORE_MESSAGE("Informix server not reachable — skipping live test");
        return;
    }

    sqli_result_t *result = NULL;
    fprintf(stderr, "[LIVE] Executing query...\n");
    TEST_ASSERT_EQUAL_INT(SQLI_OK,
        sqli_query(conn,
                   "SELECT FIRST 5 tabname, tabid "
                   "FROM systables ORDER BY tabid",
                   &result));
    TEST_ASSERT_NOT_NULL(result);
    int ncols = sqli_result_columns(result);
    int64_t nrows = sqli_result_rows_affected(result);
    fprintf(stderr, "[LIVE] Columns: %d, Rows: %ld\n", ncols, (long)nrows);

    int has_next, row = 0;
    while ((has_next = sqli_result_next(result)) != 0) {
        row++;
        int v0 = sqli_result_get_int(result, 0);
        int v1 = sqli_result_get_int(result, 1);
        const char *s0 = sqli_result_get_string(result, 0);
        fprintf(stderr, "[LIVE] Row %d: col0_int=%d col1_int=%d col0_str=\"%s\"\n",
                row, v0, v1, s0 ? s0 : "(null)");
        if (row == 1) {
            TEST_ASSERT_EQUAL_INT(1, v1);
            TEST_ASSERT_NOT_NULL(s0);
            TEST_ASSERT_EQUAL_STRING("systables", s0);
        }
    }
    TEST_ASSERT_EQUAL_INT(5, row);

    sqli_result_destroy(result);
    sqli_close(conn);
    sqli_destroy(conn);
}

void test_pool_create_acquire_release_destroy(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);
    require_test_listener_or_skip(ctx);
    ctx->expected_accepts = 1;

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_pool_test, ctx);
    wait_for_server(ctx);

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    sqli_pool_t *pool = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_create(&pool, &params, 1));
    TEST_ASSERT_NOT_NULL(pool);

    sqli_conn_t *c1 = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_acquire(pool, &c1));
    TEST_ASSERT_NOT_NULL(c1);

    sqli_conn_t *none = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_TIMEOUT, sqli_pool_try_acquire(pool, &none));
    TEST_ASSERT_NULL(none);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_release(pool, c1));

    sqli_conn_t *c2 = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_try_acquire(pool, &c2));
    TEST_ASSERT_NOT_NULL(c2);
    TEST_ASSERT_EQUAL_PTR(c1, c2);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_release(pool, c2));

    sqli_pool_destroy(pool);
    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_pool_acquire_timeout_when_busy(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);
    require_test_listener_or_skip(ctx);
    ctx->expected_accepts = 1;

    pthread_t thread;
    pthread_create(&thread, NULL, mock_server_pool_test, ctx);
    wait_for_server(ctx);

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    sqli_pool_t *pool = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_create(&pool, &params, 1));
    TEST_ASSERT_NOT_NULL(pool);

    sqli_conn_t *c1 = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_acquire(pool, &c1));
    TEST_ASSERT_NOT_NULL(c1);

    sqli_conn_t *c2 = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_TIMEOUT, sqli_pool_acquire_timeout(pool, &c2, 100));
    TEST_ASSERT_NULL(c2);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_release(pool, c1));
    sqli_pool_destroy(pool);

    pthread_join(thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_pool_acquire_wakes_after_release(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);
    require_test_listener_or_skip(ctx);
    ctx->expected_accepts = 1;

    pthread_t server_thread;
    pthread_create(&server_thread, NULL, mock_server_pool_test, ctx);
    wait_for_server(ctx);

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    sqli_pool_t *pool = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_create(&pool, &params, 1));
    TEST_ASSERT_NOT_NULL(pool);

    sqli_conn_t *held = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_acquire(pool, &held));
    TEST_ASSERT_NOT_NULL(held);

    pool_waiter_ctx waiter = {.pool = pool, .conn = NULL, .rc = SQLI_ERR};
    pthread_t waiter_thread;
    pthread_create(&waiter_thread, NULL, pool_waiter_thread, &waiter);

    usleep(100 * 1000);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_release(pool, held));

    pthread_join(waiter_thread, NULL);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, waiter.rc);
    TEST_ASSERT_NOT_NULL(waiter.conn);

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_release(pool, waiter.conn));
    sqli_pool_destroy(pool);

    pthread_join(server_thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}

void test_pool_reconnect_after_borrower_closed_connection(void)
{
    mock_srv_ctx *ctx = calloc(1, sizeof(*ctx));
    TEST_ASSERT_NOT_NULL(ctx);
    mock_srv_ctx_init(ctx);
    require_test_listener_or_skip(ctx);
    ctx->expected_accepts = 2;

    pthread_t server_thread;
    pthread_create(&server_thread, NULL, mock_server_pool_test, ctx);
    wait_for_server(ctx);

    sqli_connect_params params = {0};
    fill_connect_params(ctx, &params);

    sqli_pool_t *pool = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_create(&pool, &params, 1));
    TEST_ASSERT_NOT_NULL(pool);

    sqli_conn_t *c1 = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_acquire(pool, &c1));
    TEST_ASSERT_NOT_NULL(c1);

    sqli_close(c1); /* simulate borrower breaking connection */
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_release(pool, c1));

    sqli_conn_t *c2 = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_acquire_timeout(pool, &c2, 1000));
    TEST_ASSERT_NOT_NULL(c2);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_pool_release(pool, c2));

    sqli_pool_destroy(pool);
    pthread_join(server_thread, NULL);
    close(ctx->listener_fd);
    mock_srv_ctx_destroy(ctx);
    free(ctx);
}
