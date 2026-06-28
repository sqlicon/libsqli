#define _GNU_SOURCE
#include "sqli_internal.h"

#include "sqli_tcp.h"
#include "sqli_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <endian.h>
#include <stdint.h>
#include <poll.h>

static void sqli_retry_sleep_ms(uint32_t delay_ms)
{
    if (delay_ms == 0)
        return;
    struct timespec req;
    req.tv_sec = (time_t)(delay_ms / 1000u);
    req.tv_nsec = (long)((delay_ms % 1000u) * 1000000u);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}

static void sqli_stmt_best_effort_control(sqli_stmt_t *stmt, uint8_t opcode)
{
    if (stmt == NULL || stmt->conn == NULL || stmt->socket_fd < 0 || stmt->stmt_id < 0)
        return;

    uint16_t sid = (uint16_t)stmt->stmt_id;
    uint8_t msg[8] = {
        0, SQLI_SQ_ID,
        (uint8_t)(sid >> 8), (uint8_t)sid,
        0, opcode,
        0, SQLI_SQ_EOT
    };
    (void)sqli_tcp_send(stmt->socket_fd, msg, sizeof(msg));
}

static int sqli_sql_is_read_only(const char *sql)
{
    if (sql == NULL)
        return 0;

    const unsigned char *p = (const unsigned char *)sql;
    while (*p != '\0' && isspace(*p))
        p++;

    char tok[16];
    size_t n = 0;
    while (*p != '\0' && isalpha(*p) && n < sizeof(tok) - 1) {
        tok[n++] = (char)toupper(*p);
        p++;
    }
    tok[n] = '\0';
    if (n == 0)
        return 0;

    return strcmp(tok, "SELECT") == 0 ||
           strcmp(tok, "WITH") == 0 ||
           strcmp(tok, "SHOW") == 0 ||
           strcmp(tok, "DESCRIBE") == 0 ||
           strcmp(tok, "EXPLAIN") == 0 ||
           strcmp(tok, "VALUES") == 0;
}

/* ----------------------------------------------------------------
 * sqli_prepare — send SQ_PREPARE and get statement ID
 *
 * Message:
 *   SQ_EOT | SQ_PREPARE(2) | num_placeholders(2) | sql(len+pad) |
 *   SQ_NDESCRIBE(22) | SQ_WANTDONE(49)
 *
 * Response: dispatch loop (SQ_DESCRIBE → SQ_DONE → SQ_EOT).
 * The DESCRIBE frame carries stmt_id at bytes [2..3].
 * ---------------------------------------------------------------- */

sqli_status sqli_prepare(sqli_conn_t *conn, const char *sql,
                         int *param_count, sqli_stmt_t **stmt)
{
    if (conn == NULL || sql == NULL || stmt == NULL)
        return SQLI_INVALID_STATE;

    if (conn->state != SQLI_CONN_READY) {
        set_error_context(conn, "prepare/precheck", SQLI_SQ_PREPARE);
        set_error(conn, "connection not ready");
        return SQLI_INVALID_STATE;
    }

    /* Count ? placeholders */
    int nph = 0;
    for (const char *cp = sql; *cp; cp++)
        if (*cp == '?') nph++;
    if (sqli_send_prepare(conn, sql) != SQLI_OK) {
        set_error_context(conn, "prepare/send", SQLI_SQ_PREPARE);
        set_error(conn, "failed to send PREPARE");
        return SQLI_IO_ERROR;
    }

    /* Receive and parse response via dispatch loop */
    sqli_result_t prep_result;
    memset(&prep_result, 0, sizeof(prep_result));

    sqli_status rc = sqli_receive_dispatch(conn->socket_fd, &prep_result, conn);
    if (rc != SQLI_OK) {
        sqli_result_cleanup(&prep_result);
        set_error_context(conn, "prepare/recv", SQLI_SQ_PREPARE);
        if (!conn->error_info.has_error)
            set_error(conn, "PREPARE: error receiving response");
        return rc;
    }

    /* stmt_id is in the DESCRIBE frame */
    int32_t stmt_id = prep_result.stmt_id;
    int pcount = (int)prep_result.column_count; /* informational only */
    uint8_t *server_param_types = NULL;
    int server_param_type_count = 0;
    if (prep_result.column_count > 0 && prep_result.columns != NULL) {
        server_param_type_count = prep_result.column_count;
        server_param_types = calloc((size_t)server_param_type_count, sizeof(uint8_t));
        if (server_param_types == NULL) {
            sqli_result_cleanup(&prep_result);
            return SQLI_ALLOC_FAIL;
        }
        for (int i = 0; i < server_param_type_count; i++)
            server_param_types[i] = (uint8_t)prep_result.columns[(size_t)i].type;
    }

    if (param_count != NULL)
        *param_count = nph;

    sqli_result_cleanup(&prep_result);

    sqli_log(SQLI_LOG_DEBUG, "prepared statement %d with %d params (server=%d)",
             stmt_id, nph, pcount);

    /* Create statement object */
    sqli_stmt_t *s = calloc(1, sizeof(*s));
    if (s == NULL)
        return SQLI_ALLOC_FAIL;

    s->socket_fd   = conn->socket_fd;
    s->conn        = conn;
    s->stmt_id     = (int)stmt_id;
    s->param_count = nph;   /* use counted placeholders as authoritative */
    s->read_only   = sqli_sql_is_read_only(sql);
    s->param_server_types = server_param_types;
    s->param_server_type_count = server_param_type_count;
    s->executed    = false;
    s->result_valid = false;
    memset(&s->result, 0, sizeof(s->result));

    if (nph > 0) {
        s->param_cap = nph;
        s->params = calloc((size_t)nph, sizeof(sqli_bound_param));
        if (s->params == NULL) {
            free(s->param_server_types);
            free(s);
            return SQLI_ALLOC_FAIL;
        }
    }

    *stmt = s;
    return SQLI_OK;
}

