#ifndef SQLI_PLATFORM_WIN_H
#define SQLI_PLATFORM_WIN_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <synchapi.h>
#include <processthreadsapi.h>
#include <process.h>
#include <intrin.h>
#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <BaseTsd.h>

typedef SSIZE_T ssize_t;
typedef int socklen_t;
typedef int pid_t;
typedef int uid_t;

struct passwd {
    char *pw_name;
    char *pw_dir;
};

#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef EINPROGRESS
#define EINPROGRESS WSAEWOULDBLOCK
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK WSAEWOULDBLOCK
#endif
#ifndef EAGAIN
#define EAGAIN WSAEWOULDBLOCK
#endif
#ifndef ECONNRESET
#define ECONNRESET WSAECONNRESET
#endif

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef TIME_UTC
#define TIME_UTC 1
#endif

#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s
#define getpid _getpid
#define getcwd _getcwd
#define access _access

#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 1
#endif

static inline char *realpath(const char *path, char *resolved_path)
{
    return _fullpath(resolved_path, path, MAX_PATH);
}

static inline uid_t getuid(void)
{
    return 0;
}

static inline struct passwd *getpwuid(uid_t uid)
{
    static struct passwd pw;
    static char user_name[256];
    static char home_dir[MAX_PATH];
    const char *env_user;
    const char *env_profile;
    const char *env_home_drive;
    const char *env_home_path;

    (void)uid;

    env_user = getenv("USERNAME");
    if (env_user == NULL || *env_user == '\0')
        env_user = "windows";
    snprintf(user_name, sizeof(user_name), "%s", env_user);

    env_profile = getenv("USERPROFILE");
    if (env_profile != NULL && *env_profile != '\0') {
        snprintf(home_dir, sizeof(home_dir), "%s", env_profile);
    } else {
        env_home_drive = getenv("HOMEDRIVE");
        env_home_path = getenv("HOMEPATH");
        if (env_home_drive != NULL && env_home_path != NULL &&
            env_home_drive[0] != '\0' && env_home_path[0] != '\0') {
            snprintf(home_dir, sizeof(home_dir), "%s%s",
                     env_home_drive, env_home_path);
        } else {
            snprintf(home_dir, sizeof(home_dir), "C:\\");
        }
    }

    pw.pw_name = user_name;
    pw.pw_dir = home_dir;
    return &pw;
}

static inline ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    (void)path;
    (void)buf;
    (void)bufsiz;
    errno = ENOSYS;
    return -1;
}

static inline const char *strcasestr(const char *haystack, const char *needle)
{
    size_t needle_len;

    if (haystack == NULL || needle == NULL)
        return NULL;
    if (*needle == '\0')
        return haystack;

    needle_len = strlen(needle);
    for (const char *p = haystack; *p != '\0'; p++) {
        if (_strnicmp(p, needle, needle_len) == 0)
            return p;
    }
    return NULL;
}

typedef void *iconv_t;

static inline iconv_t iconv_open(const char *tocode, const char *fromcode)
{
    (void)tocode;
    (void)fromcode;
    errno = EINVAL;
    return (iconv_t)-1;
}

static inline size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
                           char **outbuf, size_t *outbytesleft)
{
    (void)cd;
    (void)inbuf;
    (void)inbytesleft;
    (void)outbuf;
    (void)outbytesleft;
    errno = EINVAL;
    return (size_t)-1;
}

static inline int iconv_close(iconv_t cd)
{
    (void)cd;
    return 0;
}

typedef unsigned long nfds_t;

static inline int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    int rc = WSAPoll((LPWSAPOLLFD)fds, (ULONG)nfds, timeout);
    if (rc == SOCKET_ERROR)
        errno = WSAGetLastError();
    return rc;
}

typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef HANDLE pthread_t;
typedef void *pthread_attr_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int pthread_mutex_init(pthread_mutex_t *mu, const void *attr)
{
    (void)attr;
    InitializeSRWLock(mu);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mu)
{
    (void)mu;
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mu)
{
    AcquireSRWLockExclusive(mu);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mu)
{
    ReleaseSRWLockExclusive(mu);
    return 0;
}

static inline int pthread_cond_init(pthread_cond_t *cv, const void *attr)
{
    (void)attr;
    InitializeConditionVariable(cv);
    return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t *cv)
{
    (void)cv;
    return 0;
}

static inline int pthread_cond_signal(pthread_cond_t *cv)
{
    WakeConditionVariable(cv);
    return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t *cv)
{
    WakeAllConditionVariable(cv);
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mu)
{
    return SleepConditionVariableSRW(cv, mu, INFINITE, 0) ? 0 : -1;
}

static inline int clock_gettime(int clock_id, struct timespec *ts)
{
    FILETIME ft;
    ULARGE_INTEGER uli;
    uint64_t ticks;
    uint64_t unix_100ns;

    (void)clock_id;
    if (ts == NULL)
        return -1;

    GetSystemTimePreciseAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    ticks = uli.QuadPart;
    unix_100ns = ticks - 116444736000000000ULL;
    ts->tv_sec = (time_t)(unix_100ns / 10000000ULL);
    ts->tv_nsec = (long)((unix_100ns % 10000000ULL) * 100ULL);
    return 0;
}

static inline DWORD sqli_timespec_diff_ms(const struct timespec *deadline)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_REALTIME, &now);

    int64_t sec = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
    int64_t nsec = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
    int64_t total_ms = (sec * 1000) + (nsec / 1000000);
    if (nsec > 0 && (nsec % 1000000) != 0)
        total_ms++;
    if (total_ms < 0)
        total_ms = 0;
    if (total_ms > (int64_t)INFINITE - 1)
        total_ms = (int64_t)INFINITE - 1;
    return (DWORD)total_ms;
}

static inline int pthread_cond_timedwait(pthread_cond_t *cv, pthread_mutex_t *mu,
                                         const struct timespec *abstime)
{
    DWORD timeout_ms = sqli_timespec_diff_ms(abstime);
    if (SleepConditionVariableSRW(cv, mu, timeout_ms, 0))
        return 0;
    return (GetLastError() == ERROR_TIMEOUT) ? ETIMEDOUT : -1;
}

static inline int nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)rem;
    if (req == NULL)
        return -1;
    uint64_t ms = (uint64_t)req->tv_sec * 1000u;
    ms += (uint64_t)req->tv_nsec / 1000000u;
    if (req->tv_nsec % 1000000u)
        ms++;
    Sleep((DWORD)ms);
    return 0;
}

#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif

#endif /* SQLI_PLATFORM_WIN_H */
