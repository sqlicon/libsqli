#define _POSIX_C_SOURCE 200809L
/*
 * Phase 2 unit tests for ASC-BINARY encoding helpers.
 *
 * Tests use only public API — no internal struct access.
 */

#include "unity.h"
#include "libsqli/sqli.h"
#include "sqli_internal.h"

#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Helper: encode a CONREQ body using the internal API via sqli_conn_t
 * We use the connection object's write buffer for this test.
 * ---------------------------------------------------------------- */

/*
 * We test the CONREQ encoding indirectly through the connection object.
 * The encode function writes into a stack buffer, so we need internal
 * access. We expose a minimal test function via the internal header.
 */

/* Forward declaration — only needed for test compilation */
size_t sqli_asc_encode_conreq(struct sqli_conn *c, uint8_t *buf, size_t buf_size,
                              const char *transport);
size_t sqli_wire_encode_sl_header(uint8_t *buf, size_t buf_size,
                                   uint16_t pdu_size, uint8_t sl_type,
                                   uint8_t sl_attr, uint16_t sl_opts);
size_t sqli_asc_encode_string(uint8_t *buf, size_t buf_size, const char *s);
size_t sqli_asc_encode_string_padded(uint8_t *buf, size_t buf_size, const char *s);
size_t sqli_asc_encode_ipc_preamble(struct sqli_conn *c, uint8_t *buf, size_t buf_size,
                                    const char *transport);

/* ----------------------------------------------------------------
 * Test: ASC-BINARY string encode
 * ---------------------------------------------------------------- */

void test_asc_string_encode_hello(void)
{
    uint8_t buf[64];
    size_t n = sqli_asc_encode_string(buf, sizeof(buf), "hello");
    TEST_ASSERT_EQUAL_UINT(7, n);  /* 2 length + 5 chars */
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(5, buf[1]);
    TEST_ASSERT_EQUAL_UINT8('h', buf[2]);
    TEST_ASSERT_EQUAL_UINT8('e', buf[3]);
    TEST_ASSERT_EQUAL_UINT8('l', buf[4]);
    TEST_ASSERT_EQUAL_UINT8('l', buf[5]);
    TEST_ASSERT_EQUAL_UINT8('o', buf[6]);
}

void test_asc_string_encode_empty(void)
{
    uint8_t buf[64];
    size_t n = sqli_asc_encode_string(buf, sizeof(buf), "");
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[1]);
}

void test_asc_string_encode_null(void)
{
    uint8_t buf[64];
    TEST_ASSERT_EQUAL_UINT(0, sqli_asc_encode_string(buf, sizeof(buf), NULL));
}

void test_asc_string_encode_overflow(void)
{
    uint8_t buf[3];
    size_t n = sqli_asc_encode_string(buf, sizeof(buf), "hello");
    TEST_ASSERT_EQUAL_UINT(0, n);
}

/* ----------------------------------------------------------------
 * Test: ASC-BINARY padded string encode
 * ---------------------------------------------------------------- */

void test_asc_encode_padded_odd(void)
{
    uint8_t buf[64];
    /* "hi" = 2 bytes → total 4 bytes (already even) */
    size_t n = sqli_asc_encode_string_padded(buf, sizeof(buf), "hi");
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(2, buf[1]);
    TEST_ASSERT_EQUAL_UINT8('h', buf[2]);
    TEST_ASSERT_EQUAL_UINT8('i', buf[3]);
}

void test_asc_encode_padded_even_with_pad(void)
{
    uint8_t buf[64];
    /* "hi!" = 3 bytes → total 6 bytes (2 header + 3 chars + 1 pad) */
    size_t n = sqli_asc_encode_string_padded(buf, sizeof(buf), "hi!");
    TEST_ASSERT_EQUAL_UINT(6, n);
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(3, buf[1]);
    TEST_ASSERT_EQUAL_UINT8('h', buf[2]);
    TEST_ASSERT_EQUAL_UINT8('i', buf[3]);
    TEST_ASSERT_EQUAL_UINT8('!', buf[4]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[5]);
}

