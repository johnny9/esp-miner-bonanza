#include "asic.h"

#include <esp_log.h>

#include "asic_driver.h"
#include "device_config.h"
#include "frequency_transition_bmXX.h"
#include <string.h>

static const char *TAG = "asic";

static const asic_driver_t *active_driver(const GlobalState *state)
{
    if (state == NULL) return NULL;
    return asic_driver_for_id(state->DEVICE_CONFIG.family.asic.id);
}

asic_capabilities_t ASIC_get_capabilities(const GlobalState *state)
{
    const asic_driver_t *driver = active_driver(state);
    return ASIC_capabilities_for_chip_id(driver ? driver->chip_id : 0);
}

uint8_t ASIC_init(GlobalState *state)
{
    const asic_driver_t *driver = active_driver(state);
    if (driver == NULL || driver->ops.init == NULL) {
        ESP_LOGE(TAG, "Unknown ASIC id %d",
                 state ? state->DEVICE_CONFIG.family.asic.id : -1);
        return 0;
    }
    ESP_LOGI(TAG, "Initializing %dx %s",
             state->DEVICE_CONFIG.family.asic_count, driver->name);
    return driver->ops.init(state);
}

asic_event_t *ASIC_process_work(GlobalState *state)
{
    const asic_driver_t *driver = active_driver(state);
    if (driver == NULL || driver->ops.process_work == NULL) return NULL;
    return driver->ops.process_work(state);
}

int ASIC_set_max_baud(GlobalState *state)
{
    const asic_driver_t *driver = active_driver(state);
    return driver != NULL && driver->ops.set_max_baud != NULL
        ? driver->ops.set_max_baud() : 0;
}

bool ASIC_send_work(GlobalState *state, const mining_template_t *template)
{
    const asic_driver_t *driver = active_driver(state);
    if (driver == NULL || driver->ops.send_work == NULL) {
        ESP_LOGE(TAG, "No work operation for ASIC id %d",
                 state ? state->DEVICE_CONFIG.family.asic.id : -1);
        return false;
    }
    return driver->ops.send_work(state, template);
}

bool ASIC_clear_work(GlobalState *state)
{
    const asic_driver_t *driver = active_driver(state);
    if (driver == NULL) return false;
    return driver->ops.clear_work == NULL || driver->ops.clear_work(state);
}

void ASIC_set_version_mask(GlobalState *state, uint32_t mask)
{
    const asic_driver_t *driver = active_driver(state);
    if (driver != NULL && driver->ops.set_version_mask != NULL) {
        driver->ops.set_version_mask(mask);
    }
}

void ASIC_set_frequency(GlobalState *state)
{
    const asic_driver_t *driver = active_driver(state);
    if (driver != NULL && driver->ops.set_hash_frequency != NULL) {
        do_frequency_transition(state, driver->ops.set_hash_frequency);
    }
}

void ASIC_set_nonce_space(GlobalState *state)
{
    const asic_driver_t *driver = active_driver(state);
    if (driver == NULL || driver->ops.set_nonce_space == NULL) return;
    driver->ops.set_nonce_space(
        1.0, state->POWER_MANAGEMENT_MODULE.actual_frequency,
        state->DEVICE_CONFIG.family.asic_count,
        state->DEVICE_CONFIG.family.asic.core_count);
}

double ASIC_get_asic_job_frequency_ms(GlobalState *state)
{
    const asic_driver_t *driver = active_driver(state);
    if (driver == NULL) return 500;
    if (driver->ops.job_frequency_ms != NULL)
        return driver->ops.job_frequency_ms(state);

    int asic_count = state->DEVICE_CONFIG.family.asic_count;
    int default_timeout =
        state->DEVICE_CONFIG.family.asic.default_asic_timeout /
        _next_power_of_two(asic_count);
    asic_capabilities_t capabilities = ASIC_get_capabilities(state);
    if (capabilities.work_refresh_policy ==
        ASIC_WORK_REFRESH_DRIVER_MANAGED) {
        return default_timeout;
    }
    if (capabilities.version_rolling == ASIC_VERSION_ROLLING_MIDSTATE) {
        return calculate_bm_timeout_ms(
            state->POWER_MANAGEMENT_MODULE.frequency_value, asic_count,
            state->DEVICE_CONFIG.family.asic.small_core_count,
            state->DEVICE_CONFIG.family.asic.core_count,
            capabilities.max_version_variants, 1.0, default_timeout);
    }
    return default_timeout;
}

void ASIC_read_registers(GlobalState *state)
{
    const asic_driver_t *driver = active_driver(state);
    if (driver != NULL && driver->ops.read_registers != NULL) {
        driver->ops.read_registers();
    }
}

bool ASIC_get_hashrate_counters(GlobalState *state,
                                uint32_t *difficulty_one_counters,
                                size_t counter_count)
{
    const asic_driver_t *driver = active_driver(state);
    return driver != NULL && driver->ops.hashrate_counter_snapshot != NULL &&
           driver->ops.hashrate_counter_snapshot(
               state, difficulty_one_counters, counter_count);
}

float ASIC_get_temperature(GlobalState *state)
{
    const asic_driver_t *driver = active_driver(state);
    if (driver == NULL || driver->ops.read_temperature == NULL) return -1.0f;
    return driver->ops.read_temperature(state);
}

void ASIC_record_local_result(GlobalState *state, uint8_t asic_index,
                              bool valid,
                              double nonce_difficulty)
{
    const asic_driver_t *driver = active_driver(state);
    if (driver != NULL && driver->ops.record_local_result != NULL) {
        driver->ops.record_local_result(state, asic_index, valid,
                                        nonce_difficulty);
    }
}

bool ASIC_get_health(GlobalState *state, asic_driver_health_t *health)
{
    if (health == NULL) return false;
    memset(health, 0, sizeof(*health));
    const asic_driver_t *driver = active_driver(state);
    return driver != NULL && driver->ops.health_snapshot != NULL &&
           driver->ops.health_snapshot(state, health) && health->available;
}
