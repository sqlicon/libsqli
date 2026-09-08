#define _GNU_SOURCE
#include "sqli_sblob_internal.h"
#include "libsqli/sqli_temporal.h"
#include "libsqli/sqli_decimal.h"
#include "libsqli/sqli_sblob.h"
#include "sqli_internal.h"
#include "sqli_protocol_internal.h"

#include "sqli_tcp.h"
#include "sqli_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include "sqli_endian.h"
#include <stdint.h>

static void sqli_prepare_drain_pending_tail(sqli_conn_t *conn)
{
    if (conn == NULL || conn->socket_fd < 0)
        return;

    for (int extra = 0; extra < 8; extra++) {
        if (!sqli_protocol_has_buffered_data(conn, conn->socket_fd)) {
            struct pollfd pfd;
            pfd.fd = conn->socket_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int prc = poll(&pfd, 1, 20);
            if (prc <= 0 || !(pfd.revents & POLLIN))
                break;
        }

        sqli_result_t tail;
        memset(&tail, 0, sizeof(tail));
        tail.owner_conn = conn;
        set_error_context(conn, "prepare/drain", 0);
        sqli_status rc = sqli_receive_dispatch(conn->socket_fd, &tail, conn);
        sqli_result_cleanup(&tail);
        if (rc != SQLI_OK)
            break;
    }
}

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

static void sqli_free_bound_param(sqli_bound_param *param)
{
    if (param == NULL)
        return;
    free(param->sval);
    param->sval = NULL;
    free(param->bval);
    param->bval = NULL;
    param->blen = 0;
    param->is_null = false;
}

static void sqli_free_bound_param_array(sqli_bound_param *params, int param_count)
{
    if (params == NULL)
        return;
    for (int i = 0; i < param_count; i++)
        sqli_free_bound_param(&params[(size_t)i]);
    free(params);
}

static sqli_status sqli_clone_bound_param(sqli_bound_param *dst, const sqli_bound_param *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->type = src->type;
    dst->value = src->value;
    dst->blen = src->blen;
    dst->is_null = src->is_null;
    dst->native_qualifier = src->native_qualifier;
    dst->native_length = src->native_length;
    memcpy(dst->native_bytes, src->native_bytes, sizeof(dst->native_bytes));

    if (src->sval != NULL) {
        size_t n = strlen(src->sval) + 1u;
        dst->sval = malloc(n);
        if (dst->sval == NULL)
            return SQLI_ALLOC_FAIL;
        memcpy(dst->sval, src->sval, n);
    }

    if (src->bval != NULL && src->blen > 0) {
        dst->bval = malloc(src->blen);
        if (dst->bval == NULL) {
            sqli_free_bound_param(dst);
            return SQLI_ALLOC_FAIL;
        }
        memcpy(dst->bval, src->bval, src->blen);
    }

    return SQLI_OK;
}

static sqli_status sqli_clone_bound_param_array(sqli_bound_param **out,
                                                const sqli_bound_param *src,
                                                int param_count)
{
    *out = NULL;
    if (param_count <= 0)
        return SQLI_OK;

    sqli_bound_param *copy = calloc((size_t)param_count, sizeof(*copy));
    if (copy == NULL)
        return SQLI_ALLOC_FAIL;

    for (int i = 0; i < param_count; i++) {
        sqli_status rc = sqli_clone_bound_param(&copy[(size_t)i], &src[(size_t)i]);
        if (rc != SQLI_OK) {
            sqli_free_bound_param_array(copy, param_count);
            return rc;
        }
    }

    *out = copy;
    return SQLI_OK;
}

static void sqli_stmt_batch_reset_rows(sqli_stmt_t *stmt)
{
    if (stmt == NULL)
        return;
    for (size_t i = 0; i < stmt->batch_count; i++) {
        sqli_free_bound_param_array(stmt->batch_rows[i].params, stmt->param_count);
        stmt->batch_rows[i].params = NULL;
    }
    free(stmt->batch_rows);
    stmt->batch_rows = NULL;
    stmt->batch_count = 0;
    stmt->batch_cap = 0;
}

