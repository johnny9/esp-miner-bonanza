#include <sys/time.h>
#include <limits.h>

#include "work_queue.h"
#include "global_state.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mining.h"
#include "string.h"
#include "esp_timer.h"

#include "asic.h"
#include "system.h"
#include "sv2_protocol.h"
#include "stratum_api.h"
#include "stratum_v2_task.h"
#include "utils.h"
#include "mining_template.h"
#include "sv2_mining_template.h"

static const char *TAG = "create_jobs_task";

static bool generate_work(GlobalState *GLOBAL_STATE, mining_notify *notification,
                          uint64_t extranonce_2, double difficulty,
                          bool clean_jobs);
static bool generate_work_sv2(GlobalState *GLOBAL_STATE, sv2_job_t *job,
                              double difficulty, bool clean_jobs);
static bool generate_work_sv2_ext(GlobalState *GLOBAL_STATE, sv2_ext_job_t *job,
                                  double difficulty,
                                  uint64_t extranonce_2_counter,
                                  bool clean_jobs);

// Free a work item using the correct free function for the protocol it was created under
static void free_work_item(GlobalState *GLOBAL_STATE, void *work, stratum_protocol_t protocol)
{
    if (!work) return;
    if (protocol == STRATUM_PROTOCOL_V2) {
        if (stratum_v2_is_extended_channel(GLOBAL_STATE)) {
            sv2_ext_job_free((sv2_ext_job_t *)work);
        } else {
            free(work);  // sv2_job_t is flat
        }
    } else {
        STRATUM_V1_free_mining_notify(work);
    }
}

