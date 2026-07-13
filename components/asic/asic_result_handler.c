#include "asic_result_handler.h"

#include <string.h>

asic_result_status_t asic_result_handle(
    const asic_result_t *result, const asic_result_context_t *context,
    const asic_result_callbacks_t *callbacks)
{
    if (result == NULL || context == NULL || callbacks == NULL ||
        context->job_store == NULL) {
        return ASIC_RESULT_REJECTED_WORK;
    }

    mining_template_t template;
    if (!asic_job_store_snapshot(context->job_store, result->work_handle,
                                 &template)) {
        return ASIC_RESULT_REJECTED_WORK;
    }

    bool metadata_valid =
        template.share.protocol >= MINING_PROTOCOL_SV1 &&
        template.share.protocol <= MINING_PROTOCOL_SV2_EXTENDED &&
        template.share.job_id != NULL &&
        template.share.extranonce2 != NULL &&
        template.share.extranonce2_len <= MINING_MAX_EXTRANONCE2_SIZE &&
        result->version_bits ==
            (result->final_version ^ template.version) &&
        (result->version_bits & ~template.version_mask) == 0;
    if (!metadata_valid) {
        mining_template_free(&template);
        return ASIC_RESULT_REJECTED_WORK;
    }

    asic_share_submission_t share = {
        .result = result,
        .username = context->username,
        .job_id = template.share.job_id,
        .extranonce2 = template.share.extranonce2,
        .extranonce2_len = template.share.extranonce2_len,
        .numeric_job_id = template.share.numeric_job_id,
        .nonce = result->nonce,
        .nonce_diff = mining_test_nonce_value(
            &template, result->nonce, result->final_ntime,
            result->final_version),
        .ntime = result->final_ntime,
        .base_version = template.version,
        .final_version = result->final_version,
        .version_bits = result->version_bits,
        .pool_difficulty = template.share.pool_difficulty,
        .target = template.target,
        .protocol = template.share.protocol,
    };
    memcpy(share.extranonce2_bin, template.share.extranonce2_bin,
           template.share.extranonce2_len);

    if (context->self_test) {
        if (callbacks->record_self_test != NULL) {
            callbacks->record_self_test(context->callback_context,
                                        share.nonce_diff);
        }
        mining_template_free(&template);
        return ASIC_RESULT_RECORDED_SELF_TEST;
    }

    if (share.nonce_diff >= share.pool_difficulty) {
        switch (share.protocol) {
            case MINING_PROTOCOL_SV1: {
                bool ready = callbacks->sv1_transport_ready == NULL ||
                    callbacks->sv1_transport_ready(
                        context->callback_context, &share);
                if (ready && callbacks->submit_sv1 != NULL) {
                    callbacks->submit_sv1(context->callback_context, &share);
                }
                break;
            }
            case MINING_PROTOCOL_SV2_STANDARD:
                if (callbacks->submit_sv2_standard != NULL) {
                    callbacks->submit_sv2_standard(
                        context->callback_context, &share);
                }
                break;
            case MINING_PROTOCOL_SV2_EXTENDED:
                if (callbacks->submit_sv2_extended != NULL) {
                    callbacks->submit_sv2_extended(
                        context->callback_context, &share);
                }
                break;
        }
    }

    if (callbacks->account_share != NULL) {
        callbacks->account_share(context->callback_context, &share);
    }
    mining_template_free(&template);
    return ASIC_RESULT_ACCOUNTED;
}
