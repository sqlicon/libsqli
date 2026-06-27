#ifndef SQLI_PROTOCOL_INTERNAL_H
#define SQLI_PROTOCOL_INTERNAL_H

#include "sqli_internal.h"

bool sqli_protocol_has_buffered_data(sqli_conn_t *conn, int fd);
void sqli_stmt_close_release(sqli_conn_t *conn, int stmt_id);
int64_t sqli_decode_ifx_int8(const uint8_t *buf, bool *is_null);
sqli_status sqli_fetchblob_materialize(sqli_result_t *result, int col_index,
                                       uint8_t **blob_buf, size_t *blob_len);
bool sqli_is_lob_like_type(uint8_t type);

#endif
