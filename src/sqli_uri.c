#define _POSIX_C_SOURCE 200809L
#include "sqli_internal.h"

#include "sqli_tcp.h"
#include "sqli_tls.h"
#include "sqli_unix.h"

#include <uriparser/Uri.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <pwd.h>

/* ----------------------------------------------------------------
 * URI parsing helpers
 * ---------------------------------------------------------------- */

static void url_decode(const char *src, char *dst, size_t dst_sz)
{
    if (src == NULL || dst == NULL || dst_sz == 0) {
        if (dst && dst_sz > 0) dst[0] = '\0';
        return;
    }

    size_t j = 0;
    const char *s = src;
    while (*s && j + 1 < dst_sz) {
        if (*s == '%' && s[1] && s[2] && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], '\0'};
            dst[j++] = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            dst[j++] = ' ';
            s++;
        } else {
            dst[j++] = *s++;
        }
    }
    dst[j] = '\0';
}

static void range_to_str(const char *base, const UriTextRangeA *r, char *out, size_t out_sz)
{
    if (r == NULL || !r->afterLast || !r->first || r->first >= r->afterLast) {
        out[0] = '\0';
        return;
    }
    size_t len = (size_t)(r->afterLast - r->first);
    if (len >= out_sz)
        len = out_sz - 1;
    memcpy(out, base + (r->first - base), len);
    out[len] = '\0';
}

/* Build a null-terminated copy of the path from the segment list. */
static void path_to_str(const UriUriA *uri, char *out, size_t out_sz)
{
    out[0] = '\0';
    size_t pos = 0;
    const UriPathSegmentA *seg = uri->pathHead;
    while (seg && pos + 1 < out_sz) {
        size_t slen = (size_t)(seg->text.afterLast - seg->text.first);
        if (slen > 0) {
            if (pos < out_sz - 1) {
                size_t copy = slen;
                if (pos + copy >= out_sz)
                    copy = out_sz - pos - 1;
                memcpy(out + pos, seg->text.first, copy);
                pos += copy;
            }
        }
        seg = seg->next;
    }
    out[pos] = '\0';
}

/* Parse the query string into a key-value lookup.
 * Returns number of entries, -1 on allocation failure. */
typedef struct {
    char key[128];
    char value[256];
} uri_query_entry;

static int parse_query_string(const char *query, uri_query_entry *entries, int max_entries)
{
    if (query == NULL || *query == '\0' || entries == NULL || max_entries <= 0)
        return 0;

    /* Make a mutable copy */
    size_t qlen = strlen(query);
    char *buf = malloc(qlen + 1);
    if (buf == NULL)
        return -1;
    memcpy(buf, query, qlen + 1);

    int count = 0;
    char *saveptr = NULL;
    char *token = strtok_r(buf, "&", &saveptr);
    while (token && count < max_entries) {
        char *eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            url_decode(token, entries[count].key, sizeof(entries[count].key));
            url_decode(eq + 1, entries[count].value, sizeof(entries[count].value));
        } else {
            url_decode(token, entries[count].key, sizeof(entries[count].key));
            entries[count].value[0] = '\0';
        }
        count++;
        token = strtok_r(NULL, "&", &saveptr);
    }
    free(buf);
    return count;
}

static const char *find_query_value(const uri_query_entry *entries, int count,
                                    const char *key)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].key, key) == 0 && entries[i].value[0] != '\0')
            return entries[i].value;
    }
    return NULL;
}

/* ----------------------------------------------------------------
 * Protocol detection
 * ---------------------------------------------------------------- */

typedef enum {
    URI_PROTO_ONSOCTCP,
    URI_PROTO_ONSOCSSL,
    URI_PROTO_ONIPCSTR
} uri_protocol;

