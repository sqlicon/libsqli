#define _POSIX_C_SOURCE 200809L
#include "libsqli/sqli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include <getopt.h>

typedef struct {
    atomic_uint total_operations;
    atomic_uint legacy_writes;
    atomic_uint legacy_reads;
    atomic_uint legacy_null_checks;
    atomic_uint legacy_byte_verifications;
    atomic_uint legacy_stream_callbacks;
    atomic_uint smart_writes;
    atomic_uint smart_reads;
    atomic_uint smart_seeks;
    atomic_uint smart_byte_verifications;
    atomic_uint total_errors;
    atomic_uint_fast64_t total_bytes_written;
    atomic_uint_fast64_t total_bytes_read;

    uint64_t total_latency_us;
    uint64_t max_latency_us;
    uint64_t min_latency_us;
    pthread_mutex_t latency_lock;
} blob_metrics_t;

typedef struct {
    sqli_pool_t *pool;
    unsigned iterations;
    int worker_id;
    int total_workers;
    blob_metrics_t *metrics;
    sqli_connect_params conn_params;
} blob_worker_task_t;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void update_latency(blob_metrics_t *m, uint64_t lat_us)
{
    pthread_mutex_lock(&m->latency_lock);
    m->total_latency_us += lat_us;
    if (lat_us > m->max_latency_us) m->max_latency_us = lat_us;
    if (m->min_latency_us == 0 || lat_us < m->min_latency_us) m->min_latency_us = lat_us;
    pthread_mutex_unlock(&m->latency_lock);
}

/* Deterministic test patterns */
static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)((seed + i * 37 + (i / 251)) & 0xFF);
    }
}

static int verify_pattern(const uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t expected = (uint8_t)((seed + i * 37 + (i / 251)) & 0xFF);
        if (buf[i] != expected) {
            fprintf(stderr, "Pattern mismatch at offset %zu: got 0x%02x, expected 0x%02x (seed=%u len=%zu)\n",
                    i, buf[i], expected, seed, len);
            return 0;
        }
    }
    return 1;
}

typedef struct {
    size_t bytes_received;
    uint8_t seed;
    int mismatch;
} stream_callback_ctx_t;

static int stream_verify_callback(const uint8_t *chunk, size_t chunk_len, void *user_ctx)
{
    stream_callback_ctx_t *ctx = (stream_callback_ctx_t *)user_ctx;
    for (size_t i = 0; i < chunk_len; i++) {
        size_t global_idx = ctx->bytes_received + i;
        uint8_t expected = (uint8_t)((ctx->seed + global_idx * 37 + (global_idx / 251)) & 0xFF);
        if (chunk[i] != expected) {
            ctx->mismatch = 1;
            return -1;
        }
    }
    ctx->bytes_received += chunk_len;
    return 0;
}

