/* Fixed receive fixtures, optionally checked against independent server literals.
 * This test never calls a libsqli encoder to construct expected bytes.
 */
#include "libsqli/sqli.h"
#include "sqli_internal.h"
#include "native_wire_test.h"
#include "sqli_temporal_codec.h"
#include "sqli_decimal_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    fixture_wire_capacity = 18,
    fixture_sql_capacity = 256,
    fixture_text_capacity = 64,
    value_column = 0,
    sentinel_column = 1,
    fixture_columns = 2,
    integer_wire_width = 4,
    sentinel_value = 2468,
    exit_success = 0,
    exit_failure = 1,
    exit_usage = 2,
    exit_skip = 77
};

struct wire_fixture {
    const char *name;
    const char *expression;
    sqli_column_type type;
    uint32_t qualifier;
    uint8_t wire[fixture_wire_capacity];
    size_t wire_length;
    const char *text;
    bool is_null;
};

/* These are receive payloads, not SQ_BIND parameter frames. */
static const struct wire_fixture fixtures[] = {
    {"date_epoch", "MDY(12,31,1899)", SQLI_TYPE_DATE, 4,
     {0, 0, 0, 0}, 4, "1899-12-31", false},
    {"date_before_epoch", "MDY(12,30,1899)", SQLI_TYPE_DATE, 4,
     {0xff, 0xff, 0xff, 0xff}, 4, "1899-12-30", false},
    {"date_unix_epoch", "MDY(1,1,1970)", SQLI_TYPE_DATE, 4,
     {0, 0, 0x63, 0xe0}, 4, "1970-01-01", false},
    {"date_null", "CAST(NULL AS DATE)", SQLI_TYPE_DATE, 4,
     {0x80, 0, 0, 0}, 4, "", true},
    {"decimal_scale", "CAST(123.4500 AS DECIMAL(8,4))", SQLI_TYPE_DECIMAL, 0x0804,
     {0xc2, 1, 23, 45, 0}, 5, "123.4500", false},
    {"decimal_negative", "CAST(-123.4500 AS DECIMAL(8,4))", SQLI_TYPE_DECIMAL, 0x0804,
     {0x3d, 98, 76, 55, 0}, 5, "-123.4500", false},
    {"numeric_alias", "CAST(123.4500 AS NUMERIC(8,4))", SQLI_TYPE_DECIMAL, 0x0804,
     {0xc2, 1, 23, 45, 0}, 5, "123.4500", false},
    {"decimal_zero", "CAST(0 AS DECIMAL(8,4))", SQLI_TYPE_DECIMAL, 0x0804,
     {0x80, 0, 0, 0, 0}, 5, "0.0000", false},
    {"decimal_null", "CAST(NULL AS DECIMAL(8,4))", SQLI_TYPE_DECIMAL, 0x0804,
     {0, 0, 0, 0, 0}, 5, "", true},
    {"money", "CAST(123.45 AS MONEY(8,2))", SQLI_TYPE_MONEY, 0x0802,
     {0xc2, 1, 23, 45, 0}, 5, "123.45", false},
    {"decimal_minimum", "CAST('1e-130' AS DECIMAL(32))", SQLI_TYPE_DECIMAL, 0x20ff,
     {0x80,1}, 18, "1E-130", false},
    {"decimal_negative_minimum", "CAST('-1e-130' AS DECIMAL(32))", SQLI_TYPE_DECIMAL, 0x20ff,
     {0x7f,99}, 18, "-1E-130", false},
    {"decimal_high_exponent", "CAST('1e125' AS DECIMAL(32))", SQLI_TYPE_DECIMAL, 0x20ff,
     {0xff,10}, 18, "1E+125", false},
    {"decimal_negative_high_exponent", "CAST('-1e125' AS DECIMAL(32))", SQLI_TYPE_DECIMAL, 0x20ff,
     {0,90}, 18, "-1E+125", false},
    {"decimal_floating_scale", "CAST(123.4500 AS DECIMAL(32))", SQLI_TYPE_DECIMAL, 0x20ff,
     {0xc2,1,23,45}, 18, "123.45", false},
    {"decimal_floating_null", "CAST(NULL AS DECIMAL(32))", SQLI_TYPE_DECIMAL, 0x20ff,
     {0}, 18, "", true},
    {"decimal_floating_zero", "CAST(0 AS DECIMAL(32))", SQLI_TYPE_DECIMAL, 0x20ff,
     {0x80}, 18, "0", false},
    {"decimal_one_digit", "CAST(0.1 AS DECIMAL(1,1))", SQLI_TYPE_DECIMAL, 0x0101,
     {0xc0,10}, 2, "0.1", false},
    {"decimal_odd_scale", "CAST(-0.001 AS DECIMAL(3,3))", SQLI_TYPE_DECIMAL, 0x0303,
     {0x40,90,0}, 3, "-0.001", false},
    {"decimal_scale32", "CAST('1e-32' AS DECIMAL(32,32))", SQLI_TYPE_DECIMAL, 0x2020,
     {0xb1,1}, 17, "1E-32", false},
    {"decimal_scale31", "CAST('1e-31' AS DECIMAL(32,31))", SQLI_TYPE_DECIMAL, 0x201f,
     {0xb1,10}, 18, "1E-31", false},
    {"decimal_precision32", "CAST(999999999999999999999999999999.99 AS DECIMAL(32,2))",
     SQLI_TYPE_DECIMAL, 0x2002,
     {0xcf,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99},
     17, "999999999999999999999999999999.99", false},
    {"datetime_full", "DATETIME(2026-06-20 12:34:56) YEAR TO SECOND",
     SQLI_TYPE_DATETIME, 0x0e0a,
     {0xc7, 20, 26, 6, 20, 12, 34, 56}, 8, "2026-06-20 12:34:56", false},
    {"datetime_year_one", "DATETIME(0001) YEAR TO YEAR",
     SQLI_TYPE_DATETIME, 0x0400, {0xc6, 1, 0}, 3, "0001", false},
    {"datetime_partial", "DATETIME(23:59) HOUR TO MINUTE",
     SQLI_TYPE_DATETIME, 0x0468, {0xc3, 23, 59}, 3, "23:59", false},
    {"datetime_fraction", "DATETIME(.00001) FRACTION TO FRACTION(5)",
     SQLI_TYPE_DATETIME, 0x05cf, {0xbe, 10, 0, 0}, 4, ".00001", false},
    {"datetime_null", "CAST(NULL AS DATETIME YEAR TO SECOND)",
     SQLI_TYPE_DATETIME, 0x0e0a, {0}, 8, "", true},
    {"interval_negative", "-INTERVAL(999.99) SECOND(3) TO FRACTION(2)",
     SQLI_TYPE_INTERVAL, 0x05ac, {0x3d, 90, 0, 1}, 4, "-999.99", false},
    {"interval_fraction", "-INTERVAL(.00001) FRACTION TO FRACTION(5)",
     SQLI_TYPE_INTERVAL, 0x05cf, {0x41, 90, 0, 0}, 4, "-.00001", false},
    {"interval_zero", "INTERVAL(.00000) FRACTION TO FRACTION(5)",
     SQLI_TYPE_INTERVAL, 0x05cf, {0x80, 0, 0, 0}, 4, ".00000", false},
    {"interval_odd_leading", "INTERVAL(3 03:27:01.12345) DAY(3) TO FRACTION(5)",
     SQLI_TYPE_INTERVAL, 0x0e4f, {0xc4, 3, 3, 27, 1, 12, 34, 50, 0},
     9, "3 03:27:01.12345", false},
    {"interval_null", "CAST(NULL AS INTERVAL DAY(3) TO FRACTION(5))",
     SQLI_TYPE_INTERVAL, 0x0e4f, {0}, 9, "", true}
};