sqli_status sqli_prepare_with_retry(sqli_conn_t *conn, const char *sql,
                                    uint32_t max_retries, int *param_count,
                                    sqli_stmt_t **stmt)
{
    if (conn == NULL || sql == NULL || stmt == NULL)
        return SQLI_INVALID_STATE;

    *stmt = NULL;
    sqli_status rc = SQLI_ERR;
    for (uint32_t attempt = 0;; attempt++) {
        rc = sqli_prepare(conn, sql, param_count, stmt);
        if (rc == SQLI_OK)
            return SQLI_OK;
        if (attempt >= max_retries)
            return rc;

        bool should_retry = false;
        uint32_t delay_ms = 0;
        if (sqli_retry_recommend(conn, attempt, &should_retry, &delay_ms) != SQLI_OK ||
            !should_retry) {
            return rc;
        }

        sqli_log(SQLI_LOG_WARN,
                 "prepare retry attempt=%u delay_ms=%u rc=%d msg=%s",
                 attempt + 1u, delay_ms, (int)rc,
                 sqli_error(conn) ? sqli_error(conn) : "-");
        sqli_retry_sleep_ms(delay_ms);
    }
}

/* ----------------------------------------------------------------
 * sqli_bind_* — store parameter values for binding
 * ---------------------------------------------------------------- */

static sqli_status validate_param_index(sqli_stmt_t *stmt, int param_index)
{
    if (stmt == NULL || stmt->params == NULL)
        return SQLI_INVALID_STATE;
    if (param_index < 1 || param_index > stmt->param_count) {
        return SQLI_INVALID_STATE;
    }
    return SQLI_OK;
}

static bool sqli_stmt_param_needs_lob_streaming(const sqli_stmt_t *stmt, int param_index)
{
    if (stmt == NULL || param_index < 1 || param_index > stmt->param_count)
        return false;
    if (stmt->param_server_types == NULL || stmt->param_server_type_count < param_index)
        return false;

    const sqli_bound_param *par = &stmt->params[(size_t)(param_index - 1)];
    if (par->is_null)
        return false;

    uint8_t col_type = stmt->param_server_types[(size_t)(param_index - 1)];
    bool is_lob_col = (col_type == SQLI_TYPE_BYTE || col_type == SQLI_TYPE_TEXT ||
                       col_type == SQLI_TYPE_BLOB || col_type == SQLI_TYPE_CLOB);
    if (!is_lob_col)
        return false;

    return par->type == SQLI_BIND_BYTES || par->type == SQLI_BIND_STRING;
}

static sqli_status set_param_string(sqli_stmt_t *stmt, int param_index,
                                    sqli_bind_type type, const char *value)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;
    if (value == NULL) return SQLI_INVALID_STATE;

    sqli_bound_param *p = &stmt->params[(size_t)(param_index - 1)];
    size_t n = strlen(value);
    char *dup = malloc(n + 1);
    if (dup == NULL)
        return SQLI_ALLOC_FAIL;
    memcpy(dup, value, n + 1);

    free(p->sval);
    p->sval = dup;
    free(p->bval);
    p->bval = NULL;
    p->blen = 0;
    p->type = type;
    p->is_null = false;
    return SQLI_OK;
}

static sqli_status set_param_bytes(sqli_stmt_t *stmt, int param_index,
                                   const uint8_t *value, size_t len)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;
    if (value == NULL || len == 0 || len > 0xFFFFu)
        return SQLI_INVALID_STATE;

    sqli_bound_param *p = &stmt->params[(size_t)(param_index - 1)];
    uint8_t *dup = malloc(len);
    if (dup == NULL)
        return SQLI_ALLOC_FAIL;
    memcpy(dup, value, len);

    free(p->bval);
    p->bval = dup;
    p->blen = len;
    free(p->sval);
    p->sval = NULL;
    p->type = SQLI_BIND_BYTES;
    p->is_null = false;
    return SQLI_OK;
}

sqli_status sqli_bind_int(sqli_stmt_t *stmt, int param_index, int32_t value)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[(size_t)(param_index - 1)];
    p->type = SQLI_BIND_INT;
    p->value.ival = value;
    p->is_null = false;
    free(p->sval);
    p->sval = NULL;
    free(p->bval);
    p->bval = NULL;
    p->blen = 0;
    return SQLI_OK;
}

