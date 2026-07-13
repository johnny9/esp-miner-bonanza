#ifndef BZM_BRIDGE_H
#define BZM_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BZM_BRIDGE_CONTROL_BAUD_RATE 115200
#define BZM_BRIDGE_CONTROL_TX_GPIO 43
#define BZM_BRIDGE_CONTROL_RX_GPIO 44

#define BZM_BRIDGE_PAGE_GPIO 0x06
#define BZM_BRIDGE_PAGE_FAN 0x09

#define BZM_BRIDGE_GPIO_5V_ENABLE 0x01
#define BZM_BRIDGE_GPIO_ASIC_RESET 0x02
#define BZM_BRIDGE_GPIO_ASIC_TRIP 0x03
#define BZM_BRIDGE_FAN_SET_SPEED 0x10
#define BZM_BRIDGE_FAN_GET_TACH 0x20

#define BZM_BRIDGE_MAX_REQUEST_SIZE 64
#define BZM_BRIDGE_MAX_RESPONSE_SIZE 260

size_t bzm_bridge_encode_request(uint8_t id, uint8_t page, uint8_t command,
                                 const uint8_t *payload,
                                 size_t payload_length, uint8_t *encoded,
                                 size_t encoded_capacity);
esp_err_t bzm_bridge_decode_response(uint8_t expected_id,
                                     const uint8_t *frame,
                                     size_t frame_length,
                                     const uint8_t **payload,
                                     size_t *payload_length);

esp_err_t BZM_bridge_init(void);
bool BZM_bridge_is_initialized(void);
esp_err_t BZM_bridge_set_5v_enabled(bool enabled);
esp_err_t BZM_bridge_set_asic_reset(bool high);
esp_err_t BZM_bridge_pulse_asic_reset(void);
esp_err_t BZM_bridge_get_asic_trip(bool *asserted);
esp_err_t BZM_bridge_set_fan_percent(float percent);
esp_err_t BZM_bridge_get_fan_rpm(uint16_t *rpm);

#endif // BZM_BRIDGE_H
