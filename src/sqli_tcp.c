#define _GNU_SOURCE
#include "sqli_tcp.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <poll.h>
#include <fcntl.h>

#include <openssl/ssl.h>

#include "sqli_log.h"
#include "sqli_tls.h"

#include <string.h>
#include <pthread.h>

/* ----------------------------------------------------------------
 * Byte logging (hex dump) — enabled via SQLI_LOG_BYTES env var
 * ---------------------------------------------------------------- */

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

static void dump_hex_full(const char *label, const unsigned char *buf, size_t len)
{
    const char *env = getenv("SQLI_LOG_BYTES");
    if (!env || env[0] != '1')
        return;

    size_t i;
    for (i = 0; i < len; i += 32) {
        size_t chunk = (len - i) > 32 ? 32 : (len - i);
        fprintf(stderr, "[BYT %s+%04zx] ", label, i);
        size_t j;
        for (j = 0; j < chunk; j++)
            fprintf(stderr, "%02x ", buf[i + j]);
        fprintf(stderr, "\n");
    }
}

/* ----------------------------------------------------------------
 * Connection timeout (seconds)
 * ---------------------------------------------------------------- */

static const int IO_TIMEOUT_SEC = 10;

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

static int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen,
                                int timeout_sec)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
        return -1;

    int rc = connect(fd, addr, addrlen);
    if (rc == 0) {
        (void)fcntl(fd, F_SETFL, flags);
        return 0;
    }

    if (errno != EINPROGRESS) {
        (void)fcntl(fd, F_SETFL, flags);
        return -1;
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int prc = poll(&pfd, 1, timeout_sec * 1000);
    if (prc <= 0) {
        errno = ETIMEDOUT;
        (void)fcntl(fd, F_SETFL, flags);
        return -1;
    }

    int so_err = 0;
    socklen_t so_len = sizeof(so_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len) != 0) {
        (void)fcntl(fd, F_SETFL, flags);
        return -1;
    }
    if (so_err != 0) {
        errno = so_err;
        (void)fcntl(fd, F_SETFL, flags);
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags) != 0)
        return -1;
    return 0;
}

/* ----------------------------------------------------------------
 * sqli_tcp_connect
 * ---------------------------------------------------------------- */

int sqli_tcp_connect(const char *hostname, const char *service)
{
    if (hostname == NULL || service == NULL)
        return -1;

    struct addrinfo hints;
    struct addrinfo *result = NULL;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int rc = getaddrinfo(hostname, service, &hints, &result);
    if (rc != 0) {
        sqli_log(SQLI_LOG_ERROR, "getaddrinfo failed: %s", gai_strerror(rc));
        return -1;
    }

    const int io_timeout = read_timeout_from_env("SQLI_IO_TIMEOUT_SEC", IO_TIMEOUT_SEC);

    /* Try each address until we get a working socket */
    for (struct addrinfo *ai = result; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;

        /* Set timeout for connect */
        struct timeval tv;
        tv.tv_sec = io_timeout;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        /* Disable Nagle's algorithm for lower latency */
        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        if (connect_with_timeout(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen, io_timeout) == 0)
            break;

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);

    if (fd < 0) {
        sqli_log(SQLI_LOG_ERROR, "failed to connect to %s:%s (errno=%d)",
                 hostname, service, errno);
        return -1;
    }

    sqli_log(SQLI_LOG_INFO, "TCP connection established to %s:%s",
             hostname, service);
    return fd;
}

/* ----------------------------------------------------------------
 * sqli_tcp_close
 * ---------------------------------------------------------------- */

void sqli_tcp_close(int fd)
{
    if (fd < 0)
        return;

    sqli_tcp_tls_detach(fd);

    if (close(fd) != 0) {
        sqli_log(SQLI_LOG_WARN, "close(fd=%d) failed: %s", fd, strerror(errno));
    }
}

/* ----------------------------------------------------------------
 * sqli_tcp_read
 * ---------------------------------------------------------------- */

