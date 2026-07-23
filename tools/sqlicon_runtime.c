#include "sqlicon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool uri_has_param(const char *uri, const char *key)
{
    const char *query;
    size_t key_len;

    if (uri == NULL || key == NULL)
        return false;

    query = strchr(uri, '?');
    if (query == NULL)
        return false;
    query++;
    key_len = strlen(key);

    while (*query != '\0') {
        if ((query == uri || query[-1] == '?' || query[-1] == '&') &&
            strncmp(query, key, key_len) == 0 && query[key_len] == '=')
            return true;
        query++;
    }
    return false;
}

static char *build_effective_conn_uri(const sqlicon_cli_options *opt)
{
    const char *base;
    const char *client_locale;
    const char *db_locale;
    bool need_client;
    bool need_db;
    char *uri;
    size_t cap;

    if (opt == NULL || opt->conn_uri == NULL || opt->conn_uri[0] == '\0')
        return NULL;

    base = opt->conn_uri;
    client_locale = opt->client_locale;
    db_locale = opt->db_locale;
    need_client = client_locale != NULL && client_locale[0] != '\0' &&
                  !uri_has_param(base, "CLIENT_LOCALE");
    need_db = db_locale != NULL && db_locale[0] != '\0' &&
              !uri_has_param(base, "DB_LOCALE");
    if (!need_client && !need_db)
        return NULL;

    cap = strlen(base) + 1u;
    if (need_client)
        cap += strlen("&CLIENT_LOCALE=") + strlen(client_locale);
    if (need_db)
        cap += strlen("&DB_LOCALE=") + strlen(db_locale);

    uri = malloc(cap);
    if (uri == NULL)
        return NULL;

    snprintf(uri, cap, "%s", base);
    char *out = uri + strlen(uri);
    char sep = (strchr(base, '?') != NULL) ? '&' : '?';

    if (need_client) {
        out += snprintf(out, cap - (size_t)(out - uri),
                        "%cCLIENT_LOCALE=%s", sep, client_locale);
        sep = '&';
    }
    if (need_db) {
        out += snprintf(out, cap - (size_t)(out - uri),
                        "%cDB_LOCALE=%s", sep, db_locale);
    }

    return uri;
}

/* ---------------------------------------------------------------- */
/* Runtime state                                                    */
/* ---------------------------------------------------------------- */

void runtime_init(sqlicon_runtime *rt)
{
    memset(rt, 0, sizeof(*rt));
    rt->mode = SQLICON_OUTPUT_ALIGNED;
    rt->headers = true;
    rt->bail_on_error = true;
    rt->timer_on = false;
    rt->read_depth = 0;
    snprintf(rt->null_repr, sizeof(rt->null_repr), "NULL");
    rt->out = stdout;
}

void runtime_destroy(sqlicon_runtime *rt)
{
    runtime_close_output(rt);
}

void runtime_set_connection_metadata(sqlicon_runtime *rt, const sqlicon_cli_options *opt)
{
    snprintf(rt->conn_host, sizeof(rt->conn_host), "%s", opt->host ? opt->host : "");
    snprintf(rt->conn_port, sizeof(rt->conn_port), "%s", opt->port ? opt->port : "");
    snprintf(rt->conn_server, sizeof(rt->conn_server), "%s", opt->server ? opt->server : "");
    snprintf(rt->conn_database, sizeof(rt->conn_database), "%s", opt->database ? opt->database : "");
    snprintf(rt->conn_user, sizeof(rt->conn_user), "%s", opt->user ? opt->user : "");
    snprintf(rt->conn_client_locale, sizeof(rt->conn_client_locale), "%s",
             opt->client_locale ? opt->client_locale : "");
    snprintf(rt->conn_db_locale, sizeof(rt->conn_db_locale), "%s",
             opt->db_locale ? opt->db_locale : "");
    snprintf(rt->conn_profile, sizeof(rt->conn_profile), "%s",
             opt->profile_name ? opt->profile_name : "");
}

const char *output_mode_name(sqlicon_output_mode mode)
{
    switch (mode) {
    case SQLICON_OUTPUT_ALIGNED:
        return "aligned";
    case SQLICON_OUTPUT_CSV:
        return "csv";
    case SQLICON_OUTPUT_LINE:
        return "line";
    case SQLICON_OUTPUT_JSON:
        return "json";
    case SQLICON_OUTPUT_MARKDOWN:
        return "markdown";
    default:
        return "unknown";
    }
}

/* ---------------------------------------------------------------- */
/* Output redirection                                               */
/* ---------------------------------------------------------------- */

void runtime_close_output(sqlicon_runtime *rt)
{
    if (rt->out_owned && rt->out != NULL)
        fclose(rt->out);
    rt->out = stdout;
    rt->out_owned = false;
    rt->out_path[0] = '\0';
}

int runtime_open_persistent_output(sqlicon_runtime *rt, const char *path)
{
    FILE *fp = fopen(path, "w");
    if (fp == NULL)
        return -1;
    runtime_close_output(rt);
    rt->out = fp;
    rt->out_owned = true;
    snprintf(rt->out_path, sizeof(rt->out_path), "%s", path);
    return 0;
}

FILE *runtime_acquire_output(sqlicon_runtime *rt, bool *close_after)
{
    *close_after = false;
    if (rt->once_path[0] == '\0')
        return rt->out != NULL ? rt->out : stdout;

    FILE *fp = fopen(rt->once_path, "w");
    if (fp == NULL)
        return NULL;
    rt->once_path[0] = '\0';
    *close_after = true;
    return fp;
}

void runtime_release_output(FILE *out, bool close_after)
{
    if (close_after && out != NULL)
        fclose(out);
}

/* ---------------------------------------------------------------- */
/* Connection                                                       */
/* ---------------------------------------------------------------- */

sqlicon_exit_code open_connection(const sqlicon_cli_options *opt, sqli_conn_t **out_conn)
{
    sqli_conn_t *conn = NULL;
    char *effective_uri = NULL;
    sqli_status rc = sqli_create(&conn);
    if (rc != SQLI_OK || conn == NULL) {
        fprintf(stderr, "error: sqli_create failed (%d)\n", (int)rc);
        return SQLICON_EXIT_CONNECTION_ERROR;
    }

    if (opt->conn_uri != NULL && opt->conn_uri[0] != '\0') {
        effective_uri = build_effective_conn_uri(opt);
        rc = sqli_connect_uri(conn,
                              effective_uri != NULL ? effective_uri : opt->conn_uri,
                              opt->user, opt->password);
    } else {
        sqli_connect_params p;
        memset(&p, 0, sizeof(p));
        p.hostname = opt->host;
        p.service = opt->port;
        p.server = (opt->server != NULL) ? opt->server : "";
        p.database = opt->database;
        p.username = opt->user;
        p.password = opt->password;
        p.client_locale = (opt->client_locale != NULL) ? opt->client_locale : "en_US.utf8";
        p.db_locale = (opt->db_locale != NULL) ? opt->db_locale : "en_US.8859-1";

        rc = sqli_connect(conn, &p);
    }

    if (rc != SQLI_OK) {
        fprintf(stderr, "error: connect failed (%d): %s\n",
                (int)rc, sqli_error(conn) ? sqli_error(conn) : "(unknown)");
        free(effective_uri);
        sqli_destroy(conn);
        return SQLICON_EXIT_CONNECTION_ERROR;
    }

    free(effective_uri);
    *out_conn = conn;
    return SQLICON_EXIT_OK;
}
