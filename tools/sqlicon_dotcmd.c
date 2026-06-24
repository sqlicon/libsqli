#include "sqlicon.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Dot command help                                                 */
/* ---------------------------------------------------------------- */

static void print_dot_help(FILE *out)
{
    fprintf(out,
            "Dot commands:\n"
            "  .help                 Show this help\n"
            "  .quit | .exit         Exit shell\n"
            "  .mode MODE            Set output mode: aligned|csv|line|json|markdown\n"
            "  .width IDX WIDTH      Set minimum display width for column IDX in aligned mode\n"
            "  .width off            Reset all width overrides\n"
            "  .headers on|off       Toggle headers\n"
            "  .bail on|off          Stop or continue on SQL errors\n"
            "  .timer on|off         Show statement runtime on stderr\n"
            "  .nullvalue TEXT       Set NULL display text\n"
            "  .output FILE|stdout   Redirect output persistently\n"
            "  .once FILE            Redirect next statement output once\n"
            "  .read FILE            Execute SQL/dot commands from script\n"
            "  .explain on|off       Execute SET EXPLAIN ON/OFF\n"
            "  .use DATABASE         Switch active database in current session\n"
            "  .databases            List available databases\n"
            "  .status               Show current session/database metadata\n"
            "  .schema TABLE         Show table columns with SQL type names\n"
            "  .indexes TABLE        Show table index metadata with columns\n"
            "  .views [TABLE]        Show view definition(s)\n"
            "  .constraints TABLE    Show table constraints (PK/FK/UNIQUE/CHECK)\n"
            "  .dump [TABLE]         Export table rows as INSERT statements\n"
            "  .import FILE TABLE    Import CSV file (header row) into table\n"
            "  .tables               List user tables\n");
}

/* ---------------------------------------------------------------- */
/* Dot command dispatcher                                           */
/* ---------------------------------------------------------------- */

