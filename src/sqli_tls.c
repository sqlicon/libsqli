#define _GNU_SOURCE
#include "sqli_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <errno.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "sqli_internal.h"
#include "sqli_log.h"

/* ----------------------------------------------------------------
 * TLS session registry
 * ---------------------------------------------------------------- */

tls_entry *g_tls_entries = NULL;
pthread_mutex_t g_tls_mutex = PTHREAD_MUTEX_INITIALIZER;

tls_entry *find_tls_entry(int fd)
{
    for (tls_entry *e = g_tls_entries; e != NULL; e = e->next) {
        if (e->fd == fd)
            return e;
    }
    return NULL;
}

int sqli_tcp_tls_attach(int fd, void *ssl_ctx, void *ssl)
{
    if (fd < 0 || ssl_ctx == NULL || ssl == NULL)
        return -1;

    pthread_mutex_lock(&g_tls_mutex);
    tls_entry *existing = find_tls_entry(fd);
    if (existing != NULL) {
        SSL_shutdown(existing->ssl);
        SSL_free(existing->ssl);
        SSL_CTX_free(existing->ctx);
        existing->ctx = ssl_ctx;
        existing->ssl = ssl;
        pthread_mutex_unlock(&g_tls_mutex);
        return 0;
    }

    tls_entry *e = calloc(1, sizeof(*e));
    if (e == NULL) {
        pthread_mutex_unlock(&g_tls_mutex);
        return -1;
    }
    e->fd = fd;
    e->ctx = ssl_ctx;
    e->ssl = ssl;
    e->next = g_tls_entries;
    g_tls_entries = e;
    pthread_mutex_unlock(&g_tls_mutex);
    return 0;
}

void sqli_tcp_tls_detach(int fd)
{
    pthread_mutex_lock(&g_tls_mutex);
    tls_entry *prev = NULL;
    tls_entry *cur = g_tls_entries;
    while (cur != NULL) {
        if (cur->fd == fd) {
            if (prev != NULL)
                prev->next = cur->next;
            else
                g_tls_entries = cur->next;
            SSL_shutdown(cur->ssl);
            SSL_free(cur->ssl);
            SSL_CTX_free(cur->ctx);
            free(cur);
            pthread_mutex_unlock(&g_tls_mutex);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
    pthread_mutex_unlock(&g_tls_mutex);
}

/* ----------------------------------------------------------------
 * TLS connection setup (SSL_CTX + handshake + attach)
 * ---------------------------------------------------------------- */

static bool parse_bool_env(const char *v)
{
    if (v == NULL)
        return false;
    return strcasecmp(v, "1") == 0 ||
           strcasecmp(v, "true") == 0 ||
           strcasecmp(v, "yes") == 0 ||
           strcasecmp(v, "on") == 0;
}

sqli_status sqli_tls_connect(sqli_conn_t *c, const sqli_connect_params *params)
{
    const bool env_ssl = parse_bool_env(getenv("SQLI_SSL_ENABLE"));
    const bool ssl_enable = params->ssl_enable || env_ssl;
    if (!ssl_enable)
        return SQLI_OK;

    bool ssl_verify = params->ssl_verify_peer;
    if (!ssl_verify)
        ssl_verify = parse_bool_env(getenv("SQLI_SSL_VERIFY"));

    const char *ca_file = params->ssl_ca_file;
    if (ca_file == NULL || ca_file[0] == '\0')
        ca_file = getenv("SQLI_SSL_CA_FILE");

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
        set_error_context(c, "connect/tls_ctx", 0);
        set_error(c, "failed to create TLS context");
        return SQLI_IO_ERROR;
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (ssl_verify) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        if (ca_file != NULL && ca_file[0] != '\0') {
            if (SSL_CTX_load_verify_locations(ctx, ca_file, NULL) != 1) {
                SSL_CTX_free(ctx);
                set_error_context(c, "connect/tls_ca", 0);
                set_error(c, "failed to load TLS CA file");
                return SQLI_IO_ERROR;
            }
        } else {
            if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
                SSL_CTX_free(ctx);
                set_error_context(c, "connect/tls_ca", 0);
                set_error(c, "failed to load default TLS CA paths");
                return SQLI_IO_ERROR;
            }
        }
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }

    SSL *ssl = SSL_new(ctx);
    if (ssl == NULL) {
        SSL_CTX_free(ctx);
        set_error_context(c, "connect/tls_ssl", 0);
        set_error(c, "failed to create TLS session");
        return SQLI_IO_ERROR;
    }

    if (SSL_set_fd(ssl, c->socket_fd) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        set_error_context(c, "connect/tls_fd", 0);
        set_error(c, "failed to bind TLS session to socket");
        return SQLI_IO_ERROR;
    }

    if (ssl_verify && c->hostname[0] != '\0') {
        if (SSL_set1_host(ssl, c->hostname) != 1) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            set_error_context(c, "connect/tls_host", 0);
            set_error(c, "failed to set TLS verification hostname");
            return SQLI_IO_ERROR;
        }
    }

    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        set_error_context(c, "connect/tls_handshake", 0);
        set_error(c, "TLS handshake failed");
        return SQLI_IO_ERROR;
    }

    if (sqli_tcp_tls_attach(c->socket_fd, ctx, ssl) != 0) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        set_error_context(c, "connect/tls_attach", 0);
        set_error(c, "failed to attach TLS transport");
        return SQLI_IO_ERROR;
    }

    sqli_log(SQLI_LOG_INFO, "TLS transport established (verify=%d)", ssl_verify ? 1 : 0);
    return SQLI_OK;
}