/* Fixed fixture bytes remain independent of the production codec. The encoded
 * output is also used by the live bind probe; parameter framing is test-only. */
static bool codec_payload(const struct wire_fixture *fixture, uint8_t *encoded, size_t *length)
{
    char text[fixture_text_capacity] = {0};
    size_t required = 0;
    bool is_null = false;
    bool ok;
    if (fixture->type == SQLI_TYPE_DATE) {
        sqli_date_t value;
        ok = sqli_date_decode_wire(fixture->wire, fixture->wire_length, &value) == SQLI_OK &&
             sqli_date_format(&value, text, sizeof(text), &required, &is_null) == SQLI_OK &&
             sqli_date_encode_wire(&value, encoded, fixture_wire_capacity, length) == SQLI_OK;
    } else if (fixture->type == SQLI_TYPE_DATETIME) {
        sqli_datetime_t *value = NULL;
        ok = sqli_datetime_create(&value) == SQLI_OK &&
             sqli_datetime_decode_wire(fixture->wire, fixture->wire_length, (uint16_t)fixture->qualifier, value) == SQLI_OK &&
             sqli_datetime_format(value, text, sizeof(text), &required, &is_null) == SQLI_OK &&
             sqli_datetime_encode_wire(value, (uint16_t)fixture->qualifier, encoded, fixture_wire_capacity, length) == SQLI_OK;
        sqli_datetime_destroy(value);
        /* Fixtures retain server text spelling; the new full formatter uses T. */
        enum { calendar_date_length = 10 };
        if (strlen(text) > calendar_date_length && text[calendar_date_length] == 'T')
            text[calendar_date_length] = ' ';
    } else if (fixture->type == SQLI_TYPE_INTERVAL) {
        sqli_interval_t *value = NULL;
        ok = sqli_interval_create(&value) == SQLI_OK &&
             sqli_interval_decode_wire(fixture->wire, fixture->wire_length, (uint16_t)fixture->qualifier, value) == SQLI_OK &&
             sqli_interval_format(value, text, sizeof(text), &required, &is_null) == SQLI_OK &&
             sqli_interval_encode_wire(value, (uint16_t)fixture->qualifier, encoded, fixture_wire_capacity, length) == SQLI_OK;
        sqli_interval_destroy(value);
    } else {
        sqli_decimal_t *value = NULL;
        ok = sqli_decimal_create(&value) == SQLI_OK &&
             sqli_decimal_decode_wire(fixture->wire, fixture->wire_length, (uint16_t)fixture->qualifier, value) == SQLI_OK &&
             sqli_decimal_format(value, text, sizeof(text), &required, &is_null) == SQLI_OK &&
             sqli_decimal_encode_wire(value, (uint16_t)fixture->qualifier, encoded, fixture_wire_capacity, length) == SQLI_OK;
        sqli_decimal_destroy(value);
    }
    return ok && is_null == fixture->is_null && strcmp(text, fixture->text) == 0 &&
           *length == fixture->wire_length && memcmp(encoded, fixture->wire, *length) == 0;
}

