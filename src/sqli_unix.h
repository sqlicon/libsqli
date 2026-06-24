#ifndef SQLI_UNIX_H
#define SQLI_UNIX_H

#include <stddef.h>

/*
 * Open a connection to a Unix domain socket.
 *
 * Returns a non-negative file descriptor on success, -1 on error.
 * The caller is responsible for calling sqli_tcp_close() when done.
 *
 * Connection timeout: 10 seconds (configurable via SQLI_IO_TIMEOUT_SEC).
 */
int sqli_unix_connect(const char *socket_path);

#endif /* SQLI_UNIX_H */
