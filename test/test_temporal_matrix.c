/* Deterministic temporal matrix, adapted from docs/test_temporal_matrix.c.
 * CTest runs generator checks offline; --live requires SQLI_TEST_*.
 * Expected semantic fields are generated independently of libsqli's decoder.
 */
#include "libsqli/sqli.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MATRIX_CAPACITY 2048
#define MATRIX_CASES 2036
#define DEFAULT_SEED UINT64_C(0xc0ffee)

enum field { YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, FRACTION };
static const char *const field_names[] = {
    "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND", "FRACTION"
};
struct temporal_case {
    char type[64];
    char value[64];
    int fields[7];
    int start, end, scale, precision, variant;
    bool interval, negative, is_null;
};
struct matrix {
    struct temporal_case cases[MATRIX_CAPACITY];
    size_t count;
    uint64_t rng;
};

static unsigned power10(unsigned n)
{
    unsigned value = 1;
    while (n-- > 0)
        value *= 10;
    return value;
}

static unsigned random_bounded(struct matrix *m, unsigned limit)
{
    uint64_t x = m->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    m->rng = x;
    return (unsigned)(x % limit);
}

static bool build_case(struct matrix *m, bool interval, int start, int end,
                       int scale, int precision, int variant)
{
    if (m->count == MATRIX_CAPACITY)
        return false;
    struct temporal_case *c = &m->cases[m->count++];
    c->interval = interval;
    c->start = start;
    c->end = end;
    c->scale = scale;
    c->precision = precision;
    c->variant = variant;
    c->is_null = variant == (interval ? 5 : 3);
    c->negative = interval && (variant == 2 || variant == 4);
    bool maximum = variant == 1 || (interval && variant == 2);
    bool random = interval ? (variant == 3 || variant == 4) : variant == 2;
    char first[24], last[24];
    if (interval && start != FRACTION)
        snprintf(first, sizeof(first), "%s(%d)", field_names[start], precision);
    else
        snprintf(first, sizeof(first), "%s", field_names[start]);
    if (end == FRACTION)
        snprintf(last, sizeof(last), "FRACTION(%d)", scale);
    else
        snprintf(last, sizeof(last), "%s", field_names[end]);
    int n = snprintf(c->type, sizeof(c->type), "%s %s TO %s",
                     interval ? "INTERVAL" : "DATETIME", first, last);
    if (n < 0 || (size_t)n >= sizeof(c->type))
        return false;

    bool nonzero = false;
    size_t used = 0;
    if (c->negative)
        c->value[used++] = '-';
    for (int f = start; f <= end; f++) {
        static const int datetime_min[] = {1, 1, 1, 0, 0, 0, 0};
        static const int datetime_max[] = {9999, 12, 31, 23, 59, 59, 0};
        static const int interval_max[] = {0, 11, 0, 23, 59, 59, 0};
        int low = interval ? 0 : datetime_min[f];
        int high = interval ? interval_max[f] : datetime_max[f];
        if (f == FRACTION)
            high = (int)power10((unsigned)scale) - 1;
        else if (interval && f == start)
            high = (int)power10((unsigned)precision) - 1;
        /* Random partial dates must be valid regardless of omitted fields. */
        if (!interval && f == DAY && random)
            high = 28;
        int value = maximum ? high : low;
        if (random)
            value = low + (int)random_bounded(m, (unsigned)(high - low + 1));
        c->fields[f] = value;
        nonzero = nonzero || value != 0;
        const char *separator = "";
        if (f == FRACTION)
            separator = ".";
        else if (f > start)
            separator = f <= DAY ? "-" : f == HOUR ? " " : ":";
        int width = f == FRACTION ? scale : (!interval && f == YEAR ? 4 : 2);
        if (interval && f == start && f != FRACTION)
            width = 1;
        n = snprintf(c->value + used, sizeof(c->value) - used,
                     "%s%0*d", separator, width, value);
        if (n < 0 || (size_t)n >= sizeof(c->value) - used)
            return false;
        used += (size_t)n;
    }
    if (!nonzero && c->negative) {
        memmove(c->value, c->value + 1, strlen(c->value));
        c->negative = false;
    }
    return true;
}

