#define _GNU_SOURCE
#include "sqli_unix.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "sqli_log.h"

static int read_timeout_from_env(const char *name, int fallback)
{
    const char *v = getenv(name);
    if (v == NULL || *v == '\0')
        return fallback;

    char *end = NULL;
    long parsed = strtol(v, &end, 10);
    if (end == v || *end != '\0')
        return fallback;
    if (parsed < 1 || parsed > INT_MAX)
        return fallback;
    return (int)parsed;
}

static const int IO_TIMEOUT_SEC = 10;

int sqli_unix_connect(const char *socket_path)
{
    if (socket_path == NULL || socket_path[0] != '/')
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    size_t path_len = strlen(socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
        sqli_log(SQLI_LOG_ERROR, "Unix socket path too long: %s", socket_path);
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        sqli_log(SQLI_LOG_ERROR, "failed to create Unix socket: %s", strerror(errno));
        return -1;
    }

    const int io_timeout = read_timeout_from_env("SQLI_IO_TIMEOUT_SEC", IO_TIMEOUT_SEC);
    struct timeval tv;
    tv.tv_sec = io_timeout;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memcpy(addr.sun_path, socket_path, path_len);

    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        sqli_log(SQLI_LOG_ERROR, "failed to connect to Unix socket %s: %s",
                 socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    sqli_log(SQLI_LOG_INFO, "Unix socket connection established to %s", socket_path);
    return fd;
}