ssize_t sqli_tcp_read(int fd, unsigned char *buf, size_t count)
{
    if (fd < 0 || buf == NULL || count == 0)
        return -1;

    bool tls_seen = false;
    size_t total = 0;

    while (total < count) {
        ssize_t n;
        int ssl_err = 0;
        bool use_tls = false;
        void *ssl_ptr = NULL;

        pthread_mutex_lock(&g_tls_mutex);
        tls_entry *te = find_tls_entry(fd);
        if (te != NULL) {
            ssl_ptr = te->ssl;
            tls_seen = true;
            use_tls = true;
        }
        pthread_mutex_unlock(&g_tls_mutex);

        if (use_tls) {
            n = SSL_read((SSL *)ssl_ptr, buf + total, (int)(count - total));
            if (n <= 0) {
                ssl_err = SSL_get_error((SSL *)ssl_ptr, (int)n);
            }
        }

        if (use_tls) {
            if (n <= 0) {
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                    struct pollfd pfd;
                    pfd.fd = fd;
                    pfd.events = (ssl_err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                    int io_timeout = read_timeout_from_env("SQLI_IO_TIMEOUT_SEC", IO_TIMEOUT_SEC);
                    int prc = poll(&pfd, 1, io_timeout * 1000);
                    if (prc <= 0) {
                        sqli_log(SQLI_LOG_ERROR, "tls read timeout");
                        return -1;
                    }
                    continue;
                }
                sqli_log(SQLI_LOG_ERROR, "tls read failed (ssl_err=%d)", ssl_err);
                return -1;
            }
        } else {
            if (tls_seen) {
                sqli_log(SQLI_LOG_ERROR, "tls context detached during read");
                errno = ECONNRESET;
                return -1;
            }
            n = read(fd, buf + total, count - total);
            if (n < 0) {
                if (errno == EINTR)
                    continue;  /* retry on signal */
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    sqli_log(SQLI_LOG_ERROR, "socket read timeout");
                    return -1;
                }
                sqli_log(SQLI_LOG_ERROR, "socket read failed: %s", strerror(errno));
                return -1;
            }
        }
        if (n == 0)
            break;  /* EOF */

        total += (size_t)n;
    }

    if (total > 0)
        dump_hex("READ", buf, total);

    return (ssize_t)total;
}

ssize_t sqli_tcp_read_some(int fd, unsigned char *buf, size_t count)
{
    if (fd < 0 || buf == NULL || count == 0)
        return -1;

    bool tls_seen = false;

    for (;;) {
        ssize_t n;
        int ssl_err = 0;
        bool use_tls = false;
        void *ssl_ptr = NULL;

        pthread_mutex_lock(&g_tls_mutex);
        tls_entry *te = find_tls_entry(fd);
        if (te != NULL) {
            ssl_ptr = te->ssl;
            tls_seen = true;
            use_tls = true;
        }
        pthread_mutex_unlock(&g_tls_mutex);

        if (use_tls) {
            n = SSL_read((SSL *)ssl_ptr, buf, (int)count);
            if (n <= 0) {
                ssl_err = SSL_get_error((SSL *)ssl_ptr, (int)n);
            }
        }

        if (use_tls) {
            if (n <= 0) {
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                    struct pollfd pfd;
                    pfd.fd = fd;
                    pfd.events = (ssl_err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                    int io_timeout = read_timeout_from_env("SQLI_IO_TIMEOUT_SEC", IO_TIMEOUT_SEC);
                    int prc = poll(&pfd, 1, io_timeout * 1000);
                    if (prc <= 0) {
                        sqli_log(SQLI_LOG_ERROR, "tls read timeout");
                        return -1;
                    }
                    continue;
                }
                sqli_log(SQLI_LOG_ERROR, "tls read failed (ssl_err=%d)", ssl_err);
                return -1;
            }
        } else {
            if (tls_seen) {
                sqli_log(SQLI_LOG_ERROR, "tls context detached during read");
                errno = ECONNRESET;
                return -1;
            }
            n = read(fd, buf, count);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    sqli_log(SQLI_LOG_ERROR, "socket read timeout");
                    return -1;
                }
                sqli_log(SQLI_LOG_ERROR, "socket read failed: %s", strerror(errno));
                return -1;
            }
        }

        if (n > 0)
            dump_hex("READ", buf, (size_t)n);
        return n; /* includes EOF (0) */
    }
}

