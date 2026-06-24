/*
 * Phase 7 — URI connection tests.
 *
 * Tests sqli_connect_uri parsing, protocol detection, credential
 * extraction, and query parameter handling.
 */

#include "libsqli/sqli.h"
#include "sqli_internal.h"

#include "unity.h"

#include <string.h>
#include <stdlib.h>

/* ----------------------------------------------------------------
 * Null / invalid input
 * ---------------------------------------------------------------- */

void test_uri_null_conn(void)
{
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_connect_uri(NULL, "informix+onsoctcp://h:9088/d?INFORMIXSERVER=s", NULL, NULL));
}

void test_uri_null_uri(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_connect_uri(conn, NULL, NULL, NULL));
    sqli_destroy(conn);
}

void test_uri_empty_uri(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_connect_uri(conn, "", NULL, NULL));
    sqli_destroy(conn);
}

void test_uri_bad_syntax(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_connect_uri(conn, "not a uri at all!!!://", NULL, NULL));
    sqli_destroy(conn);
}

void test_uri_unsupported_scheme(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_connect_uri(conn, "mysql://host:3306/db", NULL, NULL));
    sqli_destroy(conn);
}

/* ----------------------------------------------------------------
 * Missing mandatory INFORMIXSERVER
 * ---------------------------------------------------------------- */

void test_uri_missing_server_param(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_connect_uri(conn, "informix+onsoctcp://host:9088/db", NULL, NULL));
    sqli_destroy(conn);
}

/* ----------------------------------------------------------------
 * Missing host/port for TCP protocols
 * ---------------------------------------------------------------- */

void test_uri_onsoctcp_missing_host(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    /* No host:port in authority */
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, sqli_connect_uri(conn, "informix+onsoctcp:///db?INFORMIXSERVER=s", NULL, NULL));
    sqli_destroy(conn);
}

/* ----------------------------------------------------------------
 * Successful parse — onsoctcp (fails on actual connect)
 * ---------------------------------------------------------------- */

void test_uri_onsoctcp_full(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    /* Should fail on TCP connect (not on parsing) */
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsoctcp://admin:secret@db-host:9088/customer_db?INFORMIXSERVER=ol_informix1",
        NULL, NULL);
    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR, rc);
    TEST_ASSERT_NOT_NULL(sqli_error(conn));
    sqli_destroy(conn);
}

void test_uri_onsoctcp_no_creds(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsoctcp://db-host:9088/db?INFORMIXSERVER=s",
        NULL, NULL);
    /* Should fail on TCP connect, not on parsing */
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

void test_uri_onsoctcp_explicit_creds_override(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    /* URI has user:pass but explicit creds override */
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsoctcp://uri_user:uri_pass@db-host:9088/db?INFORMIXSERVER=s",
        "explicit_user", "explicit_pass");
    /* Should fail on TCP connect, not on parsing */
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

void test_uri_onsoctcp_default_db(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    /* Empty path should default to "informix" */
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsoctcp://db-host:9088/?INFORMIXSERVER=s",
        NULL, NULL);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

void test_uri_onsoctcp_url_encoded_creds(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    /* URL-encoded password with special characters */
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsoctcp://user:p%40ss%3Dword@db-host:9088/db?INFORMIXSERVER=s",
        NULL, NULL);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

/* ----------------------------------------------------------------
 * onsocssl protocol
 * ---------------------------------------------------------------- */

void test_uri_onsocssl_full(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsocssl://admin:secret@db-host:9089/db?INFORMIXSERVER=ol_ssl&SSL_CA_FILE=/etc/ssl/ca.pem",
        NULL, NULL);
    /* Should fail on TCP connect, not on parsing */
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

void test_uri_onsocssl_verify_peer_off(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsocssl://db-host:9089/db?INFORMIXSERVER=s&SSL_VERIFY_PEER=false",
        NULL, NULL);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

/* ----------------------------------------------------------------
 * onipcstr protocol
 * ---------------------------------------------------------------- */

void test_uri_onipcstr_full(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onipcstr:///customer_db?INFORMIXSERVER=ol_informix1_str",
        NULL, NULL);
    /* Should fail on Unix socket connect, not on parsing */
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

void test_uri_onipcstr_custom_socket(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onipcstr:///db?INFORMIXSERVER=s&SOCKET_PATH=/var/lib/informix/socket",
        NULL, NULL);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

/* ----------------------------------------------------------------
 * Query parameter parsing
 * ---------------------------------------------------------------- */

void test_uri_client_locale(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsoctcp://db-host:9088/db?INFORMIXSERVER=s&CLIENT_LOCALE=en_US.utf8&DB_LOCALE=en_US.utf8",
        NULL, NULL);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

void test_uri_multiple_query_params(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsoctcp://db-host:9088/db?INFORMIXSERVER=s&CLIENT_LOCALE=en_US.utf8&DELIMIDENT=Y",
        NULL, NULL);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

/* ----------------------------------------------------------------
 * Edge cases
 * ---------------------------------------------------------------- */

void test_uri_user_only_no_password(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsoctcp://user@db-host:9088/db?INFORMIXSERVER=s",
        NULL, NULL);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

void test_uri_explicit_user_only(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsoctcp://db-host:9088/db?INFORMIXSERVER=s",
        "myuser", NULL);
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

void test_uri_explicit_password_only(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsoctcp://db-host:9088/db?INFORMIXSERVER=s",
        NULL, "mypass");
    TEST_ASSERT_NOT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}

void test_uri_onsoctcp_missing_port(void)
{
    sqli_conn_t *conn = NULL;
    TEST_ASSERT_EQUAL_INT(SQLI_OK, sqli_create(&conn));
    /* Host without port — should fail since port is required */
    sqli_status rc = sqli_connect_uri(conn,
        "informix+onsoctcp://db-host/db?INFORMIXSERVER=s",
        NULL, NULL);
    /* uriparser may or may not parse this; if it does, port check should fail */
    TEST_ASSERT_EQUAL_INT(SQLI_INVALID_STATE, rc);
    sqli_destroy(conn);
}