static bool check_result(sqli_result_t *result, const struct wire_fixture *fixture)
{
    if (result == NULL || result->column_count != fixture_columns ||
        sqli_result_fetch(result) != SQLI_OK)
        return false;
    const sqli_column_info *column = &result->columns[value_column];
    if (column->type != fixture->type || column->encoded_length != fixture->qualifier) {
        fprintf(stderr, "%s: descriptor type=%d qualifier=0x%04x\n",
                fixture->name, (int)column->type, (unsigned)column->encoded_length);
        return false;
    }
    /* Compare the tuple directly, including the following sentinel. This is
     * independent of the production column-width and value extractors. */
    if (result->tuple_buffer == NULL ||
        result->tuple_len != fixture->wire_length + integer_wire_width ||
        memcmp(result->tuple_buffer, fixture->wire, fixture->wire_length) != 0) {
        fprintf(stderr, "%s: unexpected tuple bytes:", fixture->name);
        if (result->tuple_buffer != NULL) {
            for (size_t i = 0; i < result->tuple_len; i++)
                fprintf(stderr, " %02x", (unsigned)result->tuple_buffer[i]);
        }
        fputc('\n', stderr);
        return false;
    }
    if (sqli_result_get_int(result, sentinel_column) != sentinel_value ||
        sqli_result_is_null(result, value_column) != fixture->is_null)
        return false;
    const char *text = NULL;
    char decimal_text[fixture_text_capacity];
    switch (fixture->type) {
    case SQLI_TYPE_DATE:
        text = sqli_result_get_date_string(result, value_column);
        break;
    case SQLI_TYPE_DATETIME:
        text = sqli_result_get_datetime_string(result, value_column);
        break;
    case SQLI_TYPE_INTERVAL:
        text = sqli_result_get_interval_string(result, value_column);
        break;
    default: {
        sqli_decimal_t *value = NULL;
        size_t required = 0;
        bool is_null = false;
        bool ok = sqli_decimal_create(&value) == SQLI_OK &&
            sqli_result_get_decimal(result, value_column, value) == SQLI_OK &&
            sqli_decimal_format(value, decimal_text, sizeof(decimal_text), &required, &is_null) == SQLI_OK;
        sqli_decimal_destroy(value);
        if (!ok)
            return false;
        if (is_null)
            decimal_text[0] = '\0';
        text = decimal_text;
        break;
    }
    }
    if (text == NULL || strcmp(text, fixture->text) != 0) {
        fprintf(stderr, "%s: expected text '%s', received '%s'\n",
                fixture->name, fixture->text, text != NULL ? text : "(missing)");
        return false;
    }
    return sqli_result_fetch(result) == SQLI_EOF;
}

