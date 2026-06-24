#ifndef SQLICON_H
#define SQLICON_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <signal.h>

#include "libsqli/sqli.h"
#include "linenoise.h"

/* ---------------------------------------------------------------- */
/* Exit codes                                                       */
/* ---------------------------------------------------------------- */

typedef enum {
    SQLICON_EXIT_OK = 0,
    SQLICON_EXIT_SQL_ERROR = 1,
    SQLICON_EXIT_CONNECTION_ERROR = 2,
    SQLICON_EXIT_MISUSE = 3,
    SQLICON_EXIT_PROFILE_EXISTS = 4,
    SQLICON_EXIT_PROFILE_NOT_FOUND = 5,
    SQLICON_EXIT_PROFILE_DECRYPT_FAIL = 6
} sqlicon_exit_code;

/* ---------------------------------------------------------------- */
/* Modes                                                            */
/* ---------------------------------------------------------------- */

typedef enum {
    SQLICON_MODE_INTERACTIVE = 0,
    SQLICON_MODE_INLINE_QUERY = 1,
    SQLICON_MODE_STDIN_BATCH = 2,
    SQLICON_MODE_SCRIPT = 3
} sqlicon_mode;

typedef enum {
    SQLICON_OUTPUT_ALIGNED = 0,
    SQLICON_OUTPUT_CSV = 1,
    SQLICON_OUTPUT_LINE = 2,
    SQLICON_OUTPUT_JSON = 3,
    SQLICON_OUTPUT_MARKDOWN = 4
} sqlicon_output_mode;

/* ---------------------------------------------------------------- */
/* CLI options                                                      */
/* ---------------------------------------------------------------- */

typedef struct {
    bool show_help;
    bool show_profile_secret;
    bool confirm_delete;
    const char *inline_query;
    const char *script_path;
    const char *profile_name;
    const char *profile_create;
    const char *profile_show;
    const char *profile_update;
    const char *profile_delete;
    const char *profile_set_default;
    const char *profile_test;
    bool profile_list;
    const char *host;
    const char *port;
    const char *server;
    const char *database;
    const char *user;
    const char *password;
    const char *client_locale;
    const char *db_locale;
    const char *conn_uri;
} sqlicon_cli_options;

/* ---------------------------------------------------------------- */
/* Profile                                                          */
/* ---------------------------------------------------------------- */

typedef struct {
    char *name;
    char *host;
    char *port;
    char *server;
    char *database;
    char *user;
    char *password;
    char *client_locale;
    char *db_locale;
} sqlicon_profile;

typedef struct {
    sqlicon_profile *items;
    size_t count;
    size_t cap;
    char *default_name;
} sqlicon_profile_store;

typedef struct {
    char *host;
    char *port;
    char *server;
    char *database;
    char *user;
    char *password;
    char *client_locale;
    char *db_locale;
} sqlicon_profile_override;

enum {
    SQLICON_PROFILE_LOAD_OK = 0,
    SQLICON_PROFILE_LOAD_ERROR = -1,
    SQLICON_PROFILE_LOAD_DECRYPT_FAIL = -2
};

/* ---------------------------------------------------------------- */
/* Runtime state                                                    */
/* ---------------------------------------------------------------- */

typedef struct {
    sqlicon_output_mode mode;
    bool headers;
    bool bail_on_error;
    bool timer_on;
    int read_depth;
    char null_repr[64];
    FILE *out;
    bool out_owned;
    char out_path[512];
    char once_path[512];
    int width_overrides[256];
    char conn_host[128];
    char conn_port[32];
    char conn_server[128];
    char conn_database[128];
    char conn_user[128];
    char conn_client_locale[64];
    char conn_db_locale[64];
    char conn_profile[128];
} sqlicon_runtime;

/* ---------------------------------------------------------------- */
/* Table buffer                                                     */
/* ---------------------------------------------------------------- */

typedef struct {
    char **cells;
    bool *is_null;
} sqlicon_row_data;

typedef struct {
    int cols;
    int rows;
    int cap;
    sqlicon_row_data *data;
    size_t *widths;
    char **col_names;
} sqlicon_table_buffer;

/* ---------------------------------------------------------------- */
/* Signal state (defined in cli.c)                                  */
/* ---------------------------------------------------------------- */

extern volatile sig_atomic_t g_query_active;
extern volatile sig_atomic_t g_sigint_during_query;
extern volatile sig_atomic_t g_sigint_idle_count;
extern volatile sig_atomic_t g_exit_requested;

/* ---------------------------------------------------------------- */
/* Inline utilities                                                 */
/* ---------------------------------------------------------------- */