void create_jobs_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;

    double difficulty = GLOBAL_STATE->pool_difficulty;
    void *current_work = NULL;
    stratum_protocol_t current_work_protocol = GLOBAL_STATE->stratum_protocol;
    uint64_t extranonce_2 = 0;
    bool clean_jobs_pending = false;
    int timeout_ms = ASIC_get_asic_job_frequency_ms(GLOBAL_STATE);

    ESP_LOGI(TAG, "ASIC Job Interval: %d ms", timeout_ms);
    ESP_LOGI(TAG, "ASIC Ready!");

    while (1) {
        // Read protocol dynamically each iteration (coordinator may have switched it)
        stratum_protocol_t active_protocol = GLOBAL_STATE->stratum_protocol;

        // If protocol changed, discard current_work (it belongs to the old protocol)
        // Always update current_work_protocol so the post-dequeue check doesn't
        // incorrectly discard the first valid work item from the new protocol.
        if (active_protocol != current_work_protocol) {
            if (current_work != NULL) {
                ESP_LOGI(TAG, "Protocol switched from %s to %s, discarding current work",
                         current_work_protocol == STRATUM_PROTOCOL_V2 ? STRATUM_V2 : STRATUM_V1,
                         active_protocol == STRATUM_PROTOCOL_V2 ? STRATUM_V2 : STRATUM_V1);
                free_work_item(GLOBAL_STATE, current_work, current_work_protocol);
                current_work = NULL;
                clean_jobs_pending = false;
            }
            current_work_protocol = active_protocol;
        }

        uint64_t start_time = esp_timer_get_time();
        void *new_work = queue_dequeue_timeout(&GLOBAL_STATE->stratum_queue, timeout_ms);
        timeout_ms -= (esp_timer_get_time() - start_time) / 1000;

        if (new_work != NULL) {
            active_protocol = GLOBAL_STATE->stratum_protocol;

            // Free previous work using the protocol it was created under
            free_work_item(GLOBAL_STATE, current_work, current_work_protocol);
            current_work = NULL;

            if (active_protocol != current_work_protocol) {
                // Protocol switched during our blocking dequeue.
                // The dequeued item may be from either the old or new protocol —
                // we cannot safely determine which type it is, so discard it.
                // free() is safe for both sv2_job_t (flat) and mining_notify (malloc'd;
                // internal strings leak but this is a rare protocol-switch event).
                ESP_LOGW(TAG, "Protocol switch detected during dequeue, discarding stale item");
                free(new_work);
                current_work_protocol = active_protocol;
                clean_jobs_pending = false;
                timeout_ms = ASIC_get_asic_job_frequency_ms(GLOBAL_STATE);
                continue;
            }

            // Protocol unchanged — item matches current_work_protocol. Safe to cast.
            if (current_work_protocol == STRATUM_PROTOCOL_V2) {
                if (stratum_v2_is_extended_channel(GLOBAL_STATE)) {
                    ESP_LOGI(TAG, "New Work Dequeued SV2 ext job %lu", ((sv2_ext_job_t *)new_work)->job_id);
                } else {
                    ESP_LOGI(TAG, "New Work Dequeued SV2 job %lu", ((sv2_job_t *)new_work)->job_id);
                }
            } else {
                ESP_LOGI(TAG, "New Work Dequeued %s", ((mining_notify *)new_work)->job_id);
            }

            current_work = new_work;
            GLOBAL_STATE->SYSTEM_MODULE.last_work_received_us =
                esp_timer_get_time();

            if (GLOBAL_STATE->new_set_mining_difficulty_msg) {
                ESP_LOGI(TAG, "New pool difficulty %.2f", GLOBAL_STATE->pool_difficulty);
                difficulty = GLOBAL_STATE->pool_difficulty;
                GLOBAL_STATE->new_set_mining_difficulty_msg = false;
            }

            if (GLOBAL_STATE->new_stratum_version_rolling_msg && GLOBAL_STATE->ASIC_initalized) {
                ESP_LOGI(TAG, "Set chip version rolls %i", (int)(GLOBAL_STATE->version_mask >> 13));
                ASIC_set_version_mask(GLOBAL_STATE, GLOBAL_STATE->version_mask);
                GLOBAL_STATE->new_stratum_version_rolling_msg = false;
            }

            extranonce_2 = 0;

            // Check clean_jobs flag
            bool clean;
            if (current_work_protocol == STRATUM_PROTOCOL_V2) {
                if (stratum_v2_is_extended_channel(GLOBAL_STATE)) {
                    clean = ((sv2_ext_job_t *)current_work)->clean_jobs;
                } else {
                    clean = ((sv2_job_t *)current_work)->clean_jobs;
                }
            } else {
                clean = ((mining_notify *)current_work)->clean_jobs;
            }
            if (!clean) {
                clean_jobs_pending = false;
                continue;
            }
            clean_jobs_pending = true;
        } else {
            if (current_work == NULL) {
                vTaskDelay(100 / portTICK_PERIOD_MS);
                continue;
            }
            // SV2 standard channel: the ASIC has enough nonce+version space
            // (2^32 nonces x version rolls) to keep mining without re-feeding.
            // Re-sending the same job restarts the nonce search from 0 and
            // produces duplicate shares. Only send work on new jobs.
            // (V1 and SV2 extended are fine — extranonce_2 gives unique work each time.)
            asic_capabilities_t capabilities =
                ASIC_get_capabilities(GLOBAL_STATE);
            if (active_protocol == STRATUM_PROTOCOL_V2 &&
                !stratum_v2_is_extended_channel(GLOBAL_STATE) &&
                ASIC_capabilities_support_static_work(&capabilities)) {
                timeout_ms = ASIC_get_asic_job_frequency_ms(GLOBAL_STATE);
                continue;
            }
        }

        // Final protocol check before generating work — protocol may have switched
        // during a timeout dequeue while we still hold stale current_work
        active_protocol = GLOBAL_STATE->stratum_protocol;
        if (active_protocol != current_work_protocol) {
            free_work_item(GLOBAL_STATE, current_work, current_work_protocol);
            current_work = NULL;
            clean_jobs_pending = false;
            current_work_protocol = active_protocol;
            timeout_ms = ASIC_get_asic_job_frequency_ms(GLOBAL_STATE);
            continue;
        }

        // Generate and send job
        bool sent = false;
        if (active_protocol == STRATUM_PROTOCOL_V2) {
            if (stratum_v2_is_extended_channel(GLOBAL_STATE)) {
                sent = generate_work_sv2_ext(
                    GLOBAL_STATE, (sv2_ext_job_t *)current_work, difficulty,
                    extranonce_2, clean_jobs_pending);
                extranonce_2++;
            } else {
                sent = generate_work_sv2(
                    GLOBAL_STATE, (sv2_job_t *)current_work, difficulty,
                    clean_jobs_pending);
            }
        } else {
            sent = generate_work(
                GLOBAL_STATE, (mining_notify *)current_work, extranonce_2,
                difficulty, clean_jobs_pending);
            extranonce_2++;
        }
        if (sent) clean_jobs_pending = false;
        timeout_ms = ASIC_get_asic_job_frequency_ms(GLOBAL_STATE);
    }
}

