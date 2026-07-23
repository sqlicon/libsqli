#include "sqli_charset.h"

#include <errno.h>
#include <iconv.h>
#include <stdlib.h>
#include <string.h>

static bool next_charset_capacity(size_t current, size_t minimum, size_t *out)
{
    if (out == NULL)
        return false;
    if (current == 0)
        current = 1;
    while (current < minimum) {
        if (current > (SIZE_MAX / 2))
            return false;
        current *= 2;
    }
    *out = current;
    return true;
}

void sqli_charset_decoder_init(sqli_charset_decoder *decoder)
{
    if (decoder == NULL)
        return;
    decoder->handle = (iconv_t)-1;
    decoder->ready = false;
}

void sqli_charset_decoder_close(sqli_charset_decoder *decoder)
{
    if (decoder == NULL || !decoder->ready || decoder->handle == (iconv_t)-1)
        return;
    iconv_close(decoder->handle);
    decoder->handle = (iconv_t)-1;
    decoder->ready = false;
}

bool sqli_charset_decoder_open(sqli_charset_decoder *decoder,
                               const char *to_cs, const char *from_cs)
{
    if (decoder == NULL || to_cs == NULL || from_cs == NULL)
        return false;

    sqli_charset_decoder_close(decoder);
    decoder->handle = iconv_open(to_cs, from_cs);
    if (decoder->handle == (iconv_t)-1)
        return false;

    decoder->ready = true;
    return true;
}

bool sqli_charset_decoder_convert(sqli_charset_decoder *decoder,
                                  const char *input, size_t input_len,
                                  char *output, size_t *output_len)
{
    if (decoder == NULL || !decoder->ready || decoder->handle == (iconv_t)-1 ||
        input == NULL || output == NULL || output_len == NULL || *output_len == 0)
        return false;

    (void)iconv(decoder->handle, NULL, NULL, NULL, NULL);

    char *in_ptr = (char *)input;
    size_t in_left = input_len;
    char *out_ptr = output;
    size_t out_left = *output_len - 1;
    if (iconv(decoder->handle, &in_ptr, &in_left, &out_ptr, &out_left) == (size_t)-1)
        return false;

    *out_ptr = '\0';
    *output_len = (size_t)(out_ptr - output);
    return true;
}

bool sqli_charset_convert_buffer(const char *to_cs, const char *from_cs,
                                 const char *input, size_t input_len,
                                 char *output, size_t *output_len)
{
    sqli_charset_decoder decoder;

    sqli_charset_decoder_init(&decoder);
    if (!sqli_charset_decoder_open(&decoder, to_cs, from_cs))
        return false;

    bool ok = sqli_charset_decoder_convert(&decoder, input, input_len,
                                           output, output_len);
    sqli_charset_decoder_close(&decoder);
    return ok;
}

bool sqli_charset_convert_alloc(const char *to_cs, const char *from_cs,
                                const char *input, size_t input_len,
                                uint8_t **output, size_t *output_len)
{
    if (to_cs == NULL || from_cs == NULL || input == NULL ||
        output == NULL || output_len == NULL)
        return false;

    sqli_charset_decoder decoder;
    sqli_charset_decoder_init(&decoder);
    if (!sqli_charset_decoder_open(&decoder, to_cs, from_cs))
        return false;

    size_t out_cap = (input_len * 4u) + 32u;
    uint8_t *buf = malloc(out_cap + 1u);
    if (buf == NULL) {
        sqli_charset_decoder_close(&decoder);
        return false;
    }

    char *in_ptr = (char *)input;
    size_t in_left = input_len;
    char *out_ptr = (char *)buf;
    size_t out_left = out_cap;

    while (true) {
        size_t rc = iconv(decoder.handle, &in_ptr, &in_left, &out_ptr, &out_left);
        if (rc != (size_t)-1)
            break;
        if (errno != E2BIG) {
            free(buf);
            sqli_charset_decoder_close(&decoder);
            return false;
        }

        size_t used = (size_t)(out_ptr - (char *)buf);
        size_t new_cap = 0;
        if (!next_charset_capacity(out_cap, out_cap + 1u, &new_cap)) {
            free(buf);
            sqli_charset_decoder_close(&decoder);
            return false;
        }

        uint8_t *grown = realloc(buf, new_cap + 1u);
        if (grown == NULL) {
            free(buf);
            sqli_charset_decoder_close(&decoder);
            return false;
        }

        buf = grown;
        out_cap = new_cap;
        out_ptr = (char *)buf + used;
        out_left = out_cap - used;
    }

    *output_len = (size_t)(out_ptr - (char *)buf);
    buf[*output_len] = '\0';
    *output = buf;
    sqli_charset_decoder_close(&decoder);
    return true;
}
