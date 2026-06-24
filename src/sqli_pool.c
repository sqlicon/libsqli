#define _POSIX_C_SOURCE 200809L
#include "sqli_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

typedef struct {
    sqli_conn_t *conn;
    bool in_use;
} sqli_pool_slot;

struct sqli_pool {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool shutting_down;
    size_t size;
    sqli_pool_slot *slots;
    sqli_connect_params params;
};

static void free_pool_params(sqli_pool_t *pool)
{
    if (pool == NULL)
        return;
    free((void *)pool->params.server);
    free((void *)pool->params.hostname);
    free((void *)pool->params.service);
    free((void *)pool->params.database);
    free((void *)pool->params.username);
    if (pool->params.password != NULL) {
        memset((void *)pool->params.password, 0, strlen(pool->params.password));
        free((void *)pool->params.password);
    }
    free((void *)pool->params.client_locale);
    free((void *)pool->params.db_locale);
    free((void *)pool->params.ssl_ca_file);
    memset(&pool->params, 0, sizeof(pool->params));
}

static char *dup_nullable(const char *s)
{
    if (s == NULL)
        return NULL;
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d == NULL)
        return NULL;
    memcpy(d, s, n + 1);
    return d;
}

static sqli_status copy_pool_params(sqli_pool_t *pool, const sqli_connect_params *in)
{
    memset(&pool->params, 0, sizeof(pool->params));
    pool->params.server = dup_nullable(in->server);
    pool->params.hostname = dup_nullable(in->hostname);
    pool->params.service = dup_nullable(in->service);
    pool->params.database = dup_nullable(in->database);
    pool->params.username = dup_nullable(in->username);
    pool->params.password = dup_nullable(in->password);
    pool->params.client_locale = dup_nullable(in->client_locale);
    pool->params.db_locale = dup_nullable(in->db_locale);
    pool->params.ssl_ca_file = dup_nullable(in->ssl_ca_file);
    pool->params.ssl_enable = in->ssl_enable;
    pool->params.ssl_verify_peer = in->ssl_verify_peer;

    if ((in->server != NULL && pool->params.server == NULL) ||
        (in->hostname != NULL && pool->params.hostname == NULL) ||
        (in->service != NULL && pool->params.service == NULL) ||
        (in->database != NULL && pool->params.database == NULL) ||
        (in->username != NULL && pool->params.username == NULL) ||
        (in->password != NULL && pool->params.password == NULL) ||
        (in->client_locale != NULL && pool->params.client_locale == NULL) ||
        (in->db_locale != NULL && pool->params.db_locale == NULL) ||
        (in->ssl_ca_file != NULL && pool->params.ssl_ca_file == NULL)) {
        free_pool_params(pool);
        return SQLI_ALLOC_FAIL;
    }

    return SQLI_OK;
}

static sqli_status ensure_slot_connected(sqli_pool_t *pool, size_t idx)
{
    if (pool == NULL || idx >= pool->size)
        return SQLI_INVALID_STATE;

    sqli_pool_slot *slot = &pool->slots[idx];
    if (slot->conn != NULL &&
        slot->conn->state == SQLI_CONN_READY &&
        slot->conn->socket_fd >= 0) {
        return SQLI_OK;
    }

    if (slot->conn != NULL) {
        sqli_close(slot->conn);
        sqli_destroy(slot->conn);
        slot->conn = NULL;
    }

    sqli_conn_t *conn = NULL;
    sqli_status rc = sqli_create(&conn);
    if (rc != SQLI_OK)
        return rc;

    rc = sqli_connect(conn, &pool->params);
    if (rc != SQLI_OK) {
        sqli_destroy(conn);
        return rc;
    }

    slot->conn = conn;
    return SQLI_OK;
}

static sqli_status acquire_from_pool_locked(sqli_pool_t *pool, sqli_conn_t **conn)
{
    for (size_t i = 0; i < pool->size; i++) {
        if (pool->slots[i].in_use)
            continue;

        sqli_status rc = ensure_slot_connected(pool, i);
        if (rc != SQLI_OK)
            continue;

        pool->slots[i].in_use = true;
        *conn = pool->slots[i].conn;
        return SQLI_OK;
    }
    return SQLI_TIMEOUT;
}

