#include "sqli_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>

/* ----------------------------------------------------------------
 * Module-level log level (atomic for thread-safe lazy init)
 * ---------------------------------------------------------------- */

static _Atomic sqli_log_level g_log_level = ATOMIC_VAR_INIT(SQLI_LOG_ERROR);

/* ----------------------------------------------------------------
 * Log level names (for prefix output)
 * ---------------------------------------------------------------- */

static const char *log_level_name[] = {
    [SQLI_LOG_NONE]    = "NONE",
    [SQLI_LOG_ERROR]   = "ERROR",
    [SQLI_LOG_WARN]    = "WARN ",
    [SQLI_LOG_INFO]    = "INFO ",
    [SQLI_LOG_DEBUG]   = "DEBUG",
};

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

void sqli_log(sqli_log_level level, const char *fmt, ...)
{
    sqli_log_level current = atomic_load_explicit(&g_log_level, memory_order_relaxed);

    if (level > current)
        return;

    va_list ap;
    va_start(ap, fmt);

    fprintf(stderr, "[sqli][%s] ", log_level_name[level]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);

    va_end(ap);
}

/* Internal setter used by sqli_conn.c */
void sqli_log_set_level(sqli_log_level level)
{
    atomic_store_explicit(&g_log_level, level, memory_order_release);
}
