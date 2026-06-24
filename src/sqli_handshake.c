#define _GNU_SOURCE
#include "sqli_internal.h"

#include "sqli_tcp.h"
#include "sqli_tls.h"
#include "sqli_unix.h"
#include "sqli_log.h"
#include "sqli_msg_catalog.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

/* ----------------------------------------------------------------
 * Environment variables sent to the server (spec §4.3, Bug #32)
 * INFORMIXSTACKSIZE and ONEDB_STACKSIZE removed (not in spec)
 * ---------------------------------------------------------------- */

const char *env_primary_list[ENV_PRIMARY_COUNT] = {
    "DB_LOCALE", "CLIENT_LOCALE", "DELIMIDENT", "DBDATE", "GL_DATE",
    "RASHELP", "STMT_CACHE", "IFX_LONGID", "DBPATH", "NODEFDAC",
    "CLNT_PAM_CAPABLE", "IFX_UPDDESC"
};

const char *env_secondary_list[ENV_SECONDARY_COUNT] = {
    "DBTIME", "TZ"
};

/* ----------------------------------------------------------------
 * Helper: copy string into pre-allocated buffer
 * ---------------------------------------------------------------- */

static sqli_status str_copy(char *buf, size_t buf_size, const char *src)
{
    if (src == NULL)
        return SQLI_OK;

    size_t len = strlen(src);
    if (len >= buf_size)
        len = buf_size - 1;

    memcpy(buf, src, len);
    buf[len] = '\0';
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * Local socket read helpers (needed for handshake-specific parsing)
 * ---------------------------------------------------------------- */

static sqli_status hs_read_exact(int fd, uint8_t *buf, size_t count)
{
    ssize_t total = 0;
    while ((size_t)total < count) {
        ssize_t n = sqli_tcp_read(fd, buf + (size_t)total, count - (size_t)total);
        if (n <= 0)
            return SQLI_IO_ERROR;
        total += n;
    }
    return SQLI_OK;
}

static sqli_status hs_read_be16(int fd, uint16_t *val)
{
    uint8_t buf[2];
    sqli_status rc = hs_read_exact(fd, buf, 2);
    if (rc != SQLI_OK) return rc;
    *val = (uint16_t)((buf[0] << 8) | buf[1]);
    return SQLI_OK;
}

static sqli_status hs_drain(int fd, size_t count)
{
    uint8_t tmp[256];
    while (count > 0) {
        size_t chunk = count > sizeof(tmp) ? sizeof(tmp) : count;
        sqli_status rc = hs_read_exact(fd, tmp, chunk);
        if (rc != SQLI_OK) return rc;
        count -= chunk;
    }
    return SQLI_OK;
}

static uint32_t parse_u32_env_local(const char *name, uint32_t dflt,
                                    uint32_t min_v, uint32_t max_v)
{
    const char *v = getenv(name);
    if (v == NULL || *v == '\0')
        return dflt;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (end == v || *end != '\0' || n < (unsigned long)min_v || n > (unsigned long)max_v)
        return dflt;
    return (uint32_t)n;
}

static void setup_locale_decoder(sqli_conn_t *c)
{
    if (c == NULL)
        return;
    c->decode_cp1252_utf8 = false;
    c->decode_locale_checked = true;
    if (c->decode_cd_ready && c->decode_cd != (iconv_t)-1) {
        iconv_close(c->decode_cd);
        c->decode_cd = (iconv_t)-1;
        c->decode_cd_ready = false;
    }

    if (c->client_locale == NULL || c->db_locale == NULL)
        return;

    bool client_utf8 = (strcasestr(c->client_locale, "utf8") != NULL) ||
                       (strcasestr(c->client_locale, "utf-8") != NULL);
    bool db_cp1252 = (strcasestr(c->db_locale, "cp1252") != NULL) ||
                     (strcasestr(c->db_locale, "1252") != NULL);
    if (!client_utf8 || !db_cp1252)
        return;
    c->decode_cp1252_utf8 = true;

    c->decode_cd = iconv_open("UTF-8", "WINDOWS-1252");
    if (c->decode_cd != (iconv_t)-1)
        c->decode_cd_ready = true;
}

/* ----------------------------------------------------------------
 * Parse CONACC body (Bug #8)
 *
 * Extracts cap_1 and checks svcError (tag 102).
 * ---------------------------------------------------------------- */

static inline uint16_t buf_r16(const uint8_t *b, size_t pos)
{
    return (uint16_t)((b[pos] << 8) | b[pos + 1]);
}

static inline uint32_t buf_r32(const uint8_t *b, size_t pos)
{
    return ((uint32_t)b[pos]   << 24) | ((uint32_t)b[pos+1] << 16) |
           ((uint32_t)b[pos+2] << 8)  |  (uint32_t)b[pos+3];
}

static sqli_status parse_conacc_body(sqli_conn_t *c,
                                     const uint8_t *body, uint16_t body_len)
{
    if (body_len < 4)
        return SQLI_PROTO_ERROR;

    size_t pos = 0;

    /* Skip outer tag 100 (2 bytes) + tag 101 (2 bytes) */
    if (pos + 4 > body_len) goto scan_for_102;
    pos += 4;

    /* Protocol version (4 bytes) */
    if (pos + 4 > body_len) goto scan_for_102;
    pos += 4;

    /* Float type: [2-byte length][N bytes] */
    if (pos + 2 > body_len) goto scan_for_102;
    {
        uint16_t ft_len = buf_r16(body, pos); pos += 2;
        if (pos + ft_len > body_len) goto scan_for_102;
        pos += ft_len;
    }

    /* Tag 108: product type marker (2 bytes) + fixed 12 bytes */
    if (pos + 2 + 12 > body_len) goto scan_for_102;
    pos += 2 + 12;

    /* Product version: [2-byte length][N bytes] */
    if (pos + 2 > body_len) goto scan_for_102;
    {
        uint16_t pv_len = buf_r16(body, pos); pos += 2;
        if (pos + pv_len > body_len) goto scan_for_102;
        pos += pv_len;
    }

    /* RDS: [2-byte length][N bytes] */
    if (pos + 2 > body_len) goto scan_for_102;
    {
        uint16_t rds_len = buf_r16(body, pos); pos += 2;
        if (pos + rds_len > body_len) goto scan_for_102;
        pos += rds_len;
    }

    /* App ID: [2-byte length][N bytes] */
    if (pos + 2 > body_len) goto scan_for_102;
    {
        uint16_t aid_len = buf_r16(body, pos); pos += 2;
        if (pos + aid_len > body_len) goto scan_for_102;
        pos += aid_len;
    }

    /* Cap_1 (4 bytes) */
    if (pos + 4 > body_len) goto scan_for_102;
    c->caps.cap_1 = (int32_t)buf_r32(body, pos);
    pos += 4;

    /* Skip Cap_2(4), Cap_3(4), flags(2) */
    pos += 10;

scan_for_102:
    /* Scan forward for tag 102 to check svcError */
    while (pos + 8 <= body_len) {
        uint16_t tag = buf_r16(body, pos);
        if (tag == 102) {
            /* svcError is at byte offset +6 from tag start */
            int16_t svc_error = (int16_t)buf_r16(body, pos + 6);
            if (svc_error != 0) {
                set_error_fmt(c, "server authentication error: %d", svc_error);
                return SQLI_AUTH_FAIL;
            }
            break;
        }
        if (tag == SQLI_TAG_END) /* 127 */
            break;
        pos++;
    }

    return SQLI_OK;
}

static sqli_status handle_conrej_body(sqli_conn_t *c, uint8_t resp_type, uint16_t resp_pdu)
{
    if (resp_pdu < SQLI_SL_HEADER_SIZE) {
        set_error_context(c, "connect/conrej", resp_type);
        set_error(c, "invalid CONREJ pdu size");
        return SQLI_PROTO_ERROR;
    }

    uint16_t conrej_body_len = (uint16_t)(resp_pdu - SQLI_SL_HEADER_SIZE);
    if (conrej_body_len == 0) {
        set_error_context(c, "connect/conrej", resp_type);
        set_error(c, "empty CONREJ body");
        return SQLI_PROTO_ERROR;
    }

    uint8_t *conrej_body = malloc(conrej_body_len);
    if (conrej_body == NULL)
        return SQLI_ALLOC_FAIL;

    sqli_status rc = hs_read_exact(c->socket_fd, conrej_body, conrej_body_len);
    if (rc != SQLI_OK) {
        free(conrej_body);
        set_error_context(c, "connect/conrej", resp_type);
        set_error(c, "failed to read CONREJ body");
        return rc;
    }

    rc = parse_conacc_body(c, conrej_body, conrej_body_len);
    free(conrej_body);
    return rc;
}


/* ----------------------------------------------------------------
 * SQ_PROTOCOLS exchange (Bug #7)
 *
 * Client sends 9 capability bytes; server responds with its own.
 * Parse server bytes[5,7,8] for PAM, Remove64KLimit,
 * large_tuple_mode, long_row_id.
 * ---------------------------------------------------------------- */

static sqli_status do_sq_protocols(sqli_conn_t *c)
{
    int fd = c->socket_fd;
    set_error_context(c, "connect/protocols", SQLI_SQ_PROTOCOLS);

    /* Client capability bytes (9 bytes) + 1 pad + chained SQ_EOT */
    static const uint8_t proto_req[] = {
        0x00, 0x7E,                                          /* opcode 126 */
        0x00, 0x09,                                          /* length = 9 */
        0xFF, 0xFC, 0x7F, 0xFC, 0x3C, 0x8C, 0xAA, 0x97, 0x06, 0x00, /* 9B + pad */
        0x00, 0x0C                                           /* chained SQ_EOT */
    };
    if (sqli_tcp_send(fd, proto_req, sizeof(proto_req)) != (ssize_t)sizeof(proto_req)) {
        set_error(c, "SQ_PROTOCOLS: failed to send");
        return SQLI_IO_ERROR;
    }

    /* Read server response */
    uint16_t proto_opcode, proto_len;
    sqli_status rc = hs_read_be16(fd, &proto_opcode);
    if (rc != SQLI_OK) return rc;
    rc = hs_read_be16(fd, &proto_len);
    if (rc != SQLI_OK) return rc;

    if (proto_opcode != SQLI_SQ_PROTOCOLS) {
        set_error_fmt(c, "SQ_PROTOCOLS: unexpected opcode %d", proto_opcode);
        return SQLI_PROTO_ERROR;
    }

    /* Read up to 16 capability bytes */
    uint8_t server_protocols[16] = {0};
    size_t to_read = proto_len > 16 ? 16 : proto_len;
    rc = hs_read_exact(fd, server_protocols, to_read);
    if (rc != SQLI_OK) return rc;

    /* Drain any extra bytes + padding */
    size_t extra = proto_len > to_read ? proto_len - to_read : 0;
    size_t pad   = proto_len % 2 != 0 ? 1 : 0;
    if (extra + pad > 0) {
        rc = hs_drain(fd, extra + pad);
        if (rc != SQLI_OK) return rc;
    }

    /* Parse capability bits */
    if (proto_len >= 6)
        c->caps.has_pam        = (server_protocols[5] & 0x08) != 0;
    if (proto_len >= 8) {
        c->caps.remove_64k_limit = (server_protocols[7] & 0x02) != 0;
        c->caps.extended_describe = (server_protocols[7] & 0x01) != 0;
    }
    if (proto_len >= 9) {
        c->caps.large_tuple_mode = (server_protocols[8] & 0x04) != 0;
        c->caps.long_row_id      = (server_protocols[8] & 0x02) != 0;
    }

    /* Server chains SQ_EOT after its SQ_PROTOCOLS response — drain it */
    {
        uint16_t trailing;
        rc = hs_read_be16(fd, &trailing);
        if (rc != SQLI_OK) return rc;
        if (trailing != SQLI_SQ_EOT) {
            set_error_fmt(c, "SQ_PROTOCOLS: expected trailing EOT, got %d", trailing);
            return SQLI_PROTO_ERROR;
        }
    }

    sqli_log(SQLI_LOG_INFO,
             "SQ_PROTOCOLS: pam=%d remove64k=%d extDescribe=%d large_tuple=%d long_rowid=%d",
             c->caps.has_pam, c->caps.remove_64k_limit,
             c->caps.extended_describe,
             c->caps.large_tuple_mode, c->caps.long_row_id);
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * PAM handshake (Bug #23)
 *
 * After SQ_PROTOCOLS, if caps.has_pam:
 *   send SQ_ACK+SQ_EOT, loop on SQ_CHALLENGE until SQ_ACCEPT or SQ_EXIT.
 *   Style 1/2 → respond with password.
 *   Style 3/4 → informational, respond with ACK+EOT.
 * ---------------------------------------------------------------- */

static sqli_status do_pam_handshake(sqli_conn_t *c)
{
    int fd = c->socket_fd;
    set_error_context(c, "connect/pam", SQLI_SQ_CHALLENGE);

    /* Initiate: SQ_ACK(128) + SQ_EOT(12) */
    uint8_t ack_eot[] = { 0x00, SQLI_SQ_ACK, 0x00, SQLI_SQ_EOT };
    if (sqli_tcp_send(fd, ack_eot, 4) != 4) {
        set_error(c, "PAM: failed to send ACK+EOT");
        return SQLI_IO_ERROR;
    }

    for (;;) {
        uint16_t opcode;
        sqli_status rc = hs_read_be16(fd, &opcode);
        if (rc != SQLI_OK) return rc;

        if (opcode == SQLI_SQ_ACCEPT) /* 127 */
            return SQLI_OK;

        if (opcode == SQLI_SQ_EXIT) { /* 56 */
            set_error(c, "PAM: server rejected authentication");
            return SQLI_AUTH_FAIL;
        }

        if (opcode != SQLI_SQ_CHALLENGE) { /* 129 */
            set_error_fmt(c, "PAM: unexpected opcode %d", opcode);
            return SQLI_PROTO_ERROR;
        }

        /* Read challenge: style(2), prompts(2), chal_len(2), data */
        uint8_t ch_hdr[6];
        rc = hs_read_exact(fd, ch_hdr, 6);
        if (rc != SQLI_OK) return rc;
        uint16_t style    = (uint16_t)((ch_hdr[0] << 8) | ch_hdr[1]);
        /* prompts at ch_hdr[2..3] — unused */
        uint16_t chal_len = (uint16_t)((ch_hdr[4] << 8) | ch_hdr[5]);

        /* Drain challenge data + even-padding */
        size_t to_drain = chal_len + (chal_len % 2 != 0 ? 1 : 0);
        if (to_drain > 0) {
            rc = hs_drain(fd, to_drain);
            if (rc != SQLI_OK) return rc;
        }

        if (style == 3 || style == 4) {
            /* Informational: send ACK+EOT */
            uint8_t resp[] = { 0x00, SQLI_SQ_ACK, 0x00, SQLI_SQ_EOT };
            sqli_tcp_send(fd, resp, 4);
        } else {
            /* Style 1/2: send password as SQ_RESPONSE(130) */
            const char *pass = (c->password != NULL) ? c->password : "";
            size_t pass_len = strlen(pass);
            if (pass_len > 512) pass_len = 512;
            size_t resp_total = 4 + pass_len + (pass_len % 2 != 0 ? 1 : 0);
            uint8_t *resp = malloc(resp_total);
            if (resp == NULL) return SQLI_ALLOC_FAIL;
            size_t rp = 0;
            resp[rp++] = 0; resp[rp++] = SQLI_SQ_RESPONSE; /* 130 */
            resp[rp++] = (uint8_t)(pass_len >> 8);
            resp[rp++] = (uint8_t)(pass_len & 0xFF);
            memcpy(resp + rp, pass, pass_len); rp += pass_len;
            if (pass_len % 2 != 0) resp[rp++] = 0;
            sqli_tcp_send(fd, resp, rp);
            free(resp);
        }
    }
}

/* ----------------------------------------------------------------
 * sqli_connect — full connection handshake
 * ---------------------------------------------------------------- */

sqli_status sqli_connect(sqli_conn_t *c, const sqli_connect_params *params)
{
    if (c == NULL || params == NULL)
        return SQLI_INVALID_STATE;

    if (c->state != SQLI_CONN_CLOSED) {
        set_error_context(c, "connect/precheck", 0);
        set_error(c, "connection already in use");
        return SQLI_INVALID_STATE;
    }

    if (params->hostname == NULL || params->service == NULL) {
        set_error_context(c, "connect/precheck", 0);
        set_error(c, "hostname and service are required");
        return SQLI_INVALID_STATE;
    }

    sqli_status rc = SQLI_OK;
    ssize_t sent;
    rc = str_copy(c->hostname, 256, params->hostname);
    if (rc != SQLI_OK) goto out;
    rc = str_copy(c->service, 64, params->service);
    if (rc != SQLI_OK) goto out;
    rc = str_copy(c->server, 256, params->server);
    if (rc != SQLI_OK) goto out;
    rc = str_copy(c->database, 256, params->database);
    if (rc != SQLI_OK) goto out;
    rc = str_copy(c->username, 256, params->username);
    if (rc != SQLI_OK) goto out;
    rc = str_copy(c->password, 256, params->password);
    if (rc != SQLI_OK) goto out;
    rc = str_copy(c->client_locale, 256, params->client_locale);
    if (rc != SQLI_OK) goto out;
    rc = str_copy(c->db_locale, 256, params->db_locale);
    if (rc != SQLI_OK) goto out;
c->fetch_buf_size = parse_u32_env_local("SQLI_FETCH_BUFSIZE", 4194304u, 1024u, 16u * 1024u * 1024u);
    setup_locale_decoder(c);
    c->read_buf_pos = 0;
    c->read_buf_len = 0;

    /* Detect Unix socket: if service starts with '/', treat as socket path */
    bool use_unix_socket = (c->service[0] == '/');

    if (use_unix_socket) {
        /* Override username to current Unix user, clear password */
        uid_t uid = getuid();
        struct passwd *pw = getpwuid(uid);
        if (pw != NULL) {
            str_copy(c->username, 256, pw->pw_name);
        }
        c->password[0] = '\0';
    }

    /* Step 1: Open socket (TCP or Unix domain) */
    c->state = SQLI_CONN_CONNECTING;
    set_error_context(c, "connect/tcp", 0);
    if (use_unix_socket) {
        c->socket_fd = sqli_unix_connect(c->service);
    } else {
        c->socket_fd = sqli_tcp_connect(c->hostname, c->service);
    }
    if (c->socket_fd < 0) {
        set_error(c, "TCP connection failed");
        rc = SQLI_IO_ERROR;
        goto out;
    }

    rc = sqli_tls_connect(c, params);
    if (rc != SQLI_OK)
        goto out;

    sqli_log(SQLI_LOG_INFO, "connected, starting SQLI handshake");

    const char *transport = use_unix_socket ? "ipcstr" : "tlitcp";

    if (use_unix_socket) {
        /* =========================================================
         * Unix socket (onipcstr) handshake
         *
         * Protocol differs from TCP:
         *   1. Server sends 12-byte greeting (before client sends anything)
         *   2. Client sends "sqAZ" preamble (text + base64 binary, no SL framing)
         *   3. Server sends len(2) + raw ASC-BINARY body (no SL header)
         * ========================================================= */
        c->state = SQLI_CONN_HANDSHAKE;
        set_error_context(c, "connect/ipc_greeting", 0);

        /* Step 2a: Read server greeting (12 bytes) */
        {
            uint8_t greeting[12];
            rc = hs_read_exact(c->socket_fd, greeting, 12);
            if (rc != SQLI_OK) {
                set_error(c, "failed to read server greeting");
                goto out;
            }
            sqli_log(SQLI_LOG_DEBUG,
                     "IPC greeting: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     greeting[0], greeting[1], greeting[2], greeting[3],
                     greeting[4], greeting[5], greeting[6], greeting[7],
                     greeting[8], greeting[9], greeting[10], greeting[11]);
        }

        /* Step 2b: Send "sqAZ" preamble */
        set_error_context(c, "connect/ipc_preamble", 0);
        {
            uint8_t preamble[1024];
            size_t pre_len = sqli_asc_encode_ipc_preamble(c, preamble, sizeof(preamble), transport);
            if (pre_len == 0) {
                set_error(c, "failed to encode IPC preamble");
                rc = SQLI_PROTO_ERROR;
                goto out;
            }
            sqli_log(SQLI_LOG_DEBUG, "IPC preamble: %zu bytes", pre_len);
            {
                char hex_buf[2048];
                size_t hi = 0;
                for (size_t _i = 0; _i < pre_len && hi + 3 < sizeof(hex_buf); _i++)
                    hi += (size_t)snprintf(hex_buf + hi, sizeof(hex_buf) - hi - 1, "%02x", preamble[_i]);
                sqli_log(SQLI_LOG_DEBUG, "IPC preamble hex: %s", hex_buf);
            }

            sent = sqli_tcp_send(c->socket_fd, preamble, pre_len);
            if (sent < 0 || (size_t)sent != pre_len) {
                set_error(c, "failed to send IPC preamble");
                rc = SQLI_IO_ERROR;
                goto out;
            }
        }

        /* Step 2c: Read CONACC response (len(2) + body, no SL header).
         * Informix sends the 2-byte length as the total frame size,
         * including the length field itself. */
        set_error_context(c, "connect/ipc_conacc", 0);
        {
            uint16_t frame_len;
            uint16_t body_len;
            rc = hs_read_be16(c->socket_fd, &frame_len);
            if (rc != SQLI_OK) {
                set_error(c, "failed to read CONACC length");
                goto out;
            }
            if (frame_len < 2) {
                set_error_fmt(c, "invalid CONACC frame length: %u", frame_len);
                rc = SQLI_PROTO_ERROR;
                goto out;
            }
            body_len = (uint16_t)(frame_len - 2);
            sqli_log(SQLI_LOG_DEBUG, "IPC CONACC frame_len=%u body_len=%u",
                     frame_len, body_len);

            if (body_len == 0 || body_len > 4096) {
                set_error_fmt(c, "invalid CONACC body length: %u", body_len);
                rc = SQLI_PROTO_ERROR;
                goto out;
            }

            uint8_t *resp_body = malloc(body_len);
            if (resp_body == NULL) {
                rc = SQLI_ALLOC_FAIL;
                goto out;
            }

            rc = hs_read_exact(c->socket_fd, resp_body, body_len);
            if (rc != SQLI_OK) {
                free(resp_body);
                set_error(c, "failed to read CONACC body");
                goto out;
            }

            rc = parse_conacc_body(c, resp_body, body_len);
            free(resp_body);
            if (rc != SQLI_OK)
                goto out;
        }

        sqli_log(SQLI_LOG_INFO, "IPC CONACC parsed, cap_1=%d", c->caps.cap_1);
    } else {
        /* =========================================================
         * TCP handshake (onsoctcp/onsocssl)
         *
         *   1. Client sends SL-framed CONREQ
         *   2. Server sends SL-framed CONACC (SL header + ASC-BINARY body)
         * ========================================================= */
        c->state = SQLI_CONN_HANDSHAKE;
        set_error_context(c, "connect/conreq", SQLI_SLTYPE_CONREQ);

        uint8_t conreq_buf[SQLI_SL_HEADER_SIZE + 2048];
        size_t body_len = sqli_asc_encode_conreq(c,
                                                 conreq_buf + SQLI_SL_HEADER_SIZE,
                                                 sizeof(conreq_buf) - SQLI_SL_HEADER_SIZE,
                                                 transport);
        if (body_len == 0) {
            set_error(c, "failed to encode connection request");
            rc = SQLI_PROTO_ERROR;
            goto out;
        }

        uint16_t pdu_size = (uint16_t)(body_len + SQLI_SL_HEADER_SIZE);
        sqli_log(SQLI_LOG_DEBUG, "CONREQ body_len=%zu pdu_size=%u", body_len, pdu_size);
        size_t hdr_len = sqli_wire_encode_sl_header(conreq_buf, sizeof(conreq_buf),
                                                    pdu_size, SQLI_SLTYPE_CONREQ,
                                                    SQLI_SL_PROT_SQLI, 0);
        if (hdr_len == 0) {
            set_error(c, "failed to encode SL header");
            rc = SQLI_PROTO_ERROR;
            goto out;
        }
        sqli_log(SQLI_LOG_DEBUG, "SL header[0:6]=%02x %02x %02x %02x %02x %02x",
                 conreq_buf[0], conreq_buf[1], conreq_buf[2], conreq_buf[3], conreq_buf[4], conreq_buf[5]);
        sent = sqli_tcp_send(c->socket_fd, conreq_buf, (size_t)pdu_size);
        sqli_log(SQLI_LOG_DEBUG, "sent=%zd pdu_size=%u", sent, pdu_size);
        if (sent < 0 || (size_t)sent != pdu_size) {
            set_error(c, "failed to send connection request");
            rc = SQLI_IO_ERROR;
            goto out;
        }

        /* Step 3: Read SL header from server */
        uint8_t sl_buf[SQLI_SL_HEADER_SIZE];
        ssize_t n = sqli_tcp_read(c->socket_fd, sl_buf, SQLI_SL_HEADER_SIZE);
        if (n < 0 || (size_t)n != SQLI_SL_HEADER_SIZE) {
            set_error_context(c, "connect/sl_header_read", 0);
            set_error(c, "failed to read server response header");
            rc = SQLI_IO_ERROR;
            goto out;
        }

        uint16_t resp_pdu;
        uint8_t resp_type, resp_attr;
        uint16_t resp_opts;
        rc = sqli_wire_decode_sl_header(sl_buf, SQLI_SL_HEADER_SIZE,
                                         &resp_pdu, &resp_type, &resp_attr, &resp_opts);
        if (rc != SQLI_OK)
            goto out;

        sqli_log(SQLI_LOG_DEBUG, "Server SL response: pdu=%u type=%u attr=%u opts=%u",
                 resp_pdu, resp_type, resp_attr, resp_opts);
        if (resp_type == SQLI_SLTYPE_REDIRECT) {
            set_error_context(c, "connect/sl_header", resp_type);
            set_error(c, "server returned redirect");
            rc = SQLI_ERR;
            goto out;
        }
        if (resp_type == SQLI_SLTYPE_CONREJ) {
            rc = handle_conrej_body(c, resp_type, resp_pdu);
            if (rc != SQLI_OK)
                goto out;
            sqli_log(SQLI_LOG_WARN, "server returned CONREJ with svcError=0, continuing");
            resp_type = SQLI_SLTYPE_CONACC;
        }
        if (resp_type != SQLI_SLTYPE_CONACC) {
            set_error_context(c, "connect/sl_header", resp_type);
            set_error_fmt(c, "unexpected SL type: %d", resp_type);
            rc = SQLI_PROTO_ERROR;
            goto out;
        }

        /* Step 4: Read and parse ASC-BINARY body (Bug #8) */
        uint16_t body_len_field = (uint16_t)(resp_pdu - SQLI_SL_HEADER_SIZE);
        if (body_len_field == 0) {
            set_error_context(c, "connect/conacc", resp_type);
            set_error(c, "empty ASC-BINARY body from server");
            rc = SQLI_PROTO_ERROR;
            goto out;
        }

        uint8_t *resp_body = malloc(body_len_field);
        if (resp_body == NULL) {
            rc = SQLI_ALLOC_FAIL;
            goto out;
        }

        n = sqli_tcp_read(c->socket_fd, resp_body, body_len_field);
        if (n < 0 || (size_t)n != body_len_field) {
            free(resp_body);
            set_error_context(c, "connect/conacc", resp_type);
            set_error(c, "failed to read server response body");
            rc = SQLI_IO_ERROR;
            goto out;
        }

        rc = parse_conacc_body(c, resp_body, body_len_field);
        free(resp_body);
        if (rc != SQLI_OK)
            goto out;

        sqli_log(SQLI_LOG_INFO, "CONACC parsed, cap_1=%d", c->caps.cap_1);
    }

    /* Step 5: SQ_PROTOCOLS capability exchange
     * Client sends: opcode(2) + len(2) + 9 capability bytes + 1 pad + SQ_EOT(2)
     * Server responds with the same format.
     * After reading server protocols, drain the chained SQ_EOT.
     */
    rc = do_sq_protocols(c);
    if (rc != SQLI_OK)
        goto out;

    /* Step 6: Send SQ_INFO (env vars) + SQ_EOT, receive SQ_EOT
     *
     * Format: opcode(2)=81, item_type(2)=6, total_len(2),
     *         max_name_len(2), max_val_len(2),
     *         [name_len(2)+name+pad  val_len(2)+val+pad] x N,
     *         end_name(2)=0, end_val(2)=0,
     *         SQ_EOT(2)=12
     *
     * We send a minimal env var set; the server ACKs with SQ_EOT.
     */
    c->state = SQLI_CONN_INFO;
    set_error_context(c, "connect/info", SQLI_SQ_INFO);
    {
        /* Build env var entries — match Client exactly: DBTEMP + SUBQCACHE SZ */
        typedef struct { const char *name; const char *value; } env_entry;
        env_entry envs[] = {
            { "DBTEMP",        "/tmp"       },
            { "SUBQCACHE SZ",  "10"         },
        };
        int nenvs = (int)(sizeof(envs) / sizeof(envs[0]));

        /* Compute totLen and max lengths */
        uint16_t max_name = 0, max_val = 0, totLen = 4; /* 4 = max_name(2)+max_val(2) */
        for (int i = 0; i < nenvs; i++) {
            uint16_t nl = (uint16_t)strlen(envs[i].name);
            uint16_t vl = (uint16_t)strlen(envs[i].value);
            uint16_t nlp = (uint16_t)((nl + 1) & ~1); /* padded to even */
            uint16_t vlp = (uint16_t)((vl + 1) & ~1);
            totLen = (uint16_t)(totLen + 2 + nlp + 2 + vlp);
            if (nlp > max_name) max_name = nlp;
            if (vlp > max_val)  max_val  = vlp;
        }
        totLen = (uint16_t)(totLen + 4); /* end_name(2) + end_val(2) */

        /* Write message into buffer: 14 header bytes + env data + 2 EOT */
        uint8_t info_buf[1024];
        size_t ip = 0;
        info_buf[ip++] = 0; info_buf[ip++] = SQLI_SQ_INFO;
        info_buf[ip++] = 0; info_buf[ip++] = 6;           /* item type */
        info_buf[ip++] = (uint8_t)(totLen >> 8); info_buf[ip++] = (uint8_t)(totLen & 0xFF);
        info_buf[ip++] = (uint8_t)(max_name >> 8); info_buf[ip++] = (uint8_t)(max_name & 0xFF);
        info_buf[ip++] = (uint8_t)(max_val >> 8);  info_buf[ip++] = (uint8_t)(max_val & 0xFF);

        for (int i = 0; i < nenvs; i++) {
            uint16_t nl = (uint16_t)strlen(envs[i].name);
            uint16_t vl = (uint16_t)strlen(envs[i].value);
            info_buf[ip++] = (uint8_t)(nl >> 8); info_buf[ip++] = (uint8_t)(nl & 0xFF);
            memcpy(info_buf + ip, envs[i].name, nl); ip += nl;
            if (nl & 1) info_buf[ip++] = 0; /* pad */
            info_buf[ip++] = (uint8_t)(vl >> 8); info_buf[ip++] = (uint8_t)(vl & 0xFF);
            memcpy(info_buf + ip, envs[i].value, vl); ip += vl;
            if (vl & 1) info_buf[ip++] = 0; /* pad */
        }
        /* end marker + chained SQ_EOT */
        info_buf[ip++] = 0; info_buf[ip++] = 0; /* end_name */
        info_buf[ip++] = 0; info_buf[ip++] = 0; /* end_val */
        info_buf[ip++] = 0; info_buf[ip++] = SQLI_SQ_EOT;

        sent = sqli_tcp_send(c->socket_fd, info_buf, ip);
        if (sent < 0 || (size_t)sent != ip) {
            set_error(c, "failed to send SQ_INFO");
            rc = SQLI_IO_ERROR;
            goto out;
        }

        /* Server responds with SQ_EOT */
        uint16_t info_resp;
        rc = hs_read_be16(c->socket_fd, &info_resp);
        if (rc != SQLI_OK) { set_error(c, "failed to read SQ_INFO response"); goto out; }
        if (info_resp != SQLI_SQ_EOT) {
            set_error_fmt(c, "SQ_INFO: expected EOT (%d), got %d", SQLI_SQ_EOT, info_resp);
            rc = SQLI_PROTO_ERROR;
            goto out;
        }

        /* Check for additional server data after SQ_EOT (non-blocking peek) */
        {
            uint8_t peek_byte = 0;
            ssize_t peek_rc = sqli_tcp_peek(c->socket_fd, &peek_byte, 1);
            sqli_log(SQLI_LOG_DEBUG, "post-SQ_INFO peek: rc=%zd", peek_rc);
            if (peek_rc > 0) {
                sqli_log(SQLI_LOG_DEBUG, "extra data after SQ_INFO, draining...");
                uint8_t extra_buf[256];
                ssize_t extra_len = sqli_tcp_read(c->socket_fd, extra_buf, sizeof(extra_buf));
                sqli_log(SQLI_LOG_DEBUG, "drained %zd extra bytes after SQ_INFO", extra_len);
                for (int ei = 0; ei < extra_len && ei < 32; ei++) {
                    sqli_log(SQLI_LOG_DEBUG, "  extra[%d]=0x%02x", ei, extra_buf[ei]);
                }
            }
        }
    }
    sqli_log(SQLI_LOG_INFO, "SQ_INFO exchange complete");

    /* Step 7: PAM handshake if required (Bug #23) */
    if (c->caps.has_pam) {
        c->state = SQLI_CONN_AUTH;
        sqli_log(SQLI_LOG_INFO, "starting PAM handshake");
        rc = do_pam_handshake(c);
        if (rc != SQLI_OK)
            goto out;
        sqli_log(SQLI_LOG_INFO, "PAM handshake complete");
    }

    /* Step 9: Send SQ_DBOPEN (Bug #9) */
    c->state = SQLI_CONN_DBOPEN;
    set_error_context(c, "connect/dbopen", SQLI_SQ_DBOPEN);
    sqli_log(SQLI_LOG_INFO, "opening database: %s",
             params->database ? params->database : "(default)");

    const char *db_name = params->database;
    if (db_name == NULL || db_name[0] == '\0')
        db_name = "informix";

    /* Client wire format: opcode(2) + nameLen(2) + dbName(N) + pad(0/1) + flags(2) + SQ_EOT(2)
     * Database name is padded to even boundary — required by server for odd-length names. */
    size_t db_name_len = strlen(db_name);
    uint8_t dbopen_buf[512];
    size_t dbopen_pos = 0;

    /* Opcode = SQ_DBOPEN (36) */
    dbopen_buf[dbopen_pos++] = 0;
    dbopen_buf[dbopen_pos++] = SQLI_SQ_DBOPEN;

    /* Database name length (2-byte BE) */
    dbopen_buf[dbopen_pos++] = (uint8_t)(db_name_len >> 8);
    dbopen_buf[dbopen_pos++] = (uint8_t)(db_name_len & 0xFF);

    /* Database name bytes */
    memcpy(dbopen_buf + dbopen_pos, db_name, db_name_len);
    dbopen_pos += db_name_len;

    /* Pad to even boundary (matches Client driver) */
    if (db_name_len & 1) {
        dbopen_buf[dbopen_pos++] = 0;
    }

    /* Flags = 0 */
    dbopen_buf[dbopen_pos++] = 0;
    dbopen_buf[dbopen_pos++] = 0;

    /* Chained SQ_EOT */
    dbopen_buf[dbopen_pos++] = 0;
    dbopen_buf[dbopen_pos++] = SQLI_SQ_EOT;

    sent = sqli_tcp_send(c->socket_fd, dbopen_buf, dbopen_pos);
    if (sent < 0 || (size_t)sent != dbopen_pos) {
        set_error(c, "failed to send SQ_DBOPEN");
        rc = SQLI_IO_ERROR;
        goto out;
    }

    /* Step 10: Read DBOPEN response via dispatch loop.
     *
     * The first dispatch processes messages until DONE (eof=1).
     * The server may send additional messages (SET commands, etc.)
     * in the same or subsequent packets. Use a non-blocking peek to check if
     * more data is available before calling the second dispatch.
     */
    {
        sqli_result_t dbopen_result;
        memset(&dbopen_result, 0, sizeof(dbopen_result));
        rc = sqli_receive_dispatch(c->socket_fd, &dbopen_result, c);
        sqli_result_cleanup(&dbopen_result);
        if (rc != SQLI_OK) {
            if (!c->error_info.has_error)
                set_error(c, "SQ_DBOPEN response error");
            goto out;
        }

        /* Check if more data is available from the server (non-blocking peek) */
        uint8_t peek_byte = 0;
        ssize_t peek_rc = sqli_tcp_peek(c->socket_fd, &peek_byte, 1);
        sqli_log(SQLI_LOG_DEBUG, "post-DBOPEN peek: rc=%zd", peek_rc);
        if (peek_rc > 0) {
            /* More data available — process remaining messages */
            sqli_result_t dbopen_result2;
            memset(&dbopen_result2, 0, sizeof(dbopen_result2));
            dbopen_result2.eof = 0;
            sqli_status rc2 = sqli_receive_dispatch(c->socket_fd, &dbopen_result2, c);
            sqli_result_cleanup(&dbopen_result2);
            if (rc2 != SQLI_OK) {
                if (!c->error_info.has_error)
                    set_error(c, "SQ_DBOPEN response error");
                rc = rc2;
                goto out;
            }
        }
    }

    /* Connection is ready */
    c->state = SQLI_CONN_READY;
    c->database_open = 1;
    sqli_log(SQLI_LOG_INFO, "connection ready, database: %s", db_name);
    rc = SQLI_OK;

out:
    if (rc != SQLI_OK && c->socket_fd >= 0) {
        sqli_tcp_close(c->socket_fd);
        c->socket_fd = -1;
    }
    if (rc != SQLI_OK)
        c->state = SQLI_CONN_ERROR;

    return rc;
}

/* ----------------------------------------------------------------
 * sqli_close — graceful disconnect (Bug #25)
 *
 * Send SQ_EXIT then drain SQ_XACTSTAT(99) messages until
 * SQ_EXIT(56) or SQ_EOT(12) is received.
 * ---------------------------------------------------------------- */

void sqli_close(sqli_conn_t *conn)
{
    if (conn == NULL)
        return;

    if (conn->socket_fd >= 0) {
        if (conn->state == SQLI_CONN_READY) {
            if (conn->database_open) {
                uint8_t dbclose_msg[2] = {0, SQLI_SQ_DBCLOSE};
                (void)sqli_tcp_send(conn->socket_fd, dbclose_msg, sizeof(dbclose_msg));
            }
            uint8_t exit_msg[2] = {0, SQLI_SQ_EXIT};
            sqli_tcp_send(conn->socket_fd, exit_msg, 2);

            /* Drain until SQ_EXIT(56) or SQ_EOT(12) */
            for (;;) {
                uint16_t op;
                if (hs_read_be16(conn->socket_fd, &op) != SQLI_OK)
                    break;
                if (op == SQLI_SQ_XACTSTAT) { /* 99: skip 6 bytes */
                    hs_drain(conn->socket_fd, 6);
                    continue;
                }
                /* SQ_EXIT=56 or SQ_EOT=12 → done */
                break;
            }
        }
        sqli_tcp_close(conn->socket_fd);
        conn->socket_fd = -1;
    }

    conn->state = SQLI_CONN_CLOSED;
    conn->database_open = 0;
    if (conn->decode_cd_ready && conn->decode_cd != (iconv_t)-1) {
        iconv_close(conn->decode_cd);
        conn->decode_cd = (iconv_t)-1;
        conn->decode_cd_ready = false;
    }
    clear_error(conn);

    sqli_log(SQLI_LOG_DEBUG, "connection closed");
}