static uri_protocol detect_protocol(const char *scheme, size_t scheme_len)
{
    /* Scheme format: "informix+<protocol>" */
    if (scheme_len > 9 && memcmp(scheme, "informix+", 9) == 0) {
        const char *proto = scheme + 9;
        size_t proto_len = scheme_len - 9;
        if (proto_len == 8 && strncmp(proto, "onsoctcp", 8) == 0)
            return URI_PROTO_ONSOCTCP;
        if (proto_len == 8 && strncmp(proto, "onsocssl", 8) == 0)
            return URI_PROTO_ONSOCSSL;
        if (proto_len == 8 && strncmp(proto, "onipcstr", 8) == 0)
            return URI_PROTO_ONIPCSTR;
    }
    return (uri_protocol)-1;
}

/* ----------------------------------------------------------------
 * sqli_connect_uri
 * ---------------------------------------------------------------- */

sqli_status sqli_connect_uri(sqli_conn_t *conn, const char *uri,
                             const char *username, const char *password)
{
    if (conn == NULL || uri == NULL || *uri == '\0')
        return conn ? (set_error(conn, "uri and conn are required"), SQLI_INVALID_STATE)
                    : SQLI_INVALID_STATE;

    /* Parse URI with uriparser */
    UriUriA parsed_uri;
    const char *error_pos = NULL;
    if (uriParseSingleUriA(&parsed_uri, uri, &error_pos) != URI_SUCCESS) {
        set_error(conn, "failed to parse connection URI");
        return SQLI_INVALID_STATE;
    }

    /* All string buffers at function scope so pointers remain valid
     * through the switch and into sqli_connect(). */
    char scheme_buf[64];
    char path_buf[256];
    char query_raw[1024];
    char userinfo[256];
    char decoded_uri_user[256] = {0};
    char decoded_uri_pass[256] = {0};
    char host_raw[256];
    char port_raw[16];
    char default_socket[512];
    char unix_user[256] = {0};

    range_to_str(uri, &parsed_uri.scheme, scheme_buf, sizeof(scheme_buf));

    uri_protocol proto = detect_protocol(scheme_buf, strlen(scheme_buf));
    if (proto == (uri_protocol)-1) {
        set_error(conn, "unsupported URI scheme: use informix+onsoctcp, informix+onsocssl, or informix+onipcstr");
        uriFreeUriMembersA(&parsed_uri);
        return SQLI_INVALID_STATE;
    }

    /* Extract path (database name) */
    path_to_str(&parsed_uri, path_buf, sizeof(path_buf));
    if (path_buf[0] == '\0')
        strcpy(path_buf, "informix");

    /* Extract and parse query parameters */
    range_to_str(uri, &parsed_uri.query, query_raw, sizeof(query_raw));

    uri_query_entry entries[32];
    int entry_count = parse_query_string(query_raw, entries, (int)(sizeof(entries) / sizeof(entries[0])));
    if (entry_count < 0) {
        set_error(conn, "memory allocation failed for query parameters");
        uriFreeUriMembersA(&parsed_uri);
        return SQLI_ALLOC_FAIL;
    }

    /* INFORMIXSERVER is mandatory */
    const char *server = find_query_value(entries, entry_count, "INFORMIXSERVER");
    if (server == NULL || server[0] == '\0') {
        set_error(conn, "INFORMIXSERVER query parameter is mandatory");
        uriFreeUriMembersA(&parsed_uri);
        return SQLI_INVALID_STATE;
    }

    /* Build connect params */
    sqli_connect_params params;
    memset(&params, 0, sizeof(params));

    params.server = server;
    params.database = path_buf;

    /* Extract userInfo from URI (user:password@) */
    range_to_str(uri, &parsed_uri.userInfo, userinfo, sizeof(userinfo));
    if (userinfo[0] != '\0') {
        char *colon = strchr(userinfo, ':');
        if (colon) {
            *colon = '\0';
            url_decode(userinfo, decoded_uri_user, sizeof(decoded_uri_user));
            url_decode(colon + 1, decoded_uri_pass, sizeof(decoded_uri_pass));
        } else {
            url_decode(userinfo, decoded_uri_user, sizeof(decoded_uri_user));
        }
    }

    /* Use explicit credentials if provided, otherwise fall back to URI */
    if (username != NULL && username[0] != '\0') {
        params.username = username;
    } else if (decoded_uri_user[0] != '\0') {
        params.username = decoded_uri_user;
    }

    if (password != NULL && password[0] != '\0') {
        params.password = password;
    } else if (decoded_uri_pass[0] != '\0') {
        params.password = decoded_uri_pass;
    }

    /* Extract optional locale parameters */
    const char *client_locale = find_query_value(entries, entry_count, "CLIENT_LOCALE");
    if (client_locale && client_locale[0] != '\0')
        params.client_locale = client_locale;

    const char *db_locale = find_query_value(entries, entry_count, "DB_LOCALE");
    if (db_locale && db_locale[0] != '\0')
        params.db_locale = db_locale;

    /* Protocol-specific handling */
    switch (proto) {
    case URI_PROTO_ONIPCSTR: {
        const char *socket_path = find_query_value(entries, entry_count, "SOCKET_PATH");
        if (socket_path && socket_path[0] != '\0') {
            params.service = socket_path;
        } else {
            /* Priority: $INFORMIXTMP/<server>.str → /INFORMIXTMP/<server>.str → /tmp/.infosock.<server> */
            const char *informixtmp = getenv("INFORMIXTMP");
            if (informixtmp && informixtmp[0] != '\0') {
                snprintf(default_socket, sizeof(default_socket), "%s/%s.str", informixtmp, server);
            } else if (access("/INFORMIXTMP", F_OK) == 0) {
                snprintf(default_socket, sizeof(default_socket), "/INFORMIXTMP/%s.str", server);
            } else {
                snprintf(default_socket, sizeof(default_socket), "/tmp/.infosock.%s", server);
            }
            params.service = default_socket;
        }
        params.hostname = "";

        /* Override username to current Unix user for onipcstr */
        uid_t uid = getuid();
        struct passwd *pw = getpwuid(uid);
        if (pw != NULL && (params.username == NULL || params.username[0] == '\0')) {
            strncpy(unix_user, pw->pw_name, sizeof(unix_user) - 1);
            unix_user[sizeof(unix_user) - 1] = '\0';
            params.username = unix_user;
        }
        params.password = NULL;
        params.ssl_enable = false;
        break;
    }

    case URI_PROTO_ONSOCTCP:
    case URI_PROTO_ONSOCSSL: {
        range_to_str(uri, &parsed_uri.hostText, host_raw, sizeof(host_raw));
        if (host_raw[0] == '\0') {
            set_error(conn, "hostname is required for onsoctcp/onsocssl protocol");
            uriFreeUriMembersA(&parsed_uri);
            return SQLI_INVALID_STATE;
        }
        params.hostname = host_raw;

        range_to_str(uri, &parsed_uri.portText, port_raw, sizeof(port_raw));
        if (port_raw[0] == '\0') {
            set_error(conn, "port is required for onsoctcp/onsocssl protocol");
            uriFreeUriMembersA(&parsed_uri);
            return SQLI_INVALID_STATE;
        }
        params.service = port_raw;

        params.ssl_enable = (proto == URI_PROTO_ONSOCSSL);
        params.ssl_verify_peer = true;

        /* Optional SSL parameters */
        const char *ssl_ca = find_query_value(entries, entry_count, "SSL_CA_FILE");
        if (ssl_ca && ssl_ca[0] != '\0')
            params.ssl_ca_file = ssl_ca;

        const char *ssl_verify = find_query_value(entries, entry_count, "SSL_VERIFY_PEER");
        if (ssl_verify && ssl_verify[0] != '\0') {
            params.ssl_verify_peer = (strcasecmp(ssl_verify, "true") == 0 ||
                                       strcasecmp(ssl_verify, "yes") == 0 ||
                                       strcmp(ssl_verify, "1") == 0);
        }
        break;
    }

    default:
        set_error(conn, "unsupported protocol");
        uriFreeUriMembersA(&parsed_uri);
        return SQLI_INVALID_STATE;
    }

    uriFreeUriMembersA(&parsed_uri);

    memset(userinfo, 0, sizeof(userinfo));
    memset(decoded_uri_user, 0, sizeof(decoded_uri_user));
    memset(decoded_uri_pass, 0, sizeof(decoded_uri_pass));

    return sqli_connect(conn, &params);
}
