#ifndef BM_RESULT_H_
#define BM_RESULT_H_

#include <stdbool.h>
#include <stdint.h>

#include "asic_common.h"
#include "bm_job_slot.h"

typedef enum {
    BM_RESULT_PROTOCOL_SV1,
    BM_RESULT_PROTOCOL_SV2_STANDARD,
    BM_RESULT_PROTOCOL_SV2_EXTENDED,
} bm_result_protocol_t;

typedef struct {
    const asic_result_t *result;
    const char *username;
    const char *jobid;
    const char *extranonce2;
    uint8_t extranonce2_bin[32];
    uint8_t extranonce2_len;
    uint32_t numeric_job_id;
    uint32_t nonce;
    double nonce_diff;
    uint32_t ntime;
    uint32_t base_version;
    uint32_t rolled_version;
    uint32_t version_bits;
    double pool_diff;
    uint32_t target;
} bm_share_submission;

typedef struct {
    pthread_mutex_t *valid_jobs_lock;
    const uint8_t *valid_jobs;
    bm_job *const *active_jobs;
    bm_result_protocol_t protocol;
    bool self_test;
    const char *username;
    uint8_t sv2_extranonce2_len;
    void *callback_context;
} bm_result_context;

typedef struct {
    void (*record_self_test)(void *context, double nonce_diff);
    bool (*sv1_transport_ready)(void *context, const bm_share_submission *share);
    int (*submit_sv1)(void *context, const bm_share_submission *share);
    int (*submit_sv2_standard)(void *context, const bm_share_submission *share);
    int (*submit_sv2_extended)(void *context, const bm_share_submission *share);
    void (*account_share)(void *context, const bm_share_submission *share);
} bm_result_callbacks;

typedef enum {
    BM_RESULT_REJECTED_JOB,
    BM_RESULT_RECORDED_SELF_TEST,
    BM_RESULT_ACCOUNTED,
} bm_result_status_t;

bool bm_result_to_event(const task_result *result, asic_event_t *event);

bm_result_status_t bm_result_handle(const asic_result_t *result,
                                    const bm_result_context *context,
                                    const bm_result_callbacks *callbacks);

#endif // BM_RESULT_H_