sqli_status sqli_bind_int64(sqli_stmt_t *stmt, int param_index, int64_t value)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[(size_t)(param_index - 1)];
    p->type = SQLI_BIND_BIGINT;
    p->value.ival64 = value;
    p->is_null = false;
    free(p->sval);
    p->sval = NULL;
    free(p->bval);
    p->bval = NULL;
    p->blen = 0;
    return SQLI_OK;
}

sqli_status sqli_bind_double(sqli_stmt_t *stmt, int param_index, double value)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[(size_t)(param_index - 1)];
    p->type = SQLI_BIND_FLOAT;
    p->value.dval = value;
    p->is_null = false;
    free(p->sval);
    p->sval = NULL;
    free(p->bval);
    p->bval = NULL;
    p->blen = 0;
    return SQLI_OK;
}

sqli_status sqli_bind_string(sqli_stmt_t *stmt, int param_index, const char *value)
{
    return set_param_string(stmt, param_index, SQLI_BIND_STRING, value);
}

sqli_status sqli_bind_decimal(sqli_stmt_t *stmt, int param_index, const char *value)
{
    /* Client can transmit DECIMAL text and rely on server-side cast for target column type. */
    return set_param_string(stmt, param_index, SQLI_BIND_STRING, value);
}

sqli_status sqli_bind_date(sqli_stmt_t *stmt, int param_index, const char *value)
{
    return set_param_string(stmt, param_index, SQLI_BIND_DATE, value);
}

sqli_status sqli_bind_datetime(sqli_stmt_t *stmt, int param_index, const char *value)
{
    return set_param_string(stmt, param_index, SQLI_BIND_STRING, value);
}

sqli_status sqli_bind_timestamp(sqli_stmt_t *stmt, int param_index, const sqli_timestamp_t *value)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    if (value == NULL || value->is_null) {
        return sqli_bind_null(stmt, param_index);
    }

    int y = (value->year >= 1 && value->year <= 9999) ? value->year : 1970;
    int m = (value->month >= 1 && value->month <= 12) ? value->month : 1;
    int d = (value->day >= 1 && value->day <= 31) ? value->day : 1;
    int h = (value->hour >= 0 && value->hour <= 23) ? value->hour : 0;
    int mi = (value->minute >= 0 && value->minute <= 59) ? value->minute : 0;
    int s = (value->second >= 0 && value->second <= 59) ? value->second : 0;
    int us = (value->microsecond >= 0 && value->microsecond <= 999999) ? value->microsecond : 0;

    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06d", y, m, d, h, mi, s, us);

    return set_param_string(stmt, param_index, SQLI_BIND_STRING, buf);
}

sqli_status sqli_bind_epoch_sec(sqli_stmt_t *stmt, int param_index, int64_t sec)
{
    sqli_timestamp_t ts;
    sqli_timestamp_from_epoch_sec(&ts, sec);
    return sqli_bind_timestamp(stmt, param_index, &ts);
}

sqli_status sqli_bind_epoch_ms(sqli_stmt_t *stmt, int param_index, int64_t ms)
{
    sqli_timestamp_t ts;
    sqli_timestamp_from_epoch_ms(&ts, ms);
    return sqli_bind_timestamp(stmt, param_index, &ts);
}

sqli_status sqli_bind_epoch_days(sqli_stmt_t *stmt, int param_index, int32_t days)
{
    sqli_timestamp_t ts;
    sqli_timestamp_from_epoch_days(&ts, days);
    return sqli_bind_timestamp(stmt, param_index, &ts);
}

sqli_status sqli_bind_interval(sqli_stmt_t *stmt, int param_index, const char *value)
{
    return set_param_string(stmt, param_index, SQLI_BIND_STRING, value);
}

sqli_status sqli_bind_bool(sqli_stmt_t *stmt, int param_index, bool value)
{
    return set_param_string(stmt, param_index, SQLI_BIND_STRING, value ? "t" : "f");
}

sqli_status sqli_bind_bytes(sqli_stmt_t *stmt, int param_index,
                            const uint8_t *value, size_t len)
{
    return set_param_bytes(stmt, param_index, value, len);
}

sqli_status sqli_bind_null(sqli_stmt_t *stmt, int param_index)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[(size_t)(param_index - 1)];
    p->type = SQLI_BIND_STRING;
    p->is_null = true;
    free(p->sval);
    p->sval = NULL;
    free(p->bval);
    p->bval = NULL;
    p->blen = 0;
    return SQLI_OK;
}

sqli_status sqli_bind_null_int(sqli_stmt_t *stmt, int param_index)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[(size_t)(param_index - 1)];
    p->type = SQLI_BIND_INT;
    p->is_null = true;
    free(p->sval);
    p->sval = NULL;
    free(p->bval);
    p->bval = NULL;
    p->blen = 0;
    return SQLI_OK;
}

sqli_status sqli_bind_null_int64(sqli_stmt_t *stmt, int param_index)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[(size_t)(param_index - 1)];
    p->type = SQLI_BIND_BIGINT;
    p->is_null = true;
    free(p->sval);
    p->sval = NULL;
    free(p->bval);
    p->bval = NULL;
    p->blen = 0;
    return SQLI_OK;
}

