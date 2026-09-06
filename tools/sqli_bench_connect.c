#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <unistd.h>
#include "libsqli/sqli.h"

typedef struct {
    double create_ms;
    double connect_ms;
    double query_ms;
    double close_ms;
    double destroy_ms;
    double total_ms;
    bool success;
} iter_stat;

static double get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int compare_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static void print_stats(const char *name, double *arr, size_t n)
{
    if (n == 0) return;
    qsort(arr, n, sizeof(double), compare_double);
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += arr[i];
    double mean = sum / (double)n;

    double variance_sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = arr[i] - mean;
        variance_sum += d * d;
    }
    double stddev = sqrt(variance_sum / (double)n);

    double min = arr[0];
    double max = arr[n - 1];
    double p50 = arr[(size_t)(n * 0.50)];
    double p90 = arr[(size_t)(n * 0.90)];
    double p95 = arr[(size_t)(n * 0.95)];
    double p99 = arr[(size_t)(n * 0.99)];

    printf("  %-12s: min=%6.3fms  avg=%6.3fms  p50=%6.3fms  p90=%6.3fms  p95=%6.3fms  p99=%6.3fms  max=%6.3fms  stddev=%6.3fms\n",
           name, min, mean, p50, p90, p95, p99, max, stddev);
}

int main(int argc, char **argv)
{
    const char *uri = NULL;
    const char *user = NULL;
    const char *pass = NULL;
    const char *label = "connection";
    int iterations = 100;
    int warmup = 5;
    bool do_query = false;
    bool json_out = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--uri") == 0 && i + 1 < argc) {
            uri = argv[++i];
        } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            user = argv[++i];
        } else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            pass = argv[++i];
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            warmup = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else if (strcmp(argv[i], "--query") == 0) {
            do_query = true;
        } else if (strcmp(argv[i], "--json") == 0) {
            json_out = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s --uri <uri> [--user <u>] [--password <p>] [--iterations <N>] [--warmup <N>] [--query] [--label <L>] [--json]\n", argv[0]);
            return 1;
        }
    }

    if (!uri) {
        fprintf(stderr, "Error: --uri is required\n");
        return 1;
    }

    if (iterations < 1) iterations = 1;

    /* Warmup */
    for (int i = 0; i < warmup; i++) {
        sqli_conn_t *c = NULL;
        if (sqli_create(&c) == SQLI_OK) {
            if (sqli_connect_uri(c, uri, user, pass) == SQLI_OK) {
                if (do_query) {
                    sqli_result_t *res = NULL;
                    if (sqli_query(c, "SELECT 1 FROM sysmaster:sysdual", &res) == SQLI_OK) {
                        while (sqli_result_next(res)) {}
                        sqli_result_destroy(res);
                    }
                }
                sqli_close(c);
            }
            sqli_destroy(c);
        }
    }

    iter_stat *stats = malloc(sizeof(iter_stat) * (size_t)iterations);
    if (!stats) {
        fprintf(stderr, "Allocation failure for stats\n");
        return 1;
    }

    size_t successful = 0;
    double t_start_total = get_time_sec();

    for (int i = 0; i < iterations; i++) {
        double t0 = get_time_sec();
        sqli_conn_t *c = NULL;
        sqli_status rc = sqli_create(&c);
        double t1 = get_time_sec();

        if (rc != SQLI_OK) {
            stats[i].success = false;
            continue;
        }

        rc = sqli_connect_uri(c, uri, user, pass);
        double t2 = get_time_sec();

        double t3 = t2;
        if (rc == SQLI_OK && do_query) {
            sqli_result_t *res = NULL;
            if (sqli_query(c, "SELECT 1 FROM sysmaster:sysdual", &res) == SQLI_OK) {
                while (sqli_result_next(res)) {}
                sqli_result_destroy(res);
            }
            t3 = get_time_sec();
        }

        double t4 = t3;
        if (rc == SQLI_OK) {
            sqli_close(c);
            t4 = get_time_sec();
        }

        sqli_destroy(c);
        double t5 = get_time_sec();

        stats[i].create_ms = (t1 - t0) * 1000.0;
        stats[i].connect_ms = (t2 - t1) * 1000.0;
        stats[i].query_ms = (t3 - t2) * 1000.0;
        stats[i].close_ms = (t4 - t3) * 1000.0;
        stats[i].destroy_ms = (t5 - t4) * 1000.0;
        stats[i].total_ms = (t5 - t0) * 1000.0;
        stats[i].success = (rc == SQLI_OK);

        if (rc == SQLI_OK) successful++;
    }

    double t_end_total = get_time_sec();
    double total_elapsed_sec = t_end_total - t_start_total;

    double *arr_connect = malloc(sizeof(double) * (successful ? successful : 1));
    double *arr_close = malloc(sizeof(double) * (successful ? successful : 1));
    double *arr_total = malloc(sizeof(double) * (successful ? successful : 1));
    double *arr_query = do_query ? malloc(sizeof(double) * (successful ? successful : 1)) : NULL;

    size_t idx = 0;
    for (int i = 0; i < iterations; i++) {
        if (stats[i].success) {
            arr_connect[idx] = stats[i].connect_ms;
            arr_close[idx] = stats[i].close_ms;
            arr_total[idx] = stats[i].total_ms;
            if (do_query && arr_query) arr_query[idx] = stats[i].query_ms;
            idx++;
        }
    }

    if (!json_out) {
        printf("================================================================================\n");
        printf("Benchmark: %s\n", label);
        printf("URI: %s\n", uri);
        printf("Iterations: %d | Successful: %zu | Failed: %zu | Total wall time: %.3fs\n",
               iterations, successful, (size_t)iterations - successful, total_elapsed_sec);
        if (total_elapsed_sec > 0) {
            printf("Throughput: %.2f connects/sec\n", (double)successful / total_elapsed_sec);
        }
        printf("Latency Breakdown:\n");
        print_stats("Connect", arr_connect, successful);
        if (do_query && arr_query) {
            print_stats("Query (1-row)", arr_query, successful);
        }
        print_stats("Close", arr_close, successful);
        print_stats("Total/cycle", arr_total, successful);
        printf("================================================================================\n");
    } else {
        printf("{\"label\":\"%s\",\"iterations\":%d,\"successful\":%zu,\"wall_sec\":%.4f,\"throughput\":%.2f}\n",
               label, iterations, successful, total_elapsed_sec, total_elapsed_sec > 0 ? (double)successful / total_elapsed_sec : 0.0);
    }

    free(stats);
    free(arr_connect);
    free(arr_close);
    free(arr_total);
    if (arr_query) free(arr_query);

    return (successful == (size_t)iterations) ? 0 : 1;
}
