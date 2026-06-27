#include "sqlicon.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>

/* ---------------------------------------------------------------- */
/* Static helpers                                                   */
/* ---------------------------------------------------------------- */

static bool is_valid_profile_name(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return false;
    for (const char *p = name; *p != '\0'; p++) {
        if ((*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') ||
            *p == '_' || *p == '-' || *p == '@' || *p == '.') {
            continue;
        }
        return false;
    }
    return true;
}

static bool is_numeric_port(const char *port)
{
    if (port == NULL || *port == '\0')
        return true;
    for (const char *p = port; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return false;
    }
    return true;
}

static int set_dup_field(char **dst, const char *src)
{
    char *copy = NULL;
    if (src != NULL) {
        copy = strdup(src);
        if (copy == NULL)
            return -1;
    }
    free(*dst);
    *dst = copy;
    return 0;
}

static int url_decode_dup(const char *src, size_t len, char **out)
{
    char *buf = NULL;
    size_t r;
    size_t w = 0;

    if (out == NULL)
        return -1;
    *out = NULL;

    buf = malloc(len + 1);
    if (buf == NULL)
        return -1;

    for (r = 0; r < len; r++) {
        if (src[r] == '%' && r + 2 < len &&
            isxdigit((unsigned char)src[r + 1]) &&
            isxdigit((unsigned char)src[r + 2])) {
            char hex[3];
            hex[0] = src[r + 1];
            hex[1] = src[r + 2];
            hex[2] = '\0';
            buf[w++] = (char)strtol(hex, NULL, 16);
            r += 2;
            continue;
        }
        if (src[r] == '+') {
            buf[w++] = ' ';
            continue;
        }
        buf[w++] = src[r];
    }

    buf[w] = '\0';
    *out = buf;
    return 0;
}

static int set_profile_field_from_span(char **dst, const char *src, size_t len)
{
    char *decoded = NULL;
    int rc;

    if (src == NULL || len == 0)
        return 0;

    rc = url_decode_dup(src, len, &decoded);
    if (rc != 0)
        return -1;

    rc = set_dup_field(dst, decoded);
    free(decoded);
    return rc;
}

static int merge_profile_from_uri(sqlicon_profile *p, const char *uri)
{
    const char *scheme_end;
    const char *authority;
    const char *path;
    const char *query;
    const char *server_key = "INFORMIXSERVER=";
    const char *client_locale_key = "CLIENT_LOCALE=";
    const char *db_locale_key = "DB_LOCALE=";
    const char *server_pos;
    const char *client_locale_pos;
    const char *db_locale_pos;
    const char *scheme;
    const char *userinfo_sep = NULL;
    const char *host_port;
    const char *host_end;
    const char *port_sep = NULL;
    const char *db_start;
    size_t db_len;

    if (p == NULL || uri == NULL || uri[0] == '\0')
        return 0;

    scheme_end = strstr(uri, "://");
    if (scheme_end == NULL)
        return -1;

    scheme = uri;
    if (!((size_t)(scheme_end - scheme) == strlen("informix+onsoctcp") &&
          strncmp(scheme, "informix+onsoctcp", strlen("informix+onsoctcp")) == 0) &&
        !((size_t)(scheme_end - scheme) == strlen("informix+onsocssl") &&
          strncmp(scheme, "informix+onsocssl", strlen("informix+onsocssl")) == 0) &&
        !((size_t)(scheme_end - scheme) == strlen("informix+onipcstr") &&
          strncmp(scheme, "informix+onipcstr", strlen("informix+onipcstr")) == 0)) {
        return -1;
    }

    authority = scheme_end + 3;
    path = strchr(authority, '/');
    if (path == NULL)
        return -1;

    query = strchr(path, '?');
    db_start = path + 1;
    db_len = (query != NULL) ? (size_t)(query - db_start) : strlen(db_start);
    if (db_len == 0)
        return -1;

    if (set_profile_field_from_span(&p->database, db_start, db_len) != 0)
        return -1;

    if (strncmp(uri, "informix+onipcstr://", strlen("informix+onipcstr://")) != 0) {
        userinfo_sep = memchr(authority, '@', (size_t)(path - authority));
        host_port = authority;
        if (userinfo_sep != NULL) {
            const char *colon = memchr(authority, ':', (size_t)(userinfo_sep - authority));
            if (colon != NULL) {
                if (set_profile_field_from_span(&p->user, authority, (size_t)(colon - authority)) != 0)
                    return -1;
                if (set_profile_field_from_span(&p->password, colon + 1, (size_t)(userinfo_sep - colon - 1)) != 0)
                    return -1;
            } else {
                if (set_profile_field_from_span(&p->user, authority, (size_t)(userinfo_sep - authority)) != 0)
                    return -1;
            }
            host_port = userinfo_sep + 1;
        }

        host_end = path;
        for (const char *scan = host_end; scan > host_port; scan--) {
            if (scan[-1] == ':') {
                port_sep = scan - 1;
                break;
            }
        }
        if (port_sep == NULL || port_sep == host_port || (port_sep + 1) >= host_end)
            return -1;

        if (set_profile_field_from_span(&p->host, host_port, (size_t)(port_sep - host_port)) != 0)
            return -1;
        if (set_profile_field_from_span(&p->port, port_sep + 1, (size_t)(host_end - port_sep - 1)) != 0)
            return -1;
    }

    if (query == NULL || query[1] == '\0')
        return -1;

    server_pos = strstr(query + 1, server_key);
    if (server_pos == NULL)
        return -1;
    server_pos += strlen(server_key);
    {
        const char *server_end = strchr(server_pos, '&');
        size_t server_len = (server_end != NULL) ? (size_t)(server_end - server_pos) : strlen(server_pos);
        if (server_len == 0)
            return -1;
        if (set_profile_field_from_span(&p->server, server_pos, server_len) != 0)
            return -1;
    }

    client_locale_pos = strstr(query + 1, client_locale_key);
    if (client_locale_pos != NULL) {
        const char *value = client_locale_pos + strlen(client_locale_key);
        const char *value_end = strchr(value, '&');
        size_t value_len = (value_end != NULL) ? (size_t)(value_end - value) : strlen(value);
        if (set_profile_field_from_span(&p->client_locale, value, value_len) != 0)
            return -1;
    }

    db_locale_pos = strstr(query + 1, db_locale_key);
    if (db_locale_pos != NULL) {
        const char *value = db_locale_pos + strlen(db_locale_key);
        const char *value_end = strchr(value, '&');
        size_t value_len = (value_end != NULL) ? (size_t)(value_end - value) : strlen(value);
        if (set_profile_field_from_span(&p->db_locale, value, value_len) != 0)
            return -1;
    }

    return 0;
}

static void profile_free_fields(sqlicon_profile *p)
{
    free(p->name);
    free(p->host);
    free(p->port);
    free(p->server);
    free(p->database);
    free(p->user);
    free(p->password);
    free(p->client_locale);
    free(p->db_locale);
    memset(p, 0, sizeof(*p));
}

static int ensure_profile_dir(char *path_out, size_t path_cap, char *file_out, size_t file_cap)
{
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0')
        return -1;

    char config_dir[768];
    if (snprintf(config_dir, sizeof(config_dir), "%s/.config", home) >= (int)sizeof(config_dir))
        return -1;
    if (mkdir(config_dir, 0700) != 0 && errno != EEXIST)
        return -1;

    if (snprintf(path_out, path_cap, "%s/.config/sqlicon", home) >= (int)path_cap)
        return -1;
    if (mkdir(path_out, 0700) != 0 && errno != EEXIST)
        return -1;
    (void)chmod(path_out, 0700);

    if (snprintf(file_out, file_cap, "%s/profiles.conf", path_out) >= (int)file_cap)
        return -1;
    return 0;
}

static int profile_set_field(sqlicon_profile *p, const char *field, const char *value)
{
    if (strcmp(field, "host") == 0)
        return set_dup_field(&p->host, value);
    if (strcmp(field, "port") == 0)
        return set_dup_field(&p->port, value);
    if (strcmp(field, "server") == 0)
        return set_dup_field(&p->server, value);
    if (strcmp(field, "database") == 0)
        return set_dup_field(&p->database, value);
    if (strcmp(field, "user") == 0)
        return set_dup_field(&p->user, value);
    if (strcmp(field, "password") == 0)
        return set_dup_field(&p->password, value);
    if (strcmp(field, "client_locale") == 0)
        return set_dup_field(&p->client_locale, value);
    if (strcmp(field, "db_locale") == 0)
        return set_dup_field(&p->db_locale, value);
    return 0;
}

static int unescape_value_inplace(char *s)
{
    size_t w = 0;
    for (size_t r = 0; s[r] != '\0'; r++) {
        if (s[r] == '\\') {
            r++;
            if (s[r] == '\0')
                return -1;
            switch (s[r]) {
            case 'n':
                s[w++] = '\n';
                break;
            case 'r':
                s[w++] = '\r';
                break;
            case 't':
                s[w++] = '\t';
                break;
            case '\\':
                s[w++] = '\\';
                break;
            default:
                return -1;
            }
        } else {
            s[w++] = s[r];
        }
    }
    s[w] = '\0';
    return 0;
}

static int write_escaped_value(FILE *fp, const char *v)
{
    if (v == NULL)
        return 0;
    for (const char *p = v; *p != '\0'; p++) {
        switch (*p) {
        case '\n':
            if (fputs("\\n", fp) == EOF)
                return -1;
            break;
        case '\r':
            if (fputs("\\r", fp) == EOF)
                return -1;
            break;
        case '\t':
            if (fputs("\\t", fp) == EOF)
                return -1;
            break;
        case '\\':
            if (fputs("\\\\", fp) == EOF)
                return -1;
            break;
        default:
            if (fputc(*p, fp) == EOF)
                return -1;
            break;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Profile store operations                                         */
/* ---------------------------------------------------------------- */

void profile_store_init(sqlicon_profile_store *store)
{
    memset(store, 0, sizeof(*store));
}

void profile_store_destroy(sqlicon_profile_store *store)
{
    if (store == NULL)
        return;
    for (size_t i = 0; i < store->count; i++)
        profile_free_fields(&store->items[i]);
    free(store->items);
    free(store->default_name);
    memset(store, 0, sizeof(*store));
}

sqlicon_profile *profile_store_find(sqlicon_profile_store *store, const char *name)
{
    for (size_t i = 0; i < store->count; i++) {
        if (store->items[i].name != NULL && strcmp(store->items[i].name, name) == 0)
            return &store->items[i];
    }
    return NULL;
}

sqlicon_profile *profile_store_add(sqlicon_profile_store *store, const char *name)
{
    if (store->count == store->cap) {
        size_t new_cap = (store->cap == 0) ? 8 : store->cap * 2;
        sqlicon_profile *grown = realloc(store->items, new_cap * sizeof(*grown));
        if (grown == NULL)
            return NULL;
        store->items = grown;
        store->cap = new_cap;
    }
    sqlicon_profile *p = &store->items[store->count++];
    memset(p, 0, sizeof(*p));
    p->name = strdup(name);
    if (p->name == NULL) {
        store->count--;
        return NULL;
    }
    return p;
}

void profile_override_init(sqlicon_profile_override *ov)
{
    memset(ov, 0, sizeof(*ov));
}

void profile_override_destroy(sqlicon_profile_override *ov)
{
    if (ov == NULL)
        return;
    free(ov->host);
    free(ov->port);
    free(ov->server);
    free(ov->database);
    free(ov->user);
    free(ov->password);
    free(ov->client_locale);
    free(ov->db_locale);
    memset(ov, 0, sizeof(*ov));
}

/* ---------------------------------------------------------------- */
/* Profile file I/O                                                 */
/* ---------------------------------------------------------------- */

static int load_profile_store(sqlicon_profile_store *store)
{
    char dir_path[768];
    char file_path[1024];
    if (ensure_profile_dir(dir_path, sizeof(dir_path), file_path, sizeof(file_path)) != 0)
        return SQLICON_PROFILE_LOAD_ERROR;

    FILE *fp = fopen(file_path, "r");
    if (fp == NULL) {
        if (errno == ENOENT)
            return SQLICON_PROFILE_LOAD_OK;
        return SQLICON_PROFILE_LOAD_ERROR;
    }

    int version = 1;
    char kdf_method[64];
    kdf_method[0] = '\0';
    int decrypt_fail = 0;
    char line[4096];
    while (fgets(line, sizeof(line), fp) != NULL) {
        strip_trailing_inplace(line);
        const char *trimmed = skip_spaces_str(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;
        if (strncmp(trimmed, "version=", 8) == 0) {
            int v = atoi(trimmed + 8);
            if (v > 0)
                version = v;
            continue;
        }
        if (strncmp(trimmed, "kdf_method=", 11) == 0) {
            if (snprintf(kdf_method, sizeof(kdf_method), "%s", trimmed + 11) >= (int)sizeof(kdf_method))
                decrypt_fail = 1;
            continue;
        }
        if (strncmp(trimmed, "default=", 8) == 0) {
            if (set_dup_field(&store->default_name, trimmed + 8) != 0) {
                fclose(fp);
                return SQLICON_PROFILE_LOAD_ERROR;
            }
            continue;
        }
        if (strncmp(trimmed, "profile.", 8) != 0)
            continue;

        const char *name_start = trimmed + 8;
        char *eq = strchr((char *)name_start, '=');
        const char *field_sep = NULL;
        if (eq == NULL)
            continue;
        for (const char *scan = eq; scan > name_start; scan--) {
            if (scan[-1] == '.') {
                field_sep = scan - 1;
                break;
            }
        }
        if (field_sep == NULL)
            continue;
        size_t name_len = (size_t)(field_sep - name_start);
        if (name_len == 0)
            continue;
        char *name = dup_span(name_start, name_len);
        if (name == NULL) {
            fclose(fp);
            return SQLICON_PROFILE_LOAD_ERROR;
        }
        char *field = dup_span(field_sep + 1, (size_t)(eq - (field_sep + 1)));
        char *value = strdup(eq + 1);
        if (field == NULL || value == NULL) {
            free(name);
            free(field);
            free(value);
            fclose(fp);
            return SQLICON_PROFILE_LOAD_ERROR;
        }
        if (unescape_value_inplace(value) != 0) {
            free(name);
            free(field);
            free(value);
            continue;
        }
        sqlicon_profile *p = profile_store_find(store, name);
        if (p == NULL)
            p = profile_store_add(store, name);
        free(name);
        if (p == NULL) {
            free(field);
            free(value);
            fclose(fp);
            return SQLICON_PROFILE_LOAD_ERROR;
        }
        if (strcmp(field, "password_enc") == 0) {
            if (version >= 2 &&
                kdf_method[0] != '\0' &&
                strcmp(kdf_method, "uuid_uid_v1") != 0) {
                decrypt_fail = 1;
                free(field);
                free(value);
                break;
            }
            char *plain = NULL;
            if (decrypt_profile_secret(value, &plain) != 0) {
                decrypt_fail = 1;
                free(field);
                free(value);
                break;
            }
            if (set_dup_field(&p->password, plain) != 0) {
                free(plain);
                free(field);
                free(value);
                fclose(fp);
                return SQLICON_PROFILE_LOAD_ERROR;
            }
            free(plain);
        } else {
            if (profile_set_field(p, field, value) != 0) {
                free(field);
                free(value);
                fclose(fp);
                return SQLICON_PROFILE_LOAD_ERROR;
            }
            if (version >= 2 && strcmp(field, "password") == 0 && value[0] != '\0')
                decrypt_fail = 1;
        }
        free(field);
        free(value);
    }
    fclose(fp);
    if (decrypt_fail)
        return SQLICON_PROFILE_LOAD_DECRYPT_FAIL;
    return SQLICON_PROFILE_LOAD_OK;
}

static int save_profile_store(const sqlicon_profile_store *store)
{
    char dir_path[768];
    char file_path[1024];
    if (ensure_profile_dir(dir_path, sizeof(dir_path), file_path, sizeof(file_path)) != 0)
        return -1;

    char tmp_path[1100];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", file_path) >= (int)sizeof(tmp_path))
        return -1;

    FILE *fp = fopen(tmp_path, "w");
    if (fp == NULL)
        return -1;

    if (fputs("version=2\n", fp) == EOF) {
        fclose(fp);
        return -1;
    }
    if (fputs("kdf_method=uuid_uid_v1\n", fp) == EOF ||
        fputs("enc_scheme=aes-256-gcm\n", fp) == EOF ||
        fputs("enc_fields=password\n", fp) == EOF) {
        fclose(fp);
        return -1;
    }
    if (fputs("default=", fp) == EOF ||
        write_escaped_value(fp, store->default_name != NULL ? store->default_name : "") != 0 ||
        fputc('\n', fp) == EOF) {
        fclose(fp);
        return -1;
    }

    for (size_t i = 0; i < store->count; i++) {
        const sqlicon_profile *p = &store->items[i];
#define WRITE_PROFILE_FIELD(field_name, value_ptr) \
        do { \
            if (fprintf(fp, "profile.%s.%s=", p->name, field_name) < 0 || \
                write_escaped_value(fp, value_ptr) != 0 || \
                fputc('\n', fp) == EOF) { \
                fclose(fp); \
                return -1; \
            } \
        } while (0)
        WRITE_PROFILE_FIELD("host", p->host);
        WRITE_PROFILE_FIELD("port", p->port);
        WRITE_PROFILE_FIELD("server", p->server);
        WRITE_PROFILE_FIELD("database", p->database);
        WRITE_PROFILE_FIELD("user", p->user);
        char *password_enc = NULL;
        if (encrypt_profile_secret(p->password, &password_enc) != 0) {
            fclose(fp);
            return -1;
        }
        WRITE_PROFILE_FIELD("password_enc", password_enc);
        free(password_enc);
        WRITE_PROFILE_FIELD("client_locale", p->client_locale);
        WRITE_PROFILE_FIELD("db_locale", p->db_locale);
#undef WRITE_PROFILE_FIELD
    }

    if (fflush(fp) != 0 || fclose(fp) != 0)
        return -1;
    if (chmod(tmp_path, 0600) != 0)
        return -1;
    if (rename(tmp_path, file_path) != 0)
        return -1;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Profile display                                                  */
/* ---------------------------------------------------------------- */

static void print_profile(const sqlicon_profile *p, bool show_secret)
{
    printf("name=%s\n", p->name);
    printf("host=%s\n", p->host != NULL ? p->host : "");
    printf("port=%s\n", p->port != NULL ? p->port : "");
    printf("server=%s\n", p->server != NULL ? p->server : "");
    printf("database=%s\n", p->database != NULL ? p->database : "");
    printf("user=%s\n", p->user != NULL ? p->user : "");
    if (show_secret)
        printf("password=%s\n", p->password != NULL ? p->password : "");
    else
        printf("password=%s\n", p->password != NULL && p->password[0] != '\0' ? "***" : "");
    printf("client_locale=%s\n", p->client_locale != NULL ? p->client_locale : "");
    printf("db_locale=%s\n", p->db_locale != NULL ? p->db_locale : "");
}

/* ---------------------------------------------------------------- */
/* Profile merge / action helpers                                   */
/* ---------------------------------------------------------------- */

static int merge_profile_from_options(sqlicon_profile *p, const sqlicon_cli_options *opt,
                                      bool require_all_core)
{
    sqlicon_profile uri_profile;

    memset(&uri_profile, 0, sizeof(uri_profile));
    if (opt->conn_uri != NULL && opt->conn_uri[0] != '\0') {
        if (merge_profile_from_uri(&uri_profile, opt->conn_uri) != 0) {
            profile_free_fields(&uri_profile);
            return -4;
        }
        if (uri_profile.host != NULL && set_dup_field(&p->host, uri_profile.host) != 0)
            goto oom;
        if (uri_profile.port != NULL && set_dup_field(&p->port, uri_profile.port) != 0)
            goto oom;
        if (uri_profile.server != NULL && set_dup_field(&p->server, uri_profile.server) != 0)
            goto oom;
        if (uri_profile.database != NULL && set_dup_field(&p->database, uri_profile.database) != 0)
            goto oom;
        if (uri_profile.user != NULL && set_dup_field(&p->user, uri_profile.user) != 0)
            goto oom;
        if (uri_profile.password != NULL && set_dup_field(&p->password, uri_profile.password) != 0)
            goto oom;
        if (uri_profile.client_locale != NULL && set_dup_field(&p->client_locale, uri_profile.client_locale) != 0)
            goto oom;
        if (uri_profile.db_locale != NULL && set_dup_field(&p->db_locale, uri_profile.db_locale) != 0)
            goto oom;
    }

    if (opt->host != NULL && set_dup_field(&p->host, opt->host) != 0)
        goto oom;
    if (opt->port != NULL && set_dup_field(&p->port, opt->port) != 0)
        goto oom;
    if (opt->server != NULL && set_dup_field(&p->server, opt->server) != 0)
        goto oom;
    if (opt->database != NULL && set_dup_field(&p->database, opt->database) != 0)
        goto oom;
    if (opt->user != NULL && set_dup_field(&p->user, opt->user) != 0)
        goto oom;
    if (opt->password != NULL && set_dup_field(&p->password, opt->password) != 0)
        goto oom;
    if (opt->client_locale != NULL && set_dup_field(&p->client_locale, opt->client_locale) != 0)
        goto oom;
    if (opt->db_locale != NULL && set_dup_field(&p->db_locale, opt->db_locale) != 0)
        goto oom;

    if (!is_numeric_port(p->port))
        goto invalid_port;
    if (require_all_core &&
        (p->host == NULL || p->port == NULL || p->database == NULL ||
         p->user == NULL || p->password == NULL)) {
        goto missing_core;
    }
    profile_free_fields(&uri_profile);
    return 0;

oom:
    profile_free_fields(&uri_profile);
    return -1;

invalid_port:
    profile_free_fields(&uri_profile);
    return -2;

missing_core:
    profile_free_fields(&uri_profile);
    return -3;
}

/* ---------------------------------------------------------------- */
/* Public API                                                       */
/* ---------------------------------------------------------------- */

sqlicon_exit_code maybe_load_profile_for_connect(sqlicon_cli_options *opt,
                                                 sqlicon_profile_override *ov)
{
    sqlicon_profile_store store;
    profile_store_init(&store);
    int lrc = load_profile_store(&store);
    if (lrc == SQLICON_PROFILE_LOAD_DECRYPT_FAIL) {
        profile_store_destroy(&store);
        fprintf(stderr, "error: profile decryption failed (foreign or tampered profile store)\n");
        return SQLICON_EXIT_PROFILE_DECRYPT_FAIL;
    }
    if (lrc != SQLICON_PROFILE_LOAD_OK) {
        profile_store_destroy(&store);
        fprintf(stderr, "error: failed to load profile store\n");
        return SQLICON_EXIT_MISUSE;
    }

    const char *requested = opt->profile_name;
    if ((requested == NULL || requested[0] == '\0') &&
        store.default_name != NULL && store.default_name[0] != '\0') {
        requested = store.default_name;
    }
    if (requested != NULL && requested[0] != '\0') {
        sqlicon_profile *p = profile_store_find(&store, requested);
        if (p == NULL) {
            fprintf(stderr, "error: profile '%s' not found\n", requested);
            profile_store_destroy(&store);
            return SQLICON_EXIT_PROFILE_NOT_FOUND;
        }
        if (set_dup_field(&ov->host, p->host) != 0 ||
            set_dup_field(&ov->port, p->port) != 0 ||
            set_dup_field(&ov->server, p->server) != 0 ||
            set_dup_field(&ov->database, p->database) != 0 ||
            set_dup_field(&ov->user, p->user) != 0 ||
            set_dup_field(&ov->password, p->password) != 0 ||
            set_dup_field(&ov->client_locale, p->client_locale) != 0 ||
            set_dup_field(&ov->db_locale, p->db_locale) != 0) {
            profile_store_destroy(&store);
            fprintf(stderr, "error: out of memory while loading profile\n");
            return SQLICON_EXIT_MISUSE;
        }
        opt->host = first_nonempty(opt->host, ov->host);
        opt->port = first_nonempty(opt->port, ov->port);
        opt->server = first_nonempty(opt->server, ov->server);
        opt->database = first_nonempty(opt->database, ov->database);
        opt->user = first_nonempty(opt->user, ov->user);
        opt->password = first_nonempty(opt->password, ov->password);
        opt->client_locale = first_nonempty(opt->client_locale, ov->client_locale);
        opt->db_locale = first_nonempty(opt->db_locale, ov->db_locale);
    }

    profile_store_destroy(&store);
    return SQLICON_EXIT_OK;
}

bool has_profile_store_action(const sqlicon_cli_options *opt)
{
    return opt->profile_create != NULL ||
           opt->profile_show != NULL ||
           opt->profile_list ||
           opt->profile_update != NULL ||
           opt->profile_delete != NULL ||
           opt->profile_set_default != NULL;
}

sqlicon_exit_code run_profile_action(const sqlicon_cli_options *opt)
{
    sqlicon_profile_store store;
    profile_store_init(&store);
    int lrc = load_profile_store(&store);
    if (lrc == SQLICON_PROFILE_LOAD_DECRYPT_FAIL) {
        profile_store_destroy(&store);
        fprintf(stderr, "error: profile decryption failed (foreign or tampered profile store)\n");
        return SQLICON_EXIT_PROFILE_DECRYPT_FAIL;
    }
    if (lrc != SQLICON_PROFILE_LOAD_OK) {
        profile_store_destroy(&store);
        fprintf(stderr, "error: failed to load profile store\n");
        return SQLICON_EXIT_MISUSE;
    }

    sqlicon_exit_code rc = SQLICON_EXIT_OK;
    if (opt->profile_list) {
        for (size_t i = 0; i < store.count; i++) {
            const sqlicon_profile *p = &store.items[i];
            bool is_default = (store.default_name != NULL && strcmp(store.default_name, p->name) == 0);
            printf("%s%s\n", p->name, is_default ? " *" : "");
        }
        goto done;
    }

    if (opt->profile_show != NULL) {
        sqlicon_profile *p = profile_store_find(&store, opt->profile_show);
        if (p == NULL) {
            fprintf(stderr, "error: profile '%s' not found\n", opt->profile_show);
            rc = SQLICON_EXIT_PROFILE_NOT_FOUND;
            goto done;
        }
        print_profile(p, opt->show_profile_secret);
        goto done;
    }

    if (opt->profile_create != NULL) {
        if (!is_valid_profile_name(opt->profile_create)) {
            fprintf(stderr, "error: invalid profile name '%s'\n", opt->profile_create);
            rc = SQLICON_EXIT_MISUSE;
            goto done;
        }
        if (profile_store_find(&store, opt->profile_create) != NULL) {
            fprintf(stderr, "error: profile '%s' already exists\n", opt->profile_create);
            rc = SQLICON_EXIT_PROFILE_EXISTS;
            goto done;
        }
        sqlicon_profile *p = profile_store_add(&store, opt->profile_create);
        if (p == NULL) {
            rc = SQLICON_EXIT_MISUSE;
            goto done;
        }
        int mrc = merge_profile_from_options(p, opt, true);
        if (mrc == -2) {
            fprintf(stderr, "error: profile port must be numeric\n");
            rc = SQLICON_EXIT_MISUSE;
            goto done;
        }
        if (mrc == -4) {
            fprintf(stderr, "error: invalid --connect-uri for profile create\n");
            rc = SQLICON_EXIT_MISUSE;
            goto done;
        }
        if (mrc == -3) {
            fprintf(stderr, "error: profile create requires host, port, database, user, password\n");
            rc = SQLICON_EXIT_MISUSE;
            goto done;
        }
        if (mrc != 0 || save_profile_store(&store) != 0) {
            fprintf(stderr, "error: failed to save profile store\n");
            rc = SQLICON_EXIT_MISUSE;
            goto done;
        }
        printf("profile '%s' created\n", opt->profile_create);
        goto done;
    }

    if (opt->profile_update != NULL) {
        sqlicon_profile *p = profile_store_find(&store, opt->profile_update);
        if (p == NULL) {
            fprintf(stderr, "error: profile '%s' not found\n", opt->profile_update);
            rc = SQLICON_EXIT_PROFILE_NOT_FOUND;
            goto done;
        }
        int mrc = merge_profile_from_options(p, opt, false);
        if (mrc == -2) {
            fprintf(stderr, "error: profile port must be numeric\n");
            rc = SQLICON_EXIT_MISUSE;
            goto done;
        }
        if (mrc == -4) {
            fprintf(stderr, "error: invalid --connect-uri for profile update\n");
            rc = SQLICON_EXIT_MISUSE;
            goto done;
        }
        if (mrc != 0 || save_profile_store(&store) != 0) {
            fprintf(stderr, "error: failed to save profile store\n");
            rc = SQLICON_EXIT_MISUSE;
            goto done;
        }
        printf("profile '%s' updated\n", opt->profile_update);
        goto done;
    }

    if (opt->profile_delete != NULL) {
        if (!opt->confirm_delete) {
            fprintf(stderr, "error: --profile-delete requires --yes\n");
            rc = SQLICON_EXIT_MISUSE;
            goto done;
        }
        size_t idx = SIZE_MAX;
        for (size_t i = 0; i < store.count; i++) {
            if (strcmp(store.items[i].name, opt->profile_delete) == 0) {
                idx = i;
                break;
            }
        }
        if (idx == SIZE_MAX) {
            fprintf(stderr, "error: profile '%s' not found\n", opt->profile_delete);
            rc = SQLICON_EXIT_PROFILE_NOT_FOUND;
            goto done;
        }
        profile_free_fields(&store.items[idx]);
        for (size_t i = idx + 1; i < store.count; i++)
            store.items[i - 1] = store.items[i];
        store.count--;
        if (store.default_name != NULL && strcmp(store.default_name, opt->profile_delete) == 0) {
            free(store.default_name);
            store.default_name = NULL;
        }
        if (save_profile_store(&store) != 0) {
            fprintf(stderr, "error: failed to save profile store\n");
            rc = SQLICON_EXIT_MISUSE;
            goto done;
        }
        printf("profile '%s' deleted\n", opt->profile_delete);
        goto done;
    }

    if (opt->profile_set_default != NULL) {
        if (profile_store_find(&store, opt->profile_set_default) == NULL) {
            fprintf(stderr, "error: profile '%s' not found\n", opt->profile_set_default);
            rc = SQLICON_EXIT_PROFILE_NOT_FOUND;
            goto done;
        }
        if (set_dup_field(&store.default_name, opt->profile_set_default) != 0 ||
            save_profile_store(&store) != 0) {
            fprintf(stderr, "error: failed to save profile store\n");
            rc = SQLICON_EXIT_MISUSE;
            goto done;
        }
        printf("default profile set to '%s'\n", opt->profile_set_default);
        goto done;
    }

done:
    profile_store_destroy(&store);
    return rc;
}
