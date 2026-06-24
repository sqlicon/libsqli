#include "sqli_internal.h"

#include "sqli_log.h"

#include <string.h>

/* ----------------------------------------------------------------
 * Public utility
 * ---------------------------------------------------------------- */

size_t sqli_pad_even(size_t n)
{
    return (n + 1) & ~(size_t)1;
}

/* ----------------------------------------------------------------
 * SL header encode/decode
 * ---------------------------------------------------------------- */

size_t sqli_wire_encode_sl_header(uint8_t *buf, size_t buf_size,
                                  uint16_t pdu_size, uint8_t sl_type,
                                  uint8_t sl_attr, uint16_t sl_opts)
{
    if (buf == NULL || buf_size < SQLI_SL_HEADER_SIZE)
        return 0;

    buf[0] = (uint8_t)((pdu_size >> 8) & 0xFF);
    buf[1] = (uint8_t)(pdu_size & 0xFF);
    buf[2] = sl_type;
    buf[3] = sl_attr;
    buf[4] = (uint8_t)(sl_opts >> 8);
    buf[5] = (uint8_t)(sl_opts & 0xFF);

    return SQLI_SL_HEADER_SIZE;
}

sqli_status sqli_wire_decode_sl_header(const uint8_t *buf, size_t buf_size,
                                       uint16_t *pdu_size, uint8_t *sl_type,
                                       uint8_t *sl_attr, uint16_t *sl_opts)
{
    if (buf == NULL || buf_size < SQLI_SL_HEADER_SIZE)
        return SQLI_IO_ERROR;

    if (pdu_size)
        *pdu_size = (uint16_t)((buf[0] << 8) | buf[1]);
    if (sl_type)
        *sl_type = buf[2];
    if (sl_attr)
        *sl_attr = buf[3];
    if (sl_opts)
        *sl_opts = (uint16_t)((buf[4] << 8) | buf[5]);

    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * ASC-BINARY string helpers
 * ---------------------------------------------------------------- */

/*
 * Encode a string as [2-byte BE length][content bytes].
 * Returns total bytes written (2 + content_len).
 */
size_t sqli_asc_encode_string(uint8_t *buf, size_t buf_size, const char *s)
{
    if (buf == NULL || s == NULL)
        return 0;

    size_t slen = (size_t)strlen(s);
    if (slen > 0xFFFFu)
        return 0;
    size_t needed = 2 + slen;

    if (buf_size < needed)
        return 0;

    buf[0] = (uint8_t)((slen >> 8) & 0xFF);
    buf[1] = (uint8_t)(slen & 0xFF);
    memcpy(buf + 2, s, slen);

    return needed;
}

/*
 * Encode a string as [2-byte BE length][content bytes][padding if odd].
 */
size_t sqli_asc_encode_string_padded(uint8_t *buf, size_t buf_size, const char *s)
{
    if (buf == NULL || s == NULL)
        return 0;

    size_t slen = (size_t)strlen(s);
    if (slen > 0xFFFFu)
        return 0;
    size_t payload = 2 + slen;
    size_t total = sqli_pad_even(payload);

    if (buf_size < total)
        return 0;

    buf[0] = (uint8_t)((slen >> 8) & 0xFF);
    buf[1] = (uint8_t)(slen & 0xFF);
    memcpy(buf + 2, s, slen);

    /* Add padding byte if needed */
    if (slen % 2 != 0)
        buf[payload] = 0;

    return total;
}
