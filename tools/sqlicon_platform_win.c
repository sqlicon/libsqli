#include "sqlicon.h"

#include <direct.h>
#include <errno.h>
#include <io.h>
#include <windows.h>
#include <winreg.h>

static char *sqlicon_strdup_win(const char *s)
{
    size_t len;
    char *copy;

    if (s == NULL)
        return NULL;
    len = strlen(s);
    copy = malloc(len + 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

static BOOL WINAPI sqlicon_console_ctrl_handler(DWORD ctrl_type)
{
    if (ctrl_type != CTRL_C_EVENT)
        return FALSE;

    if (g_query_active) {
        g_sigint_during_query = 1;
        return TRUE;
    }

    g_sigint_idle_count++;
    if (g_sigint_idle_count >= 2)
        g_exit_requested = 1;
    return TRUE;
}

int sqlicon_platform_install_signal_handlers(void)
{
    return SetConsoleCtrlHandler(sqlicon_console_ctrl_handler, TRUE) ? 0 : -1;
}

void sqlicon_platform_reset_interrupt_state(void)
{
    g_sigint_idle_count = 0;
    g_sigint_during_query = 0;
}

int sqlicon_platform_is_stdin_tty(void)
{
    return _isatty(_fileno(stdin));
}

int sqlicon_platform_clock_now(struct timespec *ts)
{
    static LARGE_INTEGER freq;
    static BOOL freq_ready = FALSE;
    LARGE_INTEGER now;
    long double seconds;

    if (ts == NULL)
        return -1;
    if (!freq_ready) {
        if (!QueryPerformanceFrequency(&freq))
            return -1;
        freq_ready = TRUE;
    }
    if (!QueryPerformanceCounter(&now))
        return -1;

    seconds = (long double)now.QuadPart / (long double)freq.QuadPart;
    ts->tv_sec = (time_t)seconds;
    ts->tv_nsec = (long)((seconds - (long double)ts->tv_sec) * 1000000000.0L);
    return 0;
}

long sqlicon_platform_process_id(void)
{
    return (long)GetCurrentProcessId();
}

char *sqlicon_platform_readline(const char *prompt)
{
    char buf[4096];

    if (prompt != NULL) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    if (fgets(buf, sizeof(buf), stdin) == NULL)
        return NULL;
    strip_trailing_inplace(buf);
    return sqlicon_strdup_win(buf);
}

void sqlicon_platform_history_load(void)
{
}

void sqlicon_platform_history_save(void)
{
}

void sqlicon_platform_history_add(const char *line)
{
    (void)line;
}

int sqlicon_platform_config_dir(char *out, size_t out_cap)
{
    const char *base = getenv("APPDATA");
    const char *profile = getenv("USERPROFILE");

    if (base != NULL && base[0] != '\0') {
        if (snprintf(out, out_cap, "%s\\sqlicon", base) >= (int)out_cap)
            return -1;
        return 0;
    }
    if (profile != NULL && profile[0] != '\0') {
        if (snprintf(out, out_cap, "%s\\AppData\\Roaming\\sqlicon", profile) >= (int)out_cap)
            return -1;
        return 0;
    }
    return -1;
}

int sqlicon_platform_mkdir_p(const char *path)
{
    char tmp[768];

    if (path == NULL || path[0] == '\0')
        return -1;
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return -1;

    for (char *p = tmp + 3; *p != '\0'; p++) {
        if (*p != '\\' && *p != '/')
            continue;
        char saved = *p;
        *p = '\0';
        if (_mkdir(tmp) != 0 && errno != EEXIST) {
            *p = saved;
            return -1;
        }
        *p = saved;
    }

    if (_mkdir(tmp) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int sqlicon_platform_restrict_dir(const char *path)
{
    (void)path;
    return 0;
}

int sqlicon_platform_restrict_file(const char *path)
{
    (void)path;
    return 0;
}

int sqlicon_platform_replace_file(const char *src_path, const char *dst_path)
{
    if (src_path == NULL || dst_path == NULL)
        return -1;
    return MoveFileExA(src_path, dst_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? 0 : -1;
}

int sqlicon_platform_profile_key_material(char *out, size_t out_cap)
{
    char machine_guid[256];
    char user_name[256];
    DWORD machine_guid_size = sizeof(machine_guid);
    DWORD user_name_size = (DWORD)sizeof(user_name);
    LONG rc;

    rc = RegGetValueA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Cryptography",
                      "MachineGuid",
                      RRF_RT_REG_SZ,
                      NULL,
                      machine_guid,
                      &machine_guid_size);
    if (rc != ERROR_SUCCESS)
        return -1;
    if (!GetUserNameA(user_name, &user_name_size))
        return -1;
    strip_trailing_inplace(machine_guid);
    strip_trailing_inplace(user_name);
    if (snprintf(out, out_cap, "%s:%s", machine_guid, user_name) >= (int)out_cap)
        return -1;
    return 0;
}
