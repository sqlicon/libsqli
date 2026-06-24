#include "libsqli/sqli.h"

#include <string.h>
#include <stdint.h>

/* Informix wire DATE epoch is 1899-12-31 (day 0). */

/* ----------------------------------------------------------------
 * DATE encoding/decoding — plain 4-byte integer (days since epoch)
 * ---------------------------------------------------------------- */

int32_t sqli_encode_date(int32_t days_since_epoch)
{
    return days_since_epoch;
}

int32_t sqli_decode_date(int32_t encoded_date)
{
    return encoded_date;
}

/* ----------------------------------------------------------------
 * DECIMAL encoding (spec §7.4)
 *
 * Wire format: [2-byte length][exponent byte][BCD digit bytes]
 *
 * exponent byte = ((exp + 64) & 0x7F) | (positive ? 0x80 : 0x00)
 *   where exp = (precision - scale) - 1
 *
 * For negative values the BCD digits are 10's-complemented.
 * ---------------------------------------------------------------- */

size_t sqli_encode_decimal(uint8_t *buf, size_t buf_size,
                           uint8_t precision, uint8_t scale,
                           int negative, const uint8_t *digits)
{
    if (buf == NULL || digits == NULL)
        return 0;
    if (precision == 0 || precision > 32)
        return 0;
    if (scale > precision)
        return 0;

    /* BCD storage: 2 digits per byte, ceil(precision/2) bytes */
    size_t bcd_len = (size_t)((precision + 1) / 2);
    /* total = 2-byte length + 1-byte exponent + bcd_len bytes */
    size_t total = 2 + 1 + bcd_len;

    if (buf_size < total)
        return 0;

    /* length field = bytes that follow the length word */
    uint16_t payload_len = (uint16_t)(1 + bcd_len);
    buf[0] = (uint8_t)(payload_len >> 8);
    buf[1] = (uint8_t)(payload_len & 0xFF);

    /* exponent: exp = (precision - scale) - 1 */
    int exp = (int)(precision - scale) - 1;
    buf[2] = (uint8_t)(((exp + 64) & 0x7F) | (negative ? 0x00u : 0x80u));

    /* pack BCD digits */
    for (size_t i = 0; i < bcd_len; i++) {
        uint8_t hi = (2 * i     < precision) ? digits[2 * i]     : 0;
        uint8_t lo = (2 * i + 1 < precision) ? digits[2 * i + 1] : 0;
        buf[3 + i] = (uint8_t)((hi << 4) | lo);
    }

    /* For negative values, apply 10's complement to BCD bytes */
    if (negative) {
        int carry = 1;
        for (int i = (int)bcd_len - 1; i >= 0 && carry; i--) {
            int lo_val = (9 - (buf[3 + i] & 0x0F)) + carry;
            carry     = lo_val / 10;
            int hi_val = (9 - (buf[3 + i] >> 4)) + carry;
            carry     = hi_val / 10;
            buf[3 + i] = (uint8_t)(((hi_val % 10) << 4) | (lo_val % 10));
        }
    }

    return total;
}

/* ----------------------------------------------------------------
 * DECIMAL decoding (spec §7.4)
 *
 * buf[0..1]  : 2-byte length (bytes that follow)
 * buf[2]     : exponent byte
 * buf[3..]   : BCD digit bytes
 * ---------------------------------------------------------------- */

