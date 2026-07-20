#ifndef BZM_RUNNING_EVIDENCE_H
#define BZM_RUNNING_EVIDENCE_H

#include <stdbool.h>
#include <stdint.h>

#define BZM_RUNNING_EVIDENCE_DETAIL_LENGTH 160U

typedef struct
{
    uint64_t dispatch_batches;
    uint64_t dispatched_logical_engines;
    uint64_t dispatched_chip_engines;
    uint64_t dispatch_failures;
    uint64_t mapped_results;
    uint64_t mapping_rejections;
    uint32_t mapping_rejection_streak;
    bool mapping_recovery_pending;
    uint64_t locally_valid_results;
    uint64_t locally_rejected_results;
    uint64_t duplicate_results;
    uint32_t local_rejection_streak;
    bool local_recovery_pending;
} bzm_running_stats_t;

typedef struct
{
    uint32_t required_chip_engine_writes;
    uint32_t minimum_valid_results;
    bool allow_mapping_recovery;
    uint32_t maximum_mapping_rejections;
    uint32_t maximum_local_rejections;
    uint32_t proof_timeout_ms;
} bzm_running_evidence_config_t;

typedef enum
{
    BZM_RUNNING_EVIDENCE_PENDING = 0,
    BZM_RUNNING_EVIDENCE_GOOD,
    BZM_RUNNING_EVIDENCE_BAD,
} bzm_running_evidence_status_t;

typedef enum
{
    BZM_RUNNING_EVIDENCE_FAULT_NONE = 0,
    BZM_RUNNING_EVIDENCE_FAULT_COUNTER_ROLLBACK,
    BZM_RUNNING_EVIDENCE_FAULT_DISPATCH,
    BZM_RUNNING_EVIDENCE_FAULT_MAPPING,
    BZM_RUNNING_EVIDENCE_FAULT_LOCAL_VALIDATION,
    BZM_RUNNING_EVIDENCE_FAULT_TIMEOUT,
    BZM_RUNNING_EVIDENCE_FAULT_INVALID_CONFIGURATION,
} bzm_running_evidence_fault_t;

typedef struct
{
    bzm_running_evidence_status_t status;
    bzm_running_evidence_fault_t fault;
    bzm_running_stats_t observed;
    uint64_t elapsed_ms;
    char detail[BZM_RUNNING_EVIDENCE_DETAIL_LENGTH];
} bzm_running_evidence_result_t;

typedef struct
{
    bool completed_once;
    bool recovery_active;
    uint64_t recovery_started_at_ms;
    uint64_t last_locally_valid_results;
} bzm_running_evidence_lifecycle_t;

bzm_running_evidence_result_t bzm_running_evidence_evaluate(
    const bzm_running_stats_t * baseline,
    const bzm_running_stats_t * current,
    const bzm_running_evidence_config_t * config,
    uint64_t elapsed_ms);

void bzm_running_evidence_lifecycle_init(bzm_running_evidence_lifecycle_t * lifecycle);

/* Establish Stage 7 once a complete dispatch and real locally verified nonce
 * arrive inside the initial timeout, even if bounded recovery is pending at
 * that sampling instant. That recovery and each later one gets a fresh
 * timeout. New locally verified proof demonstrates that any prior result
 * recovery completed; if another bounded rejection is already pending at the
 * next sample, its timeout starts fresh. Over-limit streaks and all structural
 * faults remain immediately BAD. */
bzm_running_evidence_result_t bzm_running_evidence_track(
    bzm_running_evidence_lifecycle_t * lifecycle,
    const bzm_running_stats_t * baseline,
    const bzm_running_stats_t * current,
    const bzm_running_evidence_config_t * config,
    uint64_t started_at_ms,
    uint64_t now_ms);

/* Classifies a locally verified mapped result against the configured Stage-7
 * proof floor. The caller records false as a local-validation rejection. */
bool bzm_running_result_meets_proof(double nonce_difficulty,
                                    double minimum_nonce_difficulty);

const char * bzm_running_evidence_status_name(bzm_running_evidence_status_t status);
const char * bzm_running_evidence_fault_name(bzm_running_evidence_fault_t fault);

#endif /* BZM_RUNNING_EVIDENCE_H */
