#define _POSIX_C_SOURCE 200809L
#include "sqli_internal.h"

#include "sqli_log.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

sqli_status sqli_conn_grow_buf(sqli_conn_t *c, uint8_t **buf, size_t *len,
                               size_t *cap, size_t min_size);

static bool next_doubling_capacity(size_t current, size_t minimum, size_t *out)
{
    if (out == NULL)
        return false;
    if (current == 0)
        current = 1;
    while (current < minimum) {
        if (current > (SIZE_MAX / 2))
            return false;
        current *= 2;
    }
    *out = current;
    return true;
}

static bool parse_bool_env(const char *v)
{
    if (v == NULL || *v == '\0')
        return false;
    if (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
        strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0)
        return true;
    return false;
}

static bool parse_bool_env_default(const char *v, bool default_value)
{
    if (v == NULL || *v == '\0')
        return default_value;
    if (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
        strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0)
        return true;
    if (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
        strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0)
        return false;
    return default_value;
}

static sqli_log_level parse_log_level_env(const char *v)
{
    if (v == NULL || *v == '\0')
        return SQLI_LOG_ERROR;

    if (strcasecmp(v, "NONE") == 0)
        return SQLI_LOG_NONE;
    if (strcasecmp(v, "ERROR") == 0)
        return SQLI_LOG_ERROR;
    if (strcasecmp(v, "WARN") == 0 || strcasecmp(v, "WARNING") == 0)
        return SQLI_LOG_WARN;
    if (strcasecmp(v, "INFO") == 0)
        return SQLI_LOG_INFO;
    if (strcasecmp(v, "DEBUG") == 0)
        return SQLI_LOG_DEBUG;

    return SQLI_LOG_ERROR;
}

static bool sqlcode_is_retryable_network(int sqlcode)
{
    switch (sqlcode) {
    case -908:
    case -912:
    case -913:
    case -932:
    case -936:
        return true;
    default:
        return false;
    }
}

static bool sqlcode_is_nonretryable_data(int sqlcode)
{
    switch (sqlcode) {
    case -201:   /* syntax */
    case -206:   /* table not found */
    case -23101: /* locale categories */
    case -23197: /* db locale mismatch */
        return true;
    default:
        return false;
    }
}

static bool isamcode_is_retryable(int isamcode)
{
    if (isamcode < 0)
        isamcode = -isamcode;
    switch (isamcode) {
    case 107: /* record locked */
    case 143: /* deadlock */
    case 154: /* lock timeout */
    case 7351:
        return true;
    default:
        return false;
    }
}

static bool context_indicates_network(const char *context)
{
    if (context == NULL || context[0] == '\0')
        return false;
    if (strcmp(context, "connect/tcp") == 0)
        return true;
    return false;
}

static bool message_indicates_network(const char *msg)
{
    if (msg == NULL || msg[0] == '\0')
        return false;
    return strstr(msg, "TCP connection failed") != NULL ||
           strstr(msg, "failed to connect") != NULL ||
           strstr(msg, "failed to read") != NULL ||
           strstr(msg, "failed to send") != NULL ||
           strstr(msg, "socket not connected") != NULL ||
           strstr(msg, "broken pipe") != NULL ||
           strstr(msg, "connection reset") != NULL;
}

/* ----------------------------------------------------------------
 * Buffer management helpers
 * ---------------------------------------------------------------- */

