#define _GNU_SOURCE
#include "sqli_internal.h"

#include "sqli_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>

/* ----------------------------------------------------------------
 * sqlhosts file format
 * ----------------------------------------------------------------
 * Each non-comment, non-blank line has:
 *   server_name  protocol  hostname  service  [options...]
 *
 * Comment lines start with #.
 * Lines starting with @ are include directives (ignored).
 * ---------------------------------------------------------------- */

#define MAX_LINE 1024
#define MAX_ENTRIES 256

/* Trim leading/trailing whitespace in-place. Returns pointer into s. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    if (*s == '\0')
        return s;

    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        *end-- = '\0';
    return s;
}

/* ----------------------------------------------------------------
 * sqli_parse_sqlhosts
 * ---------------------------------------------------------------- */

sqli_status sqli_parse_sqlhosts(const char *filepath,
                                sqli_sqlhosts_entry **entries,
                                int *count)
{
    if (entries == NULL || count == NULL)
        return SQLI_INVALID_STATE;

    *entries = NULL;
    *count = 0;

    /* Determine file path: use provided path, then env, then defaults */
    char path[MAX_LINE];
    if (filepath != NULL) {
        snprintf(path, sizeof(path), "%s", filepath);
    } else {
        const char *env = getenv("INFORMIXDIR");
        if (env != NULL) {
            snprintf(path, sizeof(path), "%s/etc/sqlhosts", env);
        } else {
            snprintf(path, sizeof(path), "/etc/sqlhosts");
        }
    }

    /* Check file exists */
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        sqli_log(SQLI_LOG_DEBUG, "sqlhosts: file not found: %s", path);
        *entries = NULL;
        *count = 0;
        return SQLI_OK; /* No file is not an error — empty result */
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        sqli_log(SQLI_LOG_ERROR, "sqlhosts: cannot open %s", path);
        return SQLI_IO_ERROR;
    }

    /* Allocate entries array */
    sqli_sqlhosts_entry *arr = calloc(MAX_ENTRIES, sizeof(sqli_sqlhosts_entry));
    if (arr == NULL) {
        fclose(fp);
        return SQLI_ALLOC_FAIL;
    }

    int n = 0;
    char line[MAX_LINE];
    while (n < MAX_ENTRIES && fgets(line, sizeof(line), fp) != NULL) {
        char *s = trim(line);

        /* Skip comments and blank lines */
        if (s[0] == '\0' || s[0] == '#')
            continue;

        /* Skip include directives */
        if (s[0] == '@')
            continue;

        /* Parse: server_name protocol hostname service [options...] */
        char server_name[256], protocol[64], hostname[256], service[64], options[256];
        if (sscanf(s, "%255s %63s %255s %63s %255[^\n]",
                   server_name, protocol, hostname, service, options) < 4) {
            sqli_log(SQLI_LOG_WARN, "sqlhosts: malformed line: %s", line);
            continue;
        }

        /* Trim parsed fields */
        char *p;
        if ((p = trim(server_name)) != s)
            memmove(server_name, p, strlen(p) + 1);
        if ((p = trim(protocol)) != s)
            memmove(protocol, p, strlen(p) + 1);
        if ((p = trim(hostname)) != s)
            memmove(hostname, p, strlen(p) + 1);
        if ((p = trim(service)) != s)
            memmove(service, p, strlen(p) + 1);
        if ((p = trim(options)) != s)
            memmove(options, p, strlen(p) + 1);

        /* Clear and copy */
        memset(&arr[n], 0, sizeof(sqli_sqlhosts_entry));
        snprintf(arr[n].server_name, sizeof(arr[n].server_name), "%s", server_name);
        snprintf(arr[n].protocol, sizeof(arr[n].protocol), "%s", protocol);
        snprintf(arr[n].hostname, sizeof(arr[n].hostname), "%s", hostname);
        snprintf(arr[n].service, sizeof(arr[n].service), "%s", service);
        snprintf(arr[n].options, sizeof(arr[n].options), "%s",
                 options[0] != '\0' ? options : "");

        n++;
    }

    fclose(fp);

    /* If we read fewer than MAX_ENTRIES, shrink */
    if (n < MAX_ENTRIES) {
        sqli_sqlhosts_entry *tmp = realloc(arr, (size_t)n * sizeof(sqli_sqlhosts_entry));
        if (tmp != NULL)
            arr = tmp;
    }

    *entries = arr;
    *count = n;

    sqli_log(SQLI_LOG_DEBUG, "sqlhosts: parsed %d entries from %s", n, path);
    return SQLI_OK;
}

/* ----------------------------------------------------------------
 * sqli_find_sqlhosts_entry
 * ---------------------------------------------------------------- */

const sqli_sqlhosts_entry *sqli_find_sqlhosts_entry(
    const sqli_sqlhosts_entry *entries, int count,
    const char *server_name)
{
    if (entries == NULL || server_name == NULL || count <= 0)
        return NULL;

    for (int i = 0; i < count; i++) {
        if (strcasecmp(entries[(size_t)i].server_name, server_name) == 0)
            return &entries[(size_t)i];
    }

    return NULL;
}
