#define _GNU_SOURCE
#include "sqli_internal.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pwd.h>

/* ----------------------------------------------------------------
 * ASC-BINARY body encoding for CONREQ (connection request)
 * Matches Client driver wire format exactly.
 *
 * Layout (inside tag 100):
 *   [2] tag 100: ASC-BINARY
 *   [2] tag 101: Version/Capability
 *     [4] protocol version (61)
 *     [2] float type len, [5] "IEEEM", [1] NUL
 *     [2] tag 108, [12] "sqlexec\0...\0"
 *     [2] version len, [N] "9.280\0"
 *     [2] RDS len, [N] "RDS#R000000\0"
 *     [2] app ID len, [N] "sqli\0"
 *     [4] Cap_1, [4] Cap_2, [4] Cap_3, [2] flags
 *     [2] user len, [N] user, [1] NUL
 *     [2] pass len, [N] pass, [1] NUL
 *     [8] proto "ol\0...\0", [4] proto ver, [8] transport
 *     [4] network level
 *     [2] tag 104, [2] options len, [4] options bitmask
 *     [2] server len, [N] server, [1] NUL
 *     [2] dbname (len+data+NUL or 0), [2] four zero shorts
 *     [2] tag 106, [2] env count, [per var]
 *     [2] tag 107, [4] reserved, [4] pid, [4] tid
 *     [2] hostname len, [N] hostname, [1] NUL
 *     [2] CWD len, [N] CWD, [1] NUL
 *     [2] tag 116, [2] block len, [4] res, [4] res, [2] len, [N] name, [1] NUL
 *     [2] tag 127: END
 */

/* Helper: write a 2-byte BE value into buf at pos, return next pos */
static size_t w16(uint8_t *buf, size_t pos, uint16_t val)
{
    buf[pos] = (uint8_t)((val >> 8) & 0xFF);
    buf[pos + 1] = (uint8_t)(val & 0xFF);
    return pos + 2;
}

/* Helper: write a 4-byte BE value into buf at pos, return next pos */
static size_t w32(uint8_t *buf, size_t pos, uint32_t val)
{
    buf[pos] = (uint8_t)((val >> 24) & 0xFF);
    buf[pos + 1] = (uint8_t)((val >> 16) & 0xFF);
    buf[pos + 2] = (uint8_t)((val >> 8) & 0xFF);
    buf[pos + 3] = (uint8_t)(val & 0xFF);
    return pos + 4;
}

/* Helper: write a string (no length prefix) into buf at pos, return next pos */
static size_t wstr(uint8_t *buf, size_t pos, const char *s)
{
    if (s == NULL)
        return pos;
    size_t len = strnlen(s, 256);
    memcpy(buf + pos, s, len);
    return pos + len;
}

/* ----------------------------------------------------------------
 * sqli_asc_encode_conreq
 * ---------------------------------------------------------------- */