sqli_status sqli_bind_null_double(sqli_stmt_t *stmt, int param_index)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[(size_t)(param_index - 1)];
    p->type = SQLI_BIND_FLOAT;
    p->is_null = true;
    free(p->sval);
    p->sval = NULL;
    free(p->bval);
    p->bval = NULL;
    p->blen = 0;
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * Build SQ_BIND message
 *
 * Layout:
 *   SQ_EOT(2) |
 *   SQ_ID(2) | stmtID(4-BE) |
 *   SQ_BIND(2) | stmtID_pad(2) | paramCount(2) |
 *   [per param: type(2) null_indicator(2) encoded_length(2) data] x N
 *
 * Per-param value encodings:
 *   INT:    data = 4-byte BE int32
 *   BIGINT: data = 8-byte BE int64
 *   FLOAT:  data = 8-byte BE double (htobe64 on bit pattern)
 *   NULL:   no data bytes (null_indicator = -1)
 * ---------------------------------------------------------------- */

static size_t estimate_bind_msg_size(const sqli_stmt_t *stmt)
{
    size_t n = 8; /* ID(4)+BIND(2)+paramCount(2) */
    for (int i = 0; i < stmt->param_count; i++) {
        const sqli_bound_param *par = &stmt->params[(size_t)i];
        n += 6; /* type + null + encoded_length */
        if (par->is_null)
            continue;
        switch (par->type) {
        case SQLI_BIND_INT: n += 4; break;
        case SQLI_BIND_BIGINT: n += 8; break;
        case SQLI_BIND_FLOAT: n += 8; break;
        case SQLI_BIND_STRING: {
            size_t slen = (par->sval ? strlen(par->sval) : 0);
            size_t dlen = 2 + (slen * 8u) + 32u; /* worst-case locale conversion growth */
            n += dlen + (dlen & 1u);
            break;
        }
        case SQLI_BIND_DECIMAL: {
            size_t slen = (par->sval ? strlen(par->sval) : 0);
            n += (slen + 8u);
            n += ((slen + 8u) & 1u);
            break;
        }
        case SQLI_BIND_DATE: {
            n += 4;
            break;
        }
        case SQLI_BIND_BYTES: {
            size_t blen = par->blen;
            n += blen + (blen & 1u);
            break;
        }
        default: break;
        }
    }
    return n;
}

static int parse_iso_date_ymd(const char *s, int *year, int *month, int *day)
{
    if (s == NULL || year == NULL || month == NULL || day == NULL)
        return 0;
    if (!(isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) &&
          isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]) &&
          s[4] == '-' &&
          isdigit((unsigned char)s[5]) && isdigit((unsigned char)s[6]) &&
          s[7] == '-' &&
          isdigit((unsigned char)s[8]) && isdigit((unsigned char)s[9]) &&
          s[10] == '\0'))
        return 0;
    *year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    *month = (s[5] - '0') * 10 + (s[6] - '0');
    *day = (s[8] - '0') * 10 + (s[9] - '0');
    if (*month < 1 || *month > 12 || *day < 1 || *day > 31)
        return 0;
    return 1;
}

static int is_gregorian_leap(int year)
{
    return ((year & 3) == 0) && ((year % 400 == 0) || (year % 100 != 0));
}

/* Mirror Java IfxToJavaType.convertDateToDays(Date). */
static int32_t sqli_convert_ymd_to_ifx_days(int year, int month, int day)
{
    static const int mdays_norm[13] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    int mdays[13];
    memcpy(mdays, mdays_norm, sizeof(mdays));
    if (is_gregorian_leap(year))
        mdays[2] = 29;

    int lyear = year - 1;
    /* Informix wire DATE day 0 is 1899-12-31.
     * Keep day-1 here to match server/Client conversion semantics. */
    int jdate = (lyear / 100) * 146097 / 4 + (lyear % 100) * 1461 / 4 + (day - 1) - 693594;
    for (int i = 1; i < month; i++)
        jdate += mdays[i];
    return (int32_t)jdate;
}

static int parse_decimal_ascii(const char *s, int *negative,
                               uint8_t *digits, size_t digits_cap,
                               uint8_t *precision, uint8_t *scale)
{
    if (s == NULL || negative == NULL || digits == NULL || precision == NULL || scale == NULL)
        return 0;

    const char *p = s;
    while (*p != '\0' && isspace((unsigned char)*p))
        p++;

    *negative = 0;
    if (*p == '+' || *p == '-') {
        *negative = (*p == '-') ? 1 : 0;
        p++;
    }

    size_t nd = 0;
    size_t frac = 0;
    int seen_dot = 0;
    int seen_digit = 0;
    while (*p != '\0') {
        if (*p >= '0' && *p <= '9') {
            if (nd >= digits_cap)
                return 0;
            digits[nd++] = (uint8_t)(*p - '0');
            if (seen_dot)
                frac++;
            seen_digit = 1;
            p++;
            continue;
        }
        if (*p == '.' && !seen_dot) {
            seen_dot = 1;
            p++;
            continue;
        }
        break;
    }
    while (*p != '\0' && isspace((unsigned char)*p))
        p++;
    if (*p != '\0' || !seen_digit)
        return 0;
    if (nd == 0 || nd > 31 || frac > nd)
        return 0;

    *precision = (uint8_t)nd;
    *scale = (uint8_t)frac;
    return 1;
}

