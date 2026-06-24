#define _POSIX_C_SOURCE 200809L
#include "libsqli/sqli.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    atomic_uint ok_queries;
    atomic_uint failed_queries;
    atomic_uint auth_failures;
    atomic_uint network_failures;
} stress_stats_t;

typedef struct {
    const sqli_connect_params *params;
    sqli_pool_t *pool;
    const char *sql;
    unsigned iterations;
    uint32_t retries;
    stress_stats_t *stats;
} worker_ctx_t;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void classify_error(sqli_conn_t *conn, stress_stats_t *stats)
{
    sqli_error_class klass = SQLI_ERROR_CLASS_UNKNOWN;
    bool retryable = false;
    if (sqli_error_classify(conn, &klass, &retryable) != SQLI_OK)
        return;
    (void)retryable;
    if (klass == SQLI_ERROR_CLASS_AUTH)
        atomic_fetch_add_explicit(&stats->auth_failures, 1u, memory_order_relaxed);
    else if (klass == SQLI_ERROR_CLASS_NETWORK)
        atomic_fetch_add_explicit(&stats->network_failures, 1u, memory_order_relaxed);
}

static void *worker_run(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    for (unsigned i = 0; i < ctx->iterations; i++) {
        sqli_conn_t *conn = NULL;
        bool pooled = (ctx->pool != NULL);

        if (pooled) {
            if (sqli_pool_acquire_timeout(ctx->pool, &conn, 5000) != SQLI_OK || conn == NULL) {
                atomic_fetch_add_explicit(&ctx->stats->failed_queries, 1u, memory_order_relaxed);
                continue;
            }
        } else {
            if (sqli_create(&conn) != SQLI_OK || conn == NULL) {
                atomic_fetch_add_explicit(&ctx->stats->failed_queries, 1u, memory_order_relaxed);
                continue;
            }
            if (sqli_connect(conn, ctx->params) != SQLI_OK) {
                atomic_fetch_add_explicit(&ctx->stats->failed_queries, 1u, memory_order_relaxed);
                classify_error(conn, ctx->stats);
                sqli_destroy(conn);
                continue;
            }
        }

        sqli_result_t *res = NULL;
        sqli_status rc = sqli_query_with_retry(conn, ctx->sql, ctx->retries, &res);
        if (rc == SQLI_OK) {
            while (sqli_result_next(res)) {
            }
            atomic_fetch_add_explicit(&ctx->stats->ok_queries, 1u, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&ctx->stats->failed_queries, 1u, memory_order_relaxed);
            classify_error(conn, ctx->stats);
        }

        if (res != NULL)
            sqli_result_destroy(res);
        if (pooled) {
            (void)sqli_pool_release(ctx->pool, conn);
        } else {
            sqli_close(conn);
            sqli_destroy(conn);
        }
    }
    return NULL;
}

static unsigned parse_u(const char *s, unsigned dflt)
{
    if (s == NULL || *s == '\0')
        return dflt;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v == 0 || v > 1000000ul)
        return dflt;
    return (unsigned)v;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
                "usage: %s <host> <port> <database> <user> <password> [threads] [iterations] [pool_size] [sql]\n",
                argv[0]);
        return 2;
    }

    const char *host = argv[1];
    const char *port = argv[2];
    const char *db = argv[3];
    const char *user = argv[4];
    const char *pass = argv[5];
    unsigned threads = (argc >= 7) ? parse_u(argv[6], 8) : 8;
    unsigned iters = (argc >= 8) ? parse_u(argv[7], 200) : 200;
    unsigned pool_size = (argc >= 9) ? parse_u(argv[8], threads) : threads;
    const char *sql = (argc >= 10) ? argv[9] : "SELECT FIRST 1 tabname FROM systables";

    const char *client_locale = getenv("SQLI_CLIENT_LOCALE");
    const char *db_locale = getenv("SQLI_DB_LOCALE");
    if (client_locale == NULL || *client_locale == '\0')
        client_locale = "de_DE.CP1252";
    if (db_locale == NULL || *db_locale == '\0')
        db_locale = "de_DE.CP1252";

    sqli_connect_params p;
    memset(&p, 0, sizeof(p));
    p.hostname = host;
    p.service = port;
    p.database = db;
    p.username = user;
    p.password = pass;
    p.client_locale = client_locale;
    p.db_locale = db_locale;

    sqli_pool_t *pool = NULL;
    sqli_status prc = sqli_pool_create(&pool, &p, pool_size);
    if (prc != SQLI_OK) {
        fprintf(stderr, "pool_create failed: rc=%d host=%s:%s db=%s user=%s\n",
                (int)prc, host, port, db, user);
        return 1;
    }

    pthread_t *tids = calloc(threads, sizeof(*tids));
    worker_ctx_t *w = calloc(threads, sizeof(*w));
    if (tids == NULL || w == NULL) {
        fprintf(stderr, "allocation failed\n");
        free(tids);
        free(w);
        sqli_pool_destroy(pool);
        return 1;
    }

    stress_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    uint64_t start_ms = now_ms();

    for (unsigned i = 0; i < threads; i++) {
        w[i].params = &p;
        w[i].pool = pool;
        w[i].sql = sql;
        w[i].iterations = iters;
        w[i].retries = 1;
        w[i].stats = &stats;
        pthread_create(&tids[i], NULL, worker_run, &w[i]);
    }
    for (unsigned i = 0; i < threads; i++)
        pthread_join(tids[i], NULL);

    uint64_t elapsed = now_ms() - start_ms;
    unsigned ok = atomic_load_explicit(&stats.ok_queries, memory_order_relaxed);
    unsigned failed = atomic_load_explicit(&stats.failed_queries, memory_order_relaxed);
    unsigned auth = atomic_load_explicit(&stats.auth_failures, memory_order_relaxed);
    unsigned net = atomic_load_explicit(&stats.network_failures, memory_order_relaxed);

    printf("stress_result host=%s port=%s db=%s threads=%u iterations=%u pool=%u elapsed_ms=%llu ok=%u failed=%u auth=%u network=%u\n",
           host, port, db, threads, iters, pool_size,
           (unsigned long long)elapsed, ok, failed, auth, net);

    free(tids);
    free(w);
    sqli_pool_destroy(pool);
    return (failed == 0) ? 0 : 1;
}