static void sqli_stmt_batch_fill_error_item(sqli_conn_t *conn, sqli_batch_item_result *it,
                                            sqli_status rc)
{
    if (it == NULL)
        return;
    it->status = rc;
    it->rows_affected = 0;
    it->sqlcode = 0;
    it->isamcode = 0;
    it->opcode = 0;
    it->message[0] = '\0';

    if (conn == NULL)
        return;

    if (conn->error_info.has_error) {
        it->sqlcode = conn->error_info.sqlcode;
        it->isamcode = conn->error_info.isamcode;
        it->opcode = conn->error_info.opcode;
        if (conn->error_info.message[0] != '\0')
            snprintf(it->message, sizeof(it->message), "%s", conn->error_info.message);
    }
    if (it->message[0] == '\0' && sqli_error(conn) != NULL)
        snprintf(it->message, sizeof(it->message), "%s", sqli_error(conn));
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

static sqli_status sqli_stmt_close_cursor_if_open(sqli_stmt_t *stmt)
{
    if (stmt == NULL || !stmt->cursor_open)
        return SQLI_OK;
    if (stmt->conn != NULL &&
        (stmt->result.rollback_epoch != stmt->conn->rollback_epoch ||
         (stmt->result.holdability == SQLI_CURSOR_CLOSE_AT_COMMIT &&
          stmt->result.commit_epoch != stmt->conn->commit_epoch))) {
        stmt->cursor_open = false;
        return SQLI_OK;
    }

    sqli_status rc = SQLI_OK;
    if (stmt->conn != NULL && stmt->conn->socket_fd > 0 &&
        stmt->conn->state == SQLI_CONN_READY && stmt->stmt_id >= 0) {
        /* Preserve any active error info on connection across SQ_CLOSE */
        sqli_error_info saved_info;
        char saved_errmsg[sizeof(stmt->conn->errmsg)];
        char saved_ctx[sizeof(stmt->conn->error_context)];
        uint16_t saved_op = stmt->conn->error_opcode;
        bool has_err = stmt->conn->error_info.has_error;

        if (has_err) {
            saved_info = stmt->conn->error_info;
            memcpy(saved_errmsg, stmt->conn->errmsg, sizeof(saved_errmsg));
            memcpy(saved_ctx, stmt->conn->error_context, sizeof(saved_ctx));
        }

        rc = sqli_send_stmt_close_cursor(stmt->conn, stmt->stmt_id);

        if (has_err) {
            stmt->conn->error_info = saved_info;
            memcpy(stmt->conn->errmsg, saved_errmsg, sizeof(saved_errmsg));
            memcpy(stmt->conn->error_context, saved_ctx, sizeof(saved_ctx));
            stmt->conn->error_opcode = saved_op;
        }
    } else if (stmt->socket_fd > 0 && stmt->stmt_id >= 0) {
        sqli_stmt_best_effort_control(stmt, 10); /* SQ_CLOSE */
    }

    if (rc == SQLI_OK)
        stmt->cursor_open = false;
    return rc;
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

    clear_error(conn);

    if (conn->state != SQLI_CONN_READY) {
        set_error_context(conn, "prepare/precheck", SQLI_SQ_PREPARE);
        set_error(conn, "connection not ready");
        return SQLI_INVALID_STATE;
    }

    sqli_prepare_drain_pending_tail(conn);

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

    sqli_log(SQLI_LOG_DEBUG, "prepared statement %d with %d params (server=%d)",
             stmt_id, nph, pcount);

    /* Create statement object */
    sqli_stmt_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        free(server_param_types);
        sqli_result_cleanup(&prep_result);
        return SQLI_ALLOC_FAIL;
    }

    s->socket_fd   = conn->socket_fd;
    s->conn        = conn;
    s->stmt_id     = (int)stmt_id;
    s->param_count = nph;   /* use counted placeholders as authoritative */
    s->read_only   = sqli_sql_is_read_only(sql);
    s->param_server_types = server_param_types;
    s->param_server_type_count = server_param_type_count;
    s->executed    = false;
    s->result_valid = false;
    s->result = prep_result;
    s->result.owner_conn = conn;
    s->result.cursor_type = conn->cursor_type;
    s->result.holdability = conn->holdability;
    s->result.commit_epoch = conn ? conn->commit_epoch : 0;
    s->result.rollback_epoch = conn ? conn->rollback_epoch : 0;
    s->result.stmt_id = (int)stmt_id;
    s->result.cursor = -1;
    s->result.current_row = -1;
    s->result.absolute_row_num = 0;
    s->result.at_before_first = true;
    s->result.at_after_last = false;
    s->result.tuple_buffer = NULL;
    s->result.tuple_len = 0;
    s->result.cur_cache_row = -1;

    if (nph > 0) {
        s->param_cap = nph;
        s->params = calloc((size_t)nph, sizeof(sqli_bound_param));
        if (s->params == NULL) {
            sqli_result_cleanup(&s->result);
            free(s->param_server_types);
            free(s);
            return SQLI_ALLOC_FAIL;
        }
    }

    clear_error(conn);
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

static sqli_status validate_param_index(sqli_stmt_t *stmt, size_t param_index)
{
    if (stmt == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (stmt->param_count < 0 || param_index >= (size_t)stmt->param_count)
        return SQLI_OUT_OF_RANGE;
    if (stmt->params == NULL)
        return SQLI_INVALID_STATE;
    return SQLI_OK;
}

static bool sqli_stmt_param_needs_lob_streaming(const sqli_stmt_t *stmt,
                                                const sqli_bound_param *params,
                                                size_t param_index)
{
    if (stmt == NULL || param_index >= (size_t)stmt->param_count)
        return false;
    if (stmt->param_server_types == NULL || param_index >= (size_t)stmt->param_server_type_count)
        return false;

    const sqli_bound_param *par = &params[param_index];
    if (par->is_null)
        return false;

    uint8_t col_type = stmt->param_server_types[param_index];
    bool is_lob_col = (col_type == SQLI_TYPE_BYTE || col_type == SQLI_TYPE_TEXT);
    if (!is_lob_col)
        return false;

    return par->type == SQLI_BIND_BYTES || par->type == SQLI_BIND_STRING;
}

static sqli_status set_param_string(sqli_stmt_t *stmt, size_t param_index,
                                    sqli_bind_type type, const char *value)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;
    if (value == NULL) return SQLI_INVALID_STATE;

    sqli_bound_param *p = &stmt->params[param_index];
    size_t n = strlen(value);
    char *dup = malloc(n + 1);
    if (dup == NULL)
        return SQLI_ALLOC_FAIL;
    memcpy(dup, value, n + 1);

    sqli_free_bound_param(p);
    p->sval = dup;
    p->type = type;
    p->is_null = false;
    return SQLI_OK;
}

static sqli_status set_param_bytes(sqli_stmt_t *stmt, size_t param_index,
                                   const uint8_t *value, size_t len)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;
    if (value == NULL || len == 0 || len > 0xFFFFu)
        return SQLI_INVALID_STATE;

    sqli_bound_param *p = &stmt->params[param_index];
    uint8_t *dup = malloc(len);
    if (dup == NULL)
        return SQLI_ALLOC_FAIL;
    memcpy(dup, value, len);

    sqli_free_bound_param(p);
    p->bval = dup;
    p->blen = len;
    p->type = SQLI_BIND_BYTES;
    p->is_null = false;
    return SQLI_OK;
}

sqli_status sqli_bind_int(sqli_stmt_t *stmt, size_t param_index, int32_t value)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[param_index];
    sqli_free_bound_param(p);
    p->type = SQLI_BIND_INT;
    p->value.ival = value;
    p->is_null = false;
    return SQLI_OK;
}

sqli_status sqli_bind_int64(sqli_stmt_t *stmt, size_t param_index, int64_t value)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[param_index];
    sqli_free_bound_param(p);
    p->type = SQLI_BIND_BIGINT;
    p->value.ival64 = value;
    p->is_null = false;
    return SQLI_OK;
}

sqli_status sqli_bind_double(sqli_stmt_t *stmt, size_t param_index, double value)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[param_index];
    sqli_free_bound_param(p);
    p->type = SQLI_BIND_FLOAT;
    p->value.dval = value;
    p->is_null = false;
    return SQLI_OK;
}

sqli_status sqli_bind_string(sqli_stmt_t *stmt, size_t param_index, const char *value)
{
    return set_param_string(stmt, param_index, SQLI_BIND_STRING, value);
}

sqli_status sqli_bind_decimal_string(sqli_stmt_t *stmt, size_t param_index, const char *value)
{
    /* Client can transmit DECIMAL text and rely on server-side cast for target column type. */
    return set_param_string(stmt, param_index, SQLI_BIND_STRING, value);
}

sqli_status sqli_bind_date_string(sqli_stmt_t *stmt, size_t param_index, const char *value)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_date_t date;
    sqli_status status = sqli_date_parse(&date, value, strlen(value), false);
    return status == SQLI_OK ? sqli_bind_date(stmt, param_index, &date) : status;
}

