#ifndef SQLI_LEGACY_TYPES_H
#define SQLI_LEGACY_TYPES_H
#include <stddef.h>
#include <stdint.h>
/* Historical codecs retained only for internal regression fixtures. */
/* ----------------------------------------------------------------
 * Type encoding utilities
 * ---------------------------------------------------------------- */

/*
 * Encode a DATE value as 4 big-endian bytes (days since Informix epoch).
 * Informix wire epoch: 1899-12-31 (day 0).
 * Example: 1970-01-01 => 25568.
 * Returns the 4-byte big-endian encoding.
 */
int32_t sqli_encode_date(int32_t days_since_epoch);

/*
 * Decode days since Informix epoch from a DATE value.
 */
int32_t sqli_decode_date(int32_t encoded_date);

/*
 * Encode a DATETIME value into buf using BCD Decimal wire format (spec §7.5).
 * Encodes YEAR TO SECOND (14 decimal digits: YYYYMMDDHHMMSS).
 * frac is reserved for future FRACTION support.
 *
 * Returns bytes written (same layout as sqli_encode_decimal), 0 on error.
 */
size_t sqli_encode_datetime(int year, int month, int day,
                            int hour, int minute, int second,
                            unsigned int frac,
                            uint8_t *buf, size_t buf_size);

/*
 * Decode a DATETIME value from BCD Decimal wire format.
 * buf/buf_len point to the raw wire bytes (including the 2-byte length prefix).
 */
void sqli_decode_datetime(const uint8_t *buf, size_t buf_len,
                          int *year, int *month, int *day,
                          int *hour, int *minute, int *second,
                          unsigned int *frac);

/*
 * Encode a DECIMAL value into a buffer using BCD encoding.
 *
 * precision: total number of digits (1-15)
 * scale: number of digits after decimal point (0 <= scale <= precision)
 * negative: non-zero for negative numbers
 * digits: array of 'precision' decimal digits (0-9)
 *
 * Wire format (spec §7.4): [2-byte length][exponent byte][BCD digit bytes]
 * exponent byte = ((exp+64) & 0x7F) | (positive ? 0x80 : 0x00)
 * where exp = (precision - scale) - 1.
 * Negative values are 10's-complemented in the BCD digit bytes.
 *
 * Returns bytes written, 0 on error (buffer too small).
 */
size_t sqli_encode_decimal(uint8_t *buf, size_t buf_size,
                           uint8_t precision, uint8_t scale,
                           int negative, const uint8_t *digits);


#endif