sqli_status sqli_pool_create(sqli_pool_t **pool,
                             const sqli_connect_params *params,
                             size_t pool_size)
{
    if (pool == NULL || params == NULL || pool_size == 0)
        return SQLI_INVALID_STATE;
    *pool = NULL;

    sqli_pool_t *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return SQLI_ALLOC_FAIL;

    p->size = pool_size;
    p->slots = calloc(pool_size, sizeof(*p->slots));
    if (p->slots == NULL) {
        free(p);
        return SQLI_ALLOC_FAIL;
    }

    if (pthread_mutex_init(&p->mu, NULL) != 0 ||
        pthread_cond_init(&p->cv, NULL) != 0) {
        free(p->slots);
        free(p);
        return SQLI_ERR;
    }

    sqli_status rc = copy_pool_params(p, params);
    if (rc != SQLI_OK) {
        pthread_cond_destroy(&p->cv);
        pthread_mutex_destroy(&p->mu);
        free(p->slots);
        free(p);
        return rc;
    }

    for (size_t i = 0; i < pool_size; i++) {
        sqli_conn_t *conn = NULL;
        rc = sqli_create(&conn);
        if (rc != SQLI_OK) {
            sqli_pool_destroy(p);
            return rc;
        }
        rc = sqli_connect(conn, &p->params);
        if (rc != SQLI_OK) {
            sqli_destroy(conn);
            sqli_pool_destroy(p);
            return rc;
        }
        p->slots[i].conn = conn;
    }

    *pool = p;
    return SQLI_OK;
}

sqli_status sqli_pool_acquire(sqli_pool_t *pool, sqli_conn_t **conn)
{
    return sqli_pool_acquire_timeout(pool, conn, UINT32_MAX);
}

sqli_status sqli_pool_try_acquire(sqli_pool_t *pool, sqli_conn_t **conn)
{
    return sqli_pool_acquire_timeout(pool, conn, 0);
}

sqli_status sqli_pool_acquire_timeout(sqli_pool_t *pool, sqli_conn_t **conn,
                                      uint32_t timeout_ms)
{
    if (pool == NULL || conn == NULL)
        return SQLI_INVALID_STATE;
    *conn = NULL;

    if (pthread_mutex_lock(&pool->mu) != 0)
        return SQLI_ERR;

    struct timespec abs_deadline;
    bool use_timeout = (timeout_ms != UINT32_MAX);
    if (use_timeout && timeout_ms > 0) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        uint64_t nsec = (uint64_t)now.tv_nsec + ((uint64_t)(timeout_ms % 1000u) * 1000000ull);
        abs_deadline.tv_sec = now.tv_sec + (time_t)(timeout_ms / 1000u) + (time_t)(nsec / 1000000000ull);
        abs_deadline.tv_nsec = (long)(nsec % 1000000000ull);
    }

    while (!pool->shutting_down) {
        sqli_status rc = acquire_from_pool_locked(pool, conn);
        if (rc == SQLI_OK) {
            pthread_mutex_unlock(&pool->mu);
            return SQLI_OK;
        }

        if (!use_timeout) {
            pthread_cond_wait(&pool->cv, &pool->mu);
            continue;
        }
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&pool->mu);
            return SQLI_TIMEOUT;
        }

        int wrc = pthread_cond_timedwait(&pool->cv, &pool->mu, &abs_deadline);
        if (wrc == ETIMEDOUT) {
            pthread_mutex_unlock(&pool->mu);
            return SQLI_TIMEOUT;
        }
    }

    pthread_mutex_unlock(&pool->mu);
    return SQLI_INVALID_STATE;
}

sqli_status sqli_pool_release(sqli_pool_t *pool, sqli_conn_t *conn)
{
    if (pool == NULL || conn == NULL)
        return SQLI_INVALID_STATE;
    if (pthread_mutex_lock(&pool->mu) != 0)
        return SQLI_ERR;

    for (size_t i = 0; i < pool->size; i++) {
        if (pool->slots[i].conn == conn) {
            if (pool->slots[i].conn->state != SQLI_CONN_READY ||
                pool->slots[i].conn->socket_fd < 0) {
                (void)ensure_slot_connected(pool, i);
            }
            pool->slots[i].in_use = false;
            pthread_cond_signal(&pool->cv);
            pthread_mutex_unlock(&pool->mu);
            return SQLI_OK;
        }
    }

    pthread_mutex_unlock(&pool->mu);
    return SQLI_INVALID_STATE;
}

void sqli_pool_destroy(sqli_pool_t *pool)
{
    if (pool == NULL)
        return;

    pthread_mutex_lock(&pool->mu);
    pool->shutting_down = true;
    pthread_cond_broadcast(&pool->cv);
    pthread_mutex_unlock(&pool->mu);

    if (pool->slots != NULL) {
        for (size_t i = 0; i < pool->size; i++) {
            if (pool->slots[i].conn != NULL) {
                sqli_close(pool->slots[i].conn);
                sqli_destroy(pool->slots[i].conn);
                pool->slots[i].conn = NULL;
            }
            pool->slots[i].in_use = false;
        }
        free(pool->slots);
    }

    free_pool_params(pool);
    pthread_cond_destroy(&pool->cv);
    pthread_mutex_destroy(&pool->mu);
    free(pool);
}