static size_t build_bind_msg(sqli_stmt_t *stmt, uint8_t *buf)
{
    size_t p = 0;

    /* SQ_ID (opcode 4) + 2-byte stmtID */
    uint16_t sid = (uint16_t)stmt->stmt_id;
    buf[p++] = 0; buf[p++] = SQLI_SQ_ID;
    buf[p++] = (uint8_t)(sid >> 8);
    buf[p++] = (uint8_t)sid;

    /* SQ_BIND (5) */
    buf[p++] = 0; buf[p++] = SQLI_SQ_BIND;

    /* paramCount */
    uint16_t pc = (uint16_t)stmt->param_count;
    buf[p++] = (uint8_t)(pc >> 8); buf[p++] = (uint8_t)pc;

    /* Per-parameter: type(2) + null_indicator(2) + encoded_length(2) + data */
    for (int i = 0; i < stmt->param_count; i++) {
        sqli_bound_param *par = &stmt->params[(size_t)i];

        /* Type code */
        buf[p++] = 0; buf[p++] = (uint8_t)par->type;

        if (par->is_null) {
            /* null_indicator = -1 (0xFFFF), encoded_length = 0, no data */
            buf[p++] = 0xFF; buf[p++] = 0xFF;  /* null_indicator = -1 */
            buf[p++] = 0x00; buf[p++] = 0x00;  /* encoded_length = 0 */
        } else {
            /* null_indicator = 0 */
            buf[p++] = 0x00; buf[p++] = 0x00;

            switch (par->type) {
            case SQLI_BIND_INT: {
                buf[p++] = 0; buf[p++] = 0;    /* encoded_length */
                uint32_t v = (uint32_t)par->value.ival;
                buf[p++] = (uint8_t)(v >> 24);
                buf[p++] = (uint8_t)(v >> 16);
                buf[p++] = (uint8_t)(v >> 8);
                buf[p++] = (uint8_t)v;
                break;
            }
            case SQLI_BIND_BIGINT: {
                buf[p++] = 0; buf[p++] = 0;    /* encoded_length */
                uint64_t v = (uint64_t)par->value.ival64;
                buf[p++] = (uint8_t)(v >> 56);
                buf[p++] = (uint8_t)(v >> 48);
                buf[p++] = (uint8_t)(v >> 40);
                buf[p++] = (uint8_t)(v >> 32);
                buf[p++] = (uint8_t)(v >> 24);
                buf[p++] = (uint8_t)(v >> 16);
                buf[p++] = (uint8_t)(v >> 8);
                buf[p++] = (uint8_t)v;
                break;
            }
            case SQLI_BIND_FLOAT: {
                buf[p++] = 0; buf[p++] = 0;    /* encoded_length */
                uint64_t bits;
                memcpy(&bits, &par->value.dval, 8);
                bits = htobe64(bits);
                memcpy(buf + p, &bits, 8);
                p += 8;
                break;
            }
            case SQLI_BIND_STRING: {
                uint8_t *enc = NULL;
                size_t slen = 0;
                if (par->sval != NULL) {
                    sqli_status rc = sqli_conn_encode_client_to_db(stmt->conn, par->sval, &enc, &slen);
                    if (rc != SQLI_OK || enc == NULL) {
                        free(enc);
                        enc = NULL;
                        slen = par->sval ? strlen(par->sval) : 0;
                    }
                }
                if (slen > 0xFFFFu) slen = 0xFFFFu;
                /* JavaToIfxType.JavaToIfxChar: 2-byte string length + bytes */
                buf[p++] = (uint8_t)((slen >> 8) & 0xFF);
                buf[p++] = (uint8_t)(slen & 0xFF);
                buf[p++] = (uint8_t)((slen >> 8) & 0xFF);
                buf[p++] = (uint8_t)(slen & 0xFF);
                if (slen > 0) {
                    if (enc != NULL)
                        memcpy(buf + p, enc, slen);
                    else
                        memcpy(buf + p, par->sval, slen);
                    p += slen;
                }
                free(enc);
                if ((2u + slen) & 1u)
                    buf[p++] = 0;
                break;
            }
            case SQLI_BIND_DECIMAL: {
                uint8_t digits[40];
                uint8_t precision = 0, scale = 0;
                int neg = 0;
                if (par->sval == NULL ||
                    !parse_decimal_ascii(par->sval, &neg, digits, sizeof(digits), &precision, &scale)) {
                    buf[p++] = 0; buf[p++] = 0;
                    break;
                }
                uint8_t packed[32];
                size_t n = sqli_encode_decimal(packed, sizeof(packed), precision, scale, neg, digits);
                if (n == 0) {
                    buf[p++] = 0; buf[p++] = 0;
                    break;
                }
                buf[p++] = 0;
                buf[p++] = scale; /* Client writes IfxDecimal.getScale() */
                memcpy(buf + p, packed, n);
                p += n;
                if (n & 1u)
                    buf[p++] = 0;
                break;
            }
            case SQLI_BIND_DATE: {
                int year = 0, month = 0, day = 0;
                if (par->sval == NULL || !parse_iso_date_ymd(par->sval, &year, &month, &day)) {
                    buf[p++] = 0; buf[p++] = 0;
                    break;
                }
                int32_t d = sqli_convert_ymd_to_ifx_days(year, month, day);
                buf[p++] = 0; buf[p++] = 0; /* encoded_length */
                buf[p++] = (uint8_t)((d >> 24) & 0xFF);
                buf[p++] = (uint8_t)((d >> 16) & 0xFF);
                buf[p++] = (uint8_t)((d >> 8) & 0xFF);
                buf[p++] = (uint8_t)(d & 0xFF);
                break;
            }
            case SQLI_BIND_BYTES: {
                size_t blen = par->blen;
                if (blen > 0xFFFFu) blen = 0xFFFFu;
                buf[p++] = (uint8_t)((blen >> 8) & 0xFF);
                buf[p++] = (uint8_t)(blen & 0xFF);
                if (blen > 0 && par->bval != NULL) {
                    memcpy(buf + p, par->bval, blen);
                    p += blen;
                }
                if (blen & 1u)
                    buf[p++] = 0;
                break;
            }
            default:
                buf[p++] = 0; buf[p++] = 0;    /* encoded_length = 0 */
                break;
            }
        }
    }

    return p;
}