static sqli_result_t *make_result(const struct wire_fixture *fixture)
{
    sqli_result_t *result = calloc(1, sizeof(*result));
    if (result == NULL)
        return NULL;
    result->stmt_id = -1;
    result->cursor = -1;
    result->current_row = -1;
    result->cur_cache_row = -1;
    result->column_count = fixture_columns;
    result->columns = calloc(fixture_columns, sizeof(*result->columns));
    result->rows = calloc(1, sizeof(*result->rows));
    result->row_lens = calloc(1, sizeof(*result->row_lens));
    if (result->columns == NULL || result->rows == NULL || result->row_lens == NULL)
        goto failed;
    result->row_count = 1;
    result->row_capacity = 1;
    result->row_lens[0] = fixture->wire_length + integer_wire_width;
    result->rows[0] = calloc(1, result->row_lens[0]);
    if (result->rows[0] == NULL)
        goto failed;
    memcpy(result->rows[0], fixture->wire, fixture->wire_length);
    size_t offset = fixture->wire_length;
    result->rows[0][offset + 2] = (uint8_t)(sentinel_value >> 8);
    result->rows[0][offset + 3] = (uint8_t)(sentinel_value & 0xff);
    result->columns[value_column].type = fixture->type;
    result->columns[value_column].encoded_length = fixture->qualifier;
    result->columns[sentinel_column].type = SQLI_TYPE_INT;
    result->columns[sentinel_column].encoded_length = integer_wire_width;
    result->eof = 1;
    return result;
failed:
    sqli_result_destroy(result);
    return NULL;
}

static bool execute_sql(sqli_conn_t *conn, const char *sql)
{
    sqli_result_t *result = NULL;
    sqli_status status = sqli_query(conn, sql, &result);
    sqli_result_destroy(result);
    return status == SQLI_OK;
}

static bool check_native_bind(sqli_conn_t *conn, const struct wire_fixture *fixture)
{
    char sql[fixture_sql_capacity];
    int length = snprintf(sql, sizeof(sql),
                          "SELECT FIRST 1\n"
                          "  %s AS v\n"
                          "FROM\n"
                          "  systables\n"
                          "INTO TEMP sqli_native_wire_fixture WITH NO LOG",
                          fixture->expression);
    if (length < 0 || (size_t)length >= sizeof(sql) || !execute_sql(conn, sql))
        return false;
    bool ok = false;
    sqli_stmt_t *stmt = NULL;
    sqli_result_t *result = NULL;
    int parameters = 0;
    if (!execute_sql(conn, "DELETE FROM sqli_native_wire_fixture"))
        goto cleanup;
    if (sqli_prepare(conn,
                     "INSERT INTO sqli_native_wire_fixture (v) VALUES (?)",
                     &parameters, &stmt) != SQLI_OK || parameters != 1)
        goto cleanup;
    uint8_t encoded[fixture_wire_capacity];
    size_t encoded_length = 0;
    if (!codec_payload(fixture, encoded, &encoded_length))
        goto cleanup;
    if (sqli_test_bind_wire(stmt, fixture->type, (uint16_t)fixture->qualifier,
                            encoded, encoded_length, fixture->is_null) != SQLI_OK)
        goto cleanup;
    length = snprintf(sql, sizeof(sql),
                      "SELECT\n"
                      "  v, %d\n"
                      "FROM\n"
                      "  sqli_native_wire_fixture", sentinel_value);
    if (length < 0 || (size_t)length >= sizeof(sql) ||
        sqli_query(conn, sql, &result) != SQLI_OK)
        goto cleanup;
    ok = check_result(result, fixture);
cleanup:
    sqli_result_destroy(result);
    sqli_stmt_destroy(stmt);
    if (!execute_sql(conn, "DROP TABLE sqli_native_wire_fixture"))
        ok = false;
    return ok;
}

