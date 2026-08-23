#define _POSIX_C_SOURCE 200809L
#include "sqli_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* Millisecond wall-clock timestamp, so connection-timing issues (e.g. a
 * slow handshake phase) can be diagnosed directly from the elapsed time
 * between consecutive log lines instead of needing external tracing. */
static void format_timestamp(char *buf, size_t buf_size)
{
    struct timespec ts;
    struct tm tm_buf;

    if (buf_size < 13 || clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        if (buf_size > 0)
            buf[0] = '\0';
        return;
    }

#ifdef _WIN32
    localtime_s(&tm_buf, &ts.tv_sec);
#else
    localtime_r(&ts.tv_sec, &tm_buf);
#endif

    size_t n = strftime(buf, buf_size, "%H:%M:%S", &tm_buf);
    if (n == 0 || buf_size - n < 5)
        return;
    snprintf(buf + n, buf_size - n, ".%03ld", ts.tv_nsec / 1000000);
}

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
    char ts_buf[16];
    va_start(ap, fmt);

    format_timestamp(ts_buf, sizeof(ts_buf));
    fprintf(stream, "[sqli][%s][%s] ", ts_buf, log_level_name[level]);
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
