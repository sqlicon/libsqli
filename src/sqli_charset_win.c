#include "sqli_charset.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static bool sqli_charset_resolve_codepage(const char *value, UINT *codepage)
{
    sqli_charset_spec spec;

    if (codepage == NULL)
        return false;
    if (!sqli_charset_resolve_codeset(value, &spec))
        return false;
    *codepage = (UINT)spec.windows_codepage;
    return *codepage != 0;
}

void sqli_charset_decoder_init(sqli_charset_decoder *decoder)
{
    if (decoder == NULL)
        return;
    decoder->to_codepage = 0;
    decoder->from_codepage = 0;
    decoder->ready = false;
}

void sqli_charset_decoder_close(sqli_charset_decoder *decoder)
{
    if (decoder == NULL)
        return;
    decoder->to_codepage = 0;
    decoder->from_codepage = 0;
    decoder->ready = false;
}

bool sqli_charset_decoder_open(sqli_charset_decoder *decoder,
                               const char *to_cs, const char *from_cs)
{
    if (decoder == NULL)
        return false;
    sqli_charset_decoder_close(decoder);
    if (!sqli_charset_resolve_codepage(from_cs, &decoder->from_codepage) ||
        !sqli_charset_resolve_codepage(to_cs, &decoder->to_codepage))
        return false;
    decoder->ready = true;
    return true;
}

static bool sqli_charset_convert_common(UINT to_cp, UINT from_cp,
                                        const char *input, size_t input_len,
                                        char **output, size_t *output_len,
                                        char *fixed_buf, size_t fixed_cap)
{
    WCHAR *wide = NULL;
    char *local_output = NULL;
    int wide_len;
    int out_len;

    if (input == NULL || output == NULL || output_len == NULL)
        return false;

    wide_len = MultiByteToWideChar(from_cp, 0, input, (int)input_len, NULL, 0);
    if (wide_len <= 0)
        return false;

    wide = malloc(((size_t)wide_len + 1u) * sizeof(*wide));
    if (wide == NULL)
        return false;

    if (MultiByteToWideChar(from_cp, 0, input, (int)input_len, wide, wide_len) <= 0) {
        free(wide);
        return false;
    }
    wide[wide_len] = L'\0';

    out_len = WideCharToMultiByte(to_cp, 0, wide, wide_len, NULL, 0, NULL, NULL);
    if (out_len <= 0) {
        free(wide);
        return false;
    }

    if (fixed_buf != NULL) {
        if ((size_t)out_len + 1u > fixed_cap) {
            free(wide);
            return false;
        }
        local_output = fixed_buf;
    } else {
        local_output = malloc((size_t)out_len + 1u);
        if (local_output == NULL) {
            free(wide);
            return false;
        }
    }

    if (WideCharToMultiByte(to_cp, 0, wide, wide_len, local_output, out_len,
                            NULL, NULL) <= 0) {
        if (fixed_buf == NULL)
            free(local_output);
        free(wide);
        return false;
    }

    local_output[out_len] = '\0';
    *output = local_output;
    *output_len = (size_t)out_len;
    free(wide);
    return true;
}

bool sqli_charset_decoder_convert(sqli_charset_decoder *decoder,
                                  const char *input, size_t input_len,
                                  char *output, size_t *output_len)
{
    char *converted = NULL;

    if (decoder == NULL || !decoder->ready)
        return false;

    return sqli_charset_convert_common(decoder->to_codepage, decoder->from_codepage,
                                       input, input_len, &converted, output_len,
                                       output, *output_len);
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
    char *converted = NULL;
    sqli_charset_decoder decoder;

    if (output == NULL || output_len == NULL)
        return false;

    sqli_charset_decoder_init(&decoder);
    if (!sqli_charset_decoder_open(&decoder, to_cs, from_cs))
        return false;
    if (!sqli_charset_convert_common(decoder.to_codepage, decoder.from_codepage,
                                     input, input_len, &converted,
                                     output_len, NULL, 0)) {
        sqli_charset_decoder_close(&decoder);
        return false;
    }

    *output = (uint8_t *)converted;
    sqli_charset_decoder_close(&decoder);
    return true;
}