sqli_status sqli_bind_datetime_string(sqli_stmt_t *stmt, size_t param_index, const char *value)
{
    return set_param_string(stmt, param_index, SQLI_BIND_STRING, value);
}

sqli_status sqli_bind_date(sqli_stmt_t *stmt, size_t param_index, const sqli_date_t *value)
{
    if (value == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_status status = validate_param_index(stmt, param_index);
    if (status != SQLI_OK)
        return status;
    sqli_bound_param candidate = {.type = SQLI_BIND_DATE, .is_null = value->is_null};
    status = sqli_date_encode_wire(value, candidate.native_bytes, sizeof(candidate.native_bytes),
                                   &candidate.native_length);
    if (status != SQLI_OK)
        return status;
    sqli_bound_param *parameter = &stmt->params[param_index];
    sqli_free_bound_param(parameter);
    *parameter = candidate;
    return SQLI_OK;
}

sqli_status sqli_bind_decimal(sqli_stmt_t *stmt, size_t param_index, const sqli_decimal_t *value,
                              const sqli_decimal_target_t *target)
{
    if (value == NULL || target == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (target->precision < 1 || target->precision > 32 ||
        (!target->floating_scale && target->scale > target->precision))
        return SQLI_OUT_OF_RANGE;
    sqli_status status = validate_param_index(stmt, param_index);
    if (status != SQLI_OK)
        return status;
    uint8_t scale = target->floating_scale ? UINT8_MAX : target->scale;
    sqli_bound_param candidate = {.type = SQLI_BIND_DECIMAL, .native_qualifier = scale};
    uint16_t descriptor = (uint16_t)(((unsigned)target->precision << 8) | scale);
    status = sqli_decimal_encode_wire(value, descriptor, candidate.native_bytes,
                                      sizeof(candidate.native_bytes), &candidate.native_length);
    if (status == SQLI_OK)
        status = sqli_decimal_is_null(value, &candidate.is_null);
    if (status != SQLI_OK)
        return status;
    sqli_bound_param *parameter = &stmt->params[param_index];
    sqli_free_bound_param(parameter);
    *parameter = candidate;
    return SQLI_OK;
}

static sqli_status temporal_qualifier(const sqli_temporal_range_t *target, bool interval,
                                       uint8_t leading_precision, uint16_t *out)
{
    enum { wire_second = 10, max_fraction_digits = 5, max_leading_digits = 9,
           calendar_year_digits = 4, ordinary_field_digits = 2 };
    if (target == NULL || target->first < SQLI_FIELD_YEAR || target->last > SQLI_FIELD_FRACTION ||
        target->last < target->first)
        return SQLI_INVALID_ARGUMENT;
    if (target->last == SQLI_FIELD_FRACTION) {
        if (target->fractional_digits < 1 || target->fractional_digits > max_fraction_digits)
            return SQLI_OUT_OF_RANGE;
    } else if (target->fractional_digits != 0) {
        return SQLI_INVALID_ARGUMENT;
    }
    if (interval && ((target->first == SQLI_FIELD_FRACTION && leading_precision != 0) ||
        (target->first != SQLI_FIELD_FRACTION && (leading_precision < 1 || leading_precision > max_leading_digits))))
        return SQLI_OUT_OF_RANGE;
    unsigned start = (unsigned)(target->first - SQLI_FIELD_YEAR) * 2;
    unsigned end = target->last == SQLI_FIELD_FRACTION ? (unsigned)wire_second + target->fractional_digits :
        (unsigned)(target->last - SQLI_FIELD_YEAR) * 2;
    unsigned leading = interval ? leading_precision : target->first == SQLI_FIELD_YEAR ? calendar_year_digits : ordinary_field_digits;
    unsigned digits = target->first == SQLI_FIELD_FRACTION ? target->fractional_digits : leading + end - start;
    uint16_t qualifier = (uint16_t)((digits << 8) | (start << 4) | end);
    size_t width;
    if (sqli_temporal_wire_size(qualifier, interval, &width) != SQLI_OK)
        return SQLI_INVALID_ARGUMENT;
    *out = qualifier;
    return SQLI_OK;
}

sqli_status sqli_bind_datetime(sqli_stmt_t *stmt, size_t param_index, const sqli_datetime_t *value,
                               const sqli_temporal_range_t *target)
{
    if (value == NULL || target == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_status status = validate_param_index(stmt, param_index);
    if (status != SQLI_OK)
        return status;
    sqli_bound_param candidate = {.type = SQLI_BIND_DATETIME};
    status = temporal_qualifier(target, false, 0, &candidate.native_qualifier);
    if (status == SQLI_OK)
        status = sqli_datetime_encode_wire(value, candidate.native_qualifier, candidate.native_bytes,
                                      sizeof(candidate.native_bytes), &candidate.native_length);
    if (status == SQLI_OK)
        status = sqli_datetime_is_null(value, &candidate.is_null);
    if (status != SQLI_OK)
        return status;
    sqli_bound_param *parameter = &stmt->params[param_index];
    sqli_free_bound_param(parameter);
    *parameter = candidate;
    return SQLI_OK;
}

sqli_status sqli_bind_interval(sqli_stmt_t *stmt, size_t param_index, const sqli_interval_t *value,
                               const sqli_temporal_range_t *target, uint8_t leading_precision)
{
    if (value == NULL || target == NULL)
        return SQLI_INVALID_ARGUMENT;
    sqli_status status = validate_param_index(stmt, param_index);
    if (status != SQLI_OK)
        return status;
    sqli_bound_param candidate = {.type = SQLI_BIND_INTERVAL};
    status = temporal_qualifier(target, true, leading_precision, &candidate.native_qualifier);
    if (status == SQLI_OK)
        status = sqli_interval_encode_wire(value, candidate.native_qualifier, candidate.native_bytes,
                                      sizeof(candidate.native_bytes), &candidate.native_length);
    if (status == SQLI_OK)
        status = sqli_interval_is_null(value, &candidate.is_null);
    if (status != SQLI_OK)
        return status;
    sqli_bound_param *parameter = &stmt->params[param_index];
    sqli_free_bound_param(parameter);
    *parameter = candidate;
    return SQLI_OK;
}

sqli_status sqli_bind_timestamp(sqli_stmt_t *stmt, size_t param_index, const sqli_timestamp_t *value)
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

sqli_status sqli_bind_epoch_sec(sqli_stmt_t *stmt, size_t param_index, int64_t sec)
{
    sqli_timestamp_t ts;
    sqli_timestamp_from_epoch_sec(&ts, sec);
    return sqli_bind_timestamp(stmt, param_index, &ts);
}

sqli_status sqli_bind_epoch_ms(sqli_stmt_t *stmt, size_t param_index, int64_t ms)
{
    sqli_timestamp_t ts;
    sqli_timestamp_from_epoch_ms(&ts, ms);
    return sqli_bind_timestamp(stmt, param_index, &ts);
}

sqli_status sqli_bind_epoch_days(sqli_stmt_t *stmt, size_t param_index, int32_t days)
{
    sqli_timestamp_t ts;
    sqli_timestamp_from_epoch_days(&ts, days);
    return sqli_bind_timestamp(stmt, param_index, &ts);
}

sqli_status sqli_bind_interval_string(sqli_stmt_t *stmt, size_t param_index, const char *value)
{
    return set_param_string(stmt, param_index, SQLI_BIND_STRING, value);
}

sqli_status sqli_bind_bool(sqli_stmt_t *stmt, size_t param_index, bool value)
{
    return set_param_string(stmt, param_index, SQLI_BIND_STRING, value ? "t" : "f");
}

sqli_status sqli_bind_bytes(sqli_stmt_t *stmt, size_t param_index,
                            const uint8_t *value, size_t len)
{
    return set_param_bytes(stmt, param_index, value, len);
}

sqli_status sqli_bind_null(sqli_stmt_t *stmt, size_t param_index)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[param_index];
    sqli_free_bound_param(p);
    p->type = SQLI_BIND_STRING;
    p->is_null = true;
    return SQLI_OK;
}

sqli_status sqli_bind_null_int(sqli_stmt_t *stmt, size_t param_index)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[param_index];
    sqli_free_bound_param(p);
    p->type = SQLI_BIND_INT;
    p->is_null = true;
    return SQLI_OK;
}

sqli_status sqli_bind_null_int64(sqli_stmt_t *stmt, size_t param_index)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[param_index];
    sqli_free_bound_param(p);
    p->type = SQLI_BIND_BIGINT;
    p->is_null = true;
    return SQLI_OK;
}

