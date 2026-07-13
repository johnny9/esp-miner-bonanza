#include "bzm_driver.h"

#include <pthread.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bzm_reactor.h"
#include "bzm_transport.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "global_state.h"
#include "serial.h"

static const char *TAG = "bzm";
static bzm_reactor_t REACTOR;
static bzm_serial_transport_t TRANSPORT;
static pthread_mutex_t REACTOR_LOCK = PTHREAD_MUTEX_INITIALIZER;
static bool INITIALIZED;
static float LAST_TEMPERATURE = -1.0f;
static int64_t LAST_TEMPERATURE_US;

enum {
    BZM_REG_DTS_SRST_PD = 0x2e,
    BZM_REG_TEMPSENSOR_TUNECODE = 0x30,
    BZM_REG_TEMPSENSOR_TEMP_CODE_STATUS = 0x32,
    BZM_REG_SENSOR_CLK_DIV = 0x3d,
    BZM_DEFAULT_THERMAL_SENSOR_DIV = 8,
};

int BZM_set_max_baud(void)
{
    return SERIAL_set_baud(BZM_BAUD_RATE) == ESP_OK ? BZM_BAUD_RATE : 0;
}

uint8_t BZM_init(GlobalState *state)
{
    if (state == NULL ||
        SERIAL_set_baud(BZM_set_max_baud()) != ESP_OK) {
        ESP_LOGE(TAG, "Could not select the 5 Mbaud BZM data link");
        return 0;
    }

    INITIALIZED = false;
    LAST_TEMPERATURE = -1.0f;
    LAST_TEMPERATURE_US = 0;

    uint16_t engine_count = state->DEVICE_CONFIG.family.asic.core_count;
    if (engine_count == 0 || engine_count > BZM_MAX_ACTIVE_WORK) {
        ESP_LOGE(TAG, "Unsupported BZM engine count: %u", engine_count);
        return 0;
    }

    size_t expected_asics = state->DEVICE_CONFIG.family.asic_count;
    if (expected_asics == 0 || expected_asics > BZM_MAX_ASIC_COUNT) {
        ESP_LOGE(TAG, "Unsupported BZM ASIC count: %u",
                 (unsigned)expected_asics);
        return 0;
    }

    TRANSPORT = (bzm_serial_transport_t) {
        .engine_count = engine_count,
        .enhanced_mode = true,
    };
    vTaskDelay(pdMS_TO_TICKS(1000));
    size_t detected_asics = bzm_serial_discover_chain(
        &TRANSPORT, expected_asics);
    if (detected_asics == 0) {
        ESP_LOGE(TAG, "No BZM ASICs responded during chain addressing");
        return 0;
    }
    for (size_t i = 0; i < detected_asics; ++i) {
        ESP_LOGI(TAG, "BZM ASIC %u addressed at 0x%02x",
                 (unsigned)i, TRANSPORT.asic_ids[i]);
    }
    if (detected_asics != expected_asics) {
        ESP_LOGE(TAG, "Detected %u BZM ASICs, expected %u",
                 (unsigned)detected_asics, (unsigned)expected_asics);
    }
    bzm_reactor_config_t config = {
        .engine_count = engine_count,
        .timestamp_count = 16,
        .lead_zeros = 32,
        .nonce_offset = BZM_NONCE_GAP_ENHANCED,
        .enhanced_mode = true,
    };

    pthread_mutex_lock(&REACTOR_LOCK);
    INITIALIZED = bzm_reactor_init(&REACTOR, &state->asic_job_store,
                                   &config, &BZM_SERIAL_TRANSPORT_OPS,
                                   &TRANSPORT);
    pthread_mutex_unlock(&REACTOR_LOCK);
    if (!INITIALIZED) return 0;

    ESP_LOGI(TAG, "BZM reactor ready for %u engines across %u ASICs",
             engine_count, (unsigned)detected_asics);
    return detected_asics;
}

bool BZM_send_work(GlobalState *state, const mining_template_t *template)
{
    (void)state;
    if (!INITIALIZED || template == NULL) return false;

    pthread_mutex_lock(&REACTOR_LOCK);
    if (template->clean_jobs) {
        if (!bzm_reactor_begin_flush(&REACTOR)) {
            pthread_mutex_unlock(&REACTOR_LOCK);
            ESP_LOGE(TAG, "Unable to flush engines for clean work");
            return false;
        }
        bzm_reactor_finish_flush(&REACTOR);
    }

    size_t assigned = 0;
    bzm_assign_status_t status =
        bzm_reactor_dispatch(&REACTOR, template, &assigned);
    if (status == BZM_ASSIGN_FLUSH_REQUIRED) {
        if (!bzm_reactor_begin_flush(&REACTOR)) {
            pthread_mutex_unlock(&REACTOR_LOCK);
            ESP_LOGE(TAG, "Unable to complete BZM flush barrier");
            return false;
        }
        bzm_reactor_finish_flush(&REACTOR);
        status = bzm_reactor_dispatch(&REACTOR, template, &assigned);
    }
    pthread_mutex_unlock(&REACTOR_LOCK);

    if (status != BZM_ASSIGN_OK) {
        ESP_LOGE(TAG, "BZM dispatch failed (%d) after %u engines",
                 status, (unsigned)assigned);
        return false;
    }
    return true;
}