static inline bool is_space_char(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static inline const char *skip_spaces_str(const char *s)
{
    while (*s != '\0' && is_space_char(*s))
        s++;
    return s;
}

static inline void strip_trailing_inplace(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && is_space_char(s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

static inline const char *first_nonempty(const char *a, const char *b)
{
    if (a != NULL && a[0] != '\0')
        return a;
    if (b != NULL && b[0] != '\0')
        return b;
    return NULL;
}

static inline char *dup_span(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (p == NULL)
        return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static inline const char *trim_span(const char *s, size_t len, size_t *out_len)
{
    size_t start = 0;
    while (start < len && is_space_char(s[start]))
        start++;
    size_t end = len;
    while (end > start && is_space_char(s[end - 1]))
        end--;
    *out_len = end - start;
    return s + start;
}

static inline void discard_leading_whitespace(char *buf, size_t *len)
{
    size_t i = 0;
    while (i < *len && is_space_char(buf[i]))
        i++;
    if (i == 0)
        return;
    if (i >= *len) {
        *len = 0;
        buf[0] = '\0';
        return;
    }
    memmove(buf, buf + i, *len - i);
    *len -= i;
    buf[*len] = '\0';
}

static inline bool str_case_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return false;
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static inline bool is_simple_sql_identifier(const char *ident)
{
    if (ident == NULL || ident[0] == '\0')
        return false;
    char c0 = ident[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_'))
        return false;
    for (const char *p = ident + 1; *p != '\0'; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

static inline bool dump_fetch_lob_enabled(void)
{
    static bool initialized = false;
    static bool enabled = false;
    if (!initialized) {
        const char *v = getenv("SQLICON_DUMP_FETCHLOB");
        if (v != NULL &&
            (str_case_eq(v, "1") || str_case_eq(v, "true") ||
             str_case_eq(v, "yes") || str_case_eq(v, "on"))) {
            enabled = true;
        }
        initialized = true;
    }
    return enabled;
}

/* ---------------------------------------------------------------- */
/* Function prototypes                                              */
/* ---------------------------------------------------------------- */

/* cli.c */
void print_help(FILE *out);
sqlicon_exit_code parse_args(int argc, char **argv, sqlicon_cli_options *opt);
void apply_environment(sqlicon_cli_options *opt);
sqlicon_exit_code validate_connection_options(const sqlicon_cli_options *opt);
sqlicon_mode select_mode(const sqlicon_cli_options *opt);
int sqlicon_install_signal_handlers(void);
void sqlicon_reset_interrupt_state(void);

/* profile.c */
void profile_store_init(sqlicon_profile_store *store);
void profile_store_destroy(sqlicon_profile_store *store);
sqlicon_profile *profile_store_find(sqlicon_profile_store *store, const char *name);
sqlicon_profile *profile_store_add(sqlicon_profile_store *store, const char *name);
void profile_override_init(sqlicon_profile_override *ov);
void profile_override_destroy(sqlicon_profile_override *ov);
sqlicon_exit_code maybe_load_profile_for_connect(sqlicon_cli_options *opt, sqlicon_profile_override *ov);
bool has_profile_store_action(const sqlicon_cli_options *opt);
sqlicon_exit_code run_profile_action(const sqlicon_cli_options *opt);

/* crypto.c */
int derive_profile_key_v1(unsigned char key_out[32]);
int encrypt_profile_secret(const char *plain, char **out_b64);
int decrypt_profile_secret(const char *b64, char **out_plain);

/* runtime.c */
void runtime_init(sqlicon_runtime *rt);
void runtime_destroy(sqlicon_runtime *rt);
void runtime_set_connection_metadata(sqlicon_runtime *rt, const sqlicon_cli_options *opt);
const char *output_mode_name(sqlicon_output_mode mode);
void runtime_close_output(sqlicon_runtime *rt);
int runtime_open_persistent_output(sqlicon_runtime *rt, const char *path);
FILE *runtime_acquire_output(sqlicon_runtime *rt, bool *close_after);
void runtime_release_output(FILE *out, bool close_after);
sqlicon_exit_code open_connection(const sqlicon_cli_options *opt, sqli_conn_t **out_conn);

/* table.c */
const char *result_text_by_type(sqli_result_t *result, int col_index, int ctype);
void table_buffer_init(sqlicon_table_buffer *tb, int cols);
void table_buffer_destroy(sqlicon_table_buffer *tb);
void table_buffer_collect(sqlicon_table_buffer *tb, sqli_result_t *result, const sqlicon_runtime *rt);

/* output.c */
void print_result_rows(FILE *out, const sqlicon_runtime *rt, sqli_result_t *result);

/* exec.c */
sqlicon_exit_code execute_sql_statement(sqli_conn_t *conn, const char *sql,
                                        bool noisy, bool interactive,
                                        sqlicon_runtime *rt);

/* commands.c */
sqlicon_exit_code command_width(sqlicon_runtime *rt, const char *arg);
sqlicon_exit_code command_use_database(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg);
sqlicon_exit_code command_list_databases(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg);
sqlicon_exit_code command_status(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg);
sqlicon_exit_code command_schema_table(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg);
sqlicon_exit_code command_indexes_table(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg);
sqlicon_exit_code command_views_table(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg);
sqlicon_exit_code command_constraints_table(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg);
sqlicon_exit_code command_dump_table(sqli_conn_t *conn, sqlicon_runtime *rt, const char *arg);
sqlicon_exit_code command_import_csv(sqli_conn_t *conn, const char *arg);

/* dotcmd.c */
sqlicon_exit_code handle_dot_command(sqli_conn_t *conn, sqlicon_runtime *rt,
                                     const char *line, bool *out_break_loop);

/* stream.c */
sqlicon_exit_code execute_stream(sqli_conn_t *conn, FILE *in, bool interactive,
                                 sqlicon_runtime *rt);
sqlicon_exit_code run_mode(sqlicon_mode mode, const sqlicon_cli_options *opt,
                           sqli_conn_t *conn, sqlicon_runtime *rt);

#endif /* SQLICON_H */