sqli_status sqli_bind_null_double(sqli_stmt_t *stmt, size_t param_index)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[param_index];
    sqli_free_bound_param(p);
    p->type = SQLI_BIND_FLOAT;
    p->is_null = true;
    return SQLI_OK;
}

sqli_status sqli_bind_sblob(sqli_stmt_t *stmt, size_t param_index, const sqli_sblob_t *lob)
{
    sqli_status rc = validate_param_index(stmt, param_index);
    if (rc != SQLI_OK) return rc;

    sqli_bound_param *p = &stmt->params[param_index];
    sqli_free_bound_param(p);

    if (lob == NULL) {
        sqli_sblob_type stype = SQLI_SBLOB_BLOB;
        if (stmt->param_server_types != NULL &&
            param_index < (size_t)stmt->param_server_type_count &&
            stmt->param_server_types[param_index] == SQLI_TYPE_CLOB) {
            stype = SQLI_SBLOB_CLOB;
        }
        p->type = SQLI_BIND_SBLOB;
        p->value.ival = (int32_t)stype;
        p->is_null = true;
        return SQLI_OK;
    }

    if (lob->locator_len == 0 || lob->locator_len > SQLI_SBLOB_LOCATOR_MAX) {
        if (stmt->conn)
            set_error(stmt->conn, "invalid smartblob locator length");
        return SQLI_INVALID_STATE;
    }

    uint8_t *dup = malloc(lob->locator_len);
    if (dup == NULL)
        return SQLI_ALLOC_FAIL;
    memcpy(dup, lob->locator, lob->locator_len);

    p->bval = dup;
    p->blen = lob->locator_len;
    p->value.ival = (int32_t)lob->type;
    p->type = SQLI_BIND_SBLOB;
    p->is_null = false;
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * Build SQ_BIND message
 *
 * Layout:
 *   SQ_ID(2) | stmtID(4-BE) |
 *   SQ_BIND(2) | paramCount(2) |
 *   [per param: type(2) null_indicator(2) encoded_length(2) data] x N
 *   [optional SQ_EOT(2)]
 *
 * Per-param value encodings:
 *   INT:    data = 4-byte BE int32
 *   BIGINT: data = 8-byte BE int64
 *   FLOAT:  data = 8-byte BE double (htobe64 on bit pattern)
 *   NULL:   no data bytes (null_indicator = -1)
 * ---------------------------------------------------------------- */

static size_t estimate_bind_msg_size(const sqli_stmt_t *stmt, const sqli_bound_param *params,
                                     bool include_eot)
{
    size_t n = 8; /* ID(4)+BIND(2)+paramCount(2) */
    for (int i = 0; i < stmt->param_count; i++) {
        const sqli_bound_param *par = &params[(size_t)i];
        n += 6; /* type + null + encoded_length */
        if (par->type == SQLI_BIND_SBLOB)
            n += 8; /* owner(2) + name(6) */
        if (par->is_null)
            continue;
        uint8_t stype = 0;
        if (stmt->param_server_types != NULL && i < stmt->param_server_type_count)
            stype = stmt->param_server_types[(size_t)i];
        if ((stype == SQLI_TYPE_BYTE || stype == SQLI_TYPE_TEXT) &&
            par->type != SQLI_BIND_DATETIME && par->type != SQLI_BIND_INTERVAL &&
            par->type != SQLI_BIND_DATE && par->type != SQLI_BIND_DECIMAL) {
            n += 56;
            continue;
        }
        switch (par->type) {
        case SQLI_BIND_DECIMAL:
        case SQLI_BIND_DATETIME:
        case SQLI_BIND_INTERVAL:
            n += 2 + par->native_length + (par->native_length & 1u);
            break;
        case SQLI_BIND_INT: n += 4; break;
        case SQLI_BIND_BIGINT:
        case SQLI_BIND_FLOAT:
            n += 8;
            break;
        case SQLI_BIND_STRING: {
            size_t slen = (par->sval ? strlen(par->sval) : 0);
            size_t dlen = 2 + (slen * 8u) + 32u; /* worst-case locale conversion growth */
            n += dlen + (dlen & 1u);
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
        case SQLI_BIND_SBLOB: {
            size_t blen = par->blen;
            n += 4 + blen + ((4 + blen) & 1u);
            break;
        }
        default: break;
        }
    }
    if (include_eot)
        n += 2;
    return n;
}

static size_t build_bind_msg(sqli_stmt_t *stmt, const sqli_bound_param *params,
                             uint8_t *buf, bool include_eot)
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
        const sqli_bound_param *par = &params[(size_t)i];
        uint8_t server_type = 0;
        if (stmt->param_server_types != NULL && i < stmt->param_server_type_count)
            server_type = stmt->param_server_types[(size_t)i];

        bool is_legacy_lob = (server_type == SQLI_TYPE_BYTE || server_type == SQLI_TYPE_TEXT) &&
            par->type != SQLI_BIND_DATETIME && par->type != SQLI_BIND_INTERVAL &&
            par->type != SQLI_BIND_DATE && par->type != SQLI_BIND_DECIMAL;
        uint8_t wire_type = is_legacy_lob ? server_type : (uint8_t)par->type;

        /* Type code */
        buf[p++] = 0; buf[p++] = wire_type;

        if (par->type == SQLI_BIND_SBLOB) {
            /* Extended type owner: empty string (length 0) */
            buf[p++] = 0; buf[p++] = 0;
            /* Extended type name: "blob" or "clob" */
            const char *tname = (par->value.ival == SQLI_SBLOB_CLOB) ? "clob" : "blob";
            uint16_t tnlen = (uint16_t)strlen(tname);
            buf[p++] = (uint8_t)(tnlen >> 8);
            buf[p++] = (uint8_t)(tnlen & 0xFF);
            memcpy(buf + p, tname, tnlen);
            p += tnlen;
            if ((2 + tnlen) & 1)
                buf[p++] = 0;
        }

        if (par->is_null) {
            /* null_indicator = -1 (0xFFFF), encoded_length = 0, no data */
            buf[p++] = 0xFF; buf[p++] = 0xFF;  /* null_indicator = -1 */
            buf[p++] = 0x00; buf[p++] = 0x00;  /* encoded_length = 0 */
        } else if (is_legacy_lob) {
            buf[p++] = 0x00; buf[p++] = 0x00;  /* null_indicator = 0 */
            buf[p++] = 0x00; buf[p++] = 56;    /* encoded_length = 56 */
            uint32_t total_len = 0;
            if (par->type == SQLI_BIND_BYTES) {
                total_len = (uint32_t)par->blen;
            } else if (par->type == SQLI_BIND_STRING) {
                if (server_type == SQLI_TYPE_TEXT && par->sval != NULL) {
                    uint8_t *enc = NULL;
                    size_t slen = 0;
                    if (sqli_conn_encode_client_to_db(stmt->conn, par->sval, &enc, &slen) == SQLI_OK && enc != NULL) {
                        total_len = (uint32_t)slen;
                        free(enc);
                    } else {
                        total_len = (uint32_t)strlen(par->sval);
                    }
                } else if (par->sval != NULL) {
                    total_len = (uint32_t)strlen(par->sval);
                }
            }
            uint8_t desc[56];
            memset(desc, 0, sizeof(desc));
            desc[16] = (uint8_t)((total_len >> 24) & 0xFF);
            desc[17] = (uint8_t)((total_len >> 16) & 0xFF);
            desc[18] = (uint8_t)((total_len >> 8) & 0xFF);
            desc[19] = (uint8_t)(total_len & 0xFF);
            memcpy(buf + p, desc, 56);
            p += 56;
        } else {
            /* null_indicator = 0 */
            buf[p++] = 0x00; buf[p++] = 0x00;

            switch (par->type) {
            case SQLI_BIND_DECIMAL:
            case SQLI_BIND_DATETIME:
            case SQLI_BIND_INTERVAL: {
                buf[p++] = (uint8_t)(par->native_qualifier >> 8);
                buf[p++] = (uint8_t)par->native_qualifier;
                size_t length = par->native_length;
                while (length > 1 && par->native_bytes[length - 1] == 0)
                    length--;
                buf[p++] = 0;
                buf[p++] = (uint8_t)length;
                memcpy(buf + p, par->native_bytes, length);
                p += length;
                if (length & 1u)
                    buf[p++] = 0;
                break;
            }
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
            case SQLI_BIND_DATE: {
                buf[p++] = 0; buf[p++] = 0;
                memcpy(buf + p, par->native_bytes, SQLI_DATE_WIRE_SIZE);
                p += SQLI_DATE_WIRE_SIZE;
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
            case SQLI_BIND_SBLOB: {
                buf[p++] = 0; buf[p++] = 0;    /* encoded_length = 0 */
                uint32_t blen = (uint32_t)par->blen;
                buf[p++] = (uint8_t)((blen >> 24) & 0xFF);
                buf[p++] = (uint8_t)((blen >> 16) & 0xFF);
                buf[p++] = (uint8_t)((blen >> 8) & 0xFF);
                buf[p++] = (uint8_t)(blen & 0xFF);
                if (blen > 0 && par->bval != NULL) {
                    memcpy(buf + p, par->bval, blen);
                    p += blen;
                }
                if ((4 + blen) & 1u)
                    buf[p++] = 0;
                break;
            }
            default:
                buf[p++] = 0; buf[p++] = 0;    /* encoded_length = 0 */
                break;
            }
        }
    }

    if (include_eot) {
        buf[p++] = 0;
        buf[p++] = SQLI_SQ_EOT;
    }

    return p;
}

static sqli_status sqli_stmt_send_bind_if_needed(sqli_stmt_t *stmt,
                                                 const sqli_bound_param *params,
                                                 bool terminate_group)
{
    if (stmt->param_count <= 0)
        return SQLI_OK;

    size_t bind_size = estimate_bind_msg_size(stmt, params, terminate_group);
    uint8_t *bind_buf = malloc(bind_size);
    if (bind_buf == NULL)
        return SQLI_ALLOC_FAIL;
    bind_size = build_bind_msg(stmt, params, bind_buf, terminate_group);

    ssize_t sent = sqli_tcp_send(stmt->socket_fd, bind_buf, bind_size);
    free(bind_buf);
    if (sent < 0 || (size_t)sent != bind_size) {
        set_error_context(stmt->conn, "execute/bind_send", SQLI_SQ_BIND);
        set_error(stmt->conn, "failed to send BIND");
        return SQLI_IO_ERROR;
    }

    return SQLI_OK;
}

static void sqli_stmt_prepare_result_for_execute(sqli_stmt_t *stmt)
{
    sqli_result_clear_rows(&stmt->result);
    stmt->result.owner_conn = stmt->conn;
    stmt->result.cursor_type = stmt->conn->cursor_type;
    stmt->result.holdability = stmt->conn->holdability;
    stmt->result.commit_epoch = stmt->conn ? stmt->conn->commit_epoch : 0;
    stmt->result.rollback_epoch = stmt->conn ? stmt->conn->rollback_epoch : 0;
    stmt->result.stmt_id = stmt->stmt_id;
    stmt->result.eof = 0;
    stmt->result.saw_done = false;
    stmt->result.saw_error = false;
    stmt->result.error_code = 0;
    stmt->result.ret_type_sent = false;
    stmt->result.last_was_null = false;
    stmt->result.absolute_row_num = 0;
    stmt->result.at_before_first = true;
    stmt->result.at_after_last = false;
    stmt->result_valid = false;
    stmt->executed = true;
}

static sqli_status sqli_stmt_receive_execute_result(sqli_stmt_t *stmt)
{
    int fd = stmt->socket_fd;

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

        if (stmt->result.saw_done || stmt->result.saw_error || stmt->result.eof)
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
        if (stmt->conn != NULL && fd >= 0) {
            for (int extra = 0; extra < 8; extra++) {
                if (!sqli_protocol_has_buffered_data(stmt->conn, fd)) {
                    struct pollfd pfd;
                    pfd.fd = fd;
                    pfd.events = POLLIN;
                    pfd.revents = 0;
                    int prc = poll(&pfd, 1, 20);
                    if (prc <= 0 || !(pfd.revents & POLLIN))
                        break;
                }

                sqli_result_t tail;
                memset(&tail, 0, sizeof(tail));
                tail.owner_conn = stmt->conn;
                tail.eof = 0;
                tail.saw_done = false;
                tail.saw_error = false;
                sqli_status drc = sqli_receive_dispatch(fd, &tail, stmt->conn);
                sqli_result_cleanup(&tail);
                if (drc != SQLI_OK)
                    break;
            }
        }
        stmt->result_valid = true;
        clear_error(stmt->conn);
    } else if (!stmt->conn->error_info.has_error) {
        set_error(stmt->conn, "error receiving execute response");
    }

    return rc;
}

