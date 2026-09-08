/* Live decimal receive matrix. Server casts are an independent reference for
 * the native encoder; decoding is compared numerically with the source value.
 * No production getter or text binder is used to obtain the decimal bytes. */
#include "sqli_decimal_codec.h"
#include "sqli_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { sql_capacity = 256, text_capacity = 96, coefficient_capacity = 33,
       decimal_column = 0, sentinel_column = 1, projection_columns = 2,
       integer_width = 4, sentinel = 2468, skip_status = 77 };

static bool check_case(sqli_conn_t *conn, sqli_decimal_t *source, sqli_decimal_t *decoded,
                        unsigned precision, unsigned scale, const char *text, bool is_null)
{
    char sql[sql_capacity];
    char type[text_capacity];
    int n = scale == 255 ? snprintf(type, sizeof(type), "DECIMAL(%u)", precision) :
        snprintf(type, sizeof(type), "DECIMAL(%u,%u)", precision, scale);
    if (n < 0 || (size_t)n >= sizeof(type))
        return false;
    n = is_null ? snprintf(sql, sizeof(sql), "SELECT FIRST 1 CAST(NULL AS %s), %d FROM systables", type, sentinel) :
        snprintf(sql, sizeof(sql), "SELECT FIRST 1 CAST('%s' AS %s), %d FROM systables", text, type, sentinel);
    if (n < 0 || (size_t)n >= sizeof(sql))
        return false;
    uint16_t descriptor = (uint16_t)((precision << 8) | scale);
    uint8_t encoded[SQLI_DECIMAL_WIRE_CAPACITY];
    size_t length = 0;
    sqli_result_t *result = NULL;
    bool ok = sqli_decimal_parse(source, text, strlen(text), is_null) == SQLI_OK &&
        sqli_decimal_encode_wire(source, descriptor, encoded, sizeof(encoded), &length) == SQLI_OK &&
        sqli_query(conn, sql, &result) == SQLI_OK && sqli_result_fetch(result) == SQLI_OK &&
        result->column_count == projection_columns &&
        result->columns[decimal_column].type == SQLI_TYPE_DECIMAL &&
        result->columns[decimal_column].encoded_length == descriptor &&
        result->tuple_buffer != NULL && result->tuple_len == length + integer_width &&
        memcmp(encoded, result->tuple_buffer, length) == 0 &&
        sqli_result_get_int(result, sentinel_column) == sentinel &&
        sqli_result_get_decimal(result, decimal_column, decoded) == SQLI_OK;
    if (ok) {
        bool actual_null = false;
        ok = sqli_decimal_is_null(decoded, &actual_null) == SQLI_OK && actual_null == is_null;
        if (ok && !is_null) {
            int ordering = 1;
            ok = sqli_decimal_compare(source, decoded, &ordering) == SQLI_OK && ordering == 0;
        }
        ok = ok && sqli_result_fetch(result) == SQLI_EOF;
    }
    if (!ok)
        fprintf(stderr, "decimal matrix failed: %s value=%s\n", type, is_null ? "NULL" : text);
    sqli_result_destroy(result);
    return ok;
}

static bool run_matrix(sqli_conn_t *conn, sqli_decimal_t *source, sqli_decimal_t *decoded, size_t *tested)
{
    for (unsigned precision = 1; precision <= 32; precision++) {
        char coefficient[coefficient_capacity];
        memset(coefficient, '9', precision);
        coefficient[precision] = '\0';
        for (unsigned scale = 0; scale <= precision; scale++) {
            /* At precision 32, odd alignment leaves 31 significant digits in
             * the server's 16 coefficient groups. Keep the declared scale. */
            coefficient[precision - 1] = precision == 32 && (scale & 1u) != 0 ? '0' : '9';
            for (unsigned variant = 0; variant < 4; variant++) {
                char text[text_capacity];
                int n = snprintf(text, sizeof(text), "%s%se-%u", variant == 1 ? "-" : "",
                                  variant >= 2 ? "0" : coefficient, scale);
                if (n < 0 || (size_t)n >= sizeof(text))
                    return false;
                if (!check_case(conn, source, decoded, precision, scale, text, variant == 3))
                    return false;
                (*tested)++;
            }
        }
    }
    fprintf(stderr, "decimal matrix: fixed descriptors passed (%zu cases)\n", *tested);
    for (unsigned precision = 1; precision <= 32; precision++) {
        if ((precision - 1) % 8 == 0)
            fprintf(stderr, "decimal matrix: floating precision=%u tested=%zu\n", precision, *tested);
        for (int exponent = -64; exponent <= 63; exponent++) {
            for (unsigned negative = 0; negative < 2; negative++) {
                char text[text_capacity];
                int n = snprintf(text, sizeof(text), "%s1e%d", negative ? "-" : "", 2 * (exponent - 1));
                if (n < 0 || (size_t)n >= sizeof(text))
                    return false;
                if (!check_case(conn, source, decoded, precision, 255, text, false))
                    return false;
                (*tested)++;
            }
        }
    }
    return true;
}

int main(void)
{
    sqli_connect_params params = {
        .hostname = getenv("SQLI_TEST_HOST"), .service = getenv("SQLI_TEST_PORT"),
        .database = getenv("SQLI_TEST_DB"), .username = getenv("SQLI_TEST_USER"),
        .password = getenv("SQLI_TEST_PASS"), .server = getenv("SQLI_TEST_SERVER"),
        .client_locale = getenv("SQLI_CLIENT_LOCALE"), .db_locale = getenv("SQLI_DB_LOCALE")
    };
    if (params.hostname == NULL || *params.hostname == '\0' || params.service == NULL ||
        *params.service == '\0' || params.database == NULL || *params.database == '\0' ||
        params.username == NULL || *params.username == '\0' || params.password == NULL) {
        fprintf(stderr, "decimal matrix: missing SQLI_TEST_* connection settings\n");
        return skip_status;
    }
    sqli_conn_t *conn = NULL;
    sqli_decimal_t *source = NULL, *decoded = NULL;
    size_t tested = 0;
    bool ok = sqli_create(&conn) == SQLI_OK && sqli_connect(conn, &params) == SQLI_OK &&
        sqli_decimal_create(&source) == SQLI_OK && sqli_decimal_create(&decoded) == SQLI_OK &&
        run_matrix(conn, source, decoded, &tested);
    sqli_decimal_destroy(source);
    sqli_decimal_destroy(decoded);
    sqli_destroy(conn);
    printf("decimal live matrix: tested=%zu status=%s\n", tested, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
