#include <errno.h>
#include <lwip/tcpip.h>
#include <stdlib.h>
#include <string.h>

#include "asic.h"
#include "bm_result.h"
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

static void monitor_register(void *context, const task_result *result)
{
    GlobalState *state = ((result_callback_context *)context)->state;
    hashrate_monitor_register_read(state, result->register_type, result->asic_nr,
                                   result->value, result->timestamp_us);
}

static void record_self_test(void *context, double nonce_diff)
{
    result_callback_context *callback_context = context;
    self_test_record_nonce(callback_context->state, nonce_diff);
}

static bool sv1_transport_ready(void *context, const bm_share_submission *share)
{
    result_callback_context *callback_context = context;
    GlobalState *state = callback_context->state;
    taskENTER_CRITICAL(&state->stratum_mux);
    callback_context->sv1_transport = state->transport;
    callback_context->sv1_uid = state->send_uid++;
    taskEXIT_CRITICAL(&state->stratum_mux);
    bool ready = callback_context->sv1_transport != NULL;
    if (!ready) {
        ESP_LOGW(TAG, "No stratum connection, dropping share (job 0x%02X)",
                 share->result->job_id);
    }
    return ready;
}

static int submit_sv1(void *context, const bm_share_submission *share)
{
    result_callback_context *callback_context = context;
    GlobalState *state = callback_context->state;
    esp_transport_handle_t transport = callback_context->sv1_transport;
    int uid = callback_context->sv1_uid;

    if (transport == NULL) return -1;
    uint64_t sent_time_us = 0;
    int ret = STRATUM_V1_submit_share(transport, uid, share->username,
                                      share->jobid, share->extranonce2,
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

static int submit_sv2_standard(void *context, const bm_share_submission *share)
{
    GlobalState *state = ((result_callback_context *)context)->state;
    int ret = stratum_v2_submit_share(state, share->numeric_job_id,
                                      share->nonce, share->ntime,
                                      share->rolled_version);
    if (ret < 0) {
        ESP_LOGW(TAG, "Failed to submit SV2 share (ret=%d, errno=%d: %s)",
                 ret, errno, strerror(errno));
    }
    return ret;
}

static int submit_sv2_extended(void *context, const bm_share_submission *share)
{
    GlobalState *state = ((result_callback_context *)context)->state;
    int ret = stratum_v2_submit_share_extended(
        state, share->numeric_job_id, share->nonce, share->ntime,
        share->rolled_version, share->extranonce2_bin, share->extranonce2_len);
    if (ret < 0) {
        ESP_LOGW(TAG, "Failed to submit SV2 share (ret=%d, errno=%d: %s)",
                 ret, errno, strerror(errno));
    }
    return ret;
}

static void account_share(void *context, const bm_share_submission *share)
{
    GlobalState *state = ((result_callback_context *)context)->state;
    const task_result *result = share->result;
    ESP_LOGI(TAG,
             "ID: %s, ASIC nr: %d, Core: %d/%d, ver: %08" PRIX32
             " Nonce %08" PRIX32 " diff %.1f of %g.",
             share->jobid, result->asic_nr, result->core_id,
             result->small_core_id, result->rolled_version, result->nonce,
             share->nonce_diff, share->pool_diff);

    SYSTEM_notify_found_nonce(state, share->nonce_diff, share->target);
    scoreboard_add(&state->SYSTEM_MODULE.scoreboard, share->nonce_diff,
                   share->jobid, share->extranonce2, share->ntime,
                   share->nonce, share->version_bits);
}

static const bm_result_callbacks RESULT_CALLBACKS = {
    .monitor_register = monitor_register,
    .record_self_test = record_self_test,
    .sv1_transport_ready = sv1_transport_ready,
    .submit_sv1 = submit_sv1,
    .submit_sv2_standard = submit_sv2_standard,
    .submit_sv2_extended = submit_sv2_extended,
    .account_share = account_share,
};

static bm_result_status_t handle_result(GlobalState *state,
                                        const task_result *result)
{
    result_callback_context callback_context = {.state = state};
    bm_result_protocol_t protocol = BM_RESULT_PROTOCOL_SV1;
    uint8_t extranonce2_len = 0;
    if (state->stratum_protocol == STRATUM_PROTOCOL_V2) {
        if (stratum_v2_is_extended_channel(state)) {
            protocol = BM_RESULT_PROTOCOL_SV2_EXTENDED;
            if (state->sv2_conn != NULL) {
                extranonce2_len = state->sv2_conn->extranonce_size;
            }
        } else {
            protocol = BM_RESULT_PROTOCOL_SV2_STANDARD;
        }
    }

    bm_result_context context = {
        .valid_jobs_lock = &state->valid_jobs_lock,
        .valid_jobs = state->valid_jobs,
        .active_jobs = state->ASIC_TASK_MODULE.active_jobs,
        .protocol = protocol,
        .self_test = state->SELF_TEST_MODULE.is_active,
        .username = state->SYSTEM_MODULE.is_using_fallback
                        ? state->SYSTEM_MODULE.fallback_pool_user
                        : state->SYSTEM_MODULE.pool_user,
        .sv2_extranonce2_len = extranonce2_len,
        .callback_context = &callback_context,
    };
    return bm_result_handle(result, &context, &RESULT_CALLBACKS);
}

void ASIC_result_task(void *pvParameters)
{
    GlobalState *state = pvParameters;

    while (1) {
        if (!state->ASIC_initalized) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        task_result *result = ASIC_process_work(state);
        if (result == NULL) continue;

        if (handle_result(state, result) == BM_RESULT_REJECTED_JOB) {
            ESP_LOGW(TAG, "Invalid job nonce found, 0x%02X", result->job_id);
        }
    }
}
