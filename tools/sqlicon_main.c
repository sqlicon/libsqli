#include "sqlicon.h"
#include "libsqli/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#endif

int main(int argc, char **argv)
{
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    sqlicon_cli_options opt;
    sqlicon_runtime rt;
    sqlicon_profile_override ov;
    memset(&opt, 0, sizeof(opt));
    runtime_init(&rt);
    profile_override_init(&ov);

    sqlicon_exit_code rc = parse_args(argc, argv, &opt);
    if (rc != SQLICON_EXIT_OK) {
        profile_override_destroy(&ov);
        runtime_destroy(&rt);
        return (int)rc;
    }

    if (opt.show_help) {
        print_help(stdout);
        profile_override_destroy(&ov);
        runtime_destroy(&rt);
        return (int)SQLICON_EXIT_OK;
    }

    if (opt.show_version) {
        printf("sqlicon %s\n", SQLI_VERSION);
        profile_override_destroy(&ov);
        runtime_destroy(&rt);
        return (int)SQLICON_EXIT_OK;
    }

    if (opt.finderr_code != NULL) {
        int code = opt.finderr_value;
        char msg[SQLI_ERRMSG_MAX_LEN + 1];
        int n = sqli_error_message_lookup(code, msg, sizeof(msg));
        if (n < 0) {
            fprintf(stderr, "Error code %d not found in catalog\n", code);
            rc = SQLICON_EXIT_SQL_ERROR;
        } else {
            fprintf(stdout, "[%d] %s\n", code, msg);
            rc = SQLICON_EXIT_OK;
        }
        profile_override_destroy(&ov);
        runtime_destroy(&rt);
        return (int)rc;
    }

    if (has_profile_store_action(&opt)) {
        rc = run_profile_action(&opt);
        profile_override_destroy(&ov);
        runtime_destroy(&rt);
        return (int)rc;
    }

    if (opt.profile_test != NULL) {
        opt.profile_name = opt.profile_test;
        opt.inline_query = "SELECT FIRST 1 tabid FROM systables";
    }

    if (opt.profile_test == NULL)
        apply_environment(&opt);
    else
        opt.log_level = first_nonempty(opt.log_level, getenv("SQLI_LOG_LEVEL"));
    rc = maybe_load_profile_for_connect(&opt, &ov);
    if (rc != SQLICON_EXIT_OK) {
        profile_override_destroy(&ov);
        runtime_destroy(&rt);
        return (int)rc;
    }

    rc = validate_connection_options(&opt);
    if (rc != SQLICON_EXIT_OK) {
        profile_override_destroy(&ov);
        runtime_destroy(&rt);
        return (int)rc;
    }
    runtime_set_connection_metadata(&rt, &opt);

    sqlicon_mode mode = select_mode(&opt);
    if (sqlicon_install_signal_handlers() != 0) {
        fprintf(stderr, "error: failed to install signal handlers\n");
        profile_override_destroy(&ov);
        runtime_destroy(&rt);
        return (int)SQLICON_EXIT_MISUSE;
    }

    sqli_conn_t *conn = NULL;
    rc = open_connection(&opt, &conn);
    if (rc != SQLICON_EXIT_OK) {
        profile_override_destroy(&ov);
        runtime_destroy(&rt);
        return (int)rc;
    }

    rc = run_mode(mode, &opt, conn, &rt);

    sqli_close(conn);
    sqli_destroy(conn);
    profile_override_destroy(&ov);
    runtime_destroy(&rt);
    return (int)rc;
}
