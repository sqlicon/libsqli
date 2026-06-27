#include "sqlicon.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------- */
/* Signal state                                                     */
/* ---------------------------------------------------------------- */

volatile sig_atomic_t g_query_active = 0;
volatile sig_atomic_t g_sigint_during_query = 0;
volatile sig_atomic_t g_sigint_idle_count = 0;
volatile sig_atomic_t g_exit_requested = 0;

static void sqlicon_sigint_handler(int signo)
{
    (void)signo;
    if (g_query_active) {
        g_sigint_during_query = 1;
        return;
    }

    g_sigint_idle_count++;
    if (g_sigint_idle_count >= 2)
        g_exit_requested = 1;
}

int sqlicon_install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sqlicon_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0)
        return -1;
    return 0;
}

void sqlicon_reset_interrupt_state(void)
{
    g_sigint_idle_count = 0;
    g_sigint_during_query = 0;
}

/* ---------------------------------------------------------------- */
/* Help                                                             */
/* ---------------------------------------------------------------- */

void print_help(FILE *out)
{
    fprintf(out,
            "sqlicon - interactive shell for Informix\n"
            "\n"
            "Usage:\n"
            "  sqlicon [options]\n"
            "\n"
            "Mode options:\n"
            "  -c, --command <sql>      Execute inline SQL and exit\n"
            "  -e, --execute <sql>      Execute inline SQL and exit\n"
            "  -f, --file <path>        Execute script file and exit\n"
            "  -h, --help               Show this help\n"
            "\n"
            "Connection/profile options:\n"
            "  -p, --profile <name>     Use named connection profile\n"
            "      --profile-create <n> Create profile from given connection flags\n"
            "      --profile-show <n>   Show profile (masked password)\n"
            "      --profile-list       List available profiles\n"
            "      --profile-update <n> Update profile fields from given flags\n"
            "      --profile-delete <n> Delete profile (requires --yes)\n"
            "      --profile-default <n>Set default profile name\n"
            "      --profile-test <n>   Test profile connection and exit\n"
            "      --show-secret        Show cleartext password in --profile-show\n"
            "      --yes                Confirm destructive action (--profile-delete)\n"
            "      --host <name>        Informix host name or IP\n"
            "      --port <value>       Informix service/port\n"
            "      --server <name>      Informix server name\n"
            "      --database <name>    Database name\n"
            "      --user <name>        User name\n"
            "      --password <value>   Password (avoid in shell history)\n"
            "      --client-locale <v>  Client locale\n"
            "      --db-locale <v>      Database locale\n"
            "      --connect-uri <uri>  Connection URI (onsoctcp/onsocssl/onipcstr)\n"
            "\n"
            "URI formats:\n"
            "  informix+onsoctcp://user:pass@host:port/db?INFORMIXSERVER=srv\n"
            "  informix+onsocssl://user:pass@host:port/db?INFORMIXSERVER=srv\n"
            "  informix+onipcstr:///db?INFORMIXSERVER=srv\n"
            "\n"
            "Notes:\n"
            "  - If --connect-uri is given, individual connection flags are ignored.\n"
            "  - For --profile-create/--profile-update, --connect-uri seeds host/port/server/\n"
            "    database and can be overridden by explicit connection flags.\n"
            "  - If no mode option is provided and stdin is a TTY, interactive mode starts.\n"
            "  - If stdin is redirected and no mode option is provided, stdin batch mode starts.\n"
            "  - Profile encryption protects against accidental file disclosure only; it does not\n"
            "    protect against an attacker with access to the same machine/account.\n");
}

/* ---------------------------------------------------------------- */
/* CLI argument parsing                                             */
/* ---------------------------------------------------------------- */

static bool parse_option_value(int argc, char **argv, int *index_out, const char **value_out)
{
    int index = *index_out;
    if (index + 1 >= argc)
        return false;
    *index_out = index + 1;
    *value_out = argv[index + 1];
    return true;
}

