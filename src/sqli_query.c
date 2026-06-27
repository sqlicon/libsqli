#define _POSIX_C_SOURCE 200809L
#include "sqli_internal.h"
#include "sqli_protocol_internal.h"
#include "sqli_result_internal.h"

#include "sqli_log.h"
#include "sqli_tcp.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>

static sqli_status sqli_validate_query_options(sqli_conn_t *conn,
                                               const sqli_query_options *options,
                                               sqli_query_options *resolved)
{
    if (resolved == NULL || conn == NULL)
        return SQLI_INVALID_STATE;

    resolved->cursor_type = conn->cursor_type;
    resolved->holdability = conn->holdability;
    if (resolved->cursor_type != SQLI_CURSOR_FORWARD_ONLY &&
        resolved->cursor_type != SQLI_CURSOR_SCROLL_INSENSITIVE)
        resolved->cursor_type = SQLI_CURSOR_FORWARD_ONLY;
    if (resolved->holdability != SQLI_CURSOR_HOLD_OVER_COMMIT &&
        resolved->holdability != SQLI_CURSOR_CLOSE_AT_COMMIT)
        resolved->holdability = SQLI_CURSOR_CLOSE_AT_COMMIT;
    if (options != NULL)
        *resolved = *options;

    if (resolved->cursor_type != SQLI_CURSOR_FORWARD_ONLY &&
        resolved->cursor_type != SQLI_CURSOR_SCROLL_INSENSITIVE) {
        set_error_context(conn, "query/options", 0);
        set_error(conn, "invalid cursor type");
        return SQLI_INVALID_STATE;
    }
    if (resolved->holdability != SQLI_CURSOR_HOLD_OVER_COMMIT &&
        resolved->holdability != SQLI_CURSOR_CLOSE_AT_COMMIT) {
        set_error_context(conn, "query/options", 0);
        set_error(conn, "invalid cursor holdability");
        return SQLI_INVALID_STATE;
    }
    return SQLI_OK;
}

