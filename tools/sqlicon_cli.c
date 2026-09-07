#include "sqlicon.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Signal state                                                     */
/* ---------------------------------------------------------------- */

volatile sig_atomic_t g_query_active = 0;
volatile sig_atomic_t g_sigint_during_query = 0;
volatile sig_atomic_t g_sigint_idle_count = 0;
volatile sig_atomic_t g_exit_requested = 0;

int sqlicon_install_signal_handlers(void)
{
    return sqlicon_platform_install_signal_handlers();
}

void sqlicon_reset_interrupt_state(void)
{
    sqlicon_platform_reset_interrupt_state();
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
            "  -f, --file <path>        Execute script file and exit\n"
            "      --finderr <code>     Look up Informix error code description and exit\n"
            "  -h, --help               Show this help and exit\n"
            "  -V, --version            Show the CMake project version and exit\n"
            "\n"
            "Connection/profile options:\n"
            "  -p, --profile <name>     Use named connection profile\n"
            "      --profile-create <n> Create profile from given connection flags\n"
            "      --profile-show <n>   Show profile (masked password)\n"
            "      --profile-list       List available profiles\n"
            "      --profile-update <n> Update profile fields from given flags\n"
            "      --profile-delete <n> Delete profile (requires --yes)\n"
            "      --profile-default <n> Set default profile name\n"
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
            "      --log-level <lvl>    Diagnostic log verbosity: none/error/warn/info/debug\n"
            "                           (default: error; written to stderr, or SQLI_LOG_FILE)\n"
            "\n"
            "Environment (connection operations):\n"
            "  SQLI_HOST, SQLI_PORT, SQLI_SERVER, SQLI_DATABASE (or SQLI_DB),\n"
            "  SQLI_USER, SQLI_PASSWORD, SQLI_CLIENT_LOCALE, SQLI_DB_LOCALE, SQLI_LOG_LEVEL\n"
            "  Fallbacks: INFORMIXSERVER, CLIENT_LOCALE, DB_LOCALE\n"
            "\n"
            "URI formats:\n"
            "  informix+onsoctcp://user:pass@host:port/db?INFORMIXSERVER=srv\n"
            "  informix+onsocssl://user:pass@host:port/db?INFORMIXSERVER=srv\n"
            "  informix+onipcstr:///db?INFORMIXSERVER=srv\n"
            "\n"
            "Notes:\n"
            "  - --help, --version and --finderr are standalone actions.\n"
            "  - --connect-uri is self-contained; do not combine it with profiles or connection flags.\n"
            "  - Connection precedence: explicit flags > SQLI_* environment > profile.\n"
            "  - Profile create/update use explicit flags only; URI import is unsupported.\n"
            "  - Long value options also accept --option=value; repeated options are errors.\n"
            "  - Use --password= for an explicitly empty password.\n"
            "  - If no mode option is provided and stdin is a TTY, interactive mode starts.\n"
            "  - If stdin is redirected and no mode option is provided, stdin batch mode starts.\n"
            "  - Profile encryption protects against accidental file disclosure only; it does not\n"
            "    protect against an attacker with access to the same machine/account.\n");
}

