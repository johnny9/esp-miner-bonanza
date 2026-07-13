#include "bm_result.h"

#include <string.h>

bool bm_result_to_event(const task_result *result, asic_event_t *event)
{
    if (result == NULL || event == NULL) return false;

    memset(event, 0, sizeof(*event));
    if (result->register_type != REGISTER_INVALID) {
        event->type = ASIC_EVENT_REGISTER_RESULT;
        event->data.register_result = (asic_register_result_t) {
            .register_type = result->register_type,
            .asic_index = result->asic_nr,
            .value = result->value,
            .timestamp_us = result->timestamp_us,
        };
        return true;
    }

    event->type = ASIC_EVENT_SHARE_RESULT;
    event->data.share = (asic_result_t) {
        .work_handle = result->job_id,
        .nonce = result->nonce,
        .final_ntime = result->ntime,
        .final_version = result->rolled_version,
        .version_bits = result->version_bits,
        .timestamp_us = result->timestamp_us,
        .asic_index = result->asic_nr,
        .core_id = result->core_id,
        .small_core_id = result->small_core_id,
    };
    return true;
}
