/*
 * ifx_errmsg.c -- lookup and token expansion for the embedded Informix
 * error-message catalog. See ifx_errmsg.h for the public API and
 * ifx_errmsg_data.c for the generated tables this operates on.
 */

#include "ifx_errmsg.h"
#include "libsqli/sqli.h"

extern const unsigned char ifx_dict_blob[];
extern const uint16_t ifx_dict_offsets[];
extern const unsigned char ifx_msg_blob[];

typedef struct { int32_t code; uint32_t offset; } ifx_code_entry_t;
extern const ifx_code_entry_t ifx_code_table[];
extern const size_t ifx_code_table_count;
extern const size_t ifx_dict_count;

#define IFX_TIER1_COUNT 127u /* dictionary entries reachable via a single byte 0x81..0xFF */

static const unsigned char *dict_entry(unsigned idx) {
    return &ifx_dict_blob[ifx_dict_offsets[idx]];
}

static const ifx_code_entry_t *find_code(int32_t code) {
    size_t lo = 0, hi = ifx_code_table_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int32_t c = ifx_code_table[mid].code;
        if (c == code) {
            return &ifx_code_table[mid];
        } else if (c < code) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

int ifx_errmsg_get(int32_t code, char *out, size_t outsz) {
    const ifx_code_entry_t *e = find_code(code);
    if (!e) {
        return -1;
    }

    const unsigned char *p = &ifx_msg_blob[e->offset];
    size_t used = 0;

    while (*p != 0) {
        unsigned char literal_byte;
        const unsigned char *lit;
        size_t litlen;

        if (*p == 0x80) {
            ++p;
            lit = dict_entry(IFX_TIER1_COUNT + (unsigned)*p);
            const unsigned char *q = lit;
            while (*q != 0) {
                ++q;
            }
            litlen = (size_t)(q - lit);
        } else if (*p >= 0x81) {
            lit = dict_entry((unsigned)(*p - 0x81));
            const unsigned char *q = lit;
            while (*q != 0) {
                ++q;
            }
            litlen = (size_t)(q - lit);
        } else {
            literal_byte = *p;
            lit = &literal_byte;
            litlen = 1;
        }

        if (used + litlen >= outsz) {
            return -2;
        }
        for (size_t i = 0; i < litlen; ++i) {
            out[used + i] = (char)lit[i];
        }
        used += litlen;
        ++p;
    }

    if (used >= outsz) {
        return -2;
    }
    out[used] = '\0';
    return (int)used;
}

int sqli_error_message_lookup(int32_t code, char *out, size_t outsz) {
    int rc = ifx_errmsg_get(code, out, outsz);
    if (rc == -1 && code != 0) {
        /* Fall back to sign-flipped code for ISAM codes */
        rc = ifx_errmsg_get(-code, out, outsz);
    }
    return rc;
}