/* ---------------------------------------------------------------- */
/* CLI argument parsing                                             */
/* ---------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *short_name;
    const char **value;
    bool *flag;
} cli_option;

static bool has_connection_fields(const sqlicon_cli_options *opt)
{
    return opt->host != NULL || opt->port != NULL || opt->server != NULL ||
           opt->database != NULL || opt->user != NULL || opt->password != NULL ||
           opt->client_locale != NULL || opt->db_locale != NULL;
}

static sqlicon_exit_code invalid_options(const char *message)
{
    fprintf(stderr, "error: %s\n", message);
    return SQLICON_EXIT_MISUSE;
}

sqlicon_exit_code parse_args(int argc, char **argv, sqlicon_cli_options *opt)
{
    if (opt == NULL || argv == NULL || argc < 1)
        return SQLICON_EXIT_MISUSE;
    cli_option options[] = {
        {"--help", "-h", NULL, &opt->show_help},
        {"--version", "-V", NULL, &opt->show_version},
        {"--command", "-c", &opt->inline_query, NULL},
        {"--file", "-f", &opt->script_path, NULL},
        {"--finderr", NULL, &opt->finderr_code, NULL},
        {"--profile", "-p", &opt->profile_name, NULL},
        {"--profile-create", NULL, &opt->profile_create, NULL},
        {"--profile-show", NULL, &opt->profile_show, NULL},
        {"--profile-list", NULL, NULL, &opt->profile_list},
        {"--profile-update", NULL, &opt->profile_update, NULL},
        {"--profile-delete", NULL, &opt->profile_delete, NULL},
        {"--profile-default", NULL, &opt->profile_set_default, NULL},
        {"--profile-test", NULL, &opt->profile_test, NULL},
        {"--show-secret", NULL, NULL, &opt->show_profile_secret},
        {"--yes", NULL, NULL, &opt->confirm_delete},
        {"--host", NULL, &opt->host, NULL},
        {"--port", NULL, &opt->port, NULL},
        {"--server", NULL, &opt->server, NULL},
        {"--database", NULL, &opt->database, NULL},
        {"--user", NULL, &opt->user, NULL},
        {"--password", NULL, &opt->password, NULL},
        {"--client-locale", NULL, &opt->client_locale, NULL},
        {"--db-locale", NULL, &opt->db_locale, NULL},
        {"--connect-uri", NULL, &opt->conn_uri, NULL},
        {"--log-level", NULL, &opt->log_level, NULL}
    };
    size_t option_count = 0;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *equals = strncmp(arg, "--", 2) == 0 ? strchr(arg, '=') : NULL;
        size_t name_len = equals != NULL ? (size_t)(equals - arg) : strlen(arg);
        cli_option *option = NULL;
        for (size_t j = 0; j < sizeof(options) / sizeof(options[0]); j++) {
            if ((strlen(options[j].name) == name_len &&
                 strncmp(arg, options[j].name, name_len) == 0) ||
                (equals == NULL && options[j].short_name != NULL &&
                 strcmp(arg, options[j].short_name) == 0)) {
                option = &options[j];
                break;
            }
        }
        if (option == NULL)
            return invalid_options("unknown option or positional argument; see --help");
        option_count++;
        if ((option->flag != NULL && *option->flag) ||
            (option->value != NULL && *option->value != NULL)) {
            fprintf(stderr, "error: %s may only be specified once\n", option->name);
            return SQLICON_EXIT_MISUSE;
        }
        if (option->flag != NULL) {
            if (equals != NULL)
                return invalid_options("flag options do not accept values");
            *option->flag = true;
            continue;
        }
        const char *value = equals != NULL ? equals + 1 : NULL;
        if (value == NULL && i + 1 < argc) {
            const char *next = argv[i + 1];
            bool negative_code = option->value == &opt->finderr_code &&
                next[0] == '-' && next[1] >= '0' && next[1] <= '9';
            if (next[0] != '-' || negative_code) {
                value = next;
                i++;
            }
        }
        if (value == NULL || (value[0] == '\0' && option->value != &opt->password)) {
            fprintf(stderr, "error: %s requires a value (use %s=value for values starting with '-')\n",
                    option->name, option->name);
            return SQLICON_EXIT_MISUSE;
        }
        *option->value = value;
    }

    if (opt->show_help || opt->show_version) {
        if (option_count != 1)
            return invalid_options("--help and --version must be used alone");
        return SQLICON_EXIT_OK;
    }
    if (opt->finderr_code != NULL) {
        if (option_count != 1)
            return invalid_options("--finderr must be used alone");
        const char *digits = opt->finderr_code;
        if (*digits == '-' || *digits == '+') digits++;
        if (*digits == '\0')
            return invalid_options("--finderr requires a signed decimal integer");
        for (const char *p = digits; *p != '\0'; p++) {
            if (*p < '0' || *p > '9')
                return invalid_options("--finderr requires a signed decimal integer");
        }
        errno = 0;
        char *end = NULL;
        long code = strtol(opt->finderr_code, &end, 10);
        if (errno == ERANGE || *end != '\0' || code < -INT_MAX || code > INT_MAX)
            return invalid_options("--finderr code is outside the supported integer range");
        opt->finderr_value = (int)code;
        return SQLICON_EXIT_OK;
    }
    if (opt->inline_query != NULL && opt->script_path != NULL)
        return invalid_options("--command and --file are mutually exclusive");
    int profile_actions = (opt->profile_create != NULL) + (opt->profile_show != NULL) +
        opt->profile_list + (opt->profile_update != NULL) + (opt->profile_delete != NULL) +
        (opt->profile_set_default != NULL) + (opt->profile_test != NULL);
    if (profile_actions > 1)
        return invalid_options("profile actions are mutually exclusive");
    if (profile_actions && (opt->inline_query != NULL || opt->script_path != NULL || opt->profile_name != NULL))
        return invalid_options("profile actions cannot be combined with --profile, --command or --file");
    if (opt->show_profile_secret && opt->profile_show == NULL)
        return invalid_options("--show-secret is only valid with --profile-show");
    if (opt->confirm_delete && opt->profile_delete == NULL)
        return invalid_options("--yes is only valid with --profile-delete");
    if (opt->profile_delete != NULL && !opt->confirm_delete)
        return invalid_options("--profile-delete requires --yes");
    bool fields = has_connection_fields(opt);
    if (opt->conn_uri != NULL && (fields || opt->profile_name != NULL || profile_actions))
        return invalid_options("--connect-uri cannot be combined with profiles or connection fields");
    if (profile_actions && opt->profile_create == NULL && opt->profile_update == NULL && fields)
        return invalid_options("connection fields are only valid with profile create/update or SQL execution");
    if (profile_actions && opt->profile_test == NULL && opt->log_level != NULL)
        return invalid_options("--log-level is only valid for connection operations");
    if (opt->profile_update != NULL && !fields)
        return invalid_options("--profile-update requires at least one connection field");
    return SQLICON_EXIT_OK;
}

/* ---------------------------------------------------------------- */
/* Environment & validation                                         */
/* ---------------------------------------------------------------- */