static sqli_status sqli_stmt_execute_bound_params(sqli_stmt_t *stmt,
                                                  const sqli_bound_param *params)
{
    sqli_status txn_rc = sqli_autobegin(stmt->conn, stmt->result.statement_type);
    if (txn_rc != SQLI_OK)
        return txn_rc;
    int fd = stmt->socket_fd;
    uint16_t sid = (uint16_t)stmt->stmt_id;

    if (stmt->param_count > 0) {
        sqli_status bind_rc = sqli_stmt_send_bind_if_needed(stmt, params, false);
        if (bind_rc != SQLI_OK)
            return bind_rc;

        int lob_count = 0;
        for (size_t i = 0; i < (size_t)stmt->param_count; i++) {
            if (sqli_stmt_param_needs_lob_streaming(stmt, params, i))
                lob_count++;
        }

        if (lob_count > 0) {
            uint8_t bbind_hdr[4] = {0, 41, (uint8_t)((lob_count >> 8) & 0xFF), (uint8_t)(lob_count & 0xFF)};
            if (sqli_tcp_send(fd, bbind_hdr, 4) != 4) {
                set_error_context(stmt->conn, "execute/bbind_send", 41);
                set_error(stmt->conn, "failed to send SQ_BBIND");
                return SQLI_IO_ERROR;
            }

            for (size_t i = 0; i < (size_t)stmt->param_count; i++) {
                if (!sqli_stmt_param_needs_lob_streaming(stmt, params, i))
                    continue;

                const sqli_bound_param *par = &params[i];
                const uint8_t *data = NULL;
                uint8_t *enc_buf = NULL;
                size_t total_len = 0;

                if (par->type == SQLI_BIND_BYTES) {
                    data = par->bval;
                    total_len = par->blen;
                } else if (par->type == SQLI_BIND_STRING) {
                    uint8_t col_type = stmt->param_server_types[i];
                    if (col_type == SQLI_TYPE_TEXT && par->sval != NULL) {
                        size_t slen = 0;
                        if (sqli_conn_encode_client_to_db(stmt->conn, par->sval, &enc_buf, &slen) == SQLI_OK && enc_buf != NULL) {
                            data = enc_buf;
                            total_len = slen;
                        } else {
                            data = (const uint8_t *)par->sval;
                            total_len = strlen(par->sval);
                        }
                    } else if (par->sval != NULL) {
                        data = (const uint8_t *)par->sval;
                        total_len = strlen(par->sval);
                    }
                }

                size_t rem = total_len;
                size_t cur = 0;
                while (rem > 0) {
                    uint16_t chlen = rem > 1024 ? 1024 : (uint16_t)rem;
                    uint8_t ch_hdr[4] = {0, 39, (uint8_t)(chlen >> 8), (uint8_t)(chlen & 0xFF)};
                    if (sqli_tcp_send(fd, ch_hdr, 4) != 4 ||
                        sqli_tcp_send(fd, data + cur, chlen) != (ssize_t)chlen) {
                        free(enc_buf);
                        set_error_context(stmt->conn, "execute/blob_chunk_send", 39);
                        set_error(stmt->conn, "failed to send BLOB chunk");
                        return SQLI_IO_ERROR;
                    }
                    if (chlen & 1) {
                        uint8_t pad = 0;
                        if (sqli_tcp_send(fd, &pad, 1) != 1) {
                            free(enc_buf);
                            return SQLI_IO_ERROR;
                        }
                    }
                    cur += chlen;
                    rem -= chlen;
                }
                free(enc_buf);

                uint8_t term[4] = {0, 39, 0, 0};
                if (sqli_tcp_send(fd, term, 4) != 4) {
                    set_error_context(stmt->conn, "execute/blob_term_send", 39);
                    set_error(stmt->conn, "failed to send BLOB terminator");
                    return SQLI_IO_ERROR;
                }
            }
        }
    } else {
        uint8_t id_msg[4];
        id_msg[0] = 0;
        id_msg[1] = SQLI_SQ_ID;
        id_msg[2] = (uint8_t)(sid >> 8);
        id_msg[3] = (uint8_t)sid;
        if (sqli_tcp_send(fd, id_msg, 4) != 4) {
            set_error_context(stmt->conn, "execute/id_send", SQLI_SQ_ID);
            set_error(stmt->conn, "failed to send SQ_ID");
            return SQLI_IO_ERROR;
        }
    }

    {
        uint8_t exec_msg[4] = {0, SQLI_SQ_EXECUTE, 0, SQLI_SQ_EOT};
        ssize_t sent = sqli_tcp_send(fd, exec_msg, 4);
        if (sent != 4) {
            set_error_context(stmt->conn, "execute/send", SQLI_SQ_EXECUTE);
            set_error(stmt->conn, "failed to send EXECUTE");
            return SQLI_IO_ERROR;
        }
    }

    sqli_stmt_prepare_result_for_execute(stmt);
    sqli_status rc = sqli_stmt_receive_execute_result(stmt);
    if (rc == SQLI_OK)
        sqli_track_transaction_statement(stmt->conn, stmt->result.statement_type);
    return rc;
}