void test_asc_encode_padded_null(void)
{
    uint8_t buf[64];
    TEST_ASSERT_EQUAL_UINT(0, sqli_asc_encode_string_padded(buf, sizeof(buf), NULL));
}

void test_ipc_preamble_base64_is_padded(void)
{
    sqli_conn_t *conn = NULL;
    uint8_t buf[1024];
    char *sep;
    size_t n;
    size_t payload_len;

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_NOT_NULL(conn);

    TEST_ASSERT_NOT_NULL(conn->username);
    TEST_ASSERT_NOT_NULL(conn->server);
    TEST_ASSERT_NOT_NULL(conn->client_locale);
    TEST_ASSERT_NOT_NULL(conn->db_locale);
    strcpy(conn->username, "informix");
    strcpy(conn->server, "ol_informixs");
    strcpy(conn->client_locale, "en_US.utf8");
    strcpy(conn->db_locale, "en_US.utf8");

    n = sqli_asc_encode_ipc_preamble(conn, buf, sizeof(buf), "ipcstr");
    TEST_ASSERT_GREATER_THAN_UINT(0, n);
    TEST_ASSERT_EQUAL_UINT8(0, buf[n - 1]);

    sep = strrchr((char *)buf, ':');
    TEST_ASSERT_NOT_NULL(sep);
    payload_len = (size_t)(&buf[n - 1] - (uint8_t *)(sep + 1));
    TEST_ASSERT_GREATER_THAN_UINT(0, payload_len);
    TEST_ASSERT_NOT_EQUAL(1, payload_len % 4);
    TEST_ASSERT_NOT_EQUAL('=', sep[1 + payload_len - 1]);

    sqli_destroy(conn);
}

static int b64_char_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void verify_preamble_header(const uint8_t *buf, size_t n)
{
    /* Header must start with "sq" */
    TEST_ASSERT_EQUAL_UINT8('s', buf[0]);
    TEST_ASSERT_EQUAL_UINT8('q', buf[1]);

    /* Decode the magic characters [2..9] */
    int val[8];
    for (int i = 0; i < 8; i++) {
        val[i] = b64_char_val((char)buf[2 + i]);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, val[i]);
    }

    uint16_t rest_len = (uint16_t)((((val[0] << 2) | (val[1] >> 4)) << 8) | (((val[1] & 0x0F) << 4) | (val[2] >> 2)));
    uint16_t capabilities = (uint16_t)(((((val[2] & 0x03) << 6) | val[3]) << 8) | ((val[4] << 2) | (val[5] >> 4)));
    uint16_t zero_field = (uint16_t)(((((val[5] & 0x0F) << 4) | (val[6] >> 2)) << 8) | (((val[6] & 0x03) << 6) | val[7]));

    /* rest_len must be exactly n - 4 */
    TEST_ASSERT_EQUAL_UINT16(n - 4, rest_len);

    /* capabilities must be 0x013D */
    TEST_ASSERT_EQUAL_UINT16(0x013D, capabilities);

    /* zero_field must be 0 */
    TEST_ASSERT_EQUAL_UINT16(0, zero_field);
}

void test_ipc_preamble_dynamic_magic(void)
{
    sqli_conn_t *conn = NULL;
    uint8_t buf[1024];
    size_t n;

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_NOT_NULL(conn);

    strcpy(conn->username, "informix");
    strcpy(conn->server, "ol_informixs");
    strcpy(conn->client_locale, "en_US.utf8");
    strcpy(conn->db_locale, "en_US.utf8");

    n = sqli_asc_encode_ipc_preamble(conn, buf, sizeof(buf), "ipcstr");
    TEST_ASSERT_GREATER_THAN_UINT(10, n);
    verify_preamble_header(buf, n);

    /* Test legacy style for non-informix user */
    strcpy(conn->username, "admin");
    n = sqli_asc_encode_ipc_preamble(conn, buf, sizeof(buf), "ipcstr");
    TEST_ASSERT_GREATER_THAN_UINT(10, n);
    verify_preamble_header(buf, n);

    sqli_destroy(conn);
}

static int b64_ipc_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '.') return 62;
    if (c == '_') return 63;
    return -1;
}