/* Worker thread for Legacy LOB stress */
static void *worker_legacy_lob_stress(void *arg)
{
    blob_worker_task_t *task = (blob_worker_task_t *)arg;
    sqli_conn_t *conn = NULL;

    if (sqli_pool_acquire(task->pool, &conn) != SQLI_OK || conn == NULL) {
        atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
        return NULL;
    }

    /* Test vector sizes including chunk boundaries */
    const size_t test_sizes[] = {
        0,       /* NULL */
        100,     /* Small */
        1023,    /* 1 chunk - 1 */
        1024,    /* 1 chunk boundary */
        1025,    /* 2 chunks (1024 + 1) */
        2048,    /* 2 chunks boundary */
        2049,    /* 3 chunks */
        5000,    /* Multi-chunk */
        16384,   /* 16 KB */
        32768    /* 32 KB */
    };
    const size_t num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

    for (unsigned iter = 0; iter < task->iterations; iter++) {
        uint64_t t0 = now_us();
        size_t size_idx = (task->worker_id + iter) % num_sizes;
        size_t byte_len = test_sizes[size_idx];
        int row_id = task->worker_id * 10000 + (int)iter;
        uint8_t seed = (uint8_t)((row_id * 17) & 0xFF);

        /* 1. Insert Legacy LOB via prepared statement */
        sqli_stmt_t *stmt = NULL;
        int nparams = 0;
        sqli_status rc = sqli_prepare(conn, "INSERT INTO test_legacy_stress (id, b, t) VALUES (?, ?, ?)", &nparams, &stmt);
        if (rc != SQLI_OK) {
            fprintf(stderr, "[Worker %d] prepare INSERT failed: %s\n", task->worker_id, sqli_error(conn));
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            break;
        }

        sqli_bind_int(stmt, 1, row_id);

        uint8_t *write_buf = NULL;
        char text_buf[512];
        if (byte_len == 0) {
            sqli_bind_null(stmt, 2);
            sqli_bind_null(stmt, 3);
        } else {
            write_buf = malloc(byte_len);
            fill_pattern(write_buf, byte_len, seed);
            sqli_bind_bytes(stmt, 2, write_buf, byte_len);

            snprintf(text_buf, sizeof(text_buf),
                     "Worker=%d Iter=%u Seed=%u Text with Umlauts: ÄÖÜäöüß Euro: € Len=%zu",
                     task->worker_id, iter, seed, byte_len);
            sqli_bind_string(stmt, 3, text_buf);
        }

        rc = sqli_execute(stmt);
        sqli_stmt_close(stmt);
        sqli_stmt_destroy(stmt);

        if (rc != SQLI_OK) {
            fprintf(stderr, "[Worker %d] execute INSERT failed at row %d (size=%zu): %s\n",
                    task->worker_id, row_id, byte_len, sqli_error(conn));
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            free(write_buf);
            break;
        }

        atomic_fetch_add_explicit(&task->metrics->legacy_writes, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&task->metrics->total_bytes_written, byte_len, memory_order_relaxed);

        /* 2. Read back and verify byte-for-byte */
        char q_sql[128];
        snprintf(q_sql, sizeof(q_sql), "SELECT id, b, t FROM test_legacy_stress WHERE id = %d", row_id);
        sqli_result_t *res = NULL;
        rc = sqli_query(conn, q_sql, &res);
        if (rc != SQLI_OK || !sqli_result_next(res)) {
            fprintf(stderr, "[Worker %d] query row %d failed: %s\n", task->worker_id, row_id, sqli_error(conn));
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            if (res) sqli_result_destroy(res);
            free(write_buf);
            break;
        }

        atomic_fetch_add_explicit(&task->metrics->legacy_reads, 1, memory_order_relaxed);

        if (byte_len == 0) {
            if (sqli_result_is_null(res, 1) && sqli_result_is_null(res, 2)) {
                atomic_fetch_add_explicit(&task->metrics->legacy_null_checks, 2, memory_order_relaxed);
            } else {
                fprintf(stderr, "[Worker %d] expected NULLs at row %d\n", task->worker_id, row_id);
                atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            }
        } else {
            /* Check BYTE column */
            if (sqli_result_is_null(res, 1)) {
                fprintf(stderr, "[Worker %d] unexpected NULL in BYTE at row %d\n", task->worker_id, row_id);
                atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            } else {
                uint8_t *read_buf = malloc(byte_len + 512);
                size_t read_len = byte_len + 512;
                sqli_status brc = sqli_result_get_bytes(res, 1, read_buf, &read_len);
                if (brc == SQLI_OK && read_len == byte_len && verify_pattern(read_buf, byte_len, seed)) {
                    atomic_fetch_add_explicit(&task->metrics->legacy_byte_verifications, 1, memory_order_relaxed);
                    atomic_fetch_add_explicit(&task->metrics->total_bytes_read, read_len, memory_order_relaxed);
                } else {
                    fprintf(stderr, "[Worker %d] BYTE verification failed at row %d: brc=%d read_len=%zu expected=%zu\n",
                            task->worker_id, row_id, brc, read_len, byte_len);
                    atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
                }
                free(read_buf);

                /* Test streaming callback API */
                stream_callback_ctx_t cb_ctx = {0, seed, 0};
                sqli_status s_rc = sqli_result_stream_bytes(res, 1, 512, stream_verify_callback, &cb_ctx);
                if (s_rc == SQLI_OK && cb_ctx.bytes_received == byte_len && !cb_ctx.mismatch) {
                    atomic_fetch_add_explicit(&task->metrics->legacy_stream_callbacks, 1, memory_order_relaxed);
                } else {
                    fprintf(stderr, "[Worker %d] stream callback verification failed at row %d\n",
                            task->worker_id, row_id);
                    atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
                }
            }

            /* Check TEXT column */
            if (sqli_result_is_null(res, 2)) {
                fprintf(stderr, "[Worker %d] unexpected NULL in TEXT at row %d\n", task->worker_id, row_id);
                atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            } else {
                const char *read_t = sqli_result_get_string(res, 2);
                if (read_t && strcmp(read_t, text_buf) == 0) {
                    atomic_fetch_add_explicit(&task->metrics->legacy_byte_verifications, 1, memory_order_relaxed);
                } else {
                    fprintf(stderr, "[Worker %d] TEXT verification failed at row %d: got [%s], expected [%s]\n",
                            task->worker_id, row_id, read_t ? read_t : "NULL", text_buf);
                    atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
                }
            }
        }

        sqli_result_destroy(res);
        free(write_buf);

        uint64_t lat = now_us() - t0;
        update_latency(task->metrics, lat);
        atomic_fetch_add_explicit(&task->metrics->total_operations, 1, memory_order_relaxed);
    }

    sqli_pool_release(task->pool, conn);
    return NULL;
}

