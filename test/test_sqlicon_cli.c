/* Parser tests have no terminal, profile-store or network side effects. */
#include "sqlicon.h"
#include "unity.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

int sqlicon_platform_install_signal_handlers(void) { return 0; }
void sqlicon_platform_reset_interrupt_state(void) {}
int sqlicon_platform_is_stdin_tty(void) { return true; }
/* Isolate profile I/O in a CTest-owned directory and use a test-only key. */
int sqlicon_platform_config_dir(char *out, size_t cap)
{
    const char *dir = getenv("SQLICON_TEST_PROFILE_DIR");
    if (dir == NULL) return -1;
    int n = snprintf(out, cap, "%s", dir);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
int sqlicon_platform_mkdir_p(const char *path) { (void)path; return 0; }
int sqlicon_platform_restrict_dir(const char *path) { (void)path; return 0; }
int sqlicon_platform_restrict_file(const char *path) { (void)path; return 0; }
int sqlicon_platform_replace_file(const char *src, const char *dst)
{
    if (remove(dst) != 0 && errno != ENOENT) return -1;
    return rename(src, dst);
}
int sqlicon_platform_profile_key_material(char *out, size_t cap)
{
    int n = snprintf(out, cap, "sqlicon-cli-test-key");
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
void setUp(void) {}
void tearDown(void) {}

static sqlicon_exit_code parse(const char *const *args, sqlicon_cli_options *opt)
{
    char *argv[16];
    int argc = 0;
    while (args[argc] != NULL) {
        TEST_ASSERT_LESS_THAN_INT(16, argc);
        argv[argc] = (char *)args[argc];
        argc++;
    }
    memset(opt, 0, sizeof(*opt));
    return parse_args(argc, argv, opt);
}

static void test_invalid_options(void)
{
    const char *const cases[][9] = {
        {"sqlicon", "--version", "--help", NULL},
        {"sqlicon", "--version=yes", NULL},
        {"sqlicon", "--version", "--host", "host", NULL},
        {"sqlicon", "--finderr", "abc", NULL},
        {"sqlicon", "--finderr", "123junk", NULL},
        {"sqlicon", "--finderr", "99999999999999999999999", NULL},
        {"sqlicon", "--finderr", "-2147483648", NULL},
        {"sqlicon", "--finderr", " 123", NULL},
        {"sqlicon", "--finderr", "-201", "--profile-delete", "db", "--yes", NULL},
        {"sqlicon", "--host", "--port", "9088", NULL},
        {"sqlicon", "--file", NULL},
        {"sqlicon", "--host=", NULL},
        {"sqlicon", "--host", "one", "--host=two", NULL},
        {"sqlicon", "--command", "one", "-c", "two", NULL},
        {"sqlicon", "--command", "one", "--file", "two", NULL},
        {"sqlicon", "--profile-list", "--profile-list", NULL},
        {"sqlicon", "--profile-list", "--profile-show", "db", NULL},
        {"sqlicon", "--profile-test", "db", "--host", "ignored", NULL},
        {"sqlicon", "--profile-show", "db", "--password", "ignored", NULL},
        {"sqlicon", "--profile-list", "--log-level", "debug", NULL},
        {"sqlicon", "--profile-update", "db", NULL},
        {"sqlicon", "--profile-delete", "db", NULL},
        {"sqlicon", "--profile-create", "db", "--profile", "ignored", NULL},
        {"sqlicon", "--show-secret", NULL},
        {"sqlicon", "--yes", NULL},
        {"sqlicon", "--connect-uri", "uri", "--host", "ignored", NULL},
        {"sqlicon", "--connect-uri", "uri", "--profile", "ignored", NULL},
        {"sqlicon", "--connect-uri", "uri", "--profile-create", "db", NULL},
        {"sqlicon", "--connect-uri", "uri", "--client-locale", "ignored", NULL},
        {"sqlicon", "--execute", "sql", NULL},
        {"sqlicon", "-e", "sql", NULL},
        {"sqlicon", "--passwort", "value", NULL},
        {"sqlicon", "--dblocale", "value", NULL},
        {"sqlicon", "--connect-url", "value", NULL},
        {"sqlicon", "positional", NULL}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sqlicon_cli_options opt;
        TEST_ASSERT_EQUAL_INT_MESSAGE(SQLICON_EXIT_MISUSE, parse(cases[i], &opt), cases[i][1]);
    }
}

static void test_valid_modes_and_values(void)
{
    sqlicon_cli_options opt;
    const char *const empty[] = {"sqlicon", NULL};
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, parse(empty, &opt));
    TEST_ASSERT_FALSE(opt.show_help);
    TEST_ASSERT_EQUAL_INT(SQLICON_MODE_INTERACTIVE, select_mode(&opt));
    const char *const version[] = {"sqlicon", "-V", NULL};
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, parse(version, &opt));
    TEST_ASSERT_TRUE(opt.show_version);
    const char *const query[] = {"sqlicon", "--command=-- comment\nSELECT 1", "--password=", NULL};
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, parse(query, &opt));
    TEST_ASSERT_EQUAL_STRING("-- comment\nSELECT 1", opt.inline_query);
    TEST_ASSERT_EQUAL_STRING("", opt.password);
    TEST_ASSERT_EQUAL_INT(SQLICON_MODE_INLINE_QUERY, select_mode(&opt));
    const char *const file[] = {"sqlicon", "-f", "script.sql", NULL};
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, parse(file, &opt));
    TEST_ASSERT_EQUAL_INT(SQLICON_MODE_SCRIPT, select_mode(&opt));
    const char *const code[] = {"sqlicon", "--finderr", "-201", NULL};
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, parse(code, &opt));
    TEST_ASSERT_EQUAL_INT(-201, opt.finderr_value);
    const char *const update[] = {"sqlicon", "--profile-update", "db", "--password=", NULL};
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, parse(update, &opt));
    const char *const show[] = {"sqlicon", "--profile-show", "db", "--show-secret", NULL};
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, parse(show, &opt));
    const char *const del[] = {"sqlicon", "--profile-delete", "db", "--yes", NULL};
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, parse(del, &opt));
}