static bool add_qualifier(struct matrix *m, bool interval, int start, int end,
                          int scale, int precision)
{
    for (int variant = 0; variant < (interval ? 6 : 4); variant++) {
        if (!build_case(m, interval, start, end, scale, precision, variant))
            return false;
    }
    return true;
}

static bool generate(struct matrix *m, uint64_t seed)
{
    memset(m, 0, sizeof(*m));
    m->rng = seed ? seed : UINT64_C(0x9e3779b97f4a7c15);
    for (int start = YEAR; start <= FRACTION; start++) {
        for (int end = start; end <= FRACTION; end++) {
            for (int scale = end == FRACTION ? 1 : 0;
                 scale <= (end == FRACTION ? 5 : 0); scale++) {
                if (!add_qualifier(m, false, start, end, scale, 0))
                    return false;
            }
        }
    }
    for (int start = YEAR; start <= FRACTION; start++) {
        int last = start <= MONTH ? MONTH : FRACTION;
        for (int end = start; end <= last; end++) {
            for (int scale = end == FRACTION ? 1 : 0;
                 scale <= (end == FRACTION ? 5 : 0); scale++) {
                for (int precision = start == FRACTION ? 0 : 1;
                     precision <= (start == FRACTION ? 0 : 9); precision++) {
                    if (!add_qualifier(m, true, start, end, scale, precision))
                        return false;
                }
            }
        }
    }
    return m->count == MATRIX_CASES;
}

static bool parse_number(const char *text, uint64_t *value)
{
    if (text == NULL || *text == '\0' || !isdigit((unsigned char)*text))
        return false;
    char *end;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || *end != '\0' || end == text)
        return false;
    *value = (uint64_t)parsed;
    return true;
}