/* ----------------------------------------------------------------
 * sqli_tcp_send
 * ---------------------------------------------------------------- */

ssize_t sqli_tcp_send(int fd, const unsigned char *buf, size_t count)
{
    if (fd < 0 || buf == NULL || count == 0)
        return -1;

    bool tls_seen = false;
    size_t total = 0;

    while (total < count) {
        ssize_t n;
        int ssl_err = 0;
        bool use_tls = false;
        void *ssl_ptr = NULL;

        pthread_mutex_lock(&g_tls_mutex);
        tls_entry *te = find_tls_entry(fd);
        if (te != NULL) {
            ssl_ptr = te->ssl;
            tls_seen = true;
            use_tls = true;
        }
        pthread_mutex_unlock(&g_tls_mutex);

        if (use_tls) {
            n = SSL_write((SSL *)ssl_ptr, buf + total, (int)(count - total));
            if (n <= 0) {
                ssl_err = SSL_get_error((SSL *)ssl_ptr, (int)n);
            }
        }

        if (use_tls) {
            if (n <= 0) {
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                    struct pollfd pfd;
                    pfd.fd = fd;
                    pfd.events = (ssl_err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                    int io_timeout = read_timeout_from_env("SQLI_IO_TIMEOUT_SEC", IO_TIMEOUT_SEC);
                    int prc = poll(&pfd, 1, io_timeout * 1000);
                    if (prc <= 0) {
                        sqli_log(SQLI_LOG_ERROR, "tls write timeout");
                        return -1;
                    }
                    continue;
                }
                sqli_log(SQLI_LOG_ERROR, "tls write failed (ssl_err=%d)", ssl_err);
                return -1;
            }
        } else {
            if (tls_seen) {
                sqli_log(SQLI_LOG_ERROR, "tls context detached during send");
                errno = ECONNRESET;
                return -1;
            }
            n = write(fd, buf + total, count - total);
            if (n < 0) {
                if (errno == EINTR)
                    continue;  /* retry on signal */
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    sqli_log(SQLI_LOG_ERROR, "socket write timeout");
                    return -1;
                }
                sqli_log(SQLI_LOG_ERROR, "socket write failed: %s", strerror(errno));
                return -1;
            }
        }
        if (n == 0) {
            sqli_log(SQLI_LOG_ERROR, "socket write returned 0 (EOF?)");
            break;  /* prevent infinite loop */
        }
        dump_hex_full("SEND", buf + total, (size_t)n);
        total += (size_t)n;
    }

    return (ssize_t)total;
}

ssize_t sqli_tcp_peek(int fd, unsigned char *buf, size_t count)
{
    if (fd < 0 || buf == NULL || count == 0)
        return -1;

    void *ssl_ptr = NULL;
    pthread_mutex_lock(&g_tls_mutex);
    tls_entry *te = find_tls_entry(fd);
    if (te != NULL) {
        ssl_ptr = te->ssl;
    }
    pthread_mutex_unlock(&g_tls_mutex);

    if (ssl_ptr != NULL) {
        if (SSL_pending((SSL *)ssl_ptr) <= 0) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            int prc = poll(&pfd, 1, 0);
            if (prc <= 0 || !(pfd.revents & POLLIN)) {
                errno = EAGAIN;
                return -1;
            }
        }
        int n = SSL_peek((SSL *)ssl_ptr, buf, (int)count);
        if (n <= 0) {
            int ssl_err = SSL_get_error((SSL *)ssl_ptr, n);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                errno = EAGAIN;
                return -1;
            }
            return -1;
        }
        return (ssize_t)n;
    }

    return recv(fd, buf, count, MSG_PEEK | MSG_DONTWAIT);
}