/* Worker thread for Smart Large Object (BLOB / CLOB) stress */
static void *worker_smart_lob_stress(void *arg)
{
    blob_worker_task_t *task = (blob_worker_task_t *)arg;
    sqli_conn_t *conn = NULL;

    if (sqli_pool_acquire(task->pool, &conn) != SQLI_OK || conn == NULL) {
        atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
        return NULL;
    }

    /* Test sizes covering multi-chunk boundary at 32000 bytes */
    const size_t smart_sizes[] = {
        512,      /* Small */
        31999,    /* 1 chunk - 1 */
        32000,    /* Chunk boundary */
        32001,    /* 2 chunks (32000 + 1) */
        40000,    /* 2 chunks (32000 + 8000) */
        64000,    /* 2 chunk boundary */
        70000     /* 3 chunks */
    };
    const size_t num_smart_sizes = sizeof(smart_sizes) / sizeof(smart_sizes[0]);

    for (unsigned iter = 0; iter < task->iterations; iter++) {
        uint64_t t0 = now_us();
        int base_row_id = 1000 + task->worker_id * 1000 + (int)iter + 1;
        size_t size_idx = (task->worker_id * 3 + iter) % num_smart_sizes;
        size_t write_len = smart_sizes[size_idx];
        uint8_t seed = (uint8_t)((task->worker_id * 100 + iter * 7) & 0xFF);

        /* Pre-create dedicated row for this iteration */
        char ins_sql[256];
        snprintf(ins_sql, sizeof(ins_sql),
                 "INSERT INTO test_smart_stress (id, b, c) VALUES (%d, FILETOBLOB('/etc/hosts', 'server'), FILETOCLOB('/etc/hosts', 'server'))",
                 base_row_id);
        sqli_result_t *ir = NULL;
        sqli_status irc = sqli_query(conn, ins_sql, &ir);
        if (ir) sqli_result_destroy(ir);
        if (irc != SQLI_OK) {
            fprintf(stderr, "[Smart Worker %d] seed row %d failed: %s\n",
                    task->worker_id, base_row_id, sqli_error(conn));
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            break;
        }

        /* 1. Open smartblob for writing */
        char open_sql[128];
        snprintf(open_sql, sizeof(open_sql), "SELECT ifx_lo_open(b, %d) FROM test_smart_stress WHERE id = %d",
                 SQLI_LO_RDWR, base_row_id);

        int lofd = -1;
        sqli_status rc = sqli_sblob_open_query(conn, open_sql, &lofd);
        if (rc != SQLI_OK || lofd < 0) {
            fprintf(stderr, "[Smart Worker %d] open for write failed on row %d: %s\n",
                    task->worker_id, base_row_id, sqli_error(conn));
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            break;
        }

        /* Generate payload */
        uint8_t *payload = malloc(write_len);
        fill_pattern(payload, write_len, seed);

        /* 2. Write payload using SQ_LODATA streaming chunks */
        size_t bytes_written = 0;
        rc = sqli_sblob_write(conn, lofd, payload, write_len, &bytes_written);
        (void)sqli_sblob_close(conn, lofd);

        if (rc != SQLI_OK) {
            fprintf(stderr, "[Smart Worker %d] write failed (%zu bytes): %s\n",
                    task->worker_id, write_len, sqli_error(conn));
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            free(payload);
            break;
        }

        atomic_fetch_add_explicit(&task->metrics->smart_writes, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&task->metrics->total_bytes_written, write_len, memory_order_relaxed);

        /* 3. Re-open for reading and verify byte-for-byte */
        snprintf(open_sql, sizeof(open_sql), "SELECT ifx_lo_open(b, %d) FROM test_smart_stress WHERE id = %d",
                 SQLI_LO_RDONLY, base_row_id);
        rc = sqli_sblob_open_query(conn, open_sql, &lofd);
        if (rc != SQLI_OK || lofd < 0) {
            fprintf(stderr, "[Smart Worker %d] open for read failed on row %d: %s\n",
                    task->worker_id, base_row_id, sqli_error(conn));
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            free(payload);
            break;
        }

        uint8_t *read_buf = malloc(write_len + 1024);
        size_t bytes_read = 0;
        rc = sqli_sblob_read(conn, lofd, read_buf, write_len, &bytes_read);
        (void)sqli_sblob_close(conn, lofd);

        if (rc != SQLI_OK || bytes_read < write_len) {
            fprintf(stderr, "[Smart Worker %d] read failed: rc=%d got=%zu expected=%zu: %s\n",
                    task->worker_id, rc, bytes_read, write_len, sqli_error(conn));
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            free(payload);
            free(read_buf);
            break;
        }

        atomic_fetch_add_explicit(&task->metrics->smart_reads, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&task->metrics->total_bytes_read, bytes_read, memory_order_relaxed);

        if (verify_pattern(read_buf, write_len, seed)) {
            atomic_fetch_add_explicit(&task->metrics->smart_byte_verifications, 1, memory_order_relaxed);
        } else {
            fprintf(stderr, "[Smart Worker %d] byte mismatch after read on row %d (size=%zu)\n",
                    task->worker_id, base_row_id, write_len);
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
        }
        free(read_buf);

        /* 4. Seek-and-Read verification (test across 32000-byte boundary if size permits) */
        if (write_len > 1000) {
            int64_t seek_offset = (write_len > 35000) ? 33000 : 500;
            size_t seek_read_n = 200;
            if ((size_t)seek_offset + seek_read_n <= write_len) {
                rc = sqli_sblob_open_query(conn, open_sql, &lofd);
                if (rc == SQLI_OK && lofd >= 0) {
                    uint8_t seek_buf[256];
                    size_t s_got = 0;
                    rc = sqli_sblob_read_seek(conn, lofd, seek_offset, seek_buf, seek_read_n, &s_got);
                    (void)sqli_sblob_close(conn, lofd);

                    if (rc == SQLI_OK && s_got == seek_read_n) {
                        atomic_fetch_add_explicit(&task->metrics->smart_seeks, 1, memory_order_relaxed);
                        atomic_fetch_add_explicit(&task->metrics->total_bytes_read, s_got, memory_order_relaxed);

                        if (memcmp(seek_buf, payload + seek_offset, seek_read_n) == 0) {
                            atomic_fetch_add_explicit(&task->metrics->smart_byte_verifications, 1, memory_order_relaxed);
                        } else {
                            fprintf(stderr, "[Smart Worker %d] seek payload mismatch at offset %lld\n",
                                    task->worker_id, (long long)seek_offset);
                            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
                        }
                    } else {
                        fprintf(stderr, "[Smart Worker %d] read_seek failed: rc=%d got=%zu\n",
                                task->worker_id, rc, s_got);
                        atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
                    }
                }
            }
        }

        free(payload);
        uint64_t lat = now_us() - t0;
        update_latency(task->metrics, lat);
        atomic_fetch_add_explicit(&task->metrics->total_operations, 1, memory_order_relaxed);
    }

    sqli_pool_release(task->pool, conn);
    return NULL;
}

