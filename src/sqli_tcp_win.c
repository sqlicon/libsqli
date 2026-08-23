#include "sqli_tcp.h"

#include <openssl/ssl.h>

#include "sqli_log.h"
#include "sqli_tls.h"

#include <stdlib.h>
#include <limits.h>

static const int IO_TIMEOUT_SEC = 60;

/* See sqli_tcp_posix.c: bound the per-address connect timeout for
 * non-final getaddrinfo(AF_UNSPEC) candidates so a stale/unreachable
 * address doesn't make an eventually-successful connect look hung. */
static const int MULTI_ADDR_TIMEOUT_SEC = 5;

static INIT_ONCE g_winsock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK sqli_winsock_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)param;
    (void)ctx;

    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

static int sqli_winsock_init(void)
{
    return InitOnceExecuteOnce(&g_winsock_once, sqli_winsock_init_once, NULL, NULL) ? 0 : -1;
}

static void dump_hex_full(const char *label, const unsigned char *buf, size_t len)
{
    const char *env = getenv("SQLI_LOG_BYTES");
    if (!env || env[0] != '1')
        return;

    for (size_t i = 0; i < len; i += 32) {
        size_t chunk = (len - i) > 32 ? 32 : (len - i);
        fprintf(stderr, "[BYT %s+%04zx] ", label, i);
        for (size_t j = 0; j < chunk; j++)
            fprintf(stderr, "%02x ", buf[i + j]);
        fprintf(stderr, "\n");
    }
}

static void dump_hex(const char *label, const unsigned char *buf, size_t len)
{
    const char *env = getenv("SQLI_LOG_BYTES");
    if (!env || env[0] != '1')
        return;

    fprintf(stderr, "[BYT %s] ", label);
    for (size_t i = 0; i < len && i < 256; i++) {
        if (i > 0 && i % 32 == 0)
            fprintf(stderr, "\n[BYT       ] ");
        fprintf(stderr, "%02x ", buf[i]);
    }
    fprintf(stderr, "\n");
}

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

static int connect_with_timeout(SOCKET fd, const struct sockaddr *addr, socklen_t addrlen,
                                int timeout_sec)
{
    u_long nonblocking = 1;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0)
        return -1;

    int rc = connect(fd, addr, addrlen);
    if (rc == 0) {
        nonblocking = 0;
        (void)ioctlsocket(fd, FIONBIO, &nonblocking);
        return 0;
    }

    int wsa_err = WSAGetLastError();
    if (wsa_err != WSAEWOULDBLOCK && wsa_err != WSAEINPROGRESS) {
        errno = wsa_err;
        return -1;
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int prc = poll(&pfd, 1, timeout_sec * 1000);
    if (prc <= 0) {
        errno = ETIMEDOUT;
        return -1;
    }

    int so_err = 0;
    socklen_t so_len = (socklen_t)sizeof(so_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&so_err, &so_len) != 0) {
        errno = WSAGetLastError();
        return -1;
    }
    if (so_err != 0) {
        errno = so_err;
        return -1;
    }

    nonblocking = 0;
    (void)ioctlsocket(fd, FIONBIO, &nonblocking);
    return 0;
}

