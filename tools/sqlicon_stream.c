#include "sqlicon.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------- */
/* Helper: append text to a growable buffer                         */
/* ---------------------------------------------------------------- */

static int append_text(char **buf, size_t *len, size_t *cap, const char *src, size_t src_len)
{
    if (*len > SIZE_MAX - src_len - 1)
        return -1;
    size_t need = *len + src_len + 1;
    if (need > *cap) {
        size_t new_cap = (*cap == 0) ? 512 : *cap;
        while (new_cap < need) {
            if (new_cap > SIZE_MAX / 2)
                return -1;
            new_cap *= 2;
        }
        char *grown = realloc(*buf, new_cap);
        if (grown == NULL)
            return -1;
        *buf = grown;
        *cap = new_cap;
    }
    memcpy(*buf + *len, src, src_len);
    *len += src_len;
    (*buf)[*len] = '\0';
    return 0;
}

/* ---------------------------------------------------------------- */
/* Helper: ensure a directory exists (like mkdir -p)                */
/* ---------------------------------------------------------------- */

static int ensure_dir(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    return mkdir(tmp, 0700);
}

/* ---------------------------------------------------------------- */
/* Helper: load/save history                                        */
/* ---------------------------------------------------------------- */

static void load_history(void)
{
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/sqlicon/history", home);
    linenoiseHistoryLoad(path);
}

static void save_history(void)
{
    const char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/sqlicon/history", home);
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/sqlicon", home);
    ensure_dir(dir);
    linenoiseHistorySave(path);
}

/* ---------------------------------------------------------------- */
/* Execute pending statements (split on ';')                        */
/* ---------------------------------------------------------------- */

static sqlicon_exit_code execute_pending_statements(sqli_conn_t *conn, char **pending,
                                                    size_t *pending_len, bool interactive,
                                                    sqlicon_runtime *rt)
{
    if (*pending != NULL && *pending_len > 0)
        discard_leading_whitespace(*pending, pending_len);

    for (;;) {
        char *semi = strchr(*pending, ';');
        if (semi == NULL)
            return SQLICON_EXIT_OK;

        size_t raw_len = (size_t)(semi - *pending);
        size_t trimmed_len = 0;
        const char *trimmed = trim_span(*pending, raw_len, &trimmed_len);

        if (trimmed_len > 0) {
            char *sql = dup_span(trimmed, trimmed_len);
            if (sql == NULL)
                return SQLICON_EXIT_SQL_ERROR;
            sqlicon_exit_code stmt_rc = execute_sql_statement(conn, sql, false, interactive, rt);
            free(sql);
            size_t consumed = raw_len + 1;
            size_t remain = *pending_len - consumed;
            memmove(*pending, *pending + consumed, remain);
            *pending_len = remain;
            (*pending)[*pending_len] = '\0';
            if (*pending_len > 0)
                discard_leading_whitespace(*pending, pending_len);
            if (stmt_rc != SQLICON_EXIT_OK)
                return stmt_rc;
            continue;
        }

        size_t consumed = raw_len + 1;
        size_t remain = *pending_len - consumed;
        memmove(*pending, *pending + consumed, remain);
        *pending_len = remain;
        (*pending)[*pending_len] = '\0';
        if (*pending_len > 0)
            discard_leading_whitespace(*pending, pending_len);
    }
}

/* ---------------------------------------------------------------- */
/* Stream processor (REPL / script / stdin batch)                   */
/* ---------------------------------------------------------------- */