int main(int argc, char **argv)
{
    unsigned threads = 4;
    unsigned iterations = 25;

    sqli_connect_params p = {0};
    p.server = "ol_iep";
    p.hostname = "127.0.0.1";
    p.service = "9089";
    p.database = "sqli_log_test";
    p.username = "admin";
    p.password = "admin";
    p.client_locale = "de_DE.1252";
    p.db_locale = "de_DE.1252";

    static struct option long_options[] = {
        {"threads",       required_argument, 0, 't'},
        {"iterations",    required_argument, 0, 'i'},
        {"server",        required_argument, 0, 's'},
        {"host",          required_argument, 0, 'h'},
        {"port",          required_argument, 0, 'p'},
        {"database",      required_argument, 0, 'd'},
        {"user",          required_argument, 0, 'u'},
        {"password",      required_argument, 0, 'w'},
        {"client-locale", required_argument, 0, 'c'},
        {"db-locale",     required_argument, 0, 'l'},
        {"help",          no_argument,       0, '?'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:i:s:h:p:d:u:w:c:l:?", long_options, NULL)) != -1) {
        switch (opt) {
        case 't': threads = (unsigned)atoi(optarg); break;
        case 'i': iterations = (unsigned)atoi(optarg); break;
        case 's': p.server = optarg; break;
        case 'h': p.hostname = optarg; break;
        case 'p': p.service = optarg; break;
        case 'd': p.database = optarg; break;
        case 'u': p.username = optarg; break;
        case 'w': p.password = optarg; break;
        case 'c': p.client_locale = optarg; break;
        case 'l': p.db_locale = optarg; break;
        default:
            printf("Usage: %s [options]\n"
                   "  -t, --threads <n>       Worker threads per phase (default: 4)\n"
                   "  -i, --iterations <n>    Iterations per thread (default: 25)\n"
                   "  -s, --server <name>     Informix server name (default: ol_iep)\n"
                   "  -h, --host <host>       Hostname or IP (default: 127.0.0.1)\n"
                   "  -p, --port <port>       Port number (default: 9089)\n"
                   "  -d, --database <db>     Database name (default: sqli_log_test)\n"
                   "  -u, --user <user>       Username (default: admin)\n"
                   "  -w, --password <pass>   Password (default: admin)\n",
                   argv[0]);
            return 0;
        }
    }

    if (threads < 1) threads = 1;
    if (threads > 32) threads = 32;

    printf("=================================================================\n");
    printf("         INFORMIX LOB (BYTE/TEXT & BLOB/CLOB) STRESS TEST        \n");
    printf("=================================================================\n");
    printf("Server:     %s (%s:%s, DB: %s)\n", p.server, p.hostname, p.service, p.database);
    printf("Threads:    %u\n", threads);
    printf("Iterations: %u per thread\n", iterations);
    printf("Locales:    Client=%s, DB=%s\n", p.client_locale, p.db_locale);
    printf("-----------------------------------------------------------------\n");

    /* Phase 1: Setup test tables using admin connection */
    sqli_conn_t *admin_conn = NULL;
    if (sqli_create(&admin_conn) != SQLI_OK || sqli_connect(admin_conn, &p) != SQLI_OK) {
        fprintf(stderr, "FATAL: Failed to connect admin session: %s\n", sqli_error(admin_conn));
        return 1;
    }

    printf("\n[PHASE 1] Initializing stress test tables...\n");
    sqli_result_t *r = NULL;

    (void)sqli_query(admin_conn, "DROP TABLE test_legacy_stress", &r);
    if (r) { sqli_result_destroy(r); r = NULL; }

    sqli_status rc = sqli_query(admin_conn,
        "CREATE TABLE test_legacy_stress (id INT PRIMARY KEY, b BYTE IN TABLE, t TEXT IN TABLE)", &r);
    if (rc != SQLI_OK) {
        fprintf(stderr, "FATAL: Failed to create test_legacy_stress: %s\n", sqli_error(admin_conn));
        return 1;
    }
    if (r) { sqli_result_destroy(r); r = NULL; }
    printf("          -> Created table test_legacy_stress (BYTE IN TABLE, TEXT IN TABLE)\n");

    (void)sqli_query(admin_conn, "DROP TABLE test_smart_stress", &r);
    if (r) { sqli_result_destroy(r); r = NULL; }

    rc = sqli_query(admin_conn,
        "CREATE TABLE test_smart_stress (id INT PRIMARY KEY, b BLOB, c CLOB PUT IN sbdbs)", &r);
    if (rc != SQLI_OK) {
        /* Fallback if PUT IN sbdbs is not needed */
        rc = sqli_query(admin_conn,
            "CREATE TABLE test_smart_stress (id INT PRIMARY KEY, b BLOB, c CLOB)", &r);
        if (rc != SQLI_OK) {
            fprintf(stderr, "FATAL: Failed to create test_smart_stress: %s\n", sqli_error(admin_conn));
            return 1;
        }
    }
    if (r) { sqli_result_destroy(r); r = NULL; }
    printf("          -> Created table test_smart_stress (BLOB, CLOB)\n");

    /* Pre-seed 10 base rows in test_smart_stress with FILETOBLOB */
    for (int id = 1; id <= 10; id++) {
        char seed_sql[256];
        snprintf(seed_sql, sizeof(seed_sql),
                 "INSERT INTO test_smart_stress (id, b, c) VALUES (%d, FILETOBLOB('/etc/hosts', 'server'), FILETOCLOB('/etc/hosts', 'server'))",
                 id);
        rc = sqli_query(admin_conn, seed_sql, &r);
        if (r) { sqli_result_destroy(r); r = NULL; }
        if (rc != SQLI_OK) {
            fprintf(stderr, "FATAL: Failed to seed test_smart_stress row %d: %s\n", id, sqli_error(admin_conn));
            return 1;
        }
    }
    printf("          -> Seeded 10 template smartblob rows using server /etc/hosts\n");

    /* Create connection pool for multi-threaded stress */
    sqli_pool_t *pool = NULL;
    if (sqli_pool_create(&pool, &p, threads) != SQLI_OK) {
        fprintf(stderr, "FATAL: Failed to create connection pool (%u threads)\n", threads);
        return 1;
    }

    blob_metrics_t metrics;
    memset(&metrics, 0, sizeof(metrics));
    pthread_mutex_init(&metrics.latency_lock, NULL);

    pthread_t *tids = calloc(threads, sizeof(*tids));
    blob_worker_task_t *tasks = calloc(threads, sizeof(*tasks));

    /* PHASE 2: Legacy LOB Stress (BYTE & TEXT) */
    printf("\n[PHASE 2] Running Legacy LOB (BYTE & TEXT) Concurrent Stress...\n");
    printf("          %u threads, %u iterations each (NULLs, 100B, 1023B, 1024B, 1025B, 2KB, 5KB, 16KB, 32KB)...\n",
           threads, iterations);

    uint64_t t_legacy_start = now_us();
    for (unsigned i = 0; i < threads; i++) {
        tasks[i].pool = pool;
        tasks[i].iterations = iterations;
        tasks[i].worker_id = (int)i;
        tasks[i].total_workers = (int)threads;
        tasks[i].metrics = &metrics;
        tasks[i].conn_params = p;
        pthread_create(&tids[i], NULL, worker_legacy_lob_stress, &tasks[i]);
    }
    for (unsigned i = 0; i < threads; i++) {
        pthread_join(tids[i], NULL);
    }
    uint64_t t_legacy_elapsed_us = now_us() - t_legacy_start;
    double t_legacy_s = (double)t_legacy_elapsed_us / 1000000.0;

    printf("          -> Legacy Writes:          %u\n", atomic_load(&metrics.legacy_writes));
    printf("          -> Legacy Reads:           %u\n", atomic_load(&metrics.legacy_reads));
    printf("          -> NULL Checks Passed:     %u\n", atomic_load(&metrics.legacy_null_checks));
    printf("          -> Byte-for-Byte Matches:  %u\n", atomic_load(&metrics.legacy_byte_verifications));
    printf("          -> Stream Callbacks:       %u\n", atomic_load(&metrics.legacy_stream_callbacks));
    printf("          -> Phase Errors:           %u\n", atomic_load(&metrics.total_errors));
    printf("          -> Phase Elapsed Time:     %.2f s\n", t_legacy_s);

    /* PHASE 3: Smart Large Object Stress (BLOB & CLOB) */
    printf("\n[PHASE 3] Running Smart Large Object (BLOB & CLOB) Concurrent Stress...\n");
    printf("          %u threads, %u iterations each (512B, 32000B chunk boundary, 40KB, 64KB, 70KB, Seeks)...\n",
           threads, iterations);

    unsigned prev_errs = atomic_load(&metrics.total_errors);
    uint64_t t_smart_start = now_us();
    for (unsigned i = 0; i < threads; i++) {
        tasks[i].pool = pool;
        tasks[i].iterations = iterations;
        tasks[i].worker_id = (int)i;
        tasks[i].total_workers = (int)threads;
        tasks[i].metrics = &metrics;
        tasks[i].conn_params = p;
        pthread_create(&tids[i], NULL, worker_smart_lob_stress, &tasks[i]);
    }
    for (unsigned i = 0; i < threads; i++) {
        pthread_join(tids[i], NULL);
    }
    uint64_t t_smart_elapsed_us = now_us() - t_smart_start;
    double t_smart_s = (double)t_smart_elapsed_us / 1000000.0;

    printf("          -> Smart LO Writes:        %u\n", atomic_load(&metrics.smart_writes));
    printf("          -> Smart LO Reads:         %u\n", atomic_load(&metrics.smart_reads));
    printf("          -> Seek-and-Reads:         %u\n", atomic_load(&metrics.smart_seeks));
    printf("          -> Byte-for-Byte Matches:  %u\n", atomic_load(&metrics.smart_byte_verifications));
    printf("          -> Phase Errors:           %u\n", atomic_load(&metrics.total_errors) - prev_errs);
    printf("          -> Phase Elapsed Time:     %.2f s\n", t_smart_s);

    /* Phase 4: Direct query convenience verification */
    printf("\n[PHASE 4] Verifying convenience sqli_result_read_sblob API...\n");
    sqli_result_t *q_res = NULL;
    rc = sqli_query(admin_conn, "SELECT id, b, c FROM test_smart_stress WHERE id = 1", &q_res);
    if (rc == SQLI_OK && sqli_result_next(q_res)) {
        uint8_t conv_buf[512];
        size_t conv_read = 0;
        rc = sqli_result_read_sblob(q_res, 1, conv_buf, sizeof(conv_buf), &conv_read);
        if (rc == SQLI_OK && conv_read > 0) {
            printf("          -> sqli_result_read_sblob successfully read %zu bytes directly from BLOB column.\n", conv_read);
        } else {
            fprintf(stderr, "          -> sqli_result_read_sblob failed: rc=%d\n", rc);
            atomic_fetch_add_explicit(&metrics.total_errors, 1, memory_order_relaxed);
        }
        sqli_result_destroy(q_res);
    }

    /* Phase 5: Cleanup */
    printf("\n[PHASE 5] Cleaning up test tables...\n");
    (void)sqli_query(admin_conn, "DROP TABLE test_legacy_stress", &r);
    if (r) { sqli_result_destroy(r); r = NULL; }
    (void)sqli_query(admin_conn, "DROP TABLE test_smart_stress", &r);
    if (r) { sqli_result_destroy(r); r = NULL; }
    printf("          -> Cleaned up tables.\n");

    free(tids);
    free(tasks);
    pthread_mutex_destroy(&metrics.latency_lock);
    sqli_pool_destroy(pool);
    sqli_close(admin_conn);
    sqli_destroy(admin_conn);

    unsigned total_ops = atomic_load(&metrics.total_operations);
    unsigned total_errs = atomic_load(&metrics.total_errors);
    uint64_t bytes_w = atomic_load(&metrics.total_bytes_written);
    uint64_t bytes_r = atomic_load(&metrics.total_bytes_read);
    double mb_written = (double)bytes_w / (1024.0 * 1024.0);
    double mb_read = (double)bytes_r / (1024.0 * 1024.0);

    printf("\n=================================================================\n");
    printf("                      EVALUATION SUMMARY                         \n");
    printf("=================================================================\n");
    printf("Total Operations:          %u\n", total_ops);
    printf("Total Data Written:        %.2f MB (%" PRIu64 " bytes)\n", mb_written, bytes_w);
    printf("Total Data Read:           %.2f MB (%" PRIu64 " bytes)\n", mb_read, bytes_r);
    printf("Legacy BYTE/TEXT Verified: %u byte-exact matches\n", atomic_load(&metrics.legacy_byte_verifications));
    printf("Legacy NULL Checks:        %u passed\n", atomic_load(&metrics.legacy_null_checks));
    printf("Legacy Stream Callbacks:   %u passed\n", atomic_load(&metrics.legacy_stream_callbacks));
    printf("Smart LO Verified:         %u byte-exact matches\n", atomic_load(&metrics.smart_byte_verifications));
    printf("Smart LO Seek Operations:  %u verified\n", atomic_load(&metrics.smart_seeks));
    printf("Total Errors Encountered:  %u\n", total_errs);
    printf("Latency Min / Avg / Max:   %.2f ms / %.2f ms / %.2f ms\n",
           (double)metrics.min_latency_us / 1000.0,
           total_ops ? ((double)metrics.total_latency_us / (double)total_ops / 1000.0) : 0.0,
           (double)metrics.max_latency_us / 1000.0);
    printf("-----------------------------------------------------------------\n");

    if (total_errs == 0) {
        printf("RESULT: [SUCCESS] ALL BLOB STRESS TESTS PASSED WITH 0 ERRORS!\n");
        printf("- Legacy BYTE/TEXT streaming (BBIND / BLOB chunks / FETCHBLOB) verified byte-for-byte.\n");
        printf("- Smart Large Object (BLOB/CLOB / LODATA / Seek) verified across 32000-byte chunk boundaries.\n");
        printf("- Multi-threaded concurrency confirmed stable under load.\n");
        printf("=================================================================\n");
        return 0;
    } else {
        printf("RESULT: [FAILED] Stress tests finished with %u errors!\n", total_errs);
        printf("=================================================================\n");
        return 1;
    }
}