sqli_status sqli_conn_grow_buf(sqli_conn_t *c, uint8_t **buf, size_t *len,
                               size_t *cap, size_t min_size)
{
    (void)c;
    if (*buf == NULL) {
        size_t sz = min_size > 256 ? min_size : 256;
        *buf = calloc(1, sz);
        if (*buf == NULL)
            return SQLI_ALLOC_FAIL;
        *cap = sz;
        *len = 0;
        return SQLI_OK;
    }

    if (*cap >= min_size)
        return SQLI_OK;

    size_t new_cap = 0;
    if (!next_doubling_capacity(*cap, min_size, &new_cap)) {
        set_error_context(c, "conn/grow_buf", 0);
        set_error(c, "buffer growth overflow");
        return SQLI_ALLOC_FAIL;
    }

    uint8_t *new_buf = realloc(*buf, new_cap);
    if (new_buf == NULL)
        return SQLI_ALLOC_FAIL;

    *buf = new_buf;
    *cap = new_cap;
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * Wire write helpers (append to dynamic write buffer)
 * ---------------------------------------------------------------- */

sqli_status sqli_conn_write_buf(sqli_conn_t *c, const uint8_t *data, size_t len)
{
    if (c == NULL || data == NULL || len == 0)
        return SQLI_INVALID_STATE;
    if (c->write_buf_len > SIZE_MAX - len)
        return SQLI_ALLOC_FAIL;

    sqli_status rc = sqli_conn_grow_buf(c, &c->write_buf, &c->write_buf_len,
                                        &c->write_buf_cap, c->write_buf_len + len);
    if (rc != SQLI_OK)
        return rc;

    memcpy(c->write_buf + c->write_buf_len, data, len);
    c->write_buf_len += len;
    return SQLI_OK;
}

sqli_status sqli_conn_write_byte(sqli_conn_t *c, uint8_t val)
{
    return sqli_conn_write_buf(c, &val, 1);
}

sqli_status sqli_conn_write_be16(sqli_conn_t *c, uint16_t val)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xFF);
    return sqli_conn_write_buf(c, buf, 2);
}

sqli_status sqli_conn_write_be32(sqli_conn_t *c, uint32_t val)
{
    uint8_t buf[4];
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val & 0xFF);
    return sqli_conn_write_buf(c, buf, 4);
}

sqli_status sqli_conn_write_str(sqli_conn_t *c, const char *s)
{
    if (s == NULL)
        return sqli_conn_write_buf(c, (const uint8_t[]){0, 0}, 2); /* null string */

    size_t slen = strnlen(s, 4096);
    uint8_t hdr[2];
    hdr[0] = (uint8_t)((slen >> 8) & 0xFF);
    hdr[1] = (uint8_t)(slen & 0xFF);

    sqli_status rc = sqli_conn_write_buf(c, hdr, 2);
    if (rc != SQLI_OK)
        return rc;

    return sqli_conn_write_buf(c, (const uint8_t *)s, slen);
}

static void locale_to_codeset(const char *locale, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0)
        return;
    out[0] = '\0';
    if (locale == NULL || *locale == '\0')
        return;

    const char *dot = strchr(locale, '.');
    const char *src = (dot != NULL) ? dot + 1 : locale;
    if (src == NULL || *src == '\0')
        return;

    size_t n = 0;
    while (src[n] != '\0' && src[n] != '@' && src[n] != '/' && n + 1 < out_sz) {
        out[n] = src[n];
        n++;
    }
    out[n] = '\0';

    if (out[0] == '\0')
        return;
    if (strcasecmp(out, "utf8") == 0 || strcasecmp(out, "utf-8") == 0) {
        snprintf(out, out_sz, "UTF-8");
        return;
    }
    if (strcasecmp(out, "cp1252") == 0 || strcmp(out, "1252") == 0 ||
        strcasecmp(out, "windows-1252") == 0) {
        snprintf(out, out_sz, "WINDOWS-1252");
        return;
    }
    if (strcasecmp(out, "8859-1") == 0 || strcasecmp(out, "iso8859-1") == 0 ||
        strcasecmp(out, "latin1") == 0 || strcasecmp(out, "iso-8859-1") == 0) {
        snprintf(out, out_sz, "ISO-8859-1");
        return;
    }
}