void apply_environment(sqlicon_cli_options *opt)
{
    opt->log_level = first_nonempty(opt->log_level, getenv("SQLI_LOG_LEVEL"));
    if (opt->conn_uri != NULL)
        return;
    opt->host = first_nonempty(opt->host, getenv("SQLI_HOST"));
    opt->port = first_nonempty(opt->port, getenv("SQLI_PORT"));
    opt->server = first_nonempty(opt->server, getenv("SQLI_SERVER"));
    if (opt->server == NULL)
        opt->server = getenv("INFORMIXSERVER");
    opt->database = first_nonempty(opt->database, getenv("SQLI_DATABASE"));
    if (opt->database == NULL)
        opt->database = getenv("SQLI_DB");
    opt->user = first_nonempty(opt->user, getenv("SQLI_USER"));
    if (opt->password == NULL)
        opt->password = getenv("SQLI_PASSWORD");
    opt->client_locale = first_nonempty(opt->client_locale, getenv("SQLI_CLIENT_LOCALE"));
    if (opt->client_locale == NULL)
        opt->client_locale = getenv("CLIENT_LOCALE");
    opt->db_locale = first_nonempty(opt->db_locale, getenv("SQLI_DB_LOCALE"));
    if (opt->db_locale == NULL)
        opt->db_locale = getenv("DB_LOCALE");
}

sqlicon_exit_code apply_log_level(const sqlicon_cli_options *opt)
{
    if (opt->log_level == NULL || opt->log_level[0] == '\0')
        return SQLICON_EXIT_OK;

    sqli_log_level level;
    if (strcmp(opt->log_level, "none") == 0) {
        level = SQLI_LOG_NONE;
    } else if (strcmp(opt->log_level, "error") == 0) {
        level = SQLI_LOG_ERROR;
    } else if (strcmp(opt->log_level, "warn") == 0) {
        level = SQLI_LOG_WARN;
    } else if (strcmp(opt->log_level, "info") == 0) {
        level = SQLI_LOG_INFO;
    } else if (strcmp(opt->log_level, "debug") == 0) {
        level = SQLI_LOG_DEBUG;
    } else {
        fprintf(stderr,
                "error: invalid --log-level '%s' (expected none/error/warn/info/debug)\n",
                opt->log_level);
        return SQLICON_EXIT_MISUSE;
    }

    sqli_log_set_level(level);
    return SQLICON_EXIT_OK;
}

bool sqlicon_valid_service(const char *service)
{
    if (service == NULL || *service == '\0')
        return false;
    if (*service == '/')
        return service[1] != '\0'; /* Unix-domain socket path */
    if (*service >= '0' && *service <= '9') {
        unsigned port = 0;
        for (const char *p = service; *p != '\0'; p++) {
            if (*p < '0' || *p > '9')
                return false;
            port = port * 10u + (unsigned)(*p - '0');
            if (port > UINT16_MAX)
                return false;
        }
        return port != 0;
    }
    for (const char *p = service; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return false;
    }
    return *service != '-';
}

sqlicon_exit_code validate_connection_options(const sqlicon_cli_options *opt)
{
    if (opt->conn_uri != NULL && opt->conn_uri[0] != '\0') {
        return SQLICON_EXIT_OK;
    }
    if (opt->host == NULL || *opt->host == '\0' ||
        opt->port == NULL || *opt->port == '\0' ||
        opt->database == NULL || *opt->database == '\0' ||
        opt->user == NULL || *opt->user == '\0' || opt->password == NULL) {
        fprintf(stderr,
                "error: missing connection settings; require host, port, database, user, password\n");
        fprintf(stderr, "hint: use --host/--port/--database/--user/--password or SQLI_* env vars\n");
        return SQLICON_EXIT_MISUSE;
    }
    if (!sqlicon_valid_service(opt->port))
        return invalid_options("--port must be 1..65535, a service name or a Unix socket path");
    return SQLICON_EXIT_OK;
}

sqlicon_mode select_mode(const sqlicon_cli_options *opt)
{
    if (opt->inline_query != NULL)
        return SQLICON_MODE_INLINE_QUERY;
    if (opt->script_path != NULL)
        return SQLICON_MODE_SCRIPT;
    if (!sqlicon_platform_is_stdin_tty())
        return SQLICON_MODE_STDIN_BATCH;
    return SQLICON_MODE_INTERACTIVE;
}
