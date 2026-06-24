/*
 * Phase 5 unit tests for sqlhosts file parser.
 *
 * Tests sqli_parse_sqlhosts and sqli_find_sqlhosts_entry.
 */

#include "unity.h"
#include "libsqli/sqli.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Create a temporary sqlhosts file with given content.
 * Returns 0 on success, -1 on failure. */
static int create_temp_sqlhosts(const char *content)
{
    int fd = open("/tmp/test_sqlhosts", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = (ssize_t)write(fd, content, strlen(content));
    close(fd);
    (void)n;
    return 0;
}

static void cleanup_temp_sqlhosts(void)
{
    unlink("/tmp/test_sqlhosts");
}

/* ----------------------------------------------------------------
 * Parse tests
 * ---------------------------------------------------------------- */

void test_parse_null_output(void)
{
    sqli_sqlhosts_entry *entries = NULL;
    int count = 0;
    /* NULL entries pointer */
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE,
                          sqli_parse_sqlhosts("/nonexistent", NULL, &count));
    /* NULL count pointer */
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE,
                          sqli_parse_sqlhosts("/nonexistent", &entries, NULL));
}

void test_parse_nonexistent_file(void)
{
    sqli_sqlhosts_entry *entries = NULL;
    int count = 0;
    sqli_status rc = sqli_parse_sqlhosts("/nonexistent/path/sqlhosts", &entries, &count);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_INT(0, count);
    TEST_ASSERT_NULL(entries);
}

void test_parse_env_sqlhosts(void)
{
    sqli_sqlhosts_entry *entries = NULL;
    int count = 0;
    /* No filepath — should try env var or default paths, both likely missing */
    sqli_status rc = sqli_parse_sqlhosts(NULL, &entries, &count);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    (void)rc; /* count may be 0 */
    if (entries != NULL) {
        free(entries);
    }
}

void test_parse_and_find(void)
{
    /* Create a test sqlhosts file */
    const char *content =
        "# This is a comment\n"
        "\n"
        "ol_tli_tcp onsoctcp 127.0.0.1 9088\n"
        "ol_hpg_tcp hpgtcp 127.0.0.1 9089\n"
        "inline_server inline 127.0.0.1 11000 some_option\n";
    create_temp_sqlhosts(content);

    sqli_sqlhosts_entry *entries = NULL;
    int count = 0;
    sqli_status rc = sqli_parse_sqlhosts("/tmp/test_sqlhosts", &entries, &count);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_GREATER_THAN(0, count);
    TEST_ASSERT_NOT_NULL(entries);

    /* Find ol_tli_tcp */
    const sqli_sqlhosts_entry *found = sqli_find_sqlhosts_entry(entries, count, "ol_tli_tcp");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("onsoctcp", found->protocol);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", found->hostname);
    TEST_ASSERT_EQUAL_STRING("9088", found->service);

    /* Find ol_hpg_tcp */
    found = sqli_find_sqlhosts_entry(entries, count, "ol_hpg_tcp");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("hpgtcp", found->protocol);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", found->hostname);
    TEST_ASSERT_EQUAL_STRING("9089", found->service);

    /* Case insensitive lookup */
    found = sqli_find_sqlhosts_entry(entries, count, "OL_TLI_TCP");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("onsoctcp", found->protocol);

    /* Nonexistent server */
    found = sqli_find_sqlhosts_entry(entries, count, "nonexistent");
    TEST_ASSERT_NULL(found);

    free(entries);
    cleanup_temp_sqlhosts();
}

void test_parse_with_empty_lines_and_comments(void)
{
    const char *content =
        "# Comment line\n"
        "\n"
        "   \n"
        "@include other_sqlhosts\n"
        "my_server onsoctcp localhost 9088\n";
    create_temp_sqlhosts(content);

    sqli_sqlhosts_entry *entries = NULL;
    int count = 0;
    sqli_status rc = sqli_parse_sqlhosts("/tmp/test_sqlhosts", &entries, &count);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_NOT_NULL(entries);

    TEST_ASSERT_EQUAL_STRING("my_server", entries[0].server_name);
    TEST_ASSERT_EQUAL_STRING("onsoctcp", entries[0].protocol);
    TEST_ASSERT_EQUAL_STRING("localhost", entries[0].hostname);
    TEST_ASSERT_EQUAL_STRING("9088", entries[0].service);

    free(entries);
    cleanup_temp_sqlhosts();
}

/* ----------------------------------------------------------------
 * Find tests
 * ---------------------------------------------------------------- */

void test_find_null_entries(void)
{
    const sqli_sqlhosts_entry *found = sqli_find_sqlhosts_entry(NULL, 0, "any");
    TEST_ASSERT_NULL(found);
}

void test_find_nonexistent_server(void)
{
    sqli_sqlhosts_entry *entries = calloc(1, sizeof(*entries));
    TEST_ASSERT_NOT_NULL(entries);
    snprintf(entries[0].server_name, sizeof(entries[0].server_name), "server1");

    const sqli_sqlhosts_entry *found = sqli_find_sqlhosts_entry(entries, 1, "nope");
    TEST_ASSERT_NULL(found);

    free(entries);
}

void test_find_null_server_name(void)
{
    sqli_sqlhosts_entry *entries = calloc(1, sizeof(*entries));
    TEST_ASSERT_NOT_NULL(entries);
    snprintf(entries[0].server_name, sizeof(entries[0].server_name), "server1");

    const sqli_sqlhosts_entry *found = sqli_find_sqlhosts_entry(entries, 1, NULL);
    TEST_ASSERT_NULL(found);

    free(entries);
}

void test_find_count_zero(void)
{
    sqli_sqlhosts_entry *entries = calloc(1, sizeof(*entries));
    TEST_ASSERT_NOT_NULL(entries);

    const sqli_sqlhosts_entry *found = sqli_find_sqlhosts_entry(entries, 0, "server1");
    TEST_ASSERT_NULL(found);

    free(entries);
}