sqlicon_exit_code handle_dot_command(sqli_conn_t *conn, sqlicon_runtime *rt,
                                     const char *line, bool *out_break_loop)
{
    *out_break_loop = false;
    const char *p = skip_spaces_str(line);
    if (*p != '.')
        return SQLICON_EXIT_OK;
    p++;

    const char *cmd_start = p;
    while (*p != '\0' && !is_space_char(*p))
        p++;
    size_t cmd_len = (size_t)(p - cmd_start);
    const char *arg = skip_spaces_str(p);

    char *cmd = dup_span(cmd_start, cmd_len);
    if (cmd == NULL)
        return SQLICON_EXIT_SQL_ERROR;

    sqlicon_exit_code rc = SQLICON_EXIT_OK;
    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        *out_break_loop = true;
    } else if (strcmp(cmd, "help") == 0) {
        print_dot_help(stdout);
    } else if (strcmp(cmd, "mode") == 0) {
        if (strcmp(arg, "aligned") == 0) {
            rt->mode = SQLICON_OUTPUT_ALIGNED;
        } else if (strcmp(arg, "csv") == 0) {
            rt->mode = SQLICON_OUTPUT_CSV;
        } else if (strcmp(arg, "line") == 0) {
            rt->mode = SQLICON_OUTPUT_LINE;
        } else if (strcmp(arg, "json") == 0) {
            rt->mode = SQLICON_OUTPUT_JSON;
        } else if (strcmp(arg, "markdown") == 0 || strcmp(arg, "md") == 0) {
            rt->mode = SQLICON_OUTPUT_MARKDOWN;
        } else {
            fprintf(stderr, "error: .mode expects aligned|csv|line|json|markdown\n");
            rc = SQLICON_EXIT_MISUSE;
        }
    } else if (strcmp(cmd, "width") == 0) {
        rc = command_width(rt, arg);
    } else if (strcmp(cmd, "headers") == 0) {
        if (strcmp(arg, "on") == 0) {
            rt->headers = true;
        } else if (strcmp(arg, "off") == 0) {
            rt->headers = false;
        } else {
            fprintf(stderr, "error: .headers expects on|off\n");
            rc = SQLICON_EXIT_MISUSE;
        }
    } else if (strcmp(cmd, "bail") == 0) {
        if (strcmp(arg, "on") == 0) {
            rt->bail_on_error = true;
        } else if (strcmp(arg, "off") == 0) {
            rt->bail_on_error = false;
        } else {
            fprintf(stderr, "error: .bail expects on|off\n");
            rc = SQLICON_EXIT_MISUSE;
        }
    } else if (strcmp(cmd, "timer") == 0) {
        if (strcmp(arg, "on") == 0) {
            rt->timer_on = true;
        } else if (strcmp(arg, "off") == 0) {
            rt->timer_on = false;
        } else {
            fprintf(stderr, "error: .timer expects on|off\n");
            rc = SQLICON_EXIT_MISUSE;
        }
    } else if (strcmp(cmd, "nullvalue") == 0) {
        snprintf(rt->null_repr, sizeof(rt->null_repr), "%s", arg);
    } else if (strcmp(cmd, "output") == 0) {
        if (arg[0] == '\0' || strcmp(arg, "stdout") == 0) {
            runtime_close_output(rt);
        } else if (runtime_open_persistent_output(rt, arg) != 0) {
            fprintf(stderr, "error: cannot open output file '%s'\n", arg);
            rc = SQLICON_EXIT_MISUSE;
        }
    } else if (strcmp(cmd, "once") == 0) {
        if (arg[0] == '\0') {
            fprintf(stderr, "error: .once requires a file path\n");
            rc = SQLICON_EXIT_MISUSE;
        } else {
            snprintf(rt->once_path, sizeof(rt->once_path), "%s", arg);
        }
    } else if (strcmp(cmd, "read") == 0) {
        if (arg[0] == '\0') {
            fprintf(stderr, "error: .read requires a file path\n");
            rc = SQLICON_EXIT_MISUSE;
        } else if (rt->read_depth >= 8) {
            fprintf(stderr, "error: .read nesting limit reached\n");
            rc = SQLICON_EXIT_MISUSE;
        } else {
            FILE *fp = fopen(arg, "r");
            if (fp == NULL) {
                fprintf(stderr, "error: cannot open script file '%s'\n", arg);
                rc = SQLICON_EXIT_MISUSE;
            } else {
                rt->read_depth++;
                rc = execute_stream(conn, fp, false, rt);
                rt->read_depth--;
                fclose(fp);
            }
        }
    } else if (strcmp(cmd, "explain") == 0) {
        if (strcmp(arg, "on") == 0) {
            rc = execute_sql_statement(conn, "SET EXPLAIN ON", false, false, rt);
        } else if (strcmp(arg, "off") == 0) {
            rc = execute_sql_statement(conn, "SET EXPLAIN OFF", false, false, rt);
        } else {
            fprintf(stderr, "error: .explain expects on|off\n");
            rc = SQLICON_EXIT_MISUSE;
        }
    } else if (strcmp(cmd, "use") == 0) {
        rc = command_use_database(conn, rt, arg);
    } else if (strcmp(cmd, "databases") == 0) {
        rc = command_list_databases(conn, rt, arg);
    } else if (strcmp(cmd, "status") == 0) {
        rc = command_status(conn, rt, arg);
    } else if (strcmp(cmd, "schema") == 0) {
        rc = command_schema_table(conn, rt, arg);
    } else if (strcmp(cmd, "indexes") == 0) {
        rc = command_indexes_table(conn, rt, arg);
    } else if (strcmp(cmd, "views") == 0) {
        rc = command_views_table(conn, rt, arg);
    } else if (strcmp(cmd, "constraints") == 0) {
        rc = command_constraints_table(conn, rt, arg);
    } else if (strcmp(cmd, "dump") == 0) {
        rc = command_dump_table(conn, rt, arg);
    } else if (strcmp(cmd, "import") == 0) {
        rc = command_import_csv(conn, arg);
    } else if (strcmp(cmd, "tables") == 0) {
        rc = execute_sql_statement(
            conn,
            "SELECT tabname FROM systables WHERE tabid >= 100 AND tabtype = 'T' ORDER BY tabname",
            false, false, rt);
    } else {
        fprintf(stderr, "error: unknown dot-command '.%s'\n", cmd);
        rc = SQLICON_EXIT_MISUSE;
    }

    free(cmd);
    return rc;
}
