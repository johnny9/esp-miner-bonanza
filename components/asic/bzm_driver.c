#include "bzm_driver.h"

#include <pthread.h>

#include "bzm_reactor.h"
#include "bzm_transport.h"
#include "esp_log.h"
#include "global_state.h"
#include "serial.h"

static const char *TAG = "bzm";
static bzm_reactor_t REACTOR;
static bzm_serial_transport_t TRANSPORT;
static pthread_mutex_t REACTOR_LOCK = PTHREAD_MUTEX_INITIALIZER;
static bool INITIALIZED;

int BZM_set_max_baud(void)
{
    return SERIAL_set_baud(BZM_BAUD_RATE) == ESP_OK ? BZM_BAUD_RATE : 0;
}

uint8_t BZM_init(GlobalState *state)
{
    if (state == NULL || BZM_set_max_baud() == 0) return 0;

    uint16_t engine_count = state->DEVICE_CONFIG.family.asic.core_count;
    if (engine_count == 0 || engine_count > BZM_MAX_ACTIVE_WORK) {
        ESP_LOGE(TAG, "Unsupported BZM engine count: %u", engine_count);
        return 0;
    }

    TRANSPORT = (bzm_serial_transport_t) {
        .asic_id = BZM_BROADCAST_ASIC,
        .engine_count = engine_count,
        .enhanced_mode = true,
    };
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

    ESP_LOGI(TAG, "BZM reactor ready for %u engines", engine_count);
    return state->DEVICE_CONFIG.family.asic_count > 0
        ? state->DEVICE_CONFIG.family.asic_count : 1;
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
