#include "bm_result.h"

#include <stdlib.h>

#include "utils.h"

bm_result_status_t bm_result_handle(const task_result *result,
                                    const bm_result_context *context,
                                    const bm_result_callbacks *callbacks)
{
    if (result->register_type != REGISTER_INVALID) {
        if (callbacks->monitor_register) {
            callbacks->monitor_register(context->callback_context, result);
        }
        return BM_RESULT_HANDLED_REGISTER;
    }

    bm_job *job = bm_job_slot_snapshot(context->valid_jobs_lock,
                                       context->valid_jobs,
                                       context->active_jobs, result->job_id);
    if (job == NULL) return BM_RESULT_REJECTED_JOB;

    bm_share_submission share = {
        .result = result,
        .username = context->username,
        .jobid = job->jobid,
        .extranonce2 = job->extranonce2,
        .nonce = result->nonce,
        .nonce_diff = test_nonce_value(job, result->nonce, result->rolled_version),
        .ntime = job->ntime,
        .base_version = job->version,
        .rolled_version = result->rolled_version,
        .version_bits = result->rolled_version ^ job->version,
        .pool_diff = job->pool_diff,
        .target = job->target,
    };

    if (context->self_test) {
        if (callbacks->record_self_test) {
            callbacks->record_self_test(context->callback_context, share.nonce_diff);
        }
        free_bm_job(job);
        return BM_RESULT_RECORDED_SELF_TEST;
    }

    if (share.nonce_diff >= share.pool_diff) {
        if (context->protocol == BM_RESULT_PROTOCOL_SV1) {
            bool ready = callbacks->sv1_transport_ready == NULL ||
                         callbacks->sv1_transport_ready(context->callback_context, &share);
            if (ready && callbacks->submit_sv1) {
                callbacks->submit_sv1(context->callback_context, &share);
            }
        } else {
            share.numeric_job_id = (uint32_t)strtoul(share.jobid, NULL, 10);
            if (context->protocol == BM_RESULT_PROTOCOL_SV2_EXTENDED) {
                share.extranonce2_len = context->sv2_extranonce2_len;
                if (share.extranonce2 != NULL) {
                    hex2bin(share.extranonce2, share.extranonce2_bin,
                            share.extranonce2_len);
                }
                if (callbacks->submit_sv2_extended) {
                    callbacks->submit_sv2_extended(context->callback_context, &share);
                }
            } else if (callbacks->submit_sv2_standard) {
                callbacks->submit_sv2_standard(context->callback_context, &share);
            }
        }
    }

    if (callbacks->account_share) {
        callbacks->account_share(context->callback_context, &share);
    }
    free_bm_job(job);
    return BM_RESULT_ACCOUNTED;
}
