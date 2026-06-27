#ifndef SQLI_RESULT_INTERNAL_H
#define SQLI_RESULT_INTERNAL_H

#include "sqli_internal.h"

sqli_status sqli_tuple_locate_column(const sqli_column_info *col_info,
                                     const uint8_t *tuple_buf,
                                     size_t tuple_len, size_t *data_start,
                                     size_t *data_len, size_t *span);
void sqli_result_prepare_row_cache(sqli_result_t *result);
bool sqli_result_is_null_internal(sqli_result_t *result, int col_index);
void sqli_result_clear_rows(sqli_result_t *result);
void sqli_base100_complement(uint8_t *digits, size_t digit_count);
bool sqli_is_stringy_type(uint8_t type);

#endif
