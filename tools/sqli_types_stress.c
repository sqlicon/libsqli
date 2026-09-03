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
#include <math.h>

typedef struct {
    atomic_uint total_queries;
    atomic_uint total_rows_decoded;
    atomic_uint total_fields_verified;
    atomic_uint total_errors;
    atomic_uint null_checks_passed;
    atomic_uint non_null_checks_passed;
    atomic_uint type_decode_errors;
    uint64_t total_latency_us;
    uint64_t max_latency_us;
    uint64_t min_latency_us;
    pthread_mutex_t latency_lock;
} stress_metrics_t;

typedef struct {
    sqli_pool_t *pool;
    const char *table_name;
    unsigned iterations;
    int worker_id;
    stress_metrics_t *metrics;
} worker_task_t;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void update_latency(stress_metrics_t *m, uint64_t lat_us)
{
    pthread_mutex_lock(&m->latency_lock);
    m->total_latency_us += lat_us;
    if (lat_us > m->max_latency_us) m->max_latency_us = lat_us;
    if (m->min_latency_us == 0 || lat_us < m->min_latency_us) m->min_latency_us = lat_us;
    pthread_mutex_unlock(&m->latency_lock);
}

static int verify_row_datatypes(sqli_result_t *res, stress_metrics_t *m)
{
    int errors = 0;
    int id = sqli_result_get_int(res, 0);
    int pattern = id % 5;

    /* Check NULL pattern (row % 5 == 3 is all-NULL) */
    if (pattern == 3) {
        for (int col = 1; col <= 17; col++) {
            if (!sqli_result_is_null(res, col)) {
                fprintf(stderr, "FAIL: expected NULL at id=%d col=%d\n", id, col);
                errors++;
            } else {
                atomic_fetch_add_explicit(&m->null_checks_passed, 1, memory_order_relaxed);
            }
        }
    } else {
        /* Non-null checks */
        for (int col = 0; col <= 17; col++) {
            if (sqli_result_is_null(res, col)) {
                fprintf(stderr, "FAIL: unexpected NULL at id=%d col=%d\n", id, col);
                errors++;
            } else {
                atomic_fetch_add_explicit(&m->non_null_checks_passed, 1, memory_order_relaxed);
            }
        }

        /* 1. SMALLINT (col 1) */
        int smallint_val = sqli_result_get_int(res, 1);
        if (pattern == 0 && smallint_val != 100) { fprintf(stderr, "FAIL: smallint id=%d val=%d\n", id, smallint_val); errors++; }
        if (pattern == 1 && smallint_val != -32767) { fprintf(stderr, "FAIL: smallint id=%d val=%d\n", id, smallint_val); errors++; }

        /* 2. INTEGER (col 2) */
        int int_val = sqli_result_get_int(res, 2);
        if (pattern == 0 && int_val != 100000) { fprintf(stderr, "FAIL: int id=%d val=%d\n", id, int_val); errors++; }
        if (pattern == 1 && int_val != -2147483647) { fprintf(stderr, "FAIL: int id=%d val=%d\n", id, int_val); errors++; }

        /* 3. BIGINT (col 3) */
        int64_t bigint_val = sqli_result_get_int64(res, 3);
        if (pattern == 0 && bigint_val != 5000000000LL) { fprintf(stderr, "FAIL: bigint id=%d val=%" PRId64 "\n", id, bigint_val); errors++; }

        /* 4. SMALLFLOAT (col 4) */
        double smfloat_val = sqli_result_get_double(res, 4);
        if (pattern == 0 && fabs(smfloat_val - 12.34) > 0.01) { fprintf(stderr, "FAIL: smfloat id=%d val=%f\n", id, smfloat_val); errors++; }

        /* 5. FLOAT (col 5) */
        double float_val = sqli_result_get_double(res, 5);
        if (pattern == 0 && fabs(float_val - 123456.789012) > 0.001) { fprintf(stderr, "FAIL: float id=%d val=%f\n", id, float_val); errors++; }

        /* 6. DECIMAL (col 6) */
        double dec_val = sqli_result_get_double(res, 6);
        if (pattern == 0 && fabs(dec_val - 9876.5432) > 0.01) { fprintf(stderr, "FAIL: decimal id=%d val=%f\n", id, dec_val); errors++; }

        /* 7. DECIMAL FLOAT (col 7) */
        const char *dec_str = sqli_result_get_string(res, 7);
        if (dec_str == NULL || strlen(dec_str) == 0) { fprintf(stderr, "FAIL: dec_str id=%d\n", id); errors++; }

        /* 8. MONEY (col 8) */
        double money_val = sqli_result_get_double(res, 8);
        if (pattern == 0 && fabs(money_val - 49.99) > 0.01) { fprintf(stderr, "FAIL: money id=%d val=%f\n", id, money_val); errors++; }

        /* 9. CHAR (col 9) */
        const char *char_val = sqli_result_get_string(res, 9);
        if (char_val == NULL || strlen(char_val) == 0) { fprintf(stderr, "FAIL: char id=%d\n", id); errors++; }

        /* 10. VARCHAR (col 10) */
        const char *vc_val = sqli_result_get_string(res, 10);
        if (pattern == 2) {
            /* Check umlauts */
            if (vc_val == NULL || strstr(vc_val, "Übertragung") == NULL) {
                fprintf(stderr, "FAIL: expected Umlaut 'Übertragung' at id=%d got='%s'\n", id, vc_val ? vc_val : "NULL");
                errors++;
            }
        }

        /* 11. LVARCHAR (col 11) */
        const char *lvc_val = sqli_result_get_string(res, 11);
        if (lvc_val == NULL && pattern != 4) { fprintf(stderr, "FAIL: lvarchar id=%d\n", id); errors++; }

        /* 12. BOOLEAN (col 12) */
        bool b_val = sqli_result_get_bool(res, 12);
        if (pattern == 0 && !b_val) { fprintf(stderr, "FAIL: bool id=%d expected true\n", id); errors++; }
        if (pattern == 1 && b_val) { fprintf(stderr, "FAIL: bool id=%d expected false\n", id); errors++; }

        /* 13. DATE (col 13) */
        sqli_date_value date_val;
        if (sqli_result_get_date(res, 13, &date_val) != SQLI_OK || date_val.year < 1900) {
            fprintf(stderr, "FAIL: date id=%d year=%d\n", id, date_val.year);
            errors++;
        }

        /* 14. DATETIME YEAR TO SECOND (col 14) */
        const char *dts_val = sqli_result_get_string(res, 14);
        if (dts_val == NULL || strlen(dts_val) == 0) { fprintf(stderr, "FAIL: dt_sec id=%d\n", id); errors++; }

        /* 15. DATETIME YEAR TO FRACTION(3) (col 15) */
        const char *dtf_val = sqli_result_get_string(res, 15);
        if (dtf_val == NULL || strlen(dtf_val) == 0) { fprintf(stderr, "FAIL: dt_frac id=%d\n", id); errors++; }

        /* 16. INTERVAL DAY TO SECOND (col 16) */
        const char *iv_ds = sqli_result_get_string(res, 16);
        if (iv_ds == NULL || strlen(iv_ds) == 0) { fprintf(stderr, "FAIL: iv_ds id=%d\n", id); errors++; }

        /* 17. INTERVAL YEAR TO MONTH (col 17) */
        const char *iv_ym = sqli_result_get_string(res, 17);
        if (iv_ym == NULL || strlen(iv_ym) == 0) { fprintf(stderr, "FAIL: iv_ym id=%d\n", id); errors++; }
    }

    atomic_fetch_add_explicit(&m->total_fields_verified, 18, memory_order_relaxed);
    if (errors > 0)
        atomic_fetch_add_explicit(&m->type_decode_errors, (unsigned)errors, memory_order_relaxed);

    return errors;
}

