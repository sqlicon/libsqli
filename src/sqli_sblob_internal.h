#ifndef SQLI_SBLOB_INTERNAL_H
#define SQLI_SBLOB_INTERNAL_H

#include "libsqli/sqli_sblob.h"

enum { SQLI_SBLOB_LOCATOR_MAX = 72 };

struct sqli_sblob {
    int lofd;
    sqli_sblob_type type;
    unsigned char locator[SQLI_SBLOB_LOCATOR_MAX];
    size_t locator_len;
    bool open;
};

#endif