static bool check_empty_result_descriptor(sqli_conn_t *conn)
{
    enum { alias_length = 128, sql_capacity = 256 };
    char alias[alias_length + 1];
    memset(alias, 'a', alias_length);
    alias[alias_length] = '\0';
    char sql[sql_capacity];
    int length = snprintf(sql, sizeof(sql),
        "SELECT 1 AS %s FROM systables WHERE 1 = 0", alias);
    if (length < 0 || (size_t)length >= sizeof(sql))
        return false;
    sqli_result_t *result = NULL;
    sqli_descriptor_t *snapshot = NULL;
    sqli_descriptor_info_t info;
    bool ok = sqli_query(conn, sql, &result) == SQLI_OK &&
        sqli_result_get_descriptor(result, &snapshot) == SQLI_OK &&
        sqli_descriptor_get_info(snapshot, &info) == SQLI_OK && info.field_count == 1 &&
        sqli_result_fetch(result) == SQLI_EOF;
    sqli_result_destroy(result);
    sqli_descriptor_field_t field;
    ok = ok && sqli_descriptor_get_field(snapshot, 0, &field) == SQLI_OK &&
        field.name.available && field.name.length == alias_length &&
        memcmp(field.name.data, alias, alias_length) == 0;
    sqli_descriptor_release(snapshot);
    return ok;
}

/* A descriptor's column count is not a universal input-parameter count.
 * These statements deliberately make the two counts differ. */