static int self_test(struct matrix *m)
{
    struct matrix *copy = malloc(sizeof(*copy));
    if (copy == NULL)
        return 1;
    bool ok = generate(m, DEFAULT_SEED) && generate(copy, DEFAULT_SEED) &&
              memcmp(m, copy, sizeof(*m)) == 0;
    size_t dt_types = 0, iv_types = 0, nulls = 0;
    for (size_t i = 0; ok && i < m->count; i++) {
        const struct temporal_case *c = &m->cases[i];
        if (c->variant == 0) {
            if (c->interval) iv_types++; else dt_types++;
            for (size_t j = 0; j < i; j++) {
                if (m->cases[j].variant == 0 && strcmp(c->type, m->cases[j].type) == 0)
                    ok = false;
            }
        }
        if (c->is_null) nulls++;
        if (c->interval && c->start != FRACTION &&
            (unsigned)c->fields[c->start] >= power10((unsigned)c->precision))
            ok = false;
        if (c->end == FRACTION &&
            (unsigned)c->fields[FRACTION] >= power10((unsigned)c->scale))
            ok = false;
    }
    ok = ok && dt_types == 56 && iv_types == 302 && nulls == 358;
    ok = ok && strcmp(m->cases[0].type, "DATETIME YEAR TO YEAR") == 0 &&
         strcmp(m->cases[0].value, "0001") == 0 &&
         strcmp(m->cases[1].value, "9999") == 0;
    ok = ok && generate(copy, DEFAULT_SEED + 1) &&
         strcmp(m->cases[2].value, copy->cases[2].value) != 0;
    uint64_t number;
    ok = ok && !parse_number("-1", &number) && !parse_number("1x", &number) &&
         !parse_number("18446744073709551616", &number);
    free(copy);
    printf("temporal generator: %zu cases, %zu DATETIME + %zu INTERVAL qualifiers: %s\n",
           m->count, dt_types, iv_types, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static bool execute_sql(sqli_conn_t *conn, const char *sql)
{
    sqli_result_t *result = NULL;
    sqli_status rc = sqli_query(conn, sql, &result);
    sqli_result_destroy(result);
    return rc == SQLI_OK;
}

static bool check_value(sqli_result_t *result, int column,
                        const struct temporal_case *c)
{
    int fields[7];
    bool is_null, negative = false;
    int scale, start, end;
    if (c->interval) {
        sqli_interval_value v;
        if (sqli_result_get_interval(result, column, &v) != SQLI_OK)
            return false;
        int decoded[] = {v.year, v.month, v.day, v.hour, v.minute, v.second, v.fraction};
        memcpy(fields, decoded, sizeof(fields));
        is_null = v.is_null;
        negative = v.negative;
        scale = v.fraction_scale;
        start = v.start_qualifier;
        end = v.end_qualifier;
    } else {
        sqli_datetime_value v;
        if (sqli_result_get_datetime(result, column, &v) != SQLI_OK)
            return false;
        int decoded[] = {v.year, v.month, v.day, v.hour, v.minute, v.second, v.fraction};
        memcpy(fields, decoded, sizeof(fields));
        is_null = v.is_null;
        scale = v.fraction_scale;
        start = v.start_qualifier;
        end = v.end_qualifier;
    }
    if (is_null != c->is_null || sqli_result_is_null(result, column) != c->is_null) {
        fprintf(stderr, "  NULL expected=%d actual=%d\n", c->is_null, is_null);
        return false;
    }
    if (c->is_null)
        return true;
    if (negative != c->negative || start != (c->start == FRACTION ? 12 : c->start * 2) ||
        end != (c->end == FRACTION ? 10 + c->scale : c->end * 2) ||
        (c->end == FRACTION && scale != c->scale)) {
        fprintf(stderr, "  metadata start=%d end=%d scale=%d negative=%d\n",
                start, end, scale, negative);
        return false;
    }
    for (int field = c->start; field <= c->end; field++) {
        if (fields[field] != c->fields[field]) {
            fprintf(stderr, "  field=%s expected=%d actual=%d\n",
                    field_names[field], c->fields[field], fields[field]);
            return false;
        }
    }
    return true;
}

/* Informix pads INTERVAL leading fields with spaces. FRACTION-only text may
 * have an optional zero before the decimal point. Preserve all other digits
 * and separators when comparing the public string getter to the server. */
static void normalize_text(char *text)
{
    size_t length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1]))
        text[--length] = '\0';
    size_t start = 0;
    while (isspace((unsigned char)text[start]))
        start++;
    if (start > 0)
        memmove(text, text + start, strlen(text + start) + 1);
    size_t sign = text[0] == '-' ? 1 : 0;
    if (text[sign] == '0' && text[sign + 1] == '.')
        memmove(text + sign, text + sign + 1, strlen(text + sign + 1) + 1);
}

static bool check_text(sqli_result_t *result, const struct temporal_case *c)
{
    if (c->is_null)
        return true;
    char server[128], client[128];
    const char *value = sqli_result_get_string(result, 6);
    if (value == NULL || strlen(value) >= sizeof(server))
        return false;
    snprintf(server, sizeof(server), "%s", value);
    value = c->interval ? sqli_result_get_interval_string(result, 1) :
                          sqli_result_get_datetime_string(result, 1);
    if (value == NULL || strlen(value) >= sizeof(client))
        return false;
    snprintf(client, sizeof(client), "%s", value);
    normalize_text(server);
    normalize_text(client);
    if (strcmp(server, client) != 0) {
        fprintf(stderr, "  text server='%s' client='%s'\n", server, client);
        return false;
    }
    return true;
}

/* Session-local temporary table prevents name collisions and leaves no durable
 * fixture on failure. Every case verifies the server value, semantic fields,
 * NULLs, and following columns in a mixed projection for both insertion paths. */