/* ----------------------------------------------------------------
 * sqli_execute — send SQ_ID + SQ_BIND + SQ_EXECUTE and receive result
 * ---------------------------------------------------------------- */

sqli_status sqli_execute(sqli_stmt_t *stmt)
{
    if (stmt == NULL)
        return SQLI_INVALID_STATE;

    int fd = stmt->socket_fd;
    uint16_t sid = (uint16_t)stmt->stmt_id;

    for (int i = 1; i <= stmt->param_count; i++) {
        if (!sqli_stmt_param_needs_lob_streaming(stmt, i))
            continue;
        set_error_context(stmt->conn, "execute/precheck", SQLI_SQ_EXECUTE);
        set_error(stmt->conn,
                  "prepared BYTE/TEXT/BLOB/CLOB parameter streaming is not implemented");
        return SQLI_PROTO_ERROR;
    }

    if (stmt->param_count > 0) {
        size_t bind_size = estimate_bind_msg_size(stmt);
        uint8_t *bind_buf = malloc(bind_size);
        if (bind_buf == NULL)
            return SQLI_ALLOC_FAIL;
        bind_size = build_bind_msg(stmt, bind_buf);

        ssize_t sent = sqli_tcp_send(fd, bind_buf, bind_size);
        free(bind_buf);
        if (sent < 0 || (size_t)sent != bind_size) {
            set_error_context(stmt->conn, "execute/bind_send", SQLI_SQ_BIND);
            set_error(stmt->conn, "failed to send BIND");
            return SQLI_IO_ERROR;
        }

    } else {
        /* No parameters: still need SQ_ID before SQ_EXECUTE. */
        uint8_t id_msg[4];
        id_msg[0] = 0; id_msg[1] = SQLI_SQ_ID;
        id_msg[2] = (uint8_t)(sid >> 8);
        id_msg[3] = (uint8_t)sid;
        if (sqli_tcp_send(fd, id_msg, 4) != 4) {
            set_error_context(stmt->conn, "execute/id_send", SQLI_SQ_ID);
            set_error(stmt->conn, "failed to send SQ_ID");
            return SQLI_IO_ERROR;
        }
    }

    /* SQ_EXECUTE (7) + SQ_EOT (12) */
    uint8_t exec_msg[4] = {0, SQLI_SQ_EXECUTE, 0, SQLI_SQ_EOT};
    ssize_t sent = sqli_tcp_send(fd, exec_msg, 4);
    if (sent != 4) {
        set_error_context(stmt->conn, "execute/send", SQLI_SQ_EXECUTE);
        set_error(stmt->conn, "failed to send EXECUTE");
        return SQLI_IO_ERROR;
    }

    /* Clear and receive result */
    sqli_result_cleanup(&stmt->result);
    memset(&stmt->result, 0, sizeof(stmt->result));
    stmt->result.owner_conn = stmt->conn;
    stmt->result_valid = false;
    stmt->executed = true;

    set_error_context(stmt->conn, "execute/recv", SQLI_SQ_EXECUTE);
    sqli_status rc = SQLI_OK;
    int guard = 0;
    for (;;) {
        stmt->result.eof = 0;
        stmt->result.saw_done = false;
        stmt->result.saw_error = false;

        rc = sqli_receive_dispatch(fd, &stmt->result, stmt->conn);
        if (rc != SQLI_OK)
            break;

        if (stmt->result.saw_done || stmt->result.saw_error)
            break;

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int prc = poll(&pfd, 1, 20);
        if (prc <= 0 || !(pfd.revents & POLLIN))
            break;

        guard++;
        if (guard > 64) {
            set_error_context(stmt->conn, "execute/recv", SQLI_SQ_EXECUTE);
            set_error(stmt->conn, "execute response did not converge to DONE");
            rc = SQLI_PROTO_ERROR;
            break;
        }
    }

    if (rc == SQLI_OK) {
        stmt->result_valid = true;
    } else if (!stmt->conn->error_info.has_error) {
        set_error(stmt->conn, "error receiving execute response");
    }

    return rc;
}