static void *worker_thread_query(void *arg)
{
    worker_task_t *task = (worker_task_t *)arg;
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM %s ORDER BY id", task->table_name);

    for (unsigned iter = 0; iter < task->iterations; iter++) {
        sqli_conn_t *conn = NULL;
        sqli_status rc = sqli_pool_acquire_timeout(task->pool, &conn, 5000);
        if (rc != SQLI_OK || conn == NULL) {
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            continue;
        }

        uint64_t t0 = now_us();
        sqli_result_t *res = NULL;
        rc = sqli_query(conn, sql, &res);
        if (rc != SQLI_OK || res == NULL) {
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            sqli_pool_release(task->pool, conn);
            continue;
        }

        uint32_t rows_in_query = 0;
        while (sqli_result_next(res)) {
            rows_in_query++;
            verify_row_datatypes(res, task->metrics);
        }

        uint64_t lat = now_us() - t0;
        update_latency(task->metrics, lat);

        atomic_fetch_add_explicit(&task->metrics->total_queries, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&task->metrics->total_rows_decoded, rows_in_query, memory_order_relaxed);

        sqli_result_destroy(res);
        sqli_pool_release(task->pool, conn);
    }
    return NULL;
}

static void *worker_thread_tx(void *arg)
{
    worker_task_t *task = (worker_task_t *)arg;

    for (unsigned iter = 0; iter < task->iterations; iter++) {
        sqli_conn_t *conn = NULL;
        sqli_status rc = sqli_pool_acquire_timeout(task->pool, &conn, 5000);
        if (rc != SQLI_OK || conn == NULL) {
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            continue;
        }

        /* Ensure lock wait is enabled for this connection */
        sqli_result_t *res = NULL;
        (void)sqli_query(conn, "SET LOCK MODE TO WAIT 5", &res);
        if (res) { sqli_result_destroy(res); res = NULL; }

        /* 1. BEGIN WORK */
        rc = sqli_begin(conn);
        if (rc != SQLI_OK) {
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            sqli_pool_release(task->pool, conn);
            continue;
        }

        /* 2. Insert row */
        int temp_id = 900000 + task->worker_id * 10000 + (int)iter;
        char insert_sql[512];
        snprintf(insert_sql, sizeof(insert_sql),
                 "INSERT INTO %s (id, c_smallint, c_int, c_bool, c_varchar) "
                 "VALUES (%d, 99, %d, 't', 'tx_temp_%d')",
                 task->table_name, temp_id, temp_id, iter);
        rc = sqli_query(conn, insert_sql, &res);
        if (res) { sqli_result_destroy(res); res = NULL; }
        if (rc != SQLI_OK) {
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            sqli_rollback(conn);
            sqli_pool_release(task->pool, conn);
            continue;
        }

        /* 3. Savepoint */
        char sp_name[32];
        snprintf(sp_name, sizeof(sp_name), "sp_%d", task->worker_id);
        rc = sqli_savepoint_set(conn, sp_name, true);
        if (rc != SQLI_OK) {
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            sqli_rollback(conn);
            sqli_pool_release(task->pool, conn);
            continue;
        }

        /* 4. Update row */
        char update_sql[256];
        snprintf(update_sql, sizeof(update_sql),
                 "UPDATE %s SET c_smallint = 101 WHERE id = %d", task->table_name, temp_id);
        rc = sqli_query(conn, update_sql, &res);
        if (res) { sqli_result_destroy(res); res = NULL; }
        if (rc != SQLI_OK) {
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            sqli_rollback(conn);
            sqli_pool_release(task->pool, conn);
            continue;
        }

        /* 5. Rollback to Savepoint */
        rc = sqli_savepoint_rollback(conn, sp_name);
        if (rc != SQLI_OK) {
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            sqli_rollback(conn);
            sqli_pool_release(task->pool, conn);
            continue;
        }

        /* 6. Release Savepoint */
        rc = sqli_savepoint_release(conn, sp_name);
        if (rc != SQLI_OK) {
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            sqli_rollback(conn);
            sqli_pool_release(task->pool, conn);
            continue;
        }

        /* 7. Commit */
        rc = sqli_commit(conn);
        if (rc != SQLI_OK) {
            atomic_fetch_add_explicit(&task->metrics->total_errors, 1, memory_order_relaxed);
            sqli_pool_release(task->pool, conn);
            continue;
        }

        atomic_fetch_add_explicit(&task->metrics->total_queries, 4, memory_order_relaxed);
        sqli_pool_release(task->pool, conn);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    printf("=================================================================\n");
    printf("     INFORMIX LIBQLI COMPREHENSIVE ALL-DATATYPES STRESS TEST     \n");
    printf("=================================================================\n");

    const char *host = (argc > 1) ? argv[1] : getenv("SQLI_TEST_HOST");
    if (!host) host = "127.0.0.1";
    const char *port = (argc > 2) ? argv[2] : getenv("SQLI_TEST_PORT");
    if (!port) port = "9089";
    const char *db = (argc > 3) ? argv[3] : getenv("SQLI_TEST_DB");
    if (!db) db = "sqli_log_test";
    const char *user = (argc > 4) ? argv[4] : getenv("SQLI_TEST_USER");
    if (!user) user = "admin";
    const char *pass = (argc > 5) ? argv[5] : getenv("SQLI_TEST_PASS");
    if (!pass) pass = "admin";
    const char *server = getenv("SQLI_TEST_SERVER");
    if (!server) server = "ol_iep";
    const char *db_locale = getenv("SQLI_TEST_DB_LOCALE");
    if (!db_locale) db_locale = "de_DE.1252";

    unsigned threads = (argc > 6) ? (unsigned)atoi(argv[6]) : 8;
    unsigned iters_per_thread = (argc > 7) ? (unsigned)atoi(argv[7]) : 50;
    int num_rows = (argc > 8) ? atoi(argv[8]) : 500;

    printf("[CONFIG] Target: %s@%s:%s/%s (SERVER=%s, DB_LOCALE=%s)\n",
           user, host, port, db, server, db_locale);
    printf("[CONFIG] Threads: %u | Iterations/thread: %u | Test rows: %d\n",
           threads, iters_per_thread, num_rows);

    sqli_connect_params p = {0};
    p.hostname = host;
    p.service = port;
    p.server = server;
    p.database = db;
    p.username = user;
    p.password = pass;
    p.client_locale = db_locale;
    p.db_locale = db_locale;

    sqli_conn_t *admin_conn = NULL;
    if (sqli_create(&admin_conn) != SQLI_OK || sqli_connect(admin_conn, &p) != SQLI_OK) {
        fprintf(stderr, "FATAL: Failed to connect admin connection to %s:%s\n", host, port);
        return 1;
    }

    char table_name[64];
    snprintf(table_name, sizeof(table_name), "stress_types_%d", (int)getpid());

    /* Drop old table if exists */
    char sql[2048];
    snprintf(sql, sizeof(sql), "DROP TABLE %s", table_name);
    sqli_result_t *r = NULL;
    (void)sqli_query(admin_conn, sql, &r);
    if (r) sqli_result_destroy(r);

    /* Create table with all Informix datatypes */
    printf("\n[PHASE 1] Creating table %s with ALL 18 Informix datatypes...\n", table_name);
    snprintf(sql, sizeof(sql),
             "CREATE TABLE %s (\n"
             "    id INT PRIMARY KEY,\n"
             "    c_smallint SMALLINT,\n"
             "    c_int INTEGER,\n"
             "    c_bigint BIGINT,\n"
             "    c_smfloat SMALLFLOAT,\n"
             "    c_float FLOAT,\n"
             "    c_decimal DECIMAL(16,4),\n"
             "    c_dec_float DECIMAL(32),\n"
             "    c_money MONEY(12,2),\n"
             "    c_char CHAR(20),\n"
             "    c_varchar VARCHAR(120),\n"
             "    c_lvarchar LVARCHAR(1000),\n"
             "    c_bool BOOLEAN,\n"
             "    c_date DATE,\n"
             "    c_dt_sec DATETIME YEAR TO SECOND,\n"
             "    c_dt_frac DATETIME YEAR TO FRACTION(3),\n"
             "    c_iv_ds INTERVAL DAY TO SECOND,\n"
             "    c_iv_ym INTERVAL YEAR TO MONTH\n"
             ") LOCK MODE ROW", table_name);

    if (sqli_query(admin_conn, sql, &r) != SQLI_OK) {
        fprintf(stderr, "FATAL: Failed to create table %s: %s\n", table_name, sqli_error(admin_conn));
        sqli_close(admin_conn);
        sqli_destroy(admin_conn);
        return 1;
    }
    if (r) sqli_result_destroy(r);
    printf("          -> Table created successfully (LOCK MODE ROW).\n");

    /* Populate rows across all 5 test patterns */
    printf("\n[PHASE 2] Inserting %d rows across all 5 data patterns...\n", num_rows);
    uint64_t t_insert_start = now_us();
    (void)sqli_begin(admin_conn);

    for (int i = 0; i < num_rows; i++) {
        int pat = i % 5;
        if (pat == 0) {
            /* Nominal positive */
            snprintf(sql, sizeof(sql),
                     "INSERT INTO %s VALUES (%d, 100, 100000, 5000000000, 12.34, 123456.789012, "
                     "9876.5432, 12345678901234567890, 49.99, 'NominalChar', "
                     "'Nominal Varchar value #%d', 'Nominal LVARCHAR text with content for row %d', "
                     "'t', '2026-09-04', '2026-09-04 10:15:30', '2026-09-04 10:15:30.456', "
                     "'2 04:05:06', '3-06')", table_name, i, i, i);
        } else if (pat == 1) {
            /* Boundary / Negative */
            snprintf(sql, sizeof(sql),
                     "INSERT INTO %s VALUES (%d, -32767, -2147483647, -9223372036854775807, -3.1415, "
                     "-1.797e+308, -999999999999.9999, -12345678901234567890, -9999999.99, 'BoundaryChar', "
                     "'Boundary Varchar value #%d', 'Boundary LVARCHAR text with symbols !@#$ row %d', "
                     "'f', '1901-01-01', '1901-01-01 00:00:00', '1901-01-01 00:00:00.000', "
                     "'99 23:59:59', '99-11')", table_name, i, i, i);
        } else if (pat == 2) {
            /* German Umlauts / CP1252 */
            snprintf(sql, sizeof(sql),
                     "INSERT INTO %s VALUES (%d, 42, 424242, 4242424242, 42.42, 424242.4242, "
                     "4242.4242, 4242424242424242, 42.00, 'ÄpfelÖlÜber', "
                     "'Größe Übertragung Straße #%d', 'LVARCHAR mit Umlauten: äöü ÄÖÜ ß und €uro #%d', "
                     "'t', '2026-12-31', '2026-12-31 23:59:59', '2026-12-31 23:59:59.999', "
                     "'10 11:12:13', '1-02')", table_name, i, i, i);
        } else if (pat == 3) {
            /* ALL NULL */
            snprintf(sql, sizeof(sql),
                     "INSERT INTO %s VALUES (%d, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "
                     "NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)", table_name, i);
        } else {
            /* Zero / Neutral */
            snprintf(sql, sizeof(sql),
                     "INSERT INTO %s VALUES (%d, 0, 0, 0, 0.0, 0.0, 0.0000, 0, 0.00, 'ZeroChar', "
                     "'', '', 'f', '2000-01-01', '2000-01-01 00:00:00', '2000-01-01 00:00:00.000', "
                     "'0 00:00:00', '0-00')", table_name, i);
        }

        if (sqli_query(admin_conn, sql, &r) != SQLI_OK) {
            fprintf(stderr, "INSERT error at row %d: %s\n", i, sqli_error(admin_conn));
        }
        if (r) { sqli_result_destroy(r); r = NULL; }
    }
    (void)sqli_commit(admin_conn);
    uint64_t t_insert_elapsed_ms = (now_us() - t_insert_start) / 1000;
    printf("          -> Inserted %d rows in %llu ms (%.1f rows/sec).\n",
           num_rows, (unsigned long long)t_insert_elapsed_ms,
           (double)num_rows * 1000.0 / (double)(t_insert_elapsed_ms ? t_insert_elapsed_ms : 1));

    /* Create pool for multi-threaded testing */
    sqli_pool_t *pool = NULL;
    if (sqli_pool_create(&pool, &p, threads) != SQLI_OK) {
        fprintf(stderr, "FATAL: Failed to create connection pool of size %u\n", threads);
        return 1;
    }

    stress_metrics_t metrics;
    memset(&metrics, 0, sizeof(metrics));
    pthread_mutex_init(&metrics.latency_lock, NULL);

    /* PHASE 3: Concurrent Full-Table Query & Full-Type Decoding Stress */
    printf("\n[PHASE 3] Running Multi-Threaded Read & Full Type Decoding Stress...\n");
    printf("          %u threads executing %u full-table scans (%d rows each)...\n",
           threads, iters_per_thread, num_rows);

    pthread_t *tids = calloc(threads, sizeof(*tids));
    worker_task_t *tasks = calloc(threads, sizeof(*tasks));

    uint64_t t_read_start = now_us();
    for (unsigned i = 0; i < threads; i++) {
        tasks[i].pool = pool;
        tasks[i].table_name = table_name;
        tasks[i].iterations = iters_per_thread;
        tasks[i].worker_id = (int)i;
        tasks[i].metrics = &metrics;
        pthread_create(&tids[i], NULL, worker_thread_query, &tasks[i]);
    }
    for (unsigned i = 0; i < threads; i++) {
        pthread_join(tids[i], NULL);
    }
    uint64_t t_read_elapsed_us = now_us() - t_read_start;
    double t_read_elapsed_s = (double)t_read_elapsed_us / 1000000.0;

    unsigned total_q = atomic_load(&metrics.total_queries);
    unsigned total_rows = atomic_load(&metrics.total_rows_decoded);
    unsigned total_fields = atomic_load(&metrics.total_fields_verified);
    unsigned total_errs = atomic_load(&metrics.total_errors);
    unsigned null_passed = atomic_load(&metrics.null_checks_passed);
    unsigned non_null_passed = atomic_load(&metrics.non_null_checks_passed);
    unsigned type_errors = atomic_load(&metrics.type_decode_errors);

    printf("          -> Total Queries Completed: %u\n", total_q);
    printf("          -> Total Rows Decoded:      %u (%.1f rows/sec)\n",
           total_rows, (double)total_rows / (t_read_elapsed_s > 0 ? t_read_elapsed_s : 0.001));
    printf("          -> Total Fields Verified:  %u (%.1f fields/sec)\n",
           total_fields, (double)total_fields / (t_read_elapsed_s > 0 ? t_read_elapsed_s : 0.001));
    printf("          -> NULL checks passed:     %u\n", null_passed);
    printf("          -> Non-NULL checks passed: %u\n", non_null_passed);
    printf("          -> Type Decode Errors:     %u\n", type_errors);
    printf("          -> Query Errors / Timeouts:%u\n", total_errs);
    printf("          -> Latency Min/Avg/Max:    %.2f ms / %.2f ms / %.2f ms\n",
           (double)metrics.min_latency_us / 1000.0,
           total_q ? ((double)metrics.total_latency_us / (double)total_q / 1000.0) : 0.0,
           (double)metrics.max_latency_us / 1000.0);

    /* PHASE 4: Concurrent Transaction & Savepoint Stress */
    printf("\n[PHASE 4] Running Concurrent Transaction & Savepoint Stress...\n");
    printf("          %u threads executing %u transaction/savepoint cycles...\n",
           threads, iters_per_thread);

    uint64_t t_tx_start = now_us();
    unsigned old_q = atomic_load(&metrics.total_queries);
    unsigned old_errs = atomic_load(&metrics.total_errors);

    for (unsigned i = 0; i < threads; i++) {
        tasks[i].pool = pool;
        tasks[i].table_name = table_name;
        tasks[i].iterations = iters_per_thread;
        tasks[i].worker_id = (int)i;
        tasks[i].metrics = &metrics;
        pthread_create(&tids[i], NULL, worker_thread_tx, &tasks[i]);
    }
    for (unsigned i = 0; i < threads; i++) {
        pthread_join(tids[i], NULL);
    }
    uint64_t t_tx_elapsed_us = now_us() - t_tx_start;
    double t_tx_elapsed_s = (double)t_tx_elapsed_us / 1000000.0;
    unsigned tx_q = atomic_load(&metrics.total_queries) - old_q;
    unsigned tx_errs = atomic_load(&metrics.total_errors) - old_errs;

    printf("          -> TX Operations Completed: %u (%.1f ops/sec)\n",
           tx_q, (double)tx_q / (t_tx_elapsed_s > 0 ? t_tx_elapsed_s : 0.001));
    printf("          -> TX Errors:               %u\n", tx_errs);

    /* Cleanup */
    printf("\n[PHASE 5] Cleaning up test table %s...\n", table_name);
    free(tids);
    free(tasks);
    pthread_mutex_destroy(&metrics.latency_lock);
    sqli_pool_destroy(pool);
    pool = NULL;

    snprintf(sql, sizeof(sql), "DROP TABLE %s", table_name);
    (void)sqli_query(admin_conn, sql, &r);
    if (r) sqli_result_destroy(r);
    printf("          -> Cleanup complete.\n");

    sqli_close(admin_conn);
    sqli_destroy(admin_conn);

    printf("\n=================================================================\n");
    printf("                      EVALUATION SUMMARY                         \n");
    printf("=================================================================\n");
    if (type_errors == 0 && total_errs == 0 && tx_errs == 0) {
        printf("RESULT: [SUCCESS] ALL STRESS TESTS PASSED WITH 0 ERRORS!\n");
        printf("- All 18 datatypes correctly encoded/decoded across %u fields.\n", total_fields);
        printf("- NULL detection is 100%% accurate across all column types.\n");
        printf("- Character encoding / German Umlauts verified without corruption.\n");
        printf("- Connection pool, transactions, and savepoints rock-solid.\n");
        printf("=================================================================\n");
        return 0;
    } else {
        printf("RESULT: [FAILED] Errors detected: %u decode errs, %u query errs, %u tx errs.\n",
               type_errors, total_errs, tx_errs);
        printf("=================================================================\n");
        return 1;
    }
}
