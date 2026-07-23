#include "sqli_charset.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *canonical_name;
    const char *iconv_name;
    unsigned int windows_codepage;
    int gls_csid;
    const char *aliases[8];
} sqli_charset_entry;

static const sqli_charset_entry g_sqli_charset_entries[] = {
    { "ASCII", "ASCII", 20127u, 364, { "ASCII", "US-ASCII", NULL } },
    { "UTF-8", "UTF-8", 65001u, 57372, { "UTF-8", "UTF8", NULL } },
    { "ISO-8859-1", "ISO-8859-1", 28591u, 819,
      { "ISO-8859-1", "ISO8859-1", "8859-1", "LATIN1", NULL } },
    { "ISO-8859-2", "ISO-8859-2", 28592u, 912,
      { "ISO-8859-2", "ISO8859-2", "8859-2", NULL } },
    { "ISO-8859-5", "ISO-8859-5", 28595u, 915,
      { "ISO-8859-5", "ISO8859-5", "8859-5", NULL } },
    { "ISO-8859-6", "ISO-8859-6", 28596u, 1089,
      { "ISO-8859-6", "ISO8859-6", "8859-6", NULL } },
    { "ISO-8859-7", "ISO-8859-7", 28597u, 813,
      { "ISO-8859-7", "ISO8859-7", "8859-7", NULL } },
    { "ISO-8859-8", "ISO-8859-8", 28598u, 916,
      { "ISO-8859-8", "ISO8859-8", "8859-8", NULL } },
    { "ISO-8859-9", "ISO-8859-9", 28599u, 920,
      { "ISO-8859-9", "ISO8859-9", "8859-9", NULL } },
    { "ISO-8859-13", "ISO-8859-13", 28603u, 57390,
      { "ISO-8859-13", "ISO8859-13", "8859-13", NULL } },
    { "ISO-8859-15", "ISO-8859-15", 28605u, 57391,
      { "ISO-8859-15", "ISO8859-15", "8859-15", NULL } },
    { "CP850", "CP850", 850u, 850, { "CP850", NULL } },
    { "CP852", "CP852", 852u, 852, { "CP852", NULL } },
    { "CP857", "CP857", 857u, 857, { "CP857", NULL } },
    { "CP864", "CP864", 864u, 864, { "CP864", NULL } },
    { "CP866", "CP866", 866u, 866, { "CP866", NULL } },
    { "CP874", "CP874", 874u, 57373, { "CP874", NULL } },
    { "CP932", "CP932", 932u, 932, { "CP932", "SHIFT_JIS", "SJIS", NULL } },
    { "CP936", "CP936", 936u, 57357, { "CP936", "GB2312", NULL } },
    { "CP949", "CP949", 949u, 57356, { "CP949", NULL } },
    { "CP950", "CP950", 950u, 950, { "CP950", "BIG5", NULL } },
    { "CP1250", "CP1250", 1250u, 1250, { "CP1250", "WINDOWS-1250", NULL } },
    { "CP1251", "CP1251", 1251u, 1251, { "CP1251", "WINDOWS-1251", NULL } },
    { "CP1252", "CP1252", 1252u, 1252, { "CP1252", "WINDOWS-1252", NULL } },
    { "CP1253", "CP1253", 1253u, 1253, { "CP1253", "WINDOWS-1253", NULL } },
    { "CP1254", "CP1254", 1254u, 1254, { "CP1254", "WINDOWS-1254", NULL } },
    { "CP1255", "CP1255", 1255u, 1255, { "CP1255", "WINDOWS-1255", NULL } },
    { "CP1256", "CP1256", 1256u, 1256, { "CP1256", "WINDOWS-1256", NULL } },
    { "CP1257", "CP1257", 1257u, 1257, { "CP1257", "WINDOWS-1257", NULL } },
};

