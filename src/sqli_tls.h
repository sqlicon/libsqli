#ifndef SQLI_TLS_H
#define SQLI_TLS_H

#include <stddef.h>
#include <stdint.h>

#include "libsqli/sqli.h"

/* TLS session registry (shared with sqli_tcp.c for I/O dispatch) */

#include <pthread.h>

typedef struct tls_entry {
    int fd;
    void *ctx;      /* SSL_CTX */
    void *ssl;      /* SSL */
    struct tls_entry *next;
} tls_entry;

extern tls_entry *g_tls_entries;
extern pthread_mutex_t g_tls_mutex;

tls_entry *find_tls_entry(int fd);
int sqli_tcp_tls_attach(int fd, void *ssl_ctx, void *ssl);
void sqli_tcp_tls_detach(int fd);

/* TLS connection setup */

sqli_status sqli_tls_connect(sqli_conn_t *conn, const sqli_connect_params *params);

#endif /* SQLI_TLS_H */