sqli_status sqli_query_ex(sqli_conn_t *conn, const char *sql,
                          const sqli_query_options *options,
                          sqli_result_t **result)
{
    if (conn == NULL || sql == NULL || result == NULL)
        return SQLI_INVALID_STATE;

    if (conn->state != SQLI_CONN_READY) {
        set_error_context(conn, "query/precheck", 0);
        set_error(conn, "connection not ready");
        return SQLI_INVALID_STATE;
    }

    sqli_query_options resolved_options;
    sqli_status rc = sqli_validate_query_options(conn, options, &resolved_options);
    if (rc != SQLI_OK)
        return rc;

    sqli_result_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        set_error_context(conn, "query/alloc", 0);
        set_error(conn, "memory allocation failed");
        return SQLI_ALLOC_FAIL;
    }
    r->owner_conn = conn;
    r->cursor_type = conn->cursor_type;
    r->holdability = conn->holdability;
    r->cursor_type = resolved_options.cursor_type;
    r->holdability = resolved_options.holdability;

    /* Phase 1: PREPARE — server returns DESCRIBE + DONE */
    rc = sqli_send_prepare(conn, sql);
    if (rc != SQLI_OK) {
        set_error_context(conn, "query/prepare_send", SQLI_SQ_PREPARE);
        set_error(conn, "failed to send prepare");
        free(r);
        return rc;
    }

    set_error_context(conn, "query/prepare_recv", SQLI_SQ_PREPARE);
    r->saw_done = false;
    r->saw_error = false;
    rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
    if (rc != SQLI_OK) {
        if (!conn->error_info.has_error)
            set_error(conn, "error receiving prepare response");
        sqli_result_destroy(r);
        return rc;
    }

    int stmt_id = r->stmt_id;
    /* stmt_id == 0 is valid; only -1 (or negative) means "no statement" */
    if (stmt_id < 0) {
        set_error_context(conn, "query/prepare_recv", SQLI_SQ_PREPARE);
        set_error(conn, "no statement ID from prepare");
        sqli_result_destroy(r);
        return SQLI_PROTO_ERROR;
    }

    /* Reset EOF flag between phases */
    r->eof = 0;

    /* Phase 2: for SELECT mimic Client flow:
     *   OPEN (+EOT) -> read ack/EOT
     *   FETCH (+optional RET_TYPE + EOT) -> read TUPLE/DONE
     * For DML use EXECUTE. */
    if (r->statement_type == 2) {
        rc = sqli_send_open(conn->socket_fd, stmt_id,
                            resolved_options.cursor_type,
                            resolved_options.holdability);
        if (rc != SQLI_OK) {
            set_error_context(conn, "query/open_send", SQLI_SQ_PREPARE);
            set_error(conn, "failed to send open");
            sqli_result_destroy(r);
            return rc;
        }
        set_error_context(conn, "query/open_recv", SQLI_SQ_PREPARE);
        r->saw_done = false;
        r->saw_error = false;
        rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
        if (rc != SQLI_OK) {
            if (!conn->error_info.has_error)
                set_error(conn, "error receiving open response");
            sqli_result_destroy(r);
            return rc;
        }

        r->eof = 0;
        if (resolved_options.cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
            rc = sqli_send_scroll_fetch(conn->socket_fd, stmt_id, r, 1, 1);
        } else {
            rc = sqli_send_fetch(conn->socket_fd, stmt_id, r);
        }
        if (rc != SQLI_OK) {
            set_error_context(conn, "query/fetch_send", SQLI_SQ_NFETCH);
            set_error(conn, "failed to send fetch");
            sqli_result_destroy(r);
            return rc;
        }
    } else {
        rc = sqli_send_execute(conn->socket_fd, stmt_id);
        if (rc != SQLI_OK) {
            set_error_context(conn, "query/execute_send", SQLI_SQ_EXECUTE);
            set_error(conn, "failed to send execute");
            sqli_result_destroy(r);
            return rc;
        }
    }

    if (r->statement_type == 2) {
        /* SELECT cursor fetch loop: keep fetching until a fetch cycle
         * returns no additional rows. */
        for (;;) {
            int prev_rows = r->row_count;
            set_error_context(conn, "query/fetch_recv", SQLI_SQ_NFETCH);
            r->saw_done = false;
            r->saw_error = false;
            rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
            if (rc != SQLI_OK) {
                if (!conn->error_info.has_error)
                    set_error(conn, "error receiving fetch response");
                sqli_result_destroy(r);
                return rc;
            }

            if (r->row_count <= prev_rows)
                break;

            r->eof = 0;
            if (resolved_options.cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
                rc = sqli_send_scroll_fetch(conn->socket_fd, stmt_id, r, 1,
                                            (int32_t)(prev_rows + 1));
            } else {
                rc = sqli_send_fetch(conn->socket_fd, stmt_id, r);
            }
            if (rc != SQLI_OK) {
                set_error_context(conn, "query/fetch_send", SQLI_SQ_NFETCH);
                set_error(conn, "failed to send fetch");
                sqli_result_destroy(r);
                return rc;
            }
        }
        if (resolved_options.cursor_type != SQLI_CURSOR_SCROLL_INSENSITIVE)
            sqli_stmt_close_release(conn, stmt_id);
        if (resolved_options.cursor_type != SQLI_CURSOR_SCROLL_INSENSITIVE)
            r->stmt_id = -1;
    } else {
        int guard = 0;
        for (;;) {
            set_error_context(conn, "query/execute_recv", SQLI_SQ_EXECUTE);
            r->saw_done = false;
            r->saw_error = false;
            rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
            if (rc != SQLI_OK) {
                if (!conn->error_info.has_error)
                    set_error(conn, "error receiving execute response");
                sqli_result_destroy(r);
                return rc;
            }
            if (r->saw_done || r->saw_error)
                break;

            if (sqli_protocol_has_buffered_data(conn, conn->socket_fd)) {
                guard++;
                if (guard > 64) {
                    set_error_context(conn, "query/execute_recv", SQLI_SQ_EXECUTE);
                    set_error(conn, "execute response did not converge to DONE");
                    sqli_result_destroy(r);
                    return SQLI_PROTO_ERROR;
                }
                continue;
            }

            uint8_t b = 0;
            ssize_t rn = sqli_tcp_peek(conn->socket_fd, &b, 1);
            if (rn <= 0) {
                struct pollfd pfd;
                pfd.fd = conn->socket_fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                (void)poll(&pfd, 1, 20);
                rn = sqli_tcp_peek(conn->socket_fd, &b, 1);
                if (rn <= 0)
                    break;
            }
            guard++;
            if (guard > 64) {
                set_error_context(conn, "query/execute_recv", SQLI_SQ_EXECUTE);
                set_error(conn, "execute response did not converge to DONE");
                sqli_result_destroy(r);
                return SQLI_PROTO_ERROR;
            }
        }

        /* Some server variants emit additional DONE/COST/EOT groups for
         * LOB statements after the first converged DONE. Drain them so the
         * next command starts on a clean opcode boundary. */
        for (int extra = 0; extra < 8; extra++) {
            if (sqli_protocol_has_buffered_data(conn, conn->socket_fd)) {
                /* Drain immediately from internal buffer before polling socket. */
            } else {
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
            tail.eof = 0;
            tail.saw_done = false;
            tail.saw_error = false;
            rc = sqli_receive_dispatch(conn->socket_fd, &tail, conn);
            sqli_result_cleanup(&tail);
            if (rc != SQLI_OK) {
                if (!conn->error_info.has_error)
                    set_error(conn, "error draining execute tail response");
                sqli_result_destroy(r);
                return rc;
            }
        }
    }

    *result = r;
    return SQLI_OK;
}

