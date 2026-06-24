#include "sqlicon.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- */
/* SQL execution                                                    */
/* ---------------------------------------------------------------- */

sqlicon_exit_code execute_sql_statement(sqli_conn_t *conn, const char *sql,
                                        bool noisy, bool interactive,
                                        sqlicon_runtime *rt)
{
    sqli_result_t *result = NULL;
    struct timespec t0;
    struct timespec t1;
    bool have_timing = false;
    if (rt->timer_on && clock_gettime(CLOCK_MONOTONIC, &t0) == 0)
        have_timing = true;

    g_query_active = 1;
    g_sigint_during_query = 0;
    sqli_status rc = sqli_query(conn, sql, &result);
    g_query_active = 0;
    if (have_timing && clock_gettime(CLOCK_MONOTONIC, &t1) != 0)
        have_timing = false;

    if (rc != SQLI_OK) {
        if (interactive && g_sigint_during_query) {
            fprintf(stderr, "query interrupted\n");
            if (result != NULL)
                sqli_result_destroy(result);
            sqlicon_reset_interrupt_state();
            return SQLICON_EXIT_OK;
        }
        fprintf(stderr, "error: SQL failed (%d): %s\n",
                (int)rc, sqli_error(conn) ? sqli_error(conn) : "(unknown)");
        if (have_timing) {
            long sec = (long)(t1.tv_sec - t0.tv_sec);
            long nsec = (long)(t1.tv_nsec - t0.tv_nsec);
            if (nsec < 0) {
                sec -= 1;
                nsec += 1000000000L;
            }
            fprintf(stderr, "time: %.3f ms\n",
                    (double)sec * 1000.0 + (double)nsec / 1000000.0);
        }
        if (result != NULL)
            sqli_result_destroy(result);
        return SQLICON_EXIT_SQL_ERROR;
    }

    bool close_after = false;
    FILE *out = runtime_acquire_output(rt, &close_after);
    if (out == NULL) {
        fprintf(stderr, "error: cannot open output destination\n");
        sqli_result_destroy(result);
        return SQLICON_EXIT_MISUSE;
    }

    print_result_rows(out, rt, result);
    fflush(out);
    runtime_release_output(out, close_after);

    if (noisy) {
        fprintf(stderr, "%lld row(s) affected\n",
                (long long)sqli_result_rows_affected(result));
    }

    sqli_result_destroy(result);
    if (have_timing) {
        long sec = (long)(t1.tv_sec - t0.tv_sec);
        long nsec = (long)(t1.tv_nsec - t0.tv_nsec);
        if (nsec < 0) {
            sec -= 1;
            nsec += 1000000000L;
        }
        fprintf(stderr, "time: %.3f ms\n",
                (double)sec * 1000.0 + (double)nsec / 1000000.0);
    }
    sqlicon_reset_interrupt_state();
    return SQLICON_EXIT_OK;
}