sqli_status sqli_conn_encode_client_to_db(sqli_conn_t *c, const char *s,
                                          uint8_t **out, size_t *out_len)
{
    if (out == NULL || out_len == NULL || s == NULL)
        return SQLI_INVALID_STATE;
    *out = NULL;
    *out_len = 0;

    size_t in_len = strlen(s);
    uint8_t *raw = malloc(in_len + 1);
    if (raw == NULL)
        return SQLI_ALLOC_FAIL;
    memcpy(raw, s, in_len);
    raw[in_len] = '\0';

    if (c == NULL || c->client_locale == NULL || c->db_locale == NULL ||
        c->client_locale[0] == '\0' || c->db_locale[0] == '\0') {
        *out = raw;
        *out_len = in_len;
        return SQLI_OK;
    }

    char from_cs[64];
    char to_cs[64];
    locale_to_codeset(c->client_locale, from_cs, sizeof(from_cs));
    locale_to_codeset(c->db_locale, to_cs, sizeof(to_cs));
    if (from_cs[0] == '\0' || to_cs[0] == '\0' || strcasecmp(from_cs, to_cs) == 0) {
        *out = raw;
        *out_len = in_len;
        return SQLI_OK;
    }

    iconv_t cd = iconv_open(to_cs, from_cs);
    if (cd == (iconv_t)-1) {
        sqli_log(SQLI_LOG_WARN, "locale encode disabled (iconv_open %s<- %s failed: %s)",
                 to_cs, from_cs, strerror(errno));
        *out = raw;
        *out_len = in_len;
        return SQLI_OK;
    }

    size_t out_cap = (in_len * 4u) + 32u;
    uint8_t *buf = malloc(out_cap + 1u);
    if (buf == NULL) {
        iconv_close(cd);
        free(raw);
        return SQLI_ALLOC_FAIL;
    }

    char *in_ptr = (char *)raw;
    size_t in_left = in_len;
    char *out_ptr = (char *)buf;
    size_t out_left = out_cap;

    while (true) {
        size_t rc = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        if (rc != (size_t)-1)
            break;
        if (errno == E2BIG) {
            size_t used = (size_t)(out_ptr - (char *)buf);
            size_t new_cap = 0;
            if (!next_doubling_capacity(out_cap, out_cap + 1u, &new_cap)) {
                iconv_close(cd);
                free(buf);
                free(raw);
                return SQLI_ALLOC_FAIL;
            }
            uint8_t *grown = realloc(buf, new_cap + 1u);
            if (grown == NULL) {
                iconv_close(cd);
                free(buf);
                free(raw);
                return SQLI_ALLOC_FAIL;
            }
            buf = grown;
            out_cap = new_cap;
            out_ptr = (char *)buf + used;
            out_left = out_cap - used;
            continue;
        }
        sqli_log(SQLI_LOG_WARN, "locale encode failed (%s -> %s), using raw bytes", from_cs, to_cs);
        iconv_close(cd);
        free(buf);
        *out = raw;
        *out_len = in_len;
        return SQLI_OK;
    }

    size_t used = (size_t)(out_ptr - (char *)buf);
    buf[used] = '\0';
    iconv_close(cd);
    free(raw);
    *out = buf;
    *out_len = used;
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * sqli_create / sqli_destroy
 * ---------------------------------------------------------------- */

sqli_status sqli_create(sqli_conn_t **conn)
{
    if (conn == NULL)
        return SQLI_INVALID_STATE;

    sqli_log_set_level(parse_log_level_env(getenv("SQLI_LOG_LEVEL")));

    sqli_conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        sqli_log(SQLI_LOG_ERROR, "memory allocation failed");
        return SQLI_ALLOC_FAIL;
    }

    clear_error(c);
    c->socket_fd = -1;
    c->state = SQLI_CONN_CLOSED;
    c->strict_protocol = parse_bool_env(getenv("SQLI_STRICT_PROTOCOL"));
    c->trim_trailing_spaces = true;
    c->trim_trailing_spaces =
        parse_bool_env_default(getenv("SQLI_TRIM_TRAILING_SPACES"), c->trim_trailing_spaces);
    c->trim_trailing_spaces =
        parse_bool_env_default(getenv("IFX_TRIMTRAILINGSPACES"), c->trim_trailing_spaces);
    c->cursor_type = SQLI_CURSOR_FORWARD_ONLY;
    c->holdability = SQLI_CURSOR_CLOSE_AT_COMMIT;
    c->fetch_buf_size = 4194304u;
    c->decode_cd = (iconv_t)-1;
    c->decode_cd_ready = false;
    c->decode_cp1252_utf8 = false;
    c->decode_locale_checked = false;
    c->read_buf_cap = 1048576u;
    c->read_buf = calloc(1, c->read_buf_cap);
    c->read_buf_pos = 0;
    c->read_buf_len = 0;

    /* Allocate and zero string buffers so sqli_connect can write into them */
    c->hostname     = calloc(1, 256);
    c->service      = calloc(1, 64);
    c->server       = calloc(1, 256);
    c->database     = calloc(1, 256);
    c->username     = calloc(1, 256);
    c->password     = calloc(1, 256);
    c->client_locale = calloc(1, 256);
    c->db_locale    = calloc(1, 256);

    if (!c->read_buf || !c->hostname || !c->service || !c->server || !c->database ||
        !c->username || !c->password || !c->client_locale || !c->db_locale) {
        free(c->read_buf);
        free(c->hostname);
        free(c->service);
        free(c->server);
        free(c->database);
        free(c->username);
        if (c->password != NULL) {
            memset(c->password, 0, 256);
            free(c->password);
        }
        free(c->client_locale);
        free(c->db_locale);
        free(c);
        sqli_log(SQLI_LOG_ERROR, "memory allocation failed for connection strings");
        return SQLI_ALLOC_FAIL;
    }

    *conn = c;
    sqli_log(SQLI_LOG_DEBUG, "connection object created");
    return SQLI_OK;
}