sqli_status sqli_query(sqli_conn_t *conn, const char *sql, sqli_result_t **result)
{
    return sqli_query_ex(conn, sql, NULL, result);
}

static sqli_status sqli_stream_deliver_rows(sqli_conn_t *conn, sqli_result_t *r,
                                            sqli_row_callback on_row, void *ctx,
                                            int64_t *row_counter)
{
    if (r == NULL)
        return SQLI_INVALID_STATE;
    if (on_row == NULL)
        return SQLI_OK;

    while (sqli_result_next(r)) {
        if (on_row(r, ctx) != 0) {
            set_error_context(conn, "query/stream_callback", 0);
            set_error(conn, "row callback aborted streaming");
            return SQLI_ERR;
        }
        if (row_counter != NULL)
            (*row_counter)++;
    }
    return SQLI_OK;
}

sqli_status sqli_query_stream(sqli_conn_t *conn, const char *sql,
                              sqli_row_callback on_row, void *ctx,
                              int64_t *out_rows)
{
    if (conn == NULL || sql == NULL)
        return SQLI_INVALID_STATE;
    if (out_rows != NULL)
        *out_rows = 0;

    if (conn->state != SQLI_CONN_READY) {
        set_error_context(conn, "query_stream/precheck", 0);
        set_error(conn, "connection not ready");
        return SQLI_INVALID_STATE;
    }

    sqli_result_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        set_error_context(conn, "query_stream/alloc", 0);
        set_error(conn, "memory allocation failed");
        return SQLI_ALLOC_FAIL;
    }
    r->owner_conn = conn;

    sqli_status rc = sqli_send_prepare(conn, sql);
    if (rc != SQLI_OK) {
        set_error_context(conn, "query_stream/prepare_send", SQLI_SQ_PREPARE);
        set_error(conn, "failed to send prepare");
        sqli_result_destroy(r);
        return rc;
    }

    set_error_context(conn, "query_stream/prepare_recv", SQLI_SQ_PREPARE);
    rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
    if (rc != SQLI_OK) {
        if (!conn->error_info.has_error)
            set_error(conn, "error receiving prepare response");
        sqli_result_destroy(r);
        return rc;
    }

    int stmt_id = r->stmt_id;
    if (stmt_id < 0) {
        set_error_context(conn, "query_stream/prepare_recv", SQLI_SQ_PREPARE);
        set_error(conn, "no statement ID from prepare");
        sqli_result_destroy(r);
        return SQLI_PROTO_ERROR;
    }

    r->eof = 0;
    int64_t delivered = 0;

    if (r->statement_type == 2) {
        rc = sqli_send_open(conn->socket_fd, stmt_id,
                            conn->cursor_type, conn->holdability);
        if (rc != SQLI_OK) {
            set_error_context(conn, "query_stream/open_send", SQLI_SQ_PREPARE);
            set_error(conn, "failed to send open");
            sqli_result_destroy(r);
            return rc;
        }
        set_error_context(conn, "query_stream/open_recv", SQLI_SQ_PREPARE);
        rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
        if (rc != SQLI_OK) {
            if (!conn->error_info.has_error)
                set_error(conn, "error receiving open response");
            sqli_result_destroy(r);
            return rc;
        }
        r->eof = 0;
        if (conn->cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
            rc = sqli_send_scroll_fetch(conn->socket_fd, stmt_id, r, 1, 1);
        } else {
            rc = sqli_send_fetch(conn->socket_fd, stmt_id, r);
        }
        if (rc != SQLI_OK) {
            set_error_context(conn, "query_stream/fetch_send", SQLI_SQ_NFETCH);
            set_error(conn, "failed to send fetch");
            sqli_result_destroy(r);
            return rc;
        }

        for (;;) {
            set_error_context(conn, "query_stream/fetch_recv", SQLI_SQ_NFETCH);
            rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
            if (rc != SQLI_OK) {
                if (!conn->error_info.has_error)
                    set_error(conn, "error receiving fetch response");
                sqli_result_destroy(r);
                return rc;
            }

            int got = r->row_count;
            rc = sqli_stream_deliver_rows(conn, r, on_row, ctx, &delivered);
            if (rc != SQLI_OK) {
                sqli_result_destroy(r);
                return rc;
            }

            sqli_result_clear_rows(r);
            if (got <= 0)
                break;

            r->eof = 0;
            if (conn->cursor_type == SQLI_CURSOR_SCROLL_INSENSITIVE) {
                rc = sqli_send_scroll_fetch(conn->socket_fd, stmt_id, r, 1,
                                            (int32_t)(got + 1));
            } else {
                rc = sqli_send_fetch(conn->socket_fd, stmt_id, r);
            }
            if (rc != SQLI_OK) {
                set_error_context(conn, "query_stream/fetch_send", SQLI_SQ_NFETCH);
                set_error(conn, "failed to send fetch");
                sqli_result_destroy(r);
                return rc;
            }
        }
        sqli_stmt_close_release(conn, stmt_id);
        r->stmt_id = -1;
    } else {
        rc = sqli_send_execute(conn->socket_fd, stmt_id);
        if (rc != SQLI_OK) {
            set_error_context(conn, "query_stream/execute_send", SQLI_SQ_EXECUTE);
            set_error(conn, "failed to send execute");
            sqli_result_destroy(r);
            return rc;
        }
        set_error_context(conn, "query_stream/execute_recv", SQLI_SQ_EXECUTE);
        rc = sqli_receive_dispatch(conn->socket_fd, r, conn);
        if (rc != SQLI_OK) {
            if (!conn->error_info.has_error)
                set_error(conn, "error receiving execute response");
            sqli_result_destroy(r);
            return rc;
        }
        rc = sqli_stream_deliver_rows(conn, r, on_row, ctx, &delivered);
        if (rc != SQLI_OK) {
            sqli_result_destroy(r);
            return rc;
        }
    }

    if (out_rows != NULL)
        *out_rows = delivered;
    sqli_result_destroy(r);
    return SQLI_OK;
}