int sqli_decode_decimal(const uint8_t *buf, size_t buf_size,
                        uint8_t precision, uint8_t *scale,
                        int *negative, uint8_t *digits)
{
    if (buf == NULL || buf_size < 3 || scale == NULL ||
        negative == NULL || digits == NULL)
        return -1;

    /* Length word */
    uint16_t payload_len = (uint16_t)((buf[0] << 8) | buf[1]);
    if (payload_len < 1 || (size_t)(payload_len + 2) > buf_size)
        return -1;

    /* Exponent byte */
    uint8_t exp_byte = buf[2];
    int positive = (exp_byte & 0x80) != 0;
    int exp = (int)(exp_byte & 0x7F) - 64;

    *negative = !positive;
    /* scale = (precision - 1) - exp */
    int sc = (int)(precision - 1) - exp;
    *scale = (sc >= 0 && sc <= 32) ? (uint8_t)sc : 0;

    /* BCD bytes start at buf[3] */
    size_t bcd_len = (size_t)(payload_len - 1);
    uint8_t bcd_copy[32];
    if (bcd_len > sizeof(bcd_copy)) bcd_len = sizeof(bcd_copy);
    memcpy(bcd_copy, buf + 3, bcd_len);

    /* Undo 10's complement for negative values */
    if (*negative) {
        int carry = 1;
        for (int i = (int)bcd_len - 1; i >= 0 && carry; i--) {
            int lo_val = (9 - (bcd_copy[i] & 0x0F)) + carry;
            carry     = lo_val / 10;
            int hi_val = (9 - (bcd_copy[i] >> 4)) + carry;
            carry     = hi_val / 10;
            bcd_copy[i] = (uint8_t)(((hi_val % 10) << 4) | (lo_val % 10));
        }
    }

    /* Unpack digits */
    for (uint8_t i = 0; i < precision; i++) {
        size_t byte_idx = i / 2;
        if (byte_idx >= bcd_len) {
            digits[i] = 0;
        } else if (i % 2 == 0) {
            digits[i] = bcd_copy[byte_idx] >> 4;
        } else {
            digits[i] = bcd_copy[byte_idx] & 0x0F;
        }
    }

    return (int)precision;
}

/* ----------------------------------------------------------------
 * DATETIME encoding (spec §7.5)
 *
 * DATETIME is encoded as Decimal BCD with 14 decimal digits:
 *   YYYY MM DD HH MI SS  (year=4, rest=2 each)
 * Uses the standard Decimal wire format (same as sqli_encode_decimal).
 * precision=14, scale=0 → no fractional part for YEAR TO SECOND.
 * ---------------------------------------------------------------- */

size_t sqli_encode_datetime(int year, int month, int day,
                            int hour, int minute, int second,
                            unsigned int frac,
                            uint8_t *buf, size_t buf_size)
{
    (void)frac; /* reserved — fractional seconds not encoded here */

    if (year < 1 || year > 9999) year = 1970;
    if (month < 1 || month > 12) month = 1;
    if (day < 1 || day > 31)     day   = 1;
    if (hour < 0 || hour > 23)   hour  = 0;
    if (minute < 0 || minute > 59) minute = 0;
    if (second < 0 || second > 59) second = 0;

    /* 14 decimal digits: YYYYMMDDHHMMSS */
    uint8_t digits[14] = {
        (uint8_t)(year  / 1000),
        (uint8_t)((year / 100) % 10),
        (uint8_t)((year / 10)  % 10),
        (uint8_t)(year  % 10),
        (uint8_t)(month / 10),
        (uint8_t)(month % 10),
        (uint8_t)(day   / 10),
        (uint8_t)(day   % 10),
        (uint8_t)(hour  / 10),
        (uint8_t)(hour  % 10),
        (uint8_t)(minute / 10),
        (uint8_t)(minute % 10),
        (uint8_t)(second / 10),
        (uint8_t)(second % 10)
    };

    return sqli_encode_decimal(buf, buf_size, 14, 0, 0, digits);
}

/* ----------------------------------------------------------------
 * DATETIME decoding
 * ---------------------------------------------------------------- */

void sqli_decode_datetime(const uint8_t *buf, size_t buf_len,
                          int *year, int *month, int *day,
                          int *hour, int *minute, int *second,
                          unsigned int *frac)
{
    *year = 1970; *month = 1; *day = 1;
    *hour = 0;    *minute = 0; *second = 0;
    if (frac != NULL) *frac = 0;

    if (buf == NULL || buf_len < 3)
        return;

    uint8_t digits[14] = {0};
    uint8_t scale;
    int neg;
    if (sqli_decode_decimal(buf, buf_len, 14, &scale, &neg, digits) < 0)
        return;

    *year   = digits[0] * 1000 + digits[1] * 100 + digits[2] * 10 + digits[3];
    *month  = digits[4] * 10  + digits[5];
    *day    = digits[6] * 10  + digits[7];
    *hour   = digits[8] * 10  + digits[9];
    *minute = digits[10] * 10 + digits[11];
    *second = digits[12] * 10 + digits[13];
}
