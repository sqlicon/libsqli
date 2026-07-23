#include "sqlicon.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linenoise.h"

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

int sqlicon_platform_install_signal_handlers(void)
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

void sqlicon_platform_reset_interrupt_state(void)
{
    g_sigint_idle_count = 0;
    g_sigint_during_query = 0;
}

int sqlicon_platform_is_stdin_tty(void)
{
    return isatty(STDIN_FILENO);
}

int sqlicon_platform_clock_now(struct timespec *ts)
{
    return clock_gettime(CLOCK_MONOTONIC, ts);
}

long sqlicon_platform_process_id(void)
{
    return (long)getpid();
}

char *sqlicon_platform_readline(const char *prompt)
{
    return linenoise(prompt);
}

static int sqlicon_platform_history_path(char *out, size_t out_cap)
{
    char dir[512];

    if (sqlicon_platform_config_dir(dir, sizeof(dir)) != 0)
        return -1;
    if (snprintf(out, out_cap, "%s/history", dir) >= (int)out_cap)
        return -1;
    return 0;
}

void sqlicon_platform_history_load(void)
{
    char path[512];

    if (sqlicon_platform_history_path(path, sizeof(path)) != 0)
        return;
    linenoiseHistoryLoad(path);
}

void sqlicon_platform_history_save(void)
{
    char dir[512];
    char path[512];

    if (sqlicon_platform_config_dir(dir, sizeof(dir)) != 0)
        return;
    if (sqlicon_platform_mkdir_p(dir) != 0)
        return;
    if (sqlicon_platform_history_path(path, sizeof(path)) != 0)
        return;
    linenoiseHistorySave(path);
}

void sqlicon_platform_history_add(const char *line)
{
    if (line != NULL && line[0] != '\0')
        linenoiseHistoryAdd(line);
}

int sqlicon_platform_config_dir(char *out, size_t out_cap)
{
    const char *home = getenv("HOME");

    if (home == NULL || home[0] == '\0')
        return -1;
    if (snprintf(out, out_cap, "%s/.config/sqlicon", home) >= (int)out_cap)
        return -1;
    return 0;
}

int sqlicon_platform_mkdir_p(const char *path)
{
    char tmp[768];

    if (path == NULL || path[0] == '\0')
        return -1;
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return -1;

    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
            *p = '/';
            return -1;
        }
        *p = '/';
    }

    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int sqlicon_platform_restrict_dir(const char *path)
{
    if (path == NULL)
        return -1;
    return chmod(path, 0700);
}

int sqlicon_platform_restrict_file(const char *path)
{
    if (path == NULL)
        return -1;
    return chmod(path, 0600);
}

int sqlicon_platform_replace_file(const char *src_path, const char *dst_path)
{
    if (src_path == NULL || dst_path == NULL)
        return -1;
    return rename(src_path, dst_path);
}

int sqlicon_platform_profile_key_material(char *out, size_t out_cap)
{
    const char *paths[] = {
        "/etc/machine-id",
        "/var/lib/dbus/machine-id",
        "/sys/class/dmi/id/product_uuid"
    };
    char line[256];

    for (size_t i = 0; i < (sizeof(paths) / sizeof(paths[0])); i++) {
        FILE *fp = fopen(paths[i], "r");
        if (fp == NULL)
            continue;
        if (fgets(line, sizeof(line), fp) != NULL) {
            fclose(fp);
            strip_trailing_inplace(line);
            if (line[0] == '\0')
                continue;
            if (snprintf(out, out_cap, "%s:%lu",
                         line, (unsigned long)getuid()) >= (int)out_cap)
                return -1;
            return 0;
        }
        fclose(fp);
    }

    return -1;
}