static void sqli_sleep_ms(uint32_t delay_ms)
{
    if (delay_ms == 0)
        return;
    struct timespec req;
    req.tv_sec = (time_t)(delay_ms / 1000u);
    req.tv_nsec = (long)((delay_ms % 1000u) * 1000000u);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}

static int sqli_sql_is_read_only(const char *sql)
{
    if (sql == NULL)
        return 0;

    const unsigned char *p = (const unsigned char *)sql;
    for (;;) {
        while (*p != '\0' && isspace(*p))
            p++;
        if (p[0] == '-' && p[1] == '-') {
            p += 2;
            while (*p != '\0' && *p != '\n')
                p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p[0] != '\0' && !(p[0] == '*' && p[1] == '/'))
                p++;
            if (p[0] == '*' && p[1] == '/')
                p += 2;
            continue;
        }
        break;
    }

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

sqli_status sqli_query_with_retry(sqli_conn_t *conn, const char *sql,
                                  uint32_t max_retries, sqli_result_t **result)
{
    if (conn == NULL || sql == NULL || result == NULL)
        return SQLI_INVALID_STATE;

    *result = NULL;
    const int read_only = sqli_sql_is_read_only(sql);
    sqli_status rc = SQLI_ERR;
    for (uint32_t attempt = 0;; attempt++) {
        rc = sqli_query(conn, sql, result);
        if (rc == SQLI_OK)
            return SQLI_OK;
        if (attempt >= max_retries)
            return rc;
        if (!read_only) {
            sqli_log(SQLI_LOG_WARN,
                     "query retry skipped for non-read-only statement");
            return rc;
        }

        bool should_retry = false;
        uint32_t delay_ms = 0;
        if (sqli_retry_recommend(conn, attempt, &should_retry, &delay_ms) != SQLI_OK ||
            !should_retry) {
            return rc;
        }

        sqli_log(SQLI_LOG_WARN,
                 "query retry attempt=%u delay_ms=%u rc=%d msg=%s",
                 attempt + 1u, delay_ms, (int)rc,
                 sqli_error(conn) ? sqli_error(conn) : "-");
        sqli_sleep_ms(delay_ms);
    }
}