void sqli_destroy(sqli_conn_t *conn)
{
    if (conn == NULL)
        return;

    /* Honor the public lifecycle contract and release transport resources too. */
    sqli_close(conn);

    if (conn->decode_cd_ready && conn->decode_cd != (iconv_t)-1) {
        iconv_close(conn->decode_cd);
        conn->decode_cd = (iconv_t)-1;
        conn->decode_cd_ready = false;
    }

    /* Release all owned resources */
    free(conn->read_buf);
    free(conn->write_buf);
    free(conn->server);
    free(conn->hostname);
    free(conn->service);
    free(conn->database);
    free(conn->username);
    if (conn->password != NULL) {
        memset(conn->password, 0, 256);
        free(conn->password);
    }
    free(conn->client_locale);
    free(conn->db_locale);

    if (conn->env_vars != NULL) {
        for (size_t i = 0; i < conn->env_var_count; i++)
            free(conn->env_vars[i]);
        free(conn->env_vars);
    }

    sqli_log(SQLI_LOG_DEBUG, "destroying connection");
    free(conn);
}

/* ----------------------------------------------------------------
 * Error retrieval
 * ---------------------------------------------------------------- */

const char *sqli_error(sqli_conn_t *conn)
{
    if (conn == NULL)
        return NULL;
    return conn->errmsg[0] != '\0' ? conn->errmsg : NULL;
}

int sqli_errno(sqli_conn_t *conn)
{
    if (conn == NULL)
        return -1;
    if (conn->error_info.has_error) {
        if (conn->error_info.sqlcode != 0)
            return conn->error_info.sqlcode;
        return -(int)conn->error_info.status;
    }
    return conn->errmsg[0] != '\0' ? -1 : 0;
}

sqli_status sqli_set_strict_protocol(sqli_conn_t *conn, bool enabled)
{
    if (conn == NULL)
        return SQLI_INVALID_STATE;
    conn->strict_protocol = enabled;
    return SQLI_OK;
}

bool sqli_get_strict_protocol(sqli_conn_t *conn)
{
    if (conn == NULL)
        return false;
    return conn->strict_protocol;
}

sqli_status sqli_set_trim_trailing_spaces(sqli_conn_t *conn, bool enabled)
{
    if (conn == NULL)
        return SQLI_INVALID_STATE;
    conn->trim_trailing_spaces = enabled;
    return SQLI_OK;
}

bool sqli_get_trim_trailing_spaces(sqli_conn_t *conn)
{
    if (conn == NULL)
        return false;
    return conn->trim_trailing_spaces;
}

sqli_status sqli_set_cursor_type(sqli_conn_t *conn, sqli_cursor_type type)
{
    if (conn == NULL)
        return SQLI_INVALID_STATE;
    if (type != SQLI_CURSOR_FORWARD_ONLY &&
        type != SQLI_CURSOR_SCROLL_INSENSITIVE)
        return SQLI_INVALID_STATE;
    conn->cursor_type = type;
    return SQLI_OK;
}

sqli_cursor_type sqli_get_cursor_type(sqli_conn_t *conn)
{
    if (conn == NULL)
        return SQLI_CURSOR_FORWARD_ONLY;
    return conn->cursor_type;
}

sqli_status sqli_set_cursor_holdability(sqli_conn_t *conn,
                                        sqli_cursor_holdability holdability)
{
    if (conn == NULL)
        return SQLI_INVALID_STATE;
    if (holdability != SQLI_CURSOR_HOLD_OVER_COMMIT &&
        holdability != SQLI_CURSOR_CLOSE_AT_COMMIT)
        return SQLI_INVALID_STATE;
    conn->holdability = holdability;
    return SQLI_OK;
}

sqli_cursor_holdability sqli_get_cursor_holdability(sqli_conn_t *conn)
{
    if (conn == NULL)
        return SQLI_CURSOR_CLOSE_AT_COMMIT;
    return conn->holdability;
}