static bool generate_work(GlobalState *GLOBAL_STATE,
                          mining_notify *notification,
                          uint64_t extranonce_2, double difficulty,
                          bool clean_jobs)
{
    mining_template_t template;
    if (!mining_template_build_sv1(
            notification, GLOBAL_STATE->extranonce_str,
            GLOBAL_STATE->extranonce_2_len, extranonce_2,
            GLOBAL_STATE->version_mask, difficulty, &template)) {
        ESP_LOGE(TAG, "Unable to build SV1 job (extranonce2 length %d)",
                 GLOBAL_STATE->extranonce_2_len);
        return false;
    }
    template.clean_jobs = clean_jobs;

    // Check if ASIC is initialized before trying to send work
    if (!GLOBAL_STATE->ASIC_initalized) {
        ESP_LOGW(TAG, "ASIC not initialized, skipping job send");
        mining_template_free(&template);
        return false;
    }

    bool sent = ASIC_send_work(GLOBAL_STATE, &template);
    if (!sent) {
        ESP_LOGE(TAG, "ASIC rejected SV1 work");
    }
    mining_template_free(&template);
    return sent;
}

// Standard channels rely on the advertised ASIC rolling capabilities for
// unique work rather than any chip-family-specific assumption here.
static bool generate_work_sv2(GlobalState *GLOBAL_STATE, sv2_job_t *sv2_job,
                              double difficulty, bool clean_jobs)
{
    mining_template_t template;
    if (!mining_template_build_sv2_standard(
            sv2_job, GLOBAL_STATE->version_mask, difficulty, &template)) {
        ESP_LOGE(TAG, "Unable to build SV2 standard job");
        return false;
    }
    template.clean_jobs = clean_jobs;

    if (!GLOBAL_STATE->ASIC_initalized) {
        ESP_LOGW(TAG, "ASIC not initialized, skipping SV2 job send");
        mining_template_free(&template);
        return false;
    }

    bool sent = ASIC_send_work(GLOBAL_STATE, &template);
    if (!sent) {
        ESP_LOGE(TAG, "ASIC rejected SV2 standard work");
    }
    mining_template_free(&template);
    return sent;
}

// Extended channel work generation: compute coinbase hash from prefix+extranonce+suffix,
// then merkle root from merkle path, then midstates. extranonce_2 provides unique work.
static bool generate_work_sv2_ext(GlobalState *GLOBAL_STATE,
                                  sv2_ext_job_t *ext_job,
                                  double difficulty,
                                  uint64_t extranonce_2_counter,
                                  bool clean_jobs)
{
    mining_template_t template;
    if (!mining_template_build_sv2_extended(
            ext_job, GLOBAL_STATE->sv2_conn, extranonce_2_counter,
            GLOBAL_STATE->version_mask, difficulty, &template)) {
        ESP_LOGE(TAG, "Unable to build SV2 extended job");
        return false;
    }
    template.clean_jobs = clean_jobs;

    if (!GLOBAL_STATE->ASIC_initalized) {
        ESP_LOGW(TAG, "ASIC not initialized, skipping SV2 ext job send");
        mining_template_free(&template);
        return false;
    }

    bool sent = ASIC_send_work(GLOBAL_STATE, &template);
    if (!sent) {
        ESP_LOGE(TAG, "ASIC rejected SV2 extended work");
    }
    mining_template_free(&template);
    return sent;
}