static size_t decode_ipc_b64(const char *in, uint8_t *out, size_t out_cap)
{
    size_t in_len = strlen(in);
    size_t out_len = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (size_t i = 0; i < in_len; i++) {
        int v = b64_ipc_val(in[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len < out_cap) {
                out[out_len++] = (uint8_t)(acc >> bits);
            }
        }
    }
    return out_len;
}

static void check_tag74_structure_for_path(const char *app_path)
{
    sqli_conn_t *conn = NULL;
    uint8_t buf[2048];
    uint8_t body[1024];

    if (app_path != NULL) {
        setenv("SQLI_IPC_APP_PATH", app_path, 1);
    } else {
        unsetenv("SQLI_IPC_APP_PATH");
    }

    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    strcpy(conn->username, "informix");
    strcpy(conn->server, "ol_test");

    size_t n = sqli_asc_encode_ipc_preamble(conn, buf, sizeof(buf), "ipcstr");
    TEST_ASSERT_GREATER_THAN_UINT(0, n);

    char *sep = strrchr((char *)buf, ':');
    TEST_ASSERT_NOT_NULL(sep);

    size_t body_len = decode_ipc_b64(sep + 1, body, sizeof(body));
    TEST_ASSERT_GREATER_THAN_UINT(30, body_len);

    /* Find tag 0x0074 */
    size_t tag74_pos = 0;
    bool found_tag74 = false;
    for (size_t i = 0; i + 4 < body_len; i++) {
        if (body[i] == 0x00 && body[i + 1] == 0x74) {
            tag74_pos = i;
            found_tag74 = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_tag74);

    uint16_t tag74_len = (uint16_t)((body[tag74_pos + 2] << 8) | body[tag74_pos + 3]);
    uint16_t path_len = (uint16_t)((body[tag74_pos + 12] << 8) | body[tag74_pos + 13]);

    /* Expected: tag74_len is 4 + 4 + 2 + path_len */
    TEST_ASSERT_EQUAL_UINT16(10 + path_len, tag74_len);

    if (app_path != NULL) {
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(strlen(app_path) + 1), path_len);
        TEST_ASSERT_EQUAL_STRING(app_path, (char *)&body[tag74_pos + 14]);
    }

    /* Tag immediately after tag 0x0074 container MUST be 0x007F (END marker) */
    size_t next_tag_pos = tag74_pos + 4 + tag74_len;
    TEST_ASSERT_LESS_OR_EQUAL_UINT(body_len - 2, next_tag_pos);
    TEST_ASSERT_EQUAL_UINT8(0x00, body[next_tag_pos]);
    TEST_ASSERT_EQUAL_UINT8(0x7F, body[next_tag_pos + 1]);

    unsetenv("SQLI_IPC_APP_PATH");
    sqli_destroy(conn);
}

void test_ipc_preamble_tag74_dynamic_length(void)
{
    /* Test with short path */
    check_tag74_structure_for_path("sqlicon");

    /* Test with long custom path */
    check_tag74_structure_for_path("/opt/informix/clients/custom/bin/sqlicon_long_path_test");

    /* Test with default (proc self exe or fallback) */
    check_tag74_structure_for_path(NULL);
}

/* ----------------------------------------------------------------
 * Test: SL header encode (public-facing wrapper)
 * ---------------------------------------------------------------- */

void test_sl_encode_conreq(void)
{
    uint8_t buf[16];
    size_t n = sqli_wire_encode_sl_header(buf, sizeof(buf),
                                           100, 1, 60, 0);
    TEST_ASSERT_EQUAL_UINT(6, n);
    TEST_ASSERT_EQUAL_UINT8(0, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(100, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(1, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(60, buf[3]);
}

void test_sl_encode_null_buf(void)
{
    size_t n = sqli_wire_encode_sl_header(NULL, 16, 100, 1, 60, 0);
    TEST_ASSERT_EQUAL_UINT(0, n);
}

void test_sl_encode_too_small(void)
{
    uint8_t buf[4] = {0};
    size_t n = sqli_wire_encode_sl_header(buf, 4, 100, 1, 60, 0);
    TEST_ASSERT_EQUAL_UINT(0, n);
}
