#ifndef SQLI_CHARSET_H
#define SQLI_CHARSET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
typedef struct {
    UINT to_codepage;
    UINT from_codepage;
    bool ready;
} sqli_charset_decoder;
#else
#include <iconv.h>
typedef struct {
    iconv_t handle;
    bool ready;
} sqli_charset_decoder;
#endif

void sqli_charset_decoder_init(sqli_charset_decoder *decoder);
void sqli_charset_decoder_close(sqli_charset_decoder *decoder);
bool sqli_charset_decoder_open(sqli_charset_decoder *decoder,
                               const char *to_cs, const char *from_cs);
bool sqli_charset_decoder_convert(sqli_charset_decoder *decoder,
                                  const char *input, size_t input_len,
                                  char *output, size_t *output_len);

bool sqli_charset_convert_buffer(const char *to_cs, const char *from_cs,
                                 const char *input, size_t input_len,
                                 char *output, size_t *output_len);

bool sqli_charset_convert_alloc(const char *to_cs, const char *from_cs,
                                const char *input, size_t input_len,
                                uint8_t **output, size_t *output_len);

#endif /* SQLI_CHARSET_H */