sqli_status sqli_error_get_info(sqli_conn_t *conn, sqli_error_info *out)
{
    if (conn == NULL || out == NULL)
        return SQLI_INVALID_STATE;
    memcpy(out, &conn->error_info, sizeof(*out));
    return SQLI_OK;
}

sqli_status sqli_error_classify(sqli_conn_t *conn, sqli_error_class *out_class,
                                bool *retryable)
{
    if (conn == NULL || out_class == NULL || retryable == NULL)
        return SQLI_INVALID_STATE;

    *out_class = SQLI_ERROR_CLASS_NONE;
    *retryable = false;

    const sqli_error_info *e = &conn->error_info;
    if (!e->has_error)
        return SQLI_OK;

    if (e->status == SQLI_AUTH_FAIL) {
        *out_class = SQLI_ERROR_CLASS_AUTH;
        return SQLI_OK;
    }
    if (e->status == SQLI_TIMEOUT || e->status == SQLI_IO_ERROR) {
        *out_class = SQLI_ERROR_CLASS_NETWORK;
        *retryable = true;
        return SQLI_OK;
    }
    if (e->status == SQLI_PROTO_ERROR && e->unknown_opcode != 0) {
        *out_class = SQLI_ERROR_CLASS_PROTOCOL;
        return SQLI_OK;
    }
    if (e->status == SQLI_ERR &&
        (context_indicates_network(e->context) || message_indicates_network(e->message))) {
        *out_class = SQLI_ERROR_CLASS_NETWORK;
        *retryable = true;
        return SQLI_OK;
    }

    if (sqlcode_is_retryable_network(e->sqlcode) || isamcode_is_retryable(e->isamcode)) {
        *out_class = SQLI_ERROR_CLASS_NETWORK;
        *retryable = true;
        return SQLI_OK;
    }

    if (e->sqlstate[0] == '4' && e->sqlstate[1] == '0') {
        *out_class = SQLI_ERROR_CLASS_SERVER;
        *retryable = true; /* transaction rollback / serialization family */
        return SQLI_OK;
    }

    if (sqlcode_is_nonretryable_data(e->sqlcode)) {
        *out_class = SQLI_ERROR_CLASS_DATA;
        return SQLI_OK;
    }

    if (e->status == SQLI_PROTO_ERROR) {
        *out_class = SQLI_ERROR_CLASS_PROTOCOL;
        return SQLI_OK;
    }
    if (e->sqlcode != 0 || e->isamcode != 0) {
        *out_class = SQLI_ERROR_CLASS_SERVER;
        return SQLI_OK;
    }

    *out_class = SQLI_ERROR_CLASS_UNKNOWN;
    return SQLI_OK;
}

bool sqli_error_is_retryable(sqli_conn_t *conn)
{
    sqli_error_class c = SQLI_ERROR_CLASS_UNKNOWN;
    bool retryable = false;
    if (sqli_error_classify(conn, &c, &retryable) != SQLI_OK)
        return false;
    return retryable;
}

sqli_status sqli_retry_recommend(sqli_conn_t *conn, uint32_t attempt,
                                 bool *out_should_retry, uint32_t *out_delay_ms)
{
    if (conn == NULL || out_should_retry == NULL || out_delay_ms == NULL)
        return SQLI_INVALID_STATE;

    *out_should_retry = false;
    *out_delay_ms = 0;

    sqli_error_class klass = SQLI_ERROR_CLASS_UNKNOWN;
    bool retryable = false;
    sqli_status rc = sqli_error_classify(conn, &klass, &retryable);
    if (rc != SQLI_OK)
        return rc;
    if (!retryable)
        return SQLI_OK;

    uint32_t base_ms = 200;
    uint32_t max_ms = 30000;
    if (klass == SQLI_ERROR_CLASS_NETWORK) {
        base_ms = 250;
        max_ms = 60000;
    } else if (klass == SQLI_ERROR_CLASS_SERVER) {
        base_ms = 100;
        max_ms = 5000;
    }

    if (attempt > 12)
        attempt = 12;
    uint64_t delay = (uint64_t)base_ms << attempt;
    if (delay > max_ms)
        delay = max_ms;

    *out_should_retry = true;
    *out_delay_ms = (uint32_t)delay;
    return SQLI_OK;
}