static sqli_status sqli_stmt_execute_select(sqli_stmt_t *stmt)
{
    sqli_status rc = sqli_autobegin(stmt->conn, stmt->result.statement_type);
    if (rc != SQLI_OK)
        return rc;
    rc = sqli_stmt_send_bind_if_needed(stmt, stmt->params, true);
    if (rc != SQLI_OK)
        return rc;

    if (stmt->param_count > 0) {
        sqli_result_t bind_result;
        memset(&bind_result, 0, sizeof(bind_result));
        bind_result.owner_conn = stmt->conn;

        set_error_context(stmt->conn, "execute/bind_recv", SQLI_SQ_BIND);
        rc = sqli_receive_dispatch(stmt->socket_fd, &bind_result, stmt->conn);
        sqli_result_cleanup(&bind_result);
        if (rc != SQLI_OK) {
            if (!stmt->conn->error_info.has_error)
                set_error(stmt->conn, "error receiving bind response");
            return rc;
        }
    }

    sqli_stmt_prepare_result_for_execute(stmt);

    rc = sqli_send_open(stmt->socket_fd, stmt->stmt_id,
                        stmt->conn->cursor_type,
                        stmt->conn->holdability);
    if (rc != SQLI_OK) {
        set_error_context(stmt->conn, "execute/open_send", SQLI_SQ_PREPARE);
        set_error(stmt->conn, "failed to send open");
        return rc;
    }

    stmt->cursor_open = true;
    set_error_context(stmt->conn, "execute/open_recv", SQLI_SQ_PREPARE);
    rc = sqli_receive_dispatch(stmt->socket_fd, &stmt->result, stmt->conn);
    if (rc != SQLI_OK) {
        if (!stmt->conn->error_info.has_error)
            set_error(stmt->conn, "error receiving open response");
        return rc;
    }

    stmt->result.eof = 0;
    if (stmt->conn->cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
        rc = sqli_send_scroll_fetch(stmt->socket_fd, stmt->stmt_id, &stmt->result, 1, 1);
    } else {
        rc = sqli_send_fetch(stmt->socket_fd, stmt->stmt_id, &stmt->result);
    }
    if (rc != SQLI_OK) {
        set_error_context(stmt->conn, "execute/fetch_send", SQLI_SQ_NFETCH);
        set_error(stmt->conn, "failed to send fetch");
        return rc;
    }

    for (;;) {
        int prev_rows = stmt->result.row_count;
        set_error_context(stmt->conn, "execute/fetch_recv", SQLI_SQ_NFETCH);
        stmt->result.saw_done = false;
        stmt->result.saw_error = false;
        rc = sqli_receive_dispatch(stmt->socket_fd, &stmt->result, stmt->conn);
        if (rc != SQLI_OK) {
            if (!stmt->conn->error_info.has_error)
                set_error(stmt->conn, "error receiving fetch response");
            return rc;
        }

        if (stmt->result.row_count <= prev_rows)
            break;

        stmt->result.eof = 0;
        if (stmt->conn->cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
            rc = sqli_send_scroll_fetch(stmt->socket_fd, stmt->stmt_id, &stmt->result, 1,
                                        (int32_t)(prev_rows + 1));
        } else {
            rc = sqli_send_fetch(stmt->socket_fd, stmt->stmt_id, &stmt->result);
        }
        if (rc != SQLI_OK) {
            set_error_context(stmt->conn, "execute/fetch_send", SQLI_SQ_NFETCH);
            set_error(stmt->conn, "failed to send fetch");
            return rc;
        }
    }

    clear_error(stmt->conn);
    stmt->result_valid = true;
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * sqli_execute — send SQ_ID + SQ_BIND + SQ_EXECUTE and receive result
 * ---------------------------------------------------------------- */

sqli_status sqli_execute(sqli_stmt_t *stmt)
{
    if (stmt == NULL || stmt->conn == NULL)
        return SQLI_INVALID_STATE;

    clear_error(stmt->conn);

    stmt->result_valid = false;
    sqli_status close_rc = sqli_stmt_close_cursor_if_open(stmt);
    if (close_rc != SQLI_OK)
        return close_rc;
    clear_error(stmt->conn);

    /* DESCRIBE for DML can describe input parameters. Only SELECT and
     * returning EXECUTE PROCEDURE/FUNCTION (statement type 56) open cursors. */
    if (stmt->result.statement_type == 2 ||
        (stmt->result.statement_type == 56 && stmt->result.column_count > 0)) {
        sqli_status rc = sqli_stmt_execute_select(stmt);
        if (rc != SQLI_OK)
            (void)sqli_stmt_close_cursor_if_open(stmt);
        return rc;
    }

    return sqli_stmt_execute_bound_params(stmt, stmt->params);
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

sqli_status sqli_stmt_batch_add(sqli_stmt_t *stmt)
{
    if (stmt == NULL)
        return SQLI_INVALID_STATE;

    if (stmt->param_count > 0 && stmt->params == NULL)
        return SQLI_INVALID_STATE;

    if (stmt->batch_count == stmt->batch_cap) {
        size_t new_cap = stmt->batch_cap == 0 ? 8u : stmt->batch_cap * 2u;
        sqli_stmt_batch_row *rows = realloc(stmt->batch_rows, new_cap * sizeof(*rows));
        if (rows == NULL)
            return SQLI_ALLOC_FAIL;
        memset(rows + stmt->batch_cap, 0, (new_cap - stmt->batch_cap) * sizeof(*rows));
        stmt->batch_rows = rows;
        stmt->batch_cap = new_cap;
    }

    sqli_bound_param *copy = NULL;
    sqli_status rc = sqli_clone_bound_param_array(&copy, stmt->params, stmt->param_count);
    if (rc != SQLI_OK)
        return rc;

    stmt->batch_rows[stmt->batch_count].params = copy;
    stmt->batch_count++;
    return SQLI_OK;
}

void sqli_stmt_batch_clear(sqli_stmt_t *stmt)
{
    sqli_stmt_batch_reset_rows(stmt);
}

size_t sqli_stmt_batch_size(const sqli_stmt_t *stmt)
{
    return stmt ? stmt->batch_count : 0u;
}

sqli_status sqli_stmt_batch_execute(sqli_stmt_t *stmt, sqli_batch_result_t **out_batch)
{
    if (stmt == NULL || out_batch == NULL)
        return SQLI_INVALID_STATE;
    *out_batch = NULL;

    if (stmt->conn != NULL)
        clear_error(stmt->conn);

    if (stmt->read_only || stmt->result.statement_type == 2 ||
        (stmt->result.statement_type == 56 && stmt->result.column_count > 0)) {
        set_error_context(stmt->conn, "stmt_batch_execute/precheck", SQLI_SQ_EXECUTE);
        set_error(stmt->conn, "prepared batch execute is only supported for DML statements");
        return SQLI_INVALID_STATE;
    }
    if (stmt->conn == NULL || stmt->conn->state != SQLI_CONN_READY) {
        set_error_context(stmt->conn, "stmt_batch_execute/precheck", SQLI_SQ_EXECUTE);
        set_error(stmt->conn, "connection not ready");
        return SQLI_INVALID_STATE;
    }

    sqli_batch_result_t *batch = calloc(1, sizeof(*batch));
    if (batch == NULL)
        return SQLI_ALLOC_FAIL;
    batch->count = stmt->batch_count;
    if (batch->count > 0) {
        batch->items = calloc(batch->count, sizeof(*batch->items));
        if (batch->items == NULL) {
            free(batch);
            return SQLI_ALLOC_FAIL;
        }
    }

    if (stmt->batch_count == 0) {
        *out_batch = batch;
        return SQLI_OK;
    }

    sqli_status rc = SQLI_OK;

    for (size_t i = 0; i < stmt->batch_count; i++) {
        rc = sqli_stmt_execute_bound_params(stmt, stmt->batch_rows[i].params);
        if (rc != SQLI_OK) {
            if (stmt->conn->error_info.has_error && stmt->result.saw_error) {
                sqli_stmt_batch_fill_error_item(stmt->conn, &batch->items[i], rc);
                batch->error_count++;
                continue;
            }
            sqli_batch_result_destroy(batch);
            return rc;
        }
        batch->items[i].status = SQLI_OK;
        batch->items[i].rows_affected = stmt->result.rows_affected;
        batch->success_count++;
    }

    *out_batch = batch;
    sqli_stmt_batch_reset_rows(stmt);
    if (batch->error_count == 0 && stmt->conn != NULL)
        clear_error(stmt->conn);
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * sqli_stmt_next
 * ---------------------------------------------------------------- */

sqli_status sqli_stmt_fetch(sqli_stmt_t *stmt)
{
    if (stmt == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (!stmt->result_valid)
        return SQLI_INVALID_STATE;
    return sqli_result_fetch(&stmt->result);
}

bool sqli_stmt_next(sqli_stmt_t *stmt)
{
    return sqli_stmt_fetch(stmt) == SQLI_OK;
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
    if (stmt->conn != NULL && stmt->conn->socket_fd > 0 &&
        stmt->conn->state == SQLI_CONN_READY && stmt->stmt_id >= 0) {
        sqli_stmt_close_release(stmt->conn, stmt->stmt_id);
    } else if (stmt->socket_fd > 0 && stmt->stmt_id >= 0) {
        sqli_stmt_best_effort_control(stmt, 10);              /* SQ_CLOSE */
        sqli_stmt_best_effort_control(stmt, SQLI_SQ_RELEASE); /* SQ_RELEASE */
    }

    if (stmt->params != NULL) {
        sqli_free_bound_param_array(stmt->params, stmt->param_count);
        stmt->params = NULL;
    }
    sqli_stmt_batch_reset_rows(stmt);
    free(stmt->param_server_types);
    stmt->param_server_types = NULL;
    stmt->param_server_type_count = 0;

    sqli_result_cleanup(&stmt->result);

    stmt->stmt_id = -1;
    stmt->cursor_open = false;
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

const char *sqli_result_column_name(sqli_result_t *result, size_t col_index)
{
    if (result == NULL || col_index >= (size_t)result->column_count)
        return NULL;
    return result->columns[(size_t)col_index].name;
}

int sqli_result_column_type(sqli_result_t *result, size_t col_index)
{
    if (result == NULL || col_index >= (size_t)result->column_count)
        return -1;
    return (int)result->columns[(size_t)col_index].type;
}

/* ----------------------------------------------------------------
 * Callable statements
 * ---------------------------------------------------------------- */

struct sqli_call {
    sqli_stmt_t *stmt;
    int param_count;
    sqli_call_param_mode *modes; /* zero-based parameter indices */
    bool out_row_ready;
};

static sqli_status sqli_call_validate_index(sqli_call_t *call, size_t param_index)
{
    if (call == NULL)
        return SQLI_INVALID_ARGUMENT;
    if (call->param_count < 0 || param_index >= (size_t)call->param_count)
        return SQLI_OUT_OF_RANGE;
    if (call->modes == NULL)
        return SQLI_INVALID_STATE;
    return SQLI_OK;
}

static sqli_status sqli_call_param_to_out_col(sqli_call_t *call, size_t param_index,
                                              int *out_col_index)
{
    sqli_status rc = sqli_call_validate_index(call, param_index);
    if (rc != SQLI_OK)
        return rc;
    if (out_col_index == NULL)
        return SQLI_INVALID_ARGUMENT;

    int out_col = 0;
    for (size_t i = 0; i < (size_t)call->param_count; i++) {
        sqli_call_param_mode m = call->modes[i];
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

sqli_status sqli_call_set_param_mode(sqli_call_t *call, size_t param_index,
                                     sqli_call_param_mode mode)
{
    if (mode != SQLI_CALL_PARAM_IN &&
        mode != SQLI_CALL_PARAM_OUT &&
        mode != SQLI_CALL_PARAM_INOUT)
        return SQLI_INVALID_STATE;
    sqli_status rc = sqli_call_validate_index(call, param_index);
    if (rc != SQLI_OK)
        return rc;
    call->modes[param_index] = mode;
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

    rc = sqli_result_fetch(res);
    if (rc == SQLI_OK)
        call->out_row_ready = true;
    return rc == SQLI_EOF ? SQLI_OK : rc;
}

sqli_status sqli_call_get_int64(sqli_call_t *call, size_t param_index,
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
    return sqli_result_get_int64(res, (size_t)col, out, is_null);
}

sqli_status sqli_call_get_double(sqli_call_t *call, size_t param_index,
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
    return sqli_result_get_double(res, (size_t)col, out, is_null);
}

sqli_status sqli_call_get_string(sqli_call_t *call, size_t param_index,
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