sqli_status sqli_execute_with_retry(sqli_stmt_t *stmt, uint32_t max_retries)
{
    if (stmt == NULL || stmt->conn == NULL)
        return SQLI_INVALID_STATE;

    sqli_status rc = SQLI_ERR;
    for (uint32_t attempt = 0;; attempt++) {
        rc = sqli_execute(stmt);
        if (rc == SQLI_OK)
            return SQLI_OK;
        if (attempt >= max_retries)
            return rc;
        if (!stmt->read_only) {
            sqli_log(SQLI_LOG_WARN,
                     "stmt retry skipped for non-read-only statement");
            return rc;
        }

        bool should_retry = false;
        uint32_t delay_ms = 0;
        if (sqli_retry_recommend(stmt->conn, attempt, &should_retry, &delay_ms) != SQLI_OK ||
            !should_retry) {
            return rc;
        }

        sqli_log(SQLI_LOG_WARN,
                 "stmt retry attempt=%u delay_ms=%u rc=%d msg=%s",
                 attempt + 1u, delay_ms, (int)rc,
                 sqli_error(stmt->conn) ? sqli_error(stmt->conn) : "-");
        sqli_retry_sleep_ms(delay_ms);
    }
}

/* ----------------------------------------------------------------
 * sqli_stmt_next
 * ---------------------------------------------------------------- */

bool sqli_stmt_next(sqli_stmt_t *stmt)
{
    if (stmt == NULL || !stmt->result_valid)
        return false;
    if (stmt->result.current_row >= 0 && stmt->result.tuple_len > 0)
        return true;
    return false;
}

/* ----------------------------------------------------------------
 * sqli_stmt_result
 * ---------------------------------------------------------------- */

sqli_result_t *sqli_stmt_result(sqli_stmt_t *stmt)
{
    if (stmt == NULL || !stmt->result_valid)
        return NULL;
    return &stmt->result;
}

/* ----------------------------------------------------------------
 * sqli_stmt_close
 * ---------------------------------------------------------------- */

void sqli_stmt_close(sqli_stmt_t *stmt)
{
    if (stmt == NULL)
        return;

    /* Close and release statement on server; keep stream aligned. */
    sqli_stmt_best_effort_control(stmt, 10);              /* SQ_CLOSE */
    sqli_stmt_best_effort_control(stmt, SQLI_SQ_RELEASE); /* SQ_RELEASE */

    if (stmt->params != NULL) {
        for (int i = 0; i < stmt->param_count; i++) {
            free(stmt->params[i].sval);
            free(stmt->params[i].bval);
        }
        free(stmt->params);
        stmt->params = NULL;
    }
    free(stmt->param_server_types);
    stmt->param_server_types = NULL;
    stmt->param_server_type_count = 0;

    sqli_result_cleanup(&stmt->result);

    stmt->stmt_id = -1;
    stmt->executed = false;
    stmt->result_valid = false;
}

/* ----------------------------------------------------------------
 * sqli_stmt_destroy
 * ---------------------------------------------------------------- */

void sqli_stmt_destroy(sqli_stmt_t *stmt)
{
    if (stmt == NULL)
        return;
    sqli_stmt_close(stmt);
    free(stmt);
}

/* ----------------------------------------------------------------
 * Column accessors
 * ---------------------------------------------------------------- */

const char *sqli_result_column_name(sqli_result_t *result, int col_index)
{
    if (result == NULL || col_index < 0 || col_index >= result->column_count)
        return NULL;
    return result->columns[(size_t)col_index].name;
}

int sqli_result_column_type(sqli_result_t *result, int col_index)
{
    if (result == NULL || col_index < 0 || col_index >= result->column_count)
        return -1;
    return (int)result->columns[(size_t)col_index].type;
}

/* ----------------------------------------------------------------
 * Callable statements
 * ---------------------------------------------------------------- */

struct sqli_call {
    sqli_stmt_t *stmt;
    int param_count;
    sqli_call_param_mode *modes; /* 1-indexed via [param_index - 1] */
    bool out_row_ready;
};

static sqli_status sqli_call_validate_index(sqli_call_t *call, int param_index)
{
    if (call == NULL || call->modes == NULL)
        return SQLI_INVALID_STATE;
    if (param_index < 1 || param_index > call->param_count)
        return SQLI_INVALID_STATE;
    return SQLI_OK;
}

static sqli_status sqli_call_param_to_out_col(sqli_call_t *call, int param_index,
                                              int *out_col_index)
{
    sqli_status rc = sqli_call_validate_index(call, param_index);
    if (rc != SQLI_OK || out_col_index == NULL)
        return SQLI_INVALID_STATE;

    int out_col = 0;
    for (int i = 1; i <= call->param_count; i++) {
        sqli_call_param_mode m = call->modes[(size_t)(i - 1)];
        if (m == SQLI_CALL_PARAM_OUT || m == SQLI_CALL_PARAM_INOUT) {
            if (i == param_index) {
                *out_col_index = out_col;
                return SQLI_OK;
            }
            out_col++;
        }
    }
    return SQLI_INVALID_STATE;
}