sqlicon_exit_code parse_args(int argc, char **argv, sqlicon_cli_options *opt)
{
    if (argc < 2) {
        opt->show_help = true;
        return SQLICON_EXIT_OK;
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            opt->show_help = true;
            continue;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "-e") == 0 || strcmp(arg, "--execute") == 0 || strcmp(arg, "--command") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->inline_query)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "-f") == 0 || strcmp(arg, "--file") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->script_path)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "-p") == 0 || strcmp(arg, "--profile") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->profile_name)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--profile-create") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->profile_create)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--profile-show") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->profile_show)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--profile-list") == 0) {
            opt->profile_list = true;
            continue;
        }
        if (strcmp(arg, "--profile-update") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->profile_update)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--profile-delete") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->profile_delete)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--profile-default") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->profile_set_default)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--profile-test") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->profile_test)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--show-secret") == 0) {
            opt->show_profile_secret = true;
            continue;
        }
        if (strcmp(arg, "--yes") == 0) {
            opt->confirm_delete = true;
            continue;
        }
        if (strcmp(arg, "--host") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->host)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--port") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->port)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--server") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->server)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--database") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->database)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--user") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->user)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--password") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->password)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--client-locale") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->client_locale)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--db-locale") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->db_locale)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }
        if (strcmp(arg, "--connect-uri") == 0) {
            if (!parse_option_value(argc, argv, &i, &opt->conn_uri)) {
                fprintf(stderr, "error: %s requires a value\n", arg);
                return SQLICON_EXIT_MISUSE;
            }
            continue;
        }

        fprintf(stderr, "error: unknown argument '%s'\n", arg);
        return SQLICON_EXIT_MISUSE;
    }

    if (opt->inline_query != NULL && opt->script_path != NULL) {
        fprintf(stderr, "error: --execute and --file are mutually exclusive\n");
        return SQLICON_EXIT_MISUSE;
    }

    int profile_actions = 0;
    if (opt->profile_create != NULL)
        profile_actions++;
    if (opt->profile_show != NULL)
        profile_actions++;
    if (opt->profile_list)
        profile_actions++;
    if (opt->profile_update != NULL)
        profile_actions++;
    if (opt->profile_delete != NULL)
        profile_actions++;
    if (opt->profile_set_default != NULL)
        profile_actions++;
    if (opt->profile_test != NULL)
        profile_actions++;
    if (profile_actions > 1) {
        fprintf(stderr, "error: profile management actions are mutually exclusive\n");
        return SQLICON_EXIT_MISUSE;
    }
    if (profile_actions > 0 && (opt->inline_query != NULL || opt->script_path != NULL)) {
        fprintf(stderr, "error: profile management actions cannot be combined with SQL execution modes\n");
        return SQLICON_EXIT_MISUSE;
    }
    if (opt->show_profile_secret && opt->profile_show == NULL) {
        fprintf(stderr, "error: --show-secret is only valid with --profile-show\n");
        return SQLICON_EXIT_MISUSE;
    }
    if (opt->confirm_delete && opt->profile_delete == NULL) {
        fprintf(stderr, "error: --yes is only valid with --profile-delete\n");
        return SQLICON_EXIT_MISUSE;
    }

    return SQLICON_EXIT_OK;
}

/* ---------------------------------------------------------------- */
/* Environment & validation                                         */
/* ---------------------------------------------------------------- */

void apply_environment(sqlicon_cli_options *opt)
{
    opt->host = first_nonempty(opt->host, getenv("SQLI_HOST"));
    opt->port = first_nonempty(opt->port, getenv("SQLI_PORT"));
    opt->server = first_nonempty(opt->server, getenv("SQLI_SERVER"));
    if (opt->server == NULL)
        opt->server = getenv("INFORMIXSERVER");
    opt->database = first_nonempty(opt->database, getenv("SQLI_DATABASE"));
    if (opt->database == NULL)
        opt->database = getenv("SQLI_DB");
    opt->user = first_nonempty(opt->user, getenv("SQLI_USER"));
    opt->password = first_nonempty(opt->password, getenv("SQLI_PASSWORD"));
    opt->client_locale = first_nonempty(opt->client_locale, getenv("SQLI_CLIENT_LOCALE"));
    if (opt->client_locale == NULL)
        opt->client_locale = getenv("CLIENT_LOCALE");
    opt->db_locale = first_nonempty(opt->db_locale, getenv("SQLI_DB_LOCALE"));
    if (opt->db_locale == NULL)
        opt->db_locale = getenv("DB_LOCALE");
}

sqlicon_exit_code validate_connection_options(const sqlicon_cli_options *opt)
{
    if (opt->conn_uri != NULL && opt->conn_uri[0] != '\0') {
        return SQLICON_EXIT_OK;
    }
    if (opt->host == NULL || opt->port == NULL || opt->database == NULL ||
        opt->user == NULL || opt->password == NULL) {
        fprintf(stderr,
                "error: missing connection settings; require host, port, database, user, password\n");
        fprintf(stderr, "hint: use --host/--port/--database/--user/--password or SQLI_* env vars\n");
        return SQLICON_EXIT_MISUSE;
    }
    return SQLICON_EXIT_OK;
}

sqlicon_mode select_mode(const sqlicon_cli_options *opt)
{
    if (opt->inline_query != NULL)
        return SQLICON_MODE_INLINE_QUERY;
    if (opt->script_path != NULL)
        return SQLICON_MODE_SCRIPT;
    if (!isatty(STDIN_FILENO))
        return SQLICON_MODE_STDIN_BATCH;
    return SQLICON_MODE_INTERACTIVE;
}