static bool sqli_token_equals(const char *lhs, const char *rhs)
{
    if (lhs == NULL || rhs == NULL)
        return false;

    while (*lhs != '\0' && *rhs != '\0') {
        unsigned char lc = (unsigned char)*lhs;
        unsigned char rc = (unsigned char)*rhs;
        if (lc == '-' || lc == '_' || lc == ' ') {
            lhs++;
            continue;
        }
        if (rc == '-' || rc == '_' || rc == ' ') {
            rhs++;
            continue;
        }
        if (tolower(lc) != tolower(rc))
            return false;
        lhs++;
        rhs++;
    }

    while (*lhs == '-' || *lhs == '_' || *lhs == ' ')
        lhs++;
    while (*rhs == '-' || *rhs == '_' || *rhs == ' ')
        rhs++;
    return *lhs == '\0' && *rhs == '\0';
}

static bool sqli_parse_decimal(const char *value, long *out)
{
    char *end = NULL;

    if (value == NULL || *value == '\0' || out == NULL)
        return false;

    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0')
        return false;

    *out = parsed;
    return true;
}

static bool sqli_charset_copy_spec(const sqli_charset_entry *entry, sqli_charset_spec *out)
{
    if (entry == NULL || out == NULL)
        return false;

    out->canonical_name = entry->canonical_name;
    out->iconv_name = entry->iconv_name;
    out->windows_codepage = entry->windows_codepage;
    out->gls_csid = entry->gls_csid;
    return true;
}

static bool sqli_charset_match_aliases(const sqli_charset_entry *entry, const char *token)
{
    size_t i;

    if (entry == NULL || token == NULL)
        return false;

    for (i = 0; i < (sizeof(entry->aliases) / sizeof(entry->aliases[0])); i++) {
        if (entry->aliases[i] == NULL)
            break;
        if (sqli_token_equals(token, entry->aliases[i]))
            return true;
    }
    return false;
}

bool sqli_charset_resolve_codeset(const char *codeset, sqli_charset_spec *out)
{
    size_t i;
    long numeric = 0;
    bool have_numeric = sqli_parse_decimal(codeset, &numeric);

    if (codeset == NULL || out == NULL)
        return false;

    for (i = 0; i < (sizeof(g_sqli_charset_entries) / sizeof(g_sqli_charset_entries[0])); i++) {
        const sqli_charset_entry *entry = &g_sqli_charset_entries[i];
        if (sqli_charset_match_aliases(entry, codeset))
            return sqli_charset_copy_spec(entry, out);
        if (have_numeric &&
            (numeric == (long)entry->gls_csid || numeric == (long)entry->windows_codepage)) {
            return sqli_charset_copy_spec(entry, out);
        }
    }

    return false;
}

bool sqli_charset_resolve_locale(const char *locale, sqli_charset_spec *out)
{
    const char *token;
    char buf[64];
    size_t n = 0;

    if (locale == NULL || out == NULL)
        return false;

    token = strchr(locale, '.');
    token = (token != NULL) ? token + 1 : locale;
    if (token == NULL || *token == '\0')
        return false;

    while (token[n] != '\0' && token[n] != '@' && token[n] != '/' && n + 1 < sizeof(buf)) {
        buf[n] = token[n];
        n++;
    }
    buf[n] = '\0';

    if (buf[0] == '\0')
        return false;

    return sqli_charset_resolve_codeset(buf, out);
}

bool sqli_charset_decoder_open_locales(sqli_charset_decoder *decoder,
                                       const char *to_locale,
                                       const char *from_locale)
{
    sqli_charset_spec to_spec;
    sqli_charset_spec from_spec;

    if (!sqli_charset_resolve_locale(to_locale, &to_spec) ||
        !sqli_charset_resolve_locale(from_locale, &from_spec))
        return false;

    if (sqli_token_equals(to_spec.canonical_name, from_spec.canonical_name))
        return false;

    return sqli_charset_decoder_open(decoder,
                                     to_spec.canonical_name,
                                     from_spec.canonical_name);
}