sqli_status sqli_call_prepare(sqli_conn_t *conn, const char *sql,
                              int *param_count, sqli_call_t **call)
{
    if (conn == NULL || sql == NULL || call == NULL)
        return SQLI_INVALID_STATE;
    *call = NULL;

    sqli_stmt_t *stmt = NULL;
    int nparams = 0;
    sqli_status rc = sqli_prepare(conn, sql, &nparams, &stmt);
    if (rc != SQLI_OK)
        return rc;

    sqli_call_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        sqli_stmt_destroy(stmt);
        return SQLI_ALLOC_FAIL;
    }
    c->stmt = stmt;
    c->param_count = nparams;
    c->out_row_ready = false;

    if (nparams > 0) {
        c->modes = calloc((size_t)nparams, sizeof(*c->modes));
        if (c->modes == NULL) {
            sqli_stmt_destroy(stmt);
            free(c);
            return SQLI_ALLOC_FAIL;
        }
        for (int i = 0; i < nparams; i++)
            c->modes[i] = SQLI_CALL_PARAM_IN;
    }

    if (param_count != NULL)
        *param_count = nparams;
    *call = c;
    return SQLI_OK;
}

sqli_stmt_t *sqli_call_stmt(sqli_call_t *call)
{
    if (call == NULL)
        return NULL;
    return call->stmt;
}

sqli_status sqli_call_set_param_mode(sqli_call_t *call, int param_index,
                                     sqli_call_param_mode mode)
{
    if (mode != SQLI_CALL_PARAM_IN &&
        mode != SQLI_CALL_PARAM_OUT &&
        mode != SQLI_CALL_PARAM_INOUT)
        return SQLI_INVALID_STATE;
    sqli_status rc = sqli_call_validate_index(call, param_index);
    if (rc != SQLI_OK)
        return rc;
    call->modes[(size_t)(param_index - 1)] = mode;
    return SQLI_OK;
}

sqli_status sqli_call_execute(sqli_call_t *call)
{
    if (call == NULL || call->stmt == NULL)
        return SQLI_INVALID_STATE;

    call->out_row_ready = false;
    sqli_status rc = sqli_execute(call->stmt);
    if (rc != SQLI_OK)
        return rc;

    bool has_out = false;
    for (int i = 0; i < call->param_count; i++) {
        if (call->modes[i] == SQLI_CALL_PARAM_OUT ||
            call->modes[i] == SQLI_CALL_PARAM_INOUT) {
            has_out = true;
            break;
        }
    }
    if (!has_out)
        return SQLI_OK;

    sqli_result_t *res = sqli_stmt_result(call->stmt);
    if (res == NULL)
        return SQLI_OK;

    if (sqli_result_next(res))
        call->out_row_ready = true;
    return SQLI_OK;
}

sqli_status sqli_call_get_int64(sqli_call_t *call, int param_index,
                                int64_t *out, bool *is_null)
{
    if (call == NULL || out == NULL || is_null == NULL || !call->out_row_ready)
        return SQLI_INVALID_STATE;
    int col = -1;
    sqli_status rc = sqli_call_param_to_out_col(call, param_index, &col);
    if (rc != SQLI_OK)
        return rc;
    sqli_result_t *res = sqli_stmt_result(call->stmt);
    if (res == NULL)
        return SQLI_INVALID_STATE;
    *out = sqli_result_get_int64(res, col);
    *is_null = sqli_result_was_null(res);
    return SQLI_OK;
}

sqli_status sqli_call_get_double(sqli_call_t *call, int param_index,
                                 double *out, bool *is_null)
{
    if (call == NULL || out == NULL || is_null == NULL || !call->out_row_ready)
        return SQLI_INVALID_STATE;
    int col = -1;
    sqli_status rc = sqli_call_param_to_out_col(call, param_index, &col);
    if (rc != SQLI_OK)
        return rc;
    sqli_result_t *res = sqli_stmt_result(call->stmt);
    if (res == NULL)
        return SQLI_INVALID_STATE;
    *out = sqli_result_get_double(res, col);
    *is_null = sqli_result_was_null(res);
    return SQLI_OK;
}

sqli_status sqli_call_get_string(sqli_call_t *call, int param_index,
                                 const char **out, bool *is_null)
{
    if (call == NULL || out == NULL || is_null == NULL || !call->out_row_ready)
        return SQLI_INVALID_STATE;
    int col = -1;
    sqli_status rc = sqli_call_param_to_out_col(call, param_index, &col);
    if (rc != SQLI_OK)
        return rc;
    sqli_result_t *res = sqli_stmt_result(call->stmt);
    if (res == NULL)
        return SQLI_INVALID_STATE;
    *out = sqli_result_get_string(res, col);
    *is_null = sqli_result_was_null(res);
    return SQLI_OK;
}

void sqli_call_destroy(sqli_call_t *call)
{
    if (call == NULL)
        return;
    sqli_stmt_destroy(call->stmt);
    free(call->modes);
    free(call);
}
