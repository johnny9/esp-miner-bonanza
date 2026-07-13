#ifndef ASIC_RESULT_HANDLER_H
#define ASIC_RESULT_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

#include "asic_job_store.h"
#include "asic_result.h"

typedef struct {
    const asic_result_t *result;
    const char *username;
    const char *job_id;
    const char *extranonce2;
    uint8_t extranonce2_bin[MINING_MAX_EXTRANONCE2_SIZE];
    uint8_t extranonce2_len;
    uint32_t numeric_job_id;
    uint32_t nonce;
    double nonce_diff;
    uint32_t ntime;
    uint32_t base_version;
    uint32_t final_version;
    uint32_t version_bits;
    double pool_difficulty;
    uint32_t target;
    mining_protocol_t protocol;
} asic_share_submission_t;

typedef struct {
    asic_job_store_t *job_store;
    bool self_test;
    const char *username;
    void *callback_context;
} asic_result_context_t;

typedef struct {
    void (*record_self_test)(void *context, double nonce_diff);
    bool (*sv1_transport_ready)(void *context,
                                const asic_share_submission_t *share);
    int (*submit_sv1)(void *context, const asic_share_submission_t *share);
    int (*submit_sv2_standard)(void *context,
                               const asic_share_submission_t *share);
    int (*submit_sv2_extended)(void *context,
                               const asic_share_submission_t *share);
    void (*account_share)(void *context,
                          const asic_share_submission_t *share);
} asic_result_callbacks_t;

typedef enum {
    ASIC_RESULT_REJECTED_WORK,
    ASIC_RESULT_RECORDED_SELF_TEST,
    ASIC_RESULT_ACCOUNTED,
} asic_result_status_t;

asic_result_status_t asic_result_handle(
    const asic_result_t *result, const asic_result_context_t *context,
    const asic_result_callbacks_t *callbacks);

#endif // ASIC_RESULT_HANDLER_H