static bool run_case(sqli_conn_t *conn, const struct temporal_case *c,
                     const char **operation)
{
    char sql[768], literal[160];
    sqli_result_t *result = NULL;
    sqli_stmt_t *stmt = NULL;
    bool created = false, ok = false;
    const char *qualifier = strchr(c->type, ' ') + 1;
    if (c->is_null)
        snprintf(literal, sizeof(literal), "CAST(NULL AS %s)", c->type);
    else
        snprintf(literal, sizeof(literal), "%s%s(%s) %s",
                 c->negative ? "-" : "", c->interval ? "INTERVAL" : "DATETIME",
                 c->value + (c->negative ? 1 : 0), qualifier);
    *operation = "create";
    snprintf(sql, sizeof(sql), "CREATE TEMP TABLE sqli_temporal_matrix (id INT, v %s) WITH NO LOG", c->type);
    if (!execute_sql(conn, sql))
        goto cleanup;
    created = true;
    *operation = "literal insert";
    snprintf(sql, sizeof(sql), "INSERT INTO sqli_temporal_matrix VALUES (1, %s)", literal);
    if (!execute_sql(conn, sql))
        goto cleanup;
    *operation = "prepare insert";
    int parameters = 0;
    if (sqli_prepare(conn, "INSERT INTO sqli_temporal_matrix VALUES (2, ?)", &parameters, &stmt) != SQLI_OK || parameters != 1)
        goto cleanup;
    *operation = "bind insert";
    sqli_status rc = c->is_null ? sqli_bind_null(stmt, 1) : c->interval ?
        sqli_bind_interval(stmt, 1, c->value) : sqli_bind_datetime(stmt, 1, c->value);
    if (rc != SQLI_OK || sqli_execute(stmt) != SQLI_OK)
        goto cleanup;
    sqli_stmt_destroy(stmt);
    stmt = NULL;
    *operation = "mixed projection";
    snprintf(sql, sizeof(sql),
             "SELECT id,v,2468,-INTERVAL(3-02) YEAR(3) TO MONTH,1357,"
             "CASE WHEN %s THEN 1 ELSE 0 END,CAST(v AS LVARCHAR(100)) "
             "FROM sqli_temporal_matrix ORDER BY id",
             c->is_null ? "v IS NULL" : "v = v");
    /* Compare stored values to the independent typed SQL literal, not merely
     * two values decoded by the same client implementation. */
    if (!c->is_null)
        snprintf(sql, sizeof(sql),
                 "SELECT id,v,2468,-INTERVAL(3-02) YEAR(3) TO MONTH,1357,"
                 "CASE WHEN v = %s THEN 1 ELSE 0 END,CAST(v AS LVARCHAR(100)) "
                 "FROM sqli_temporal_matrix ORDER BY id", literal);
    if (sqli_query(conn, sql, &result) != SQLI_OK)
        goto cleanup;
    for (int id = 1; id <= 2; id++) {
        *operation = id == 1 ? "literal row decode" : "bound row decode";
        if (!sqli_result_next(result) || sqli_result_get_int(result, 0) != id ||
            sqli_result_get_int(result, 2) != 2468 ||
            sqli_result_get_int(result, 4) != 1357 || sqli_result_get_int(result, 5) != 1 ||
            !check_value(result, 1, c) || !check_text(result, c))
            goto cleanup;
        sqli_interval_value sentinel;
        if (sqli_result_get_interval(result, 3, &sentinel) != SQLI_OK ||
            sentinel.is_null || !sentinel.negative || sentinel.year != 3 || sentinel.month != 2)
            goto cleanup;
    }
    *operation = "end of result";
    ok = !sqli_result_next(result);
cleanup:
    if (!ok) {
        sqli_error_info error = {0};
        sqli_error_get_info(conn, &error);
        fprintf(stderr, "  operation=%s sqlcode=%d isamcode=%d\n", *operation,
                error.sqlcode, error.isamcode);
    }
    sqli_result_destroy(result);
    sqli_stmt_destroy(stmt);
    if (created && !execute_sql(conn, "DROP TABLE sqli_temporal_matrix")) {
        *operation = "drop";
        return false;
    }
    return ok;
}

