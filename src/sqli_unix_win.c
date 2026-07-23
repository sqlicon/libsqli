#include "sqli_unix.h"

#include <errno.h>

#include "sqli_log.h"

int sqli_unix_connect(const char *socket_path)
{
    (void)socket_path;
    errno = ENOTSUP;
    sqli_log(SQLI_LOG_ERROR, "Unix domain sockets are not supported on Windows");
    return -1;
}