size_t sqli_asc_encode_conreq(sqli_conn_t *c, uint8_t *buf, size_t buf_size,
                              const char *transport)
{
    if (buf == NULL || buf_size < 64)
        return 0;

    size_t p = 0;

    /* Tag 100: ASC-BINARY wrapper */
    p = w16(buf, p, SQLI_TAG_ASC_BINARY);

    /* Tag 101: Version/Capability block */
    p = w16(buf, p, SQLI_TAG_VERSION_CAPS);

    /* Protocol version */
    p = w32(buf, p, SQLI_PROTO_VERSION);

    /* Float type: length(2) + "IEEEM"(5) + NUL(1) = 8 */
    uint8_t float_type[] = "IEEEM";
    p = w16(buf, p, (uint16_t)(sizeof(float_type))); /* includes NUL */
    p = wstr(buf, p, (const char *)float_type);
    buf[p++] = 0; /* NUL terminator */

    /* Tag 108: Product type — 12 bytes: "sqlexec" + 5 NULs (matches Client/server format) */
    p = w16(buf, p, SQLI_TAG_PRODUCT_TYPE);
    const char product_type[12] = "sqlexec\0\0\0\0\0";
    memcpy(buf + p, product_type, 12);
    p += 12;

    /* Product version: "9.280" + NUL */
    const char *version = "9.280";
    p = w16(buf, p, (uint16_t)(strlen(version) + 1));
    p = wstr(buf, p, version);
    buf[p++] = 0;

    /* RDS number: "RDS#R000000" + NUL — no tag, matches Client */
    const char *rds = "RDS#R000000";
    size_t rds_len = strlen(rds);  /* 10 */
    p = w16(buf, p, (uint16_t)(rds_len + 1));
    p = wstr(buf, p, rds);
    buf[p++] = 0;

    /* App ID: "sqli" + NUL */
    const char *app_id = "sqli";
    p = w16(buf, p, (uint16_t)(strlen(app_id) + 1));
    p = wstr(buf, p, app_id);
    buf[p++] = 0;

    /* Capabilities */
    p = w32(buf, p, SQLI_CLIENT_CAP_1);  /* Cap_1 */
    p = w32(buf, p, 0);                   /* Cap_2 */
    p = w32(buf, p, 0);                   /* Cap_3 */
    p = w16(buf, p, 1);                   /* flags = 1 */

    /* Username — len(2) + data + NUL */
    const char *user = c->username;
    if (user == NULL)
        user = "";
    size_t user_len = (size_t)strlen(user);
    p = w16(buf, p, (uint16_t)(user_len + 1));
    p = wstr(buf, p, user);
    buf[p++] = 0;

    /* Password — len(2) + data + NUL */
    const char *pass = c->password;
    if (pass == NULL)
        pass = "";
    size_t pass_len = (size_t)strlen(pass);
    p = w16(buf, p, (uint16_t)(pass_len + 1));
    p = wstr(buf, p, pass);
    buf[p++] = 0;

    /* Protocol name: 8 bytes, no length prefix: "ol" + 6 NULs (matches Client) */
    {
        uint8_t proto_name[8] = {0};
        memcpy(proto_name, "ol", 2);
        memcpy(buf + p, proto_name, 8);
        p += 8;
    }

    /* Protocol version: 4-byte BE integer = 61 */
    p = w32(buf, p, SQLI_PROTO_VERSION);

    /* Transport: fixed 8 bytes, no length prefix: "tlitcp"/"ipcstr" + NULs */
    {
        uint8_t transport_name[8] = {0};
        if (transport != NULL) {
            size_t tlen = strlen(transport);
            if (tlen > 7) tlen = 7;
            memcpy(transport_name, transport, tlen);
        } else {
            memcpy(transport_name, "tlitcp", 6);
        }
        memcpy(buf + p, transport_name, 8);
        p += 8;
    }

    /* Network level: 4-byte int (matches Client driver) */
    p = w32(buf, p, 1);

    /* Tag 104: Statement options — type=11, stmtoptions=3 (ASF_AMBIG_SEOL) */
    p = w16(buf, p, SQLI_TAG_STMT_OPTIONS);
    p = w16(buf, p, 11);
    p = w32(buf, p, 3u);

    /* Servername — len(2) + data + NUL */
    const char *srv = c->server;
    if (srv == NULL)
        srv = "";
    size_t srv_len = (size_t)strlen(srv);
    p = w16(buf, p, (uint16_t)(srv_len + 1));
    p = wstr(buf, p, srv);
    buf[p++] = 0;

    /* Database name — Client sends empty (opened via SQ_DBOPEN later) */
    p = w16(buf, p, 0);

    /* Reserved: four shorts = 0 */
    p = w16(buf, p, 0);
    p = w16(buf, p, 0);
    p = w16(buf, p, 0);
    p = w16(buf, p, 0);

    /* Tag 106: Environment variables — match Client env vars */
    p = w16(buf, p, SQLI_TAG_ENV_VARS);
    size_t env_start = p;
    p = w16(buf, p, 0); /* placeholder for count */

    /* Helper: write one env var (name + value), return count added */
    typedef struct { const char *name; const char *value; } env_pair;
    env_pair env_pairs[] = {
        {"DBPATH", "."},
        {"CLIENT_LOCALE", c->client_locale && c->client_locale[0] ? c->client_locale : "en_US.utf8"},
        {"CLNT_PAM_CAPABLE", "1"},
        {"IFX_UPDDESC", "1"},
        {"DB_LOCALE", c->db_locale && c->db_locale[0] ? c->db_locale : "en_US.8859-1"},
        {"NODEFDAC", "no"},
    };
    size_t num_env = sizeof(env_pairs) / sizeof(env_pairs[0]);
    size_t env_count = 0;
    for (size_t k = 0; k < num_env; k++) {
        size_t name_len = strlen(env_pairs[k].name);
        size_t val_len = strlen(env_pairs[k].value);
        if (p + 2 + name_len + 1 + 2 + val_len + 1 < buf_size) {
            p = w16(buf, p, (uint16_t)(name_len + 1));
            p = wstr(buf, p, env_pairs[k].name);
            buf[p++] = 0;
            p = w16(buf, p, (uint16_t)(val_len + 1));
            p = wstr(buf, p, env_pairs[k].value);
            buf[p++] = 0;
            env_count++;
        }
    }

    buf[env_start] = (uint8_t)(env_count >> 8);
    buf[env_start + 1] = (uint8_t)(env_count & 0xFF);

    /* Tag 107: Environment info */
    p = w16(buf, p, SQLI_TAG_ENV_INFO);
    p = w32(buf, p, 0);  /* reserved */

    /* Process ID */
    p = w32(buf, p, (uint32_t)getpid());

    /* Thread ID (use 0 as placeholder for real thread ID) */
    p = w32(buf, p, 0);

    /* Hostname — len(2) + data + NUL */
    char hostname[256];
    hostname[0] = '\0';
    if (gethostname(hostname, sizeof(hostname) - 1) != 0)
        strcpy(hostname, "localhost");
    size_t host_len = strlen(hostname);
    p = w16(buf, p, (uint16_t)(host_len + 1));
    p = wstr(buf, p, hostname);
    buf[p++] = 0;

    /* 2-byte reserved (matches Client encodeEnvPInfo) */
    p = w16(buf, p, 0);

    /* CWD — len(2) + data + NUL */
    char cwd[512];
    cwd[0] = '\0';
    if (getcwd(cwd, sizeof(cwd) - 1) == NULL)
        strcpy(cwd, ".");
    size_t cwd_len = strlen(cwd);
    p = w16(buf, p, (uint16_t)(cwd_len + 1));
    p = wstr(buf, p, cwd);
    buf[p++] = 0;

    /* Tag 116: App name block — len(2) + data + NUL */
    const char *app_name = "libsqli";
    size_t app_len = (size_t)strlen(app_name);
    p = w16(buf, p, SQLI_TAG_APP_NAME);
    p = w16(buf, p, (uint16_t)(10 + app_len + 1));
    p = w32(buf, p, 0);
    p = w32(buf, p, 0);
    p = w16(buf, p, (uint16_t)(app_len + 1));
    p = wstr(buf, p, app_name);
    buf[p++] = 0;

    /* Tag 127: End marker */
    p = w16(buf, p, SQLI_TAG_END);

    return p;
}

