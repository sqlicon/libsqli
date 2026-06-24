#ifndef SQLI_TCP_H
#define SQLI_TCP_H

/*
 * TCP socket abstraction for the SQLI protocol.
 *
 * This module provides a thin wrapper around POSIX sockets.
 * The interface is designed to be mockable for unit testing.
 */

#include <sys/types.h>
#include <stddef.h>

/*
 * Open a TCP connection to the given host:service.
 *
 * Returns a non-negative file descriptor on success, -1 on error.
 * The caller is responsible for calling sqli_tcp_close() when done.
 *
 * Connection timeout: 10 seconds (configurable via environment).
 */
int sqli_tcp_connect(const char *hostname, const char *service);

/*
 * Close a TCP socket.
 * Safe to call with fd == -1 (no-op).
 */
void sqli_tcp_close(int fd);

/*
 * Read exactly `count` bytes from the socket.
 *
 * Returns:
 *   > 0   — number of bytes actually read (will be exactly `count`)
 *   0     — connection closed by peer (EOF)
 *   -1    — error (errno set)
 *
 * Blocks until all bytes are received or an error/EOF occurs.
 */
ssize_t sqli_tcp_read(int fd, unsigned char *buf, size_t count);

/* Read up to count bytes with a single transport read operation.
 * Returns:
 *   >0 number of bytes read
 *    0 on EOF
 *   -1 on timeout/error
 */
ssize_t sqli_tcp_read_some(int fd, unsigned char *buf, size_t count);

/*
 * Send all bytes to the socket.
 *
 * Returns:
 *   > 0   — number of bytes actually sent (will be exactly `count`)
 *   -1    — error (errno set)
 *
 * Retries on partial writes and EINTR.
 */
ssize_t sqli_tcp_send(int fd, const unsigned char *buf, size_t count);

/*
 * Peek bytes without consuming them.
 * Returns:
 *   >= 0 number of bytes peeked
 *   -1 on would-block / timeout / error
 */
ssize_t sqli_tcp_peek(int fd, unsigned char *buf, size_t count);

#endif /* SQLI_TCP_H */
