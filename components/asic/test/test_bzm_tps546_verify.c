#include <string.h>

#include "bzm_tps546_verify.h"
#include "unity.h"

TEST_CASE("TPS546 verifier accepts exact encoded byte word and block fields",
          "[asic][bzm][power][tps546-verify]")
{
    const uint8_t expected_byte[] = {0xbd};
    const uint8_t expected_word[] = {0x10, 0xe0};
    const uint8_t expected_block[] = {0x13, 0x11, 0x8c, 0x1d, 0x06};
    const bzm_tps546_verify_field_t fields[] = {
        {"VOUT_OV_FAULT_RESPONSE", expected_byte, sizeof(expected_byte),
         expected_byte, sizeof(expected_byte)},
        {"VOUT_TRANSITION_RATE", expected_word, sizeof(expected_word),
         expected_word, sizeof(expected_word)},
        {"COMPENSATION_CONFIG", expected_block, sizeof(expected_block),
         expected_block, sizeof(expected_block)},
    };
    char detail[96];

    TEST_ASSERT_EQUAL(ESP_OK, bzm_tps546_verify_fields(
        fields, sizeof(fields) / sizeof(fields[0]), detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING(BZM_TPS546_VERIFY_GOOD_DETAIL, detail);
}

TEST_CASE("TPS546 verifier reports the first named byte mismatch",
          "[asic][bzm][power][tps546-verify]")
{
    const uint8_t expected_a[] = {0xbd};
    const uint8_t observed_a[] = {0xbe};
    const uint8_t expected_b[] = {0xc0};
    const uint8_t observed_b[] = {0xff};
    const bzm_tps546_verify_field_t fields[] = {
        {"VOUT_OV_FAULT_RESPONSE", expected_a, 1, observed_a, 1},
        {"IOUT_OC_FAULT_RESPONSE", expected_b, 1, observed_b, 1},
    };
    char detail[96];

    TEST_ASSERT_EQUAL(ESP_FAIL, bzm_tps546_verify_fields(
        fields, 2, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING(
        "VOUT_OV_FAULT_RESPONSE expected=0xBD observed=0xBE", detail);
}

TEST_CASE("TPS546 verifier names exact word and block mismatches",
          "[asic][bzm][power][tps546-verify]")
{
    const uint8_t expected_word[] = {0x10, 0xe0};
    const uint8_t observed_word[] = {0x11, 0xe0};
    const uint8_t expected_block[] = {0x03, 0x03, 0x03, 0x03, 0x03, 0x00};
    const uint8_t observed_block[] = {0x03, 0x03, 0x00, 0x03, 0x03, 0x00};
    char detail[96];
    bzm_tps546_verify_field_t field = {
        "VOUT_TRANSITION_RATE", expected_word, 2, observed_word, 2,
    };

    TEST_ASSERT_EQUAL(ESP_FAIL, bzm_tps546_verify_fields(
        &field, 1, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING(
        "VOUT_TRANSITION_RATE expected=0xE010 observed=0xE011", detail);

    field = (bzm_tps546_verify_field_t){
        "TELEMETRY_CONFIG", expected_block, sizeof(expected_block),
        observed_block, sizeof(observed_block),
    };
    TEST_ASSERT_EQUAL(ESP_FAIL, bzm_tps546_verify_fields(
        &field, 1, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING(
        "TELEMETRY_CONFIG[2] expected=0x03 observed=0x00", detail);
}

TEST_CASE("TPS546 verifier rejects malformed field descriptions and lengths",
          "[asic][bzm][power][tps546-verify]")
{
    const uint8_t expected[] = {0x13, 0x11, 0x8c, 0x1d, 0x06};
    const uint8_t observed[] = {0x13, 0x11, 0x8c, 0x1d};
    char detail[96];
    bzm_tps546_verify_field_t field = {
        "COMPENSATION_CONFIG", expected, sizeof(expected),
        observed, sizeof(observed),
    };

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, bzm_tps546_verify_fields(
        &field, 1, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING(
        "COMPENSATION_CONFIG length expected=5 observed=4", detail);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, bzm_tps546_verify_fields(
        NULL, 1, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("INVALID_ARGUMENT", detail);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, bzm_tps546_verify_fields(
        &field, 0, detail, sizeof(detail)));

    field.name = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, bzm_tps546_verify_fields(
        &field, 1, detail, sizeof(detail)));
    field.name = "COMPENSATION_CONFIG";
    field.expected = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, bzm_tps546_verify_fields(
        &field, 1, detail, sizeof(detail)));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, bzm_tps546_verify_fields(
        &field, 1, NULL, sizeof(detail)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, bzm_tps546_verify_fields(
        &field, 1, detail, 0));
}

TEST_CASE("TPS546 verifier always terminates a bounded detail buffer",
          "[asic][bzm][power][tps546-verify]")
{
    const uint8_t expected[] = {0xbd};
    const uint8_t observed[] = {0xbe};
    const bzm_tps546_verify_field_t field = {
        "VOUT_OV_FAULT_RESPONSE", expected, 1, observed, 1,
    };
    char detail[8];
    memset(detail, 0xa5, sizeof(detail));

    TEST_ASSERT_EQUAL(ESP_FAIL, bzm_tps546_verify_fields(
        &field, 1, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_CHAR('\0', detail[sizeof(detail) - 1]);
    TEST_ASSERT_EQUAL_STRING_LEN("VOUT_OV", detail, sizeof(detail) - 1);
}