static void test_environment_and_validation(void)
{
    sqlicon_cli_options opt = {0};
    apply_environment(&opt);
    TEST_ASSERT_EQUAL_STRING("env-host", opt.host);
    TEST_ASSERT_EQUAL_STRING("env-password", opt.password);
    opt.host = "cli-host";
    opt.password = "";
    apply_environment(&opt);
    TEST_ASSERT_EQUAL_STRING("cli-host", opt.host);
    TEST_ASSERT_EQUAL_STRING("", opt.password);
    memset(&opt, 0, sizeof(opt));
    opt.conn_uri = "informix+onsoctcp://host:9088/db";
    apply_environment(&opt);
    TEST_ASSERT_NULL(opt.host);
    TEST_ASSERT_NULL(opt.password);
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, validate_connection_options(&opt));
    opt.conn_uri = NULL;
    opt.host = "";
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_MISUSE, validate_connection_options(&opt));
    TEST_ASSERT_TRUE(sqlicon_valid_service("9088"));
    TEST_ASSERT_TRUE(sqlicon_valid_service("65535"));
    TEST_ASSERT_TRUE(sqlicon_valid_service("sqlexec"));
    TEST_ASSERT_TRUE(sqlicon_valid_service("/tmp/informix.sock"));
    TEST_ASSERT_FALSE(sqlicon_valid_service("0"));
    TEST_ASSERT_FALSE(sqlicon_valid_service("65536"));
    TEST_ASSERT_FALSE(sqlicon_valid_service("9999999999999999999"));
    TEST_ASSERT_FALSE(sqlicon_valid_service("-1"));
    opt.log_level = "invalid";
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_MISUSE, apply_log_level(&opt));
}

static void test_profile_precedence(void)
{
    char path[1024];
    TEST_ASSERT_EQUAL_INT(0, sqlicon_platform_config_dir(path, sizeof(path)));
    size_t len = strlen(path);
    int n = snprintf(path + len, sizeof(path) - len, "/profiles.conf");
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(path) - len);
    if (remove(path) != 0) TEST_ASSERT_EQUAL_INT(ENOENT, errno);
    sqlicon_cli_options create = {0};
    create.profile_create = "fixture";
    create.host = "profile-host";
    create.port = "sqlexec";
    create.database = "profile-db";
    create.user = "profile-user";
    create.password = "profile-password";
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, run_profile_action(&create));
    sqlicon_cli_options opt = {0};
    sqlicon_profile_override ov;
    profile_override_init(&ov);
    opt.profile_name = "fixture";
    opt.user = "cli-user";
    opt.password = "";
    apply_environment(&opt);
    /* Exercise an absent database setting independently of the runner's env. */
    opt.database = NULL;
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, maybe_load_profile_for_connect(&opt, &ov));
    TEST_ASSERT_EQUAL_STRING("env-host", opt.host);
    TEST_ASSERT_EQUAL_STRING("cli-user", opt.user);
    TEST_ASSERT_EQUAL_STRING("", opt.password);
    TEST_ASSERT_EQUAL_STRING("profile-db", opt.database);
    profile_override_destroy(&ov);
    memset(&opt, 0, sizeof(opt));
    opt.profile_name = "fixture";
    profile_override_init(&ov);
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, maybe_load_profile_for_connect(&opt, &ov));
    TEST_ASSERT_EQUAL_STRING("profile-host", opt.host);
    TEST_ASSERT_EQUAL_STRING("profile-password", opt.password);
    profile_override_destroy(&ov);
    /* An explicit URI must not read even a corrupt profile store. */
    FILE *file = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(fputs("version=2\n[broken]\npassword=invalid\n", file) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    memset(&opt, 0, sizeof(opt));
    opt.conn_uri = "informix+onsoctcp://host:9088/db";
    profile_override_init(&ov);
    TEST_ASSERT_EQUAL_INT(SQLICON_EXIT_OK, maybe_load_profile_for_connect(&opt, &ov));
    profile_override_destroy(&ov);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_invalid_options);
    RUN_TEST(test_valid_modes_and_values);
    RUN_TEST(test_environment_and_validation);
    RUN_TEST(test_profile_precedence);
    return UNITY_END();
}