/* ----------------------------------------------------------------
 * Base64 encoding (for onipcstr IPC preamble)
 * ---------------------------------------------------------------- */

static const char sqli_b64_alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t sqli_b64_encode(const uint8_t *src, size_t src_len,
                              char *dst, size_t dst_size)
{
    size_t out = 0;
    size_t i = 0;

    while (i + 2 < src_len) {
        if (out + 4 >= dst_size) return 0;
        dst[out++] = sqli_b64_alpha[src[i] >> 2];
        dst[out++] = sqli_b64_alpha[((src[i] & 0x03) << 4) | (src[i + 1] >> 4)];
        dst[out++] = sqli_b64_alpha[((src[i + 1] & 0x0F) << 2) | (src[i + 2] >> 6)];
        dst[out++] = sqli_b64_alpha[src[i + 2] & 0x3F];
        i += 3;
    }

    if (i < src_len) {
        if (out + 3 >= dst_size) return 0;
        dst[out++] = sqli_b64_alpha[src[i] >> 2];
        if (i + 1 < src_len) {
            dst[out++] = sqli_b64_alpha[((src[i] & 0x03) << 4) | (src[i + 1] >> 4)];
            dst[out++] = sqli_b64_alpha[(src[i + 1] & 0x0F) << 2];
        } else {
            dst[out++] = sqli_b64_alpha[(src[i] & 0x03) << 4];
        }
    }

    return out;
}

/* ----------------------------------------------------------------
 * sqli_asc_encode_ipc_preamble
 *
 * Build the "sqAZ" connection request for onipcstr (Unix socket).
 *
 * Format: "sqAZQBPQAA" + text_header + ":" + base64(binary_body) + "\0"
 *
 * The text header contains product info and environment variables.
 * The binary body contains connection parameters in a fixed layout.
 * Layout verified byte-for-byte against dbaccess 14.10.12.5 trace.
 * ---------------------------------------------------------------- */

