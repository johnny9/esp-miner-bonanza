#include <string.h>

#include "bzm_bridge.h"
#include "unity.h"

TEST_CASE("Bonanza bridge encodes the RP2040 control packet format",
          "[asic][bzm][bridge]")
{
    uint8_t frame[16];
    uint8_t level = 1;
    size_t length = bzm_bridge_encode_request(
        0x2a, BZM_BRIDGE_PAGE_GPIO, BZM_BRIDGE_GPIO_5V_ENABLE,
        &level, 1, frame, sizeof(frame));
    const uint8_t expected[] = {
        0x07, 0x00, 0x2a, 0x00, 0x06, 0x01, 0x01,
    };
    TEST_ASSERT_EQUAL(sizeof(expected), length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, sizeof(expected));

    TEST_ASSERT_EQUAL(0, bzm_bridge_encode_request(
        0, 0, 0, NULL, 1, frame, sizeof(frame)));
    TEST_ASSERT_EQUAL(0, bzm_bridge_encode_request(
        0, 0, 0, &level, 1, frame, 6));
}

TEST_CASE("Bonanza bridge validates response length id and error frames",
          "[asic][bzm][bridge]")
{
    const uint8_t *payload;
    size_t payload_length;
    uint8_t response[] = {0x05, 0x00, 0x2a, 0x34, 0x12};
    TEST_ASSERT_EQUAL(ESP_OK, bzm_bridge_decode_response(
        0x2a, response, sizeof(response), &payload, &payload_length));
    TEST_ASSERT_EQUAL_UINT32(2, payload_length);
    TEST_ASSERT_EQUAL_HEX8(0x34, payload[0]);
    TEST_ASSERT_EQUAL_HEX8(0x12, payload[1]);

    response[2] = 0x2b;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, bzm_bridge_decode_response(
        0x2a, response, sizeof(response), &payload, &payload_length));
    response[2] = 0x2a;
    response[0] = 4;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, bzm_bridge_decode_response(
        0x2a, response, sizeof(response), &payload, &payload_length));

    uint8_t timeout[] = {0x04, 0x00, 0x2a, 0x10};
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, bzm_bridge_decode_response(
        0x2a, timeout, sizeof(timeout), &payload, &payload_length));
    uint8_t invalid[] = {0x04, 0x00, 0x2a, 0x11};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, bzm_bridge_decode_response(
        0x2a, invalid, sizeof(invalid), &payload, &payload_length));
}

TEST_CASE("Bonanza bridge command constants encode every owned peripheral",
          "[asic][bzm][bridge]")
{
    typedef struct {
        uint8_t page;
        uint8_t command;
    } command_t;
    const command_t commands[] = {
        {BZM_BRIDGE_PAGE_GPIO, BZM_BRIDGE_GPIO_5V_ENABLE},
        {BZM_BRIDGE_PAGE_GPIO, BZM_BRIDGE_GPIO_ASIC_RESET},
        {BZM_BRIDGE_PAGE_GPIO, BZM_BRIDGE_GPIO_ASIC_TRIP},
        {BZM_BRIDGE_PAGE_FAN, BZM_BRIDGE_FAN_SET_SPEED},
        {BZM_BRIDGE_PAGE_FAN, BZM_BRIDGE_FAN_GET_TACH},
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        uint8_t frame[8];
        TEST_ASSERT_EQUAL_UINT32(6, bzm_bridge_encode_request(
            i, commands[i].page, commands[i].command, NULL, 0,
            frame, sizeof(frame)));
        TEST_ASSERT_EQUAL_HEX8(commands[i].page, frame[4]);
        TEST_ASSERT_EQUAL_HEX8(commands[i].command, frame[5]);
    }
}

TEST_CASE("Bonanza bridge operations fail closed while unavailable",
          "[asic][bzm][bridge][unavailable]")
{
    bool tripped = false;
    uint16_t rpm = 0;
    TEST_ASSERT_FALSE(BZM_bridge_is_initialized());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      BZM_bridge_set_5v_enabled(true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      BZM_bridge_set_asic_reset(true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      BZM_bridge_pulse_asic_reset());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      BZM_bridge_get_asic_trip(&tripped));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      BZM_bridge_set_fan_percent(1.0f));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      BZM_bridge_get_fan_rpm(&rpm));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      BZM_bridge_set_fan_percent(-0.1f));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      BZM_bridge_set_fan_percent(1.1f));
}