int sqli_tcp_connect(const char *hostname, const char *service)
{
    if (hostname == NULL || service == NULL)
        return -1;
    if (sqli_winsock_init() != 0)
        return -1;

    struct addrinfo hints;
    struct addrinfo *result = NULL;
    SOCKET fd = INVALID_SOCKET;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int rc = getaddrinfo(hostname, service, &hints, &result);
    if (rc != 0) {
        sqli_log(SQLI_LOG_ERROR, "getaddrinfo failed: %d", rc);
        return -1;
    }

    const int io_timeout = read_timeout_from_env("SQLI_IO_TIMEOUT_SEC", IO_TIMEOUT_SEC);
    const int multi_addr_timeout = read_timeout_from_env("SQLI_MULTI_ADDR_TIMEOUT_SEC",
                                                          MULTI_ADDR_TIMEOUT_SEC);

    for (struct addrinfo *ai = result; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == INVALID_SOCKET)
            continue;

        DWORD timeout_ms = (DWORD)(io_timeout * 1000);
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));

        int flag = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));

        int connect_timeout = (ai->ai_next != NULL && multi_addr_timeout < io_timeout)
                                   ? multi_addr_timeout : io_timeout;
        if (connect_with_timeout(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen, connect_timeout) == 0)
            break;

        closesocket(fd);
        fd = INVALID_SOCKET;
    }

    freeaddrinfo(result);

    if (fd == INVALID_SOCKET) {
        sqli_log(SQLI_LOG_ERROR, "failed to connect to %s:%s (wsa=%d)",
                 hostname, service, WSAGetLastError());
        return -1;
    }

    sqli_log(SQLI_LOG_INFO, "TCP connection established to %s:%s", hostname, service);
    return (int)fd;
}

void sqli_tcp_close(int fd)
{
    if (fd < 0)
        return;

    sqli_tcp_tls_detach(fd);
    if (closesocket((SOCKET)fd) != 0)
        sqli_log(SQLI_LOG_WARN, "closesocket(fd=%d) failed", fd);
}

static ssize_t sqli_socket_recv_loop(int fd, unsigned char *buf, size_t count, bool exact)
{
    size_t total = 0;

    while (total < count) {
        int n = recv((SOCKET)fd, (char *)buf + total, (int)(count - total), 0);
        if (n == 0)
            break;
        if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAEINTR)
                continue;
            if (err == WSAEWOULDBLOCK) {
                sqli_log(SQLI_LOG_ERROR, "socket read timeout");
                return -1;
            }
            errno = err;
            sqli_log(SQLI_LOG_ERROR, "socket read failed: %d", err);
            return -1;
        }
        total += (size_t)n;
        if (!exact)
            break;
    }

    if (total > 0)
        dump_hex("READ", buf, total);
    return (ssize_t)total;
}

ssize_t sqli_tcp_read(int fd, unsigned char *buf, size_t count)
{
    if (fd < 0 || buf == NULL || count == 0)
        return -1;
    return sqli_socket_recv_loop(fd, buf, count, true);
}

ssize_t sqli_tcp_read_some(int fd, unsigned char *buf, size_t count)
{
    if (fd < 0 || buf == NULL || count == 0)
        return -1;
    return sqli_socket_recv_loop(fd, buf, count, false);
}

ssize_t sqli_tcp_send(int fd, const unsigned char *buf, size_t count)
{
    if (fd < 0 || buf == NULL || count == 0)
        return -1;

    size_t total = 0;
    while (total < count) {
        int n = send((SOCKET)fd, (const char *)buf + total, (int)(count - total), 0);
        if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAEINTR)
                continue;
            if (err == WSAEWOULDBLOCK) {
                sqli_log(SQLI_LOG_ERROR, "socket write timeout");
                return -1;
            }
            errno = err;
            sqli_log(SQLI_LOG_ERROR, "socket write failed: %d", err);
            return -1;
        }
        if (n == 0) {
            sqli_log(SQLI_LOG_ERROR, "socket write returned 0 (EOF?)");
            break;
        }
        dump_hex_full("SEND", buf + total, (size_t)n);
        total += (size_t)n;
    }
    return (ssize_t)total;
}

ssize_t sqli_tcp_peek(int fd, unsigned char *buf, size_t count)
{
    struct pollfd pfd;
    int prc;

    if (fd < 0 || buf == NULL || count == 0)
        return -1;

    pfd.fd = (SOCKET)fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    prc = poll(&pfd, 1, 0);
    if (prc <= 0 || (pfd.revents & POLLIN) == 0) {
        errno = EAGAIN;
        return -1;
    }

    return recv((SOCKET)fd, (char *)buf, (int)count, MSG_PEEK);
}
