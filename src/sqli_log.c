#include "sqli_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static volatile LONG g_log_level = SQLI_LOG_ERROR;
#else
#include <stdatomic.h>
static _Atomic sqli_log_level g_log_level = ATOMIC_VAR_INIT(SQLI_LOG_ERROR);
#endif

static FILE *sqli_log_stream(void)
{
    static int initialized = 0;
    static FILE *stream = NULL;

    if (!initialized) {
        const char *path = getenv("SQLI_LOG_FILE");
        if (path != NULL && path[0] != '\0' && strcmp(path, "stderr") != 0) {
            stream = fopen(path, "a");
        }
        initialized = 1;
    }

    return stream != NULL ? stream : stderr;
}

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
#ifdef _WIN32
    sqli_log_level current = (sqli_log_level)InterlockedCompareExchange(&g_log_level, 0, 0);
#else
    sqli_log_level current = atomic_load_explicit(&g_log_level, memory_order_relaxed);
#endif

    if (level > current)
        return;

    va_list ap;
    FILE *stream = sqli_log_stream();
    va_start(ap, fmt);

    fprintf(stream, "[sqli][%s] ", log_level_name[level]);
    vfprintf(stream, fmt, ap);
    fputc('\n', stream);
    fflush(stream);

    va_end(ap);
}

/* Internal setter used by sqli_conn.c */
void sqli_log_set_level(sqli_log_level level)
{
#ifdef _WIN32
    InterlockedExchange(&g_log_level, (LONG)level);
#else
    atomic_store_explicit(&g_log_level, level, memory_order_release);
#endif
}
