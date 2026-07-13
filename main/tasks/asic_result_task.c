#include <errno.h>
#include <inttypes.h>
#include <lwip/tcpip.h>
#include <stdlib.h>
#include <string.h>

#include "asic.h"
#include "asic_result_handler.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "global_state.h"
#include "hashrate_monitor_task.h"
#include "scoreboard.h"
#include "self_test.h"
#include "stratum_api.h"
#include "stratum_v2_task.h"
#include "sv2_protocol.h"
#include "system.h"

static const char *TAG = "asic_result";

typedef struct {
    GlobalState *state;
    esp_transport_handle_t sv1_transport;
    int sv1_uid;
} result_callback_context;

static void monitor_register(GlobalState *state,
                             const asic_register_result_t *result)
{
    hashrate_monitor_register_read(state, result->register_type,
                                   result->asic_index,
                                   result->value, result->timestamp_us);
}

static void record_self_test(void *context, double nonce_diff)
{
    result_callback_context *callback_context = context;
    self_test_record_nonce(callback_context->state, nonce_diff);
}

static bool sv1_transport_ready(void *context,
                                const asic_share_submission_t *share)
{
    result_callback_context *callback_context = context;
    GlobalState *state = callback_context->state;
    taskENTER_CRITICAL(&state->stratum_mux);
    callback_context->sv1_transport = state->transport;
    callback_context->sv1_uid = state->send_uid++;
    taskEXIT_CRITICAL(&state->stratum_mux);
    bool ready = callback_context->sv1_transport != NULL;
    if (!ready) {
        ESP_LOGW(TAG,
                 "No stratum connection, dropping share (work 0x%" PRIX64 ")",
                 share->result->work_handle);
    }
    return ready;
}

static int submit_sv1(void *context, const asic_share_submission_t *share)
{
    result_callback_context *callback_context = context;
    GlobalState *state = callback_context->state;
    esp_transport_handle_t transport = callback_context->sv1_transport;
    int uid = callback_context->sv1_uid;

    if (transport == NULL) return -1;
    uint64_t sent_time_us = 0;
    int ret = STRATUM_V1_submit_share(transport, uid, share->username,
                                      share->job_id, share->extranonce2,
                                      share->ntime, share->nonce,
                                      share->version_bits, &sent_time_us);
    if (ret < 0) {
        ESP_LOGW(TAG, "Unable to write share to socket (ret: %d, errno %d: %s)",
                 ret, errno, strerror(errno));
    }

    state->SYSTEM_MODULE.process_time =
        (sent_time_us - share->result->timestamp_us) / 1000.0f;
    ESP_LOGI(TAG, "Processing time: %0.1f ms",
             state->SYSTEM_MODULE.process_time);
    return ret;
}

static int submit_sv2_standard(void *context,
                               const asic_share_submission_t *share)
{
    GlobalState *state = ((result_callback_context *)context)->state;
    int ret = stratum_v2_submit_share(state, share->numeric_job_id,
                                      share->nonce, share->ntime,
                                      share->final_version);
    if (ret < 0) {
        ESP_LOGW(TAG, "Failed to submit SV2 share (ret=%d, errno=%d: %s)",
                 ret, errno, strerror(errno));
    }
    return ret;
}

static int submit_sv2_extended(void *context,
                               const asic_share_submission_t *share)
{
    GlobalState *state = ((result_callback_context *)context)->state;
    int ret = stratum_v2_submit_share_extended(
        state, share->numeric_job_id, share->nonce, share->ntime,
        share->final_version, share->extranonce2_bin, share->extranonce2_len);
    if (ret < 0) {
        ESP_LOGW(TAG, "Failed to submit SV2 share (ret=%d, errno=%d: %s)",
                 ret, errno, strerror(errno));
    }
    return ret;
}

static void account_share(void *context,
                          const asic_share_submission_t *share)
{
    GlobalState *state = ((result_callback_context *)context)->state;
    const asic_result_t *result = share->result;
    ESP_LOGI(TAG,
             "ID: %s, ASIC nr: %d, Core: %d/%d, ver: %08" PRIX32
             " Nonce %08" PRIX32 " diff %.1f of %g.",
             share->job_id, result->asic_index, result->core_id,
             result->small_core_id, result->final_version, result->nonce,
             share->nonce_diff, share->pool_difficulty);

    SYSTEM_notify_found_nonce(state, share->nonce_diff, share->target);
    scoreboard_add(&state->SYSTEM_MODULE.scoreboard, share->nonce_diff,
                   share->job_id, share->extranonce2, share->ntime,
                   share->nonce, share->version_bits);
}

static const asic_result_callbacks_t RESULT_CALLBACKS = {
    .record_self_test = record_self_test,
    .sv1_transport_ready = sv1_transport_ready,
    .submit_sv1 = submit_sv1,
    .submit_sv2_standard = submit_sv2_standard,
    .submit_sv2_extended = submit_sv2_extended,
    .account_share = account_share,
};

static asic_result_status_t handle_result(GlobalState *state,
                                          const asic_result_t *result)
{
    result_callback_context callback_context = {.state = state};
    asic_result_context_t context = {
        .job_store = &state->asic_job_store,
        .self_test = state->SELF_TEST_MODULE.is_active,
        .username = state->SYSTEM_MODULE.is_using_fallback
                        ? state->SYSTEM_MODULE.fallback_pool_user
                        : state->SYSTEM_MODULE.pool_user,
        .callback_context = &callback_context,
    };
    return asic_result_handle(result, &context, &RESULT_CALLBACKS);
}

void ASIC_result_task(void *pvParameters)
{
    GlobalState *state = pvParameters;

    while (1) {
        if (!state->ASIC_initalized) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        asic_event_t *event = ASIC_process_work(state);
        if (event == NULL) continue;

        if (event->type == ASIC_EVENT_REGISTER_RESULT) {
            monitor_register(state, &event->data.register_result);
            continue;
        }

        if (event->type != ASIC_EVENT_SHARE_RESULT) {
            ESP_LOGW(TAG, "Ignoring unknown ASIC event type %d", event->type);
            continue;
        }

        const asic_result_t *result = &event->data.share;
        if (handle_result(state, result) == ASIC_RESULT_REJECTED_WORK) {
            ESP_LOGW(TAG, "Invalid work result found, 0x%" PRIX64,
                     result->work_handle);
        }
    }
}