static int run_live(struct matrix *m, uint64_t seed, int only)
{
    sqli_connect_params p = {0};
    p.hostname = getenv("SQLI_TEST_HOST");
    p.service = getenv("SQLI_TEST_PORT");
    p.database = getenv("SQLI_TEST_DB");
    p.username = getenv("SQLI_TEST_USER");
    p.password = getenv("SQLI_TEST_PASS");
    p.server = getenv("SQLI_TEST_SERVER");
    p.client_locale = getenv("SQLI_CLIENT_LOCALE");
    p.db_locale = getenv("SQLI_DB_LOCALE");
    if (!p.hostname || !*p.hostname || !p.service || !*p.service ||
        !p.database || !*p.database || !p.username || !*p.username || !p.password) {
        fprintf(stderr, "SQLI_TEST_HOST/PORT/DB/USER/PASS not set; temporal live test skipped\n");
        return 77;
    }
    sqli_conn_t *conn = NULL;
    if (sqli_create(&conn) != SQLI_OK)
        return 1;
    if (sqli_connect(conn, &p) != SQLI_OK) {
        fprintf(stderr, "temporal matrix: connection failed: %s\n", sqli_error(conn));
        sqli_destroy(conn);
        return 1;
    }
    size_t tested = 0, failed = 0;
    fprintf(stderr, "temporal matrix: seed=0x%" PRIx64 " generated=%zu only=%d\n", seed, m->count, only);
    for (size_t i = 0; i < m->count; i++) {
        if (only >= 0 && (size_t)only != i)
            continue;
        const struct temporal_case *c = &m->cases[i];
        const char *operation = "";
        tested++;
        if (!run_case(conn, c, &operation)) {
            failed++;
            fprintf(stderr, "FAIL seed=0x%" PRIx64 " case=%zu type='%s' value='%s' variant=%d operation=%s\n",
                    seed, i, c->type, c->is_null ? "NULL" : c->value, c->variant, operation);
        }
        if (tested % 100 == 0)
            fprintf(stderr, "temporal matrix: tested=%zu failed=%zu\n", tested, failed);
    }
    sqli_close(conn);
    sqli_destroy(conn);
    printf("temporal matrix: seed=0x%" PRIx64 " tested=%zu passed=%zu failed=%zu\n",
           seed, tested, tested - failed, failed);
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "--self-test") != 0 &&
        strcmp(argv[1], "--live") != 0 && strcmp(argv[1], "--list") != 0)) {
        fprintf(stderr, "usage: %s --self-test|--list|--live\n", argv[0]);
        return 2;
    }
    uint64_t seed = DEFAULT_SEED, only_value = 0;
    const char *seed_text = getenv("SQLI_FUZZ_SEED");
    const char *only_text = getenv("SQLI_FUZZ_ONLY");
    if ((seed_text && !parse_number(seed_text, &seed)) ||
        (only_text && (!parse_number(only_text, &only_value) || only_value >= MATRIX_CASES))) {
        fprintf(stderr, "invalid SQLI_FUZZ_SEED or SQLI_FUZZ_ONLY (case range 0..%d)\n", MATRIX_CASES - 1);
        return 2;
    }
    struct matrix *m = malloc(sizeof(*m));
    if (m == NULL)
        return 1;
    int rc = 0;
    if (strcmp(argv[1], "--self-test") == 0) {
        rc = self_test(m);
    } else if (!generate(m, seed)) {
        fprintf(stderr, "temporal generation failed or case capacity exceeded\n");
        rc = 1;
    } else if (strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < m->count; i++) {
            const struct temporal_case *c = &m->cases[i];
            printf("%zu\t%s\t%s\tvariant=%d\n", i, c->type,
                   c->is_null ? "NULL" : c->value, c->variant);
        }
    } else {
        rc = run_live(m, seed, only_text ? (int)only_value : -1);
    }
    free(m);
    return rc;
}
