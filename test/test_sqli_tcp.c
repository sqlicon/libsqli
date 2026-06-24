/*
 * Phase 2 unit tests for wire format helpers.
 *
 * Tests use only public API — no internal struct access.
 */

#include "unity.h"
#include "libsqli/sqli.h"
#include "sqli_internal.h"

/* ----------------------------------------------------------------
 * Test: SL header encode/decode
 * ---------------------------------------------------------------- */

void test_sl_header_encode_conreq(void)
{
    uint8_t buf[16];
    /* Build a CONREQ: pdu=100, type=1, attr=60, opts=0 */
    buf[0] = 0; buf[1] = 100;
    buf[2] = 1; buf[3] = 60;
    buf[4] = 0; buf[5] = 0;

    uint16_t pdu;
    uint8_t type, attr;
    uint16_t opts;

    sqli_status rc = sqli_wire_decode_sl_header(buf, sizeof(buf),
                                                 &pdu, &type, &attr, &opts);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(100, pdu);
    TEST_ASSERT_EQUAL_UINT8(1, type);
    TEST_ASSERT_EQUAL_UINT8(60, attr);
    TEST_ASSERT_EQUAL_UINT16(0, opts);
}

void test_sl_header_encode_conacc(void)
{
    uint8_t buf[16];
    buf[0] = 0; buf[1] = 200;
    buf[2] = 2; buf[3] = 60;
    buf[4] = 0; buf[5] = 0;

    uint16_t pdu;
    uint8_t type, attr;
    uint16_t opts;

    sqli_status rc = sqli_wire_decode_sl_header(buf, sizeof(buf),
                                                 &pdu, &type, &attr, &opts);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(200, pdu);
    TEST_ASSERT_EQUAL_UINT8(2, type);
    TEST_ASSERT_EQUAL_UINT8(60, attr);
}

void test_sl_header_encode_redirect(void)
{
    uint8_t buf[16];
    buf[0] = 0; buf[1] = 50;
    buf[2] = 13; buf[3] = 60;
    buf[4] = 0; buf[5] = 0;

    uint16_t pdu;
    uint8_t type;
    uint8_t attr;
    uint16_t opts;

    sqli_status rc = sqli_wire_decode_sl_header(buf, sizeof(buf),
                                                 &pdu, &type, &attr, &opts);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(50, pdu);
    TEST_ASSERT_EQUAL_UINT8(13, type);
}

void test_sl_header_decode_too_small(void)
{
    uint8_t buf[4] = {0};
    uint16_t pdu;
    uint8_t type, attr;
    uint16_t opts;

    sqli_status rc = sqli_wire_decode_sl_header(buf, 4, &pdu, &type, &attr, &opts);
    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR, rc);
}

void test_sl_header_decode_null(void)
{
    uint8_t buf[6];
    /* Valid header data */
    buf[0] = 0; buf[1] = 100;
    buf[2] = 1; buf[3] = 60;
    buf[4] = 0; buf[5] = 0;

    /* NULL output pointers — should not crash */
    sqli_status rc = sqli_wire_decode_sl_header(buf, 6,
                                                 NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(SQLI_OK, rc);
}

void test_sl_header_encode_null_buf(void)
{
    uint16_t pdu;
    uint8_t type, attr;
    uint16_t opts;

    sqli_status rc = sqli_wire_decode_sl_header(NULL, 6,
                                                 &pdu, &type, &attr, &opts);
    TEST_ASSERT_EQUAL_INT(SQLI_IO_ERROR, rc);
}