asic_event_t *BZM_process_work(GlobalState *state)
{
    (void)state;
    if (!INITIALIZED) return NULL;

    bzm_raw_result_t raw;
    static asic_event_t event;
    pthread_mutex_lock(&REACTOR_LOCK);
    bool received = bzm_serial_read_result(&raw, 20);
    bool mapped = received && bzm_reactor_map_result(&REACTOR, &raw, &event);
    pthread_mutex_unlock(&REACTOR_LOCK);
    return mapped ? &event : NULL;
}

static bool read_u32(uint8_t asic_id, uint8_t offset, uint32_t *value)
{
    uint8_t bytes[4];
    if (value == NULL ||
        !bzm_serial_read_register(&TRANSPORT, asic_id,
                                  BZM_CONTROL_ENGINE_ID, offset,
                                  bytes, sizeof(bytes))) {
        return false;
    }
    *value = (uint32_t)bytes[0] |
             ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) |
             ((uint32_t)bytes[3] << 24);
    return true;
}

static bool write_u32(uint8_t asic_id, uint8_t offset, uint32_t value)
{
    uint8_t bytes[4] = {
        value & 0xff,
        (value >> 8) & 0xff,
        (value >> 16) & 0xff,
        (value >> 24) & 0xff,
    };
    return bzm_serial_write_register_to(
        &TRANSPORT, asic_id, BZM_CONTROL_ENGINE_ID, offset,
        bytes, sizeof(bytes));
}

static bool read_chip_temperature(uint8_t asic_id, float *temperature)
{
    uint32_t original_clk_div;
    uint32_t original_dts_config;
    uint32_t original_temp_config;
    uint32_t temp_status = 0;
    bool success = read_u32(asic_id, BZM_REG_SENSOR_CLK_DIV,
                            &original_clk_div) &&
                   read_u32(asic_id, BZM_REG_DTS_SRST_PD,
                            &original_dts_config) &&
                   read_u32(asic_id, BZM_REG_TEMPSENSOR_TUNECODE,
                            &original_temp_config);
    if (!success) return false;

    uint32_t configured_clk_div =
        (original_clk_div & ~(0x1fu << 5)) |
        (BZM_DEFAULT_THERMAL_SENSOR_DIV << 5);
    uint32_t configured_dts_config =
        (original_dts_config | (1u << 8)) & ~1u;
    uint32_t configured_temp_config = original_temp_config | 1u;

    success = write_u32(asic_id, BZM_REG_SENSOR_CLK_DIV,
                        configured_clk_div) &&
              write_u32(asic_id, BZM_REG_DTS_SRST_PD,
                        configured_dts_config) &&
              write_u32(asic_id, BZM_REG_TEMPSENSOR_TUNECODE,
                        configured_temp_config);
    if (success) {
        vTaskDelay(pdMS_TO_TICKS(10));
        success = read_u32(asic_id,
                           BZM_REG_TEMPSENSOR_TEMP_CODE_STATUS,
                           &temp_status);
    }

    bool restored = write_u32(asic_id, BZM_REG_TEMPSENSOR_TUNECODE,
                              original_temp_config);
    restored = write_u32(asic_id, BZM_REG_DTS_SRST_PD,
                         original_dts_config) && restored;
    restored = write_u32(asic_id, BZM_REG_SENSOR_CLK_DIV,
                         original_clk_div) && restored;
    if (!success || !restored || (temp_status & (1u << 12)) != 0) {
        return false;
    }
    *temperature = bzm_temperature_from_code(temp_status & 0x0fff);
    return true;
}

float BZM_read_temperature(GlobalState *state)
{
    (void)state;
    if (!INITIALIZED || TRANSPORT.asic_count == 0) return -1.0f;

    int64_t now = esp_timer_get_time();
    if (LAST_TEMPERATURE_US != 0 &&
        now - LAST_TEMPERATURE_US < 2000000) {
        return LAST_TEMPERATURE;
    }

    pthread_mutex_lock(&REACTOR_LOCK);
    float total = 0.0f;
    size_t valid = 0;
    for (size_t i = 0; i < TRANSPORT.asic_count; ++i) {
        float temperature;
        if (read_chip_temperature(TRANSPORT.asic_ids[i], &temperature)) {
            total += temperature;
            valid++;
        }
    }
    pthread_mutex_unlock(&REACTOR_LOCK);

    if (valid != 0) {
        LAST_TEMPERATURE = total / valid;
    }
    LAST_TEMPERATURE_US = now;
    return LAST_TEMPERATURE;
}