static bool check_describe(sqli_conn_t *conn)
{
    static const struct {
        const char *sql;
        int parameters;
        int columns;
        sqli_column_type types[fixture_columns];
        uint32_t encoded[fixture_columns];
    } cases[] = {
        {"SELECT FIRST 1\n"
         "  CAST(? AS INTEGER) AS i, CAST(1 AS DECIMAL(8,2)) AS d\n"
         "FROM\n"
         "  systables", 1, 2, {SQLI_TYPE_INT, SQLI_TYPE_DECIMAL}, {4, 0x0802}},
        {"INSERT INTO sqli_native_describe (i, d) VALUES (?, ?)",
         2, 2, {SQLI_TYPE_INT, SQLI_TYPE_DECIMAL}, {4, 0x0802}},
        {"UPDATE sqli_native_describe\n"
         "SET d = ?\n"
         "WHERE i = ?", 2, 1, {SQLI_TYPE_DECIMAL}, {0x0802}},
        {"SELECT FIRST 1\n"
         "  CAST(1 AS DECIMAL(8,2)) AS d\n"
         "FROM\n"
         "  systables", 0, 1, {SQLI_TYPE_DECIMAL}, {0x0802}}
    };
    if (!execute_sql(conn,
                     "CREATE TEMP TABLE sqli_native_describe (\n"
                     "  i INTEGER,\n"
                     "  d DECIMAL(8,2)\n"
                     ") WITH NO LOG"))
        return false;
    bool ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sqli_stmt_t *stmt = NULL;
        int parameters = -1;
        bool matches = sqli_prepare(conn, cases[i].sql, &parameters, &stmt) == SQLI_OK;
        if (matches)
            matches = parameters == cases[i].parameters &&
                      stmt->result.column_count == cases[i].columns;
        for (int col = 0; matches && col < cases[i].columns; col++) {
            matches = stmt->result.columns[col].type == cases[i].types[col] &&
                      stmt->result.columns[col].encoded_length == cases[i].encoded[col];
        }
        sqli_descriptor_t *snapshot = NULL;
        sqli_descriptor_info_t info = {0};
        matches = matches && sqli_stmt_get_descriptor(stmt, &snapshot) == SQLI_OK &&
                  sqli_descriptor_get_info(snapshot, &info) == SQLI_OK &&
                  info.field_count == (size_t)cases[i].columns;
        for (size_t col = 0; matches && col < info.field_count; col++) {
            sqli_descriptor_field_t field;
            matches = sqli_descriptor_get_field(snapshot, col, &field) == SQLI_OK &&
                      (field.type_raw & 0xff) == cases[i].types[col] &&
                      field.encoded_length == cases[i].encoded[col];
        }
        sqli_stmt_destroy(stmt);
        stmt = NULL;
        /* Snapshot fields remain readable after closing the server statement. */
        matches = matches && sqli_descriptor_get_info(snapshot, &info) == SQLI_OK &&
                  info.field_count == (size_t)cases[i].columns;
        sqli_descriptor_release(snapshot);
        if (!matches) {
            fprintf(stderr, "FAIL descriptor fixture: %zu\n", i);
            ok = false;
        }
        sqli_stmt_destroy(stmt);
    }
    if (!execute_sql(conn, "DROP TABLE sqli_native_describe"))
        ok = false;
    if (!check_empty_result_descriptor(conn)) {
        fprintf(stderr, "FAIL empty-result descriptor fixture\n");
        ok = false;
    }
    printf("native descriptor fixtures: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int connect_live(sqli_conn_t **out)
{
    sqli_connect_params params = {0};
    params.hostname = getenv("SQLI_TEST_HOST");
    params.service = getenv("SQLI_TEST_PORT");
    params.database = getenv("SQLI_TEST_DB");
    params.username = getenv("SQLI_TEST_USER");
    params.password = getenv("SQLI_TEST_PASS");
    params.server = getenv("SQLI_TEST_SERVER");
    params.client_locale = getenv("SQLI_CLIENT_LOCALE");
    params.db_locale = getenv("SQLI_DB_LOCALE");
    if (params.hostname == NULL || params.service == NULL || params.database == NULL ||
        params.username == NULL || params.password == NULL) {
        fprintf(stderr, "native wire fixtures: SQLI_TEST_* settings missing\n");
        return exit_skip;
    }
    sqli_status status = sqli_create(out);
    if (status == SQLI_OK)
        status = sqli_connect(*out, &params);
    if (status != SQLI_OK) {
        /* Do not include credentials or connection configuration in output. */
        fprintf(stderr, "native wire fixtures: connection failed, status=%d\n", (int)status);
        sqli_destroy(*out);
        *out = NULL;
        return exit_failure;
    }
    return exit_success;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "--offline") != 0 && strcmp(argv[1], "--live") != 0 &&
        strcmp(argv[1], "--live-bind") != 0)) {
        fprintf(stderr, "usage: %s --offline|--live|--live-bind\n", argv[0]);
        return exit_usage;
    }
    bool bind = strcmp(argv[1], "--live-bind") == 0;
    bool live = bind || strcmp(argv[1], "--live") == 0;
    sqli_conn_t *conn = NULL;
    if (live) {
        int status = connect_live(&conn);
        if (status != exit_success)
            return status;
    }
    size_t failed = 0;
    size_t count = sizeof(fixtures) / sizeof(fixtures[0]);
    for (size_t i = 0; i < count; i++) {
        const struct wire_fixture *fixture = &fixtures[i];
        sqli_result_t *result = NULL;
        bool ok = true;
        if (bind) {
            ok = check_native_bind(conn, fixture);
        } else if (live) {
            char sql[fixture_sql_capacity];
            int length = snprintf(sql, sizeof(sql),
                                  "SELECT FIRST 1\n"
                                  "  %s, %d\n"
                                  "FROM\n"
                                  "  systables", fixture->expression, sentinel_value);
            ok = length >= 0 && (size_t)length < sizeof(sql);
            if (ok)
                ok = sqli_query(conn, sql, &result) == SQLI_OK;
        } else {
            result = make_result(fixture);
        }
        if (!bind)
            ok = ok && check_result(result, fixture);
        uint8_t encoded[fixture_wire_capacity];
        size_t encoded_length = 0;
        ok = ok && codec_payload(fixture, encoded, &encoded_length);
        if (!ok) {
            fprintf(stderr, "FAIL native wire fixture: %s\n", fixture->name);
            failed++;
        }
        sqli_result_destroy(result);
    }
    bool descriptors_ok = !live || bind || check_describe(conn);
    sqli_destroy(conn);
    printf("native wire fixtures (%s): tested=%zu passed=%zu failed=%zu\n",
           bind ? "live-bind" : live ? "live" : "offline", count, count - failed, failed);
    return failed == 0 && descriptors_ok ? exit_success : exit_failure;
}