size_t sqli_asc_encode_ipc_preamble(sqli_conn_t *c, uint8_t *buf, size_t buf_size,
                                    const char *transport)
{
    (void)transport; /* Always "ipcstr" for Unix socket connections */
    if (buf == NULL || buf_size < 512)
        return 0;

    const char *cl = c->client_locale && c->client_locale[0]
                     ? c->client_locale : "en_US.utf8";
    const char *dl = c->db_locale && c->db_locale[0]
                     ? c->db_locale : "en_US.utf8";
    size_t p = 10; /* Leave 10 bytes for the dynamically generated magic header */
    bool legacy_style = !(c->username != NULL && strcmp(c->username, "informix") == 0);

    /* Product string: "sqlexec <user> <ver> serial -p -fIEEEI " */
    const char *user = c->username;
    if (user == NULL) user = "";
    const char *version = "9.240";

    {
        int n = (int)snprintf((char *)buf + p, buf_size - p,
                              legacy_style
                                  ? "sqlexec %-6s %s serial -p -fIEEEI "
                                  : "sqlexec %-9s %s serial -p -fIEEEI ",
                              user, version);
        if (n < 0 || (size_t)n >= buf_size - p) return 0;
        p += (size_t)n;
    }

    /* Environment variables with trailing space before colon */
    {
        const char *srv = c->server;
        if (srv == NULL) srv = "";

        int n = (int)snprintf((char *)buf + p, buf_size - p,
                              legacy_style
                                  ? "DBPATH=//%s CLIENT_LOCALE=%s NODEFDAC=no "
                                    "CLNT_PAM_CAPABLE=1 DB_LOCALE=%s "
                                  : "DBPATH=//%s DBDATE=DMY4. DBMONEY=. CLIENT_LOCALE=%s NODEFDAC=no "
                                    "CLNT_PAM_CAPABLE=1 INFORMIXCONTIME=1 DB_LOCALE=%s ",
                              srv, cl, dl);
        if (n < 0 || (size_t)n >= buf_size - p) return 0;
        p += (size_t)n;
    }

    /* Separator */
    buf[p++] = ':';

    /* --- Binary body (before base64) --- */
    /* Layout from dbaccess trace - 191 bytes for reference server.
     * Fixed prefix (66 bytes) + variable fields. */
    uint8_t body[512];
    size_t bp = 0;

    #define wb16(v) do { \
        if (bp + 2 >= sizeof(body)) goto ipc_fail; \
        body[bp++] = (uint8_t)((v) >> 8); \
        body[bp++] = (uint8_t)((v)); \
    } while (0)

    const char *srv = c->server;
    if (srv == NULL) srv = "";

    /* === Fixed prefix (66 bytes) === */

    /* Server identifier (16 bytes): "on" at offset 6, left-aligned */
    wb16(0x006D); wb16(0x0000); wb16(0x003D);
    if (bp + 10 >= sizeof(body)) goto ipc_fail;
    body[bp++] = 'o'; body[bp++] = 'n';
    memset(body + bp, 0, 8); bp += 8;

    /* Transport (16 bytes): tag + "ipcstr" + pad + value */
    wb16(0x003D);
    if (bp + 14 >= sizeof(body)) goto ipc_fail;
    memcpy(body + bp, "ipcstr", 6); bp += 6;
    memset(body + bp, 0, 4); bp += 4;
    wb16(0x0001); wb16(0x0000);

    /* Version (6 bytes) */
    wb16(0x013C); wb16(0x0000); wb16(0x0000);

    /* Product (16 bytes): 4*NUL + "sqlexec" + 5*NUL */
    if (bp + 16 >= sizeof(body)) goto ipc_fail;
    memset(body + bp, 0, 4); bp += 4;
    memcpy(body + bp, "sqlexec", 7); bp += 7;
    memset(body + bp, 0, 5); bp += 5;

    /* App ID (5 bytes): len(2)=5 + "sqli" + NUL */
    wb16(0x0005);
    if (bp + 4 >= sizeof(body)) goto ipc_fail;
    memcpy(body + bp, "sqli", 4); bp += 4;
    body[bp++] = 0;

    /* Capabilities (6 bytes) */
    wb16(0x0006); wb16(0x0000); wb16(0x0003);

    /* === Variable fields === */

    /* Server name: len(2) + name + NUL (len includes NUL) */
    {
        size_t sl = strlen(srv);
        wb16((uint16_t)(sl + 1));
        if (bp + sl >= sizeof(body)) goto ipc_fail;
        memcpy(body + bp, srv, sl); bp += sl;
        body[bp++] = 0;
    }

    /* Per dbaccess trace: tag 0x006b + zero/pid/zero words. */
    wb16(0x006B);
    if (bp + 12 >= sizeof(body)) goto ipc_fail;
    memset(body + bp, 0, 4); bp += 4;
    pid_t pid_val = getpid();
    body[bp++] = (uint8_t)(pid_val >> 24);
    body[bp++] = (uint8_t)(pid_val >> 16);
    body[bp++] = (uint8_t)(pid_val >> 8);
    body[bp++] = (uint8_t)pid_val;
    memset(body + bp, 0, 4); bp += 4;

    /* Hostname: len(2) + name + NUL + two trailing NUL bytes observed in dbaccess */
    {
        char hostname[256];
        hostname[0] = '\0';
        gethostname(hostname, sizeof(hostname) - 1);
        wb16(0x0011);
        if (bp + strlen(hostname) + 2 >= sizeof(body)) goto ipc_fail;
        memcpy(body + bp, hostname, strlen(hostname)); bp += strlen(hostname);
        body[bp++] = 0;
        body[bp++] = 0;
        body[bp++] = 0;
    }

    /* CWD: len(2) + path + NUL (len includes NUL) */
    {
        char cwd[512];
        cwd[0] = '\0';
        if (legacy_style) {
            if (getcwd(cwd, sizeof(cwd) - 1) == NULL) {
                strcpy(cwd, ".");
            }
        } else {
            struct passwd *pw = getpwuid(getuid());
            if (pw != NULL && pw->pw_dir != NULL && pw->pw_dir[0] != '\0') {
                strncpy(cwd, pw->pw_dir, sizeof(cwd) - 1);
                cwd[sizeof(cwd) - 1] = '\0';
            } else if (getcwd(cwd, sizeof(cwd) - 1) == NULL) {
                strcpy(cwd, ".");
            }
        }
        size_t cl = strlen(cwd);
        wb16((uint16_t)(cl + 1));
        if (bp + cl >= sizeof(body)) goto ipc_fail;
        memcpy(body + bp, cwd, cl); bp += cl;
        body[bp++] = 0;
    }

    /* dbaccess sends constant value 2 in tag 0x006e. */
    wb16(0x006E); wb16(0x0004);
    if (bp + 4 >= sizeof(body)) goto ipc_fail;
    body[bp++] = 0;
    body[bp++] = 0;
    body[bp++] = 0;
    body[bp++] = 2;

    /* TID: tag(2) + tid(2) + pad(2) */
    wb16(0x0074); wb16(0x0034); wb16(0x0000);

    /* Legacy and modern dbaccess traces use different tags/values here. */
    wb16(legacy_style ? 0x03E8 : 0x03E7);
    if (bp + 4 >= sizeof(body)) goto ipc_fail;
    body[bp++] = 0; body[bp++] = 0;
    if (legacy_style) {
        body[bp++] = 0x03;
        body[bp++] = 0xE8;
    } else {
        body[bp++] = 3;
        body[bp++] = 0xDD;
    }

    /* App path: tag(2) + path + NUL */
    {
        char app_path_buf[512] = "/usr/lib/informix/current/bin/dbaccess";
        if (access(app_path_buf, X_OK) != 0) {
            ssize_t link_len = readlink("/proc/self/exe", app_path_buf, sizeof(app_path_buf) - 1);
            if (link_len > 0) {
                app_path_buf[link_len] = '\0';
            }
        } else {
            char resolved[512];
            if (realpath(app_path_buf, resolved) != NULL) {
                strncpy(app_path_buf, resolved, sizeof(app_path_buf) - 1);
                app_path_buf[sizeof(app_path_buf) - 1] = '\0';
            }
        }
        size_t al = strlen(app_path_buf);
        wb16(0x002A);
        if (bp + al + 1 >= sizeof(body)) goto ipc_fail;
        memcpy(body + bp, app_path_buf, al); bp += al;
        body[bp++] = 0;
    }

    /* End marker */
    wb16(0x007F);

    #undef wb16

    /* Base64-encode the binary body into the main buffer */
    {
        size_t b64_len = sqli_b64_encode(body, bp, (char *)buf + p, buf_size - p);
        if (b64_len == 0) return 0;
        p += b64_len;
    }

    /* NUL terminator */
    if (p + 1 >= buf_size) return 0;
    buf[p++] = 0;

    /* Generate the magic header dynamically based on the actual length */
    {
        size_t rest_len = p - 4; /* Preamble length minus the 4-byte prefix */
        uint8_t header[6];
        header[0] = (uint8_t)((rest_len >> 8) & 0xFF);
        header[1] = (uint8_t)(rest_len & 0xFF);
        header[2] = 0x01;
        header[3] = 0x3D;
        header[4] = 0x00;
        header[5] = 0x00;

        buf[0] = 's';
        buf[1] = 'q';
        size_t h_len = sqli_b64_encode(header, 6, (char *)buf + 2, 9);
        if (h_len != 8) return 0;
    }

    return p;

ipc_fail:
    return 0;
}
