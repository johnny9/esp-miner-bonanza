#include "bzm_bridge.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BZM_BRIDGE_UART UART_NUM_2
#define BZM_BRIDGE_UART_BUFFER_SIZE 512

static const char *TAG = "bzm_bridge";
static pthread_mutex_t BRIDGE_LOCK = PTHREAD_MUTEX_INITIALIZER;
static bool INITIALIZED;
static uint8_t NEXT_COMMAND_ID;

size_t bzm_bridge_encode_request(uint8_t id, uint8_t page, uint8_t command,
                                 const uint8_t *payload,
                                 size_t payload_length, uint8_t *encoded,
                                 size_t encoded_capacity)
{
    size_t frame_length = payload_length + 6;
    if (encoded == NULL || frame_length > UINT16_MAX ||
        frame_length > encoded_capacity ||
        (payload_length != 0 && payload == NULL)) {
        return 0;
    }

    encoded[0] = frame_length & 0xff;
    encoded[1] = (frame_length >> 8) & 0xff;
    encoded[2] = id;
    encoded[3] = 0;
    encoded[4] = page;
    encoded[5] = command;
    if (payload_length != 0) {
        memcpy(encoded + 6, payload, payload_length);
    }
    return frame_length;
}

esp_err_t bzm_bridge_decode_response(uint8_t expected_id,
                                     const uint8_t *frame,
                                     size_t frame_length,
                                     const uint8_t **payload,
                                     size_t *payload_length)
{
    if (frame == NULL || payload == NULL || payload_length == NULL ||
        frame_length < 3) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t declared_length = (size_t)frame[0] | ((size_t)frame[1] << 8);
    if (declared_length != frame_length || frame[2] != expected_id) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *payload = frame + 3;
    *payload_length = frame_length - 3;
    if (*payload_length == 1 &&
        ((*payload)[0] == 0x10 || (*payload)[0] == 0x11)) {
        return (*payload)[0] == 0x10 ? ESP_ERR_TIMEOUT
                                     : ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t read_exact(uint8_t *buffer, size_t length,
                            uint32_t timeout_ms)
{
    size_t offset = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (offset < length) {
        int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) return ESP_ERR_TIMEOUT;
        TickType_t ticks = pdMS_TO_TICKS((remaining_us + 999) / 1000);
        if (ticks == 0) ticks = 1;
        int received = uart_read_bytes(BZM_BRIDGE_UART, buffer + offset,
                                       length - offset, ticks);
        if (received < 0) return ESP_FAIL;
        offset += received;
    }
    return ESP_OK;
}

static esp_err_t transact(uint8_t page, uint8_t command,
                          const uint8_t *request_payload,
                          size_t request_payload_length,
                          uint8_t *response_payload,
                          size_t response_capacity,
                          size_t *response_length, uint32_t timeout_ms)
{
    if (!INITIALIZED) return ESP_ERR_INVALID_STATE;

    uint8_t request[BZM_BRIDGE_MAX_REQUEST_SIZE];
    uint8_t response[BZM_BRIDGE_MAX_RESPONSE_SIZE];
    uint8_t command_id;
    size_t request_length;
    esp_err_t err = ESP_OK;

    pthread_mutex_lock(&BRIDGE_LOCK);
    command_id = NEXT_COMMAND_ID++;
    request_length = bzm_bridge_encode_request(
        command_id, page, command, request_payload, request_payload_length,
        request, sizeof(request));
    if (request_length == 0) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    uart_flush_input(BZM_BRIDGE_UART);
    if (uart_write_bytes(BZM_BRIDGE_UART, request, request_length) !=
        (int)request_length) {
        err = ESP_FAIL;
        goto done;
    }
    err = uart_wait_tx_done(BZM_BRIDGE_UART, pdMS_TO_TICKS(100));
    if (err != ESP_OK) goto done;

    err = read_exact(response, 3, timeout_ms);
    if (err != ESP_OK) goto done;
    size_t frame_length = (size_t)response[0] | ((size_t)response[1] << 8);
    if (frame_length < 3 || frame_length > sizeof(response)) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    err = read_exact(response + 3, frame_length - 3, timeout_ms);
    if (err != ESP_OK) goto done;

    const uint8_t *decoded_payload;
    size_t decoded_length;
    err = bzm_bridge_decode_response(command_id, response, frame_length,
                                     &decoded_payload, &decoded_length);
    if (err != ESP_OK) goto done;
    if (decoded_length > response_capacity ||
        (decoded_length != 0 && response_payload == NULL)) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    if (decoded_length != 0) {
        memcpy(response_payload, decoded_payload, decoded_length);
    }
    if (response_length != NULL) *response_length = decoded_length;

done:
    pthread_mutex_unlock(&BRIDGE_LOCK);
    return err;
}

static esp_err_t set_gpio(uint8_t command, bool high)
{
    uint8_t request = high ? 1 : 0;
    uint8_t response;
    size_t response_length = 0;
    ESP_RETURN_ON_ERROR(transact(BZM_BRIDGE_PAGE_GPIO, command, &request, 1,
                                 &response, 1, &response_length, 200),
                        TAG, "bridge GPIO command failed");
    return response_length == 1 && response == request
        ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t get_gpio(uint8_t command, bool *high)
{
    if (high == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t response;
    size_t response_length = 0;
    ESP_RETURN_ON_ERROR(transact(BZM_BRIDGE_PAGE_GPIO, command, NULL, 0,
                                 &response, 1, &response_length, 200),
                        TAG, "bridge GPIO read failed");
    if (response_length != 1 || response > 1) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *high = response != 0;
    return ESP_OK;
}

esp_err_t BZM_bridge_init(void)
{
    if (INITIALIZED) return ESP_OK;

    uart_config_t config = {
        .baud_rate = BZM_BRIDGE_CONTROL_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(BZM_BRIDGE_UART, &config), TAG,
                        "control UART config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(BZM_BRIDGE_UART,
                                     BZM_BRIDGE_CONTROL_TX_GPIO,
                                     BZM_BRIDGE_CONTROL_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "control UART pin config failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(
                            BZM_BRIDGE_UART, BZM_BRIDGE_UART_BUFFER_SIZE,
                            BZM_BRIDGE_UART_BUFFER_SIZE, 0, NULL, 0),
                        TAG, "control UART install failed");

    INITIALIZED = true;
    esp_err_t err = BZM_bridge_set_5v_enabled(false);
    if (err == ESP_OK) err = BZM_bridge_set_asic_reset(false);
    if (err != ESP_OK) {
        INITIALIZED = false;
        uart_driver_delete(BZM_BRIDGE_UART);
        return err;
    }
    ESP_LOGI(TAG, "Bonanza bridge ready on TX%d/RX%d",
             BZM_BRIDGE_CONTROL_TX_GPIO, BZM_BRIDGE_CONTROL_RX_GPIO);
    return ESP_OK;
}

bool BZM_bridge_is_initialized(void)
{
    return INITIALIZED;
}

esp_err_t BZM_bridge_set_5v_enabled(bool enabled)
{
    return set_gpio(BZM_BRIDGE_GPIO_5V_ENABLE, enabled);
}

esp_err_t BZM_bridge_set_asic_reset(bool high)
{
    return set_gpio(BZM_BRIDGE_GPIO_ASIC_RESET, high);
}

esp_err_t BZM_bridge_pulse_asic_reset(void)
{
    ESP_RETURN_ON_ERROR(BZM_bridge_set_asic_reset(false), TAG,
                        "ASIC reset low failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(BZM_bridge_set_asic_reset(true), TAG,
                        "ASIC reset high failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t BZM_bridge_get_asic_trip(bool *asserted)
{
    return get_gpio(BZM_BRIDGE_GPIO_ASIC_TRIP, asserted);
}

esp_err_t BZM_bridge_set_fan_percent(float percent)
{
    if (!isfinite(percent) || percent < 0.0f || percent > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t request = (uint8_t)lroundf(percent * 100.0f);
    uint8_t response;
    size_t response_length = 0;
    ESP_RETURN_ON_ERROR(transact(BZM_BRIDGE_PAGE_FAN,
                                 BZM_BRIDGE_FAN_SET_SPEED, &request, 1,
                                 &response, 1, &response_length, 200),
                        TAG, "fan command failed");
    return response_length == 1 && response == 0
        ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t BZM_bridge_get_fan_rpm(uint16_t *rpm)
{
    if (rpm == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t response[2];
    size_t response_length = 0;
    ESP_RETURN_ON_ERROR(transact(BZM_BRIDGE_PAGE_FAN,
                                 BZM_BRIDGE_FAN_GET_TACH, NULL, 0,
                                 response, sizeof(response),
                                 &response_length, 1000),
                        TAG, "tach command failed");
    if (response_length != 2) return ESP_ERR_INVALID_RESPONSE;
    *rpm = (uint16_t)response[0] | ((uint16_t)response[1] << 8);
    return ESP_OK;
}