sqlicon_exit_code execute_stream(sqli_conn_t *conn, FILE *in, bool interactive,
                                 sqlicon_runtime *rt)
{
    char *pending = NULL;
    size_t pending_len = 0;
    size_t pending_cap = 0;
    sqlicon_exit_code rc = SQLICON_EXIT_OK;

    if (interactive) {
        linenoiseSetMultiLine(1);
        linenoiseSetCompletionCallback(NULL);
        load_history();
    }

    for (;;) {
        if (interactive && g_exit_requested) {
            rc = SQLICON_EXIT_OK;
            break;
        }

        char *line = NULL;
        if (interactive) {
            line = linenoise((pending_len == 0) ? "sqlicon> " : "...> ");
            if (line == NULL) {
                if (errno == EAGAIN) {
                    /* Ctrl+C */
                    fputc('\n', stdout);
                    if (pending_len > 0) {
                        pending_len = 0;
                        if (pending) pending[0] = '\0';
                    }
                    continue;
                }
                /* Ctrl+D or error */
                fputc('\n', stdout);
                break;
            }
        } else {
            /* Non-interactive: read from FILE* */
            char buf[4096];
            if (fgets(buf, sizeof(buf), in) == NULL) {
                if (ferror(in) != 0 && errno == EINTR) {
                    clearerr(in);
                    continue;
                }
                break;
            }
            line = strdup(buf);
            if (line == NULL) {
                rc = SQLICON_EXIT_SQL_ERROR;
                break;
            }
        }

        if (interactive && strlen(line) > 0) {
            linenoiseHistoryAdd(line);
        }

        if (pending != NULL && pending_len > 0)
            discard_leading_whitespace(pending, &pending_len);

        if (pending_len == 0) {
            const char *trimmed = skip_spaces_str(line);
            if (trimmed[0] == '.') {
                bool should_break = false;
                rc = handle_dot_command(conn, rt, trimmed, &should_break);
                if (rc != SQLICON_EXIT_OK) {
                    if (rt->bail_on_error)
                        break;
                    rc = SQLICON_EXIT_OK;
                }
                if (should_break)
                    break;
                free(line);
                continue;
            }
        }

        if (append_text(&pending, &pending_len, &pending_cap, line, strlen(line)) != 0) {
            rc = SQLICON_EXIT_SQL_ERROR;
            free(line);
            break;
        }
        free(line);

        rc = execute_pending_statements(conn, &pending, &pending_len, interactive, rt);
        if (rc != SQLICON_EXIT_OK) {
            if (rt->bail_on_error)
                break;
            rc = SQLICON_EXIT_OK;
            continue;
        }
    }

    if (rc == SQLICON_EXIT_OK) {
        size_t trimmed_len = 0;
        const char *trimmed = trim_span(pending != NULL ? pending : "", pending_len, &trimmed_len);
        if (trimmed_len > 0 && !interactive) {
            char *sql = dup_span(trimmed, trimmed_len);
            if (sql == NULL) {
                rc = SQLICON_EXIT_SQL_ERROR;
            } else {
                rc = execute_sql_statement(conn, sql, false, false, rt);
                free(sql);
                if (rc != SQLICON_EXIT_OK && !rt->bail_on_error) {
                    rc = SQLICON_EXIT_OK;
                }
            }
        } else if (trimmed_len > 0 && interactive) {
            fprintf(stderr, "warning: discarding incomplete statement\n");
        }
    }

    free(pending);

    if (interactive)
        save_history();

    return rc;
}

/* ---------------------------------------------------------------- */
/* Mode dispatcher                                                  */
/* ---------------------------------------------------------------- */

sqlicon_exit_code run_mode(sqlicon_mode mode, const sqlicon_cli_options *opt,
                           sqli_conn_t *conn, sqlicon_runtime *rt)
{
    switch (mode) {
    case SQLICON_MODE_INTERACTIVE:
        rt->bail_on_error = false;
        return execute_stream(conn, stdin, true, rt);
    case SQLICON_MODE_INLINE_QUERY:
        return execute_sql_statement(conn, opt->inline_query, true, false, rt);
    case SQLICON_MODE_STDIN_BATCH:
        return execute_stream(conn, stdin, false, rt);
    case SQLICON_MODE_SCRIPT: {
        FILE *fp = fopen(opt->script_path, "r");
        if (fp == NULL) {
            fprintf(stderr, "error: cannot open script file '%s'\n", opt->script_path);
            return SQLICON_EXIT_MISUSE;
        }
        sqlicon_exit_code rc = execute_stream(conn, fp, false, rt);
        fclose(fp);
        return rc;
    }
    }
    return SQLICON_EXIT_MISUSE;
}
