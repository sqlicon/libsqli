#include "sqlicon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    sqli_status rc = sqli_create(&conn);
    if (rc != SQLI_OK || conn == NULL) {
        fprintf(stderr, "error: sqli_create failed (%d)\n", (int)rc);
        return SQLICON_EXIT_CONNECTION_ERROR;
    }

    if (opt->conn_uri != NULL && opt->conn_uri[0] != '\0') {
        rc = sqli_connect_uri(conn, opt->conn_uri, opt->user, opt->password);
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
        sqli_destroy(conn);
        return SQLICON_EXIT_CONNECTION_ERROR;
    }

    *out_conn = conn;
    return SQLICON_EXIT_OK;
}
