#include "bzm_running_evidence.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool counter_rolled_back(const bzm_running_stats_t * baseline,
                                const bzm_running_stats_t * current)
{
    return current->dispatch_batches < baseline->dispatch_batches ||
           current->dispatched_logical_engines < baseline->dispatched_logical_engines ||
           current->dispatched_chip_engines < baseline->dispatched_chip_engines ||
           current->dispatch_failures < baseline->dispatch_failures ||
           current->mapped_results < baseline->mapped_results ||
           current->mapping_rejections < baseline->mapping_rejections ||
           current->locally_valid_results < baseline->locally_valid_results ||
           current->locally_rejected_results < baseline->locally_rejected_results;
}

static bzm_running_stats_t subtract_stats(const bzm_running_stats_t * current,
                                          const bzm_running_stats_t * baseline)
{
    return (bzm_running_stats_t){
        .dispatch_batches = current->dispatch_batches - baseline->dispatch_batches,
        .dispatched_logical_engines =
            current->dispatched_logical_engines - baseline->dispatched_logical_engines,
        .dispatched_chip_engines = current->dispatched_chip_engines - baseline->dispatched_chip_engines,
        .dispatch_failures = current->dispatch_failures - baseline->dispatch_failures,
        .mapped_results = current->mapped_results - baseline->mapped_results,
        .mapping_rejections = current->mapping_rejections - baseline->mapping_rejections,
        .mapping_rejection_streak = current->mapping_rejection_streak,
        .mapping_recovery_pending = current->mapping_recovery_pending,
        .locally_valid_results = current->locally_valid_results - baseline->locally_valid_results,
        .locally_rejected_results = current->locally_rejected_results - baseline->locally_rejected_results,
        .local_rejection_streak = current->local_rejection_streak,
        .local_recovery_pending = current->local_recovery_pending,
    };
}

static bool proof_requirements_met(const bzm_running_stats_t * observed,
                                   const bzm_running_evidence_config_t * config)
{
    return observed != NULL && config != NULL &&
           observed->dispatch_batches != 0 &&
           observed->dispatched_chip_engines >= config->required_chip_engine_writes &&
           observed->mapped_results >= config->minimum_valid_results &&
           observed->locally_valid_results >= config->minimum_valid_results;
}

static bzm_running_evidence_result_t result_with(
    bzm_running_evidence_status_t status, bzm_running_evidence_fault_t fault,
    const bzm_running_stats_t * observed, uint64_t elapsed_ms, const char * detail)
{
    bzm_running_evidence_result_t result = {
        .status = status,
        .fault = fault,
        .elapsed_ms = elapsed_ms,
    };
    if (observed != NULL)
        result.observed = *observed;
    snprintf(result.detail, sizeof(result.detail), "%s", detail != NULL ? detail : "");
    return result;
}

bool bzm_running_result_meets_proof(double nonce_difficulty,
                                    double minimum_nonce_difficulty)
{
    return isfinite(nonce_difficulty) &&
           isfinite(minimum_nonce_difficulty) &&
           minimum_nonce_difficulty > 0.0 &&
           nonce_difficulty >= minimum_nonce_difficulty;
}

bzm_running_evidence_result_t bzm_running_evidence_evaluate(
    const bzm_running_stats_t * baseline,
    const bzm_running_stats_t * current,
    const bzm_running_evidence_config_t * config,
    uint64_t elapsed_ms)
{
    if (baseline == NULL || current == NULL || config == NULL ||
        config->required_chip_engine_writes == 0 ||
        config->minimum_valid_results == 0 || config->proof_timeout_ms == 0 ||
        (config->allow_mapping_recovery &&
         config->maximum_mapping_rejections == 0)) {
        return result_with(BZM_RUNNING_EVIDENCE_BAD,
                           BZM_RUNNING_EVIDENCE_FAULT_INVALID_CONFIGURATION,
                           NULL, elapsed_ms, "Stage-7 evidence configuration is invalid");
    }
    if (counter_rolled_back(baseline, current)) {
        return result_with(BZM_RUNNING_EVIDENCE_BAD,
                           BZM_RUNNING_EVIDENCE_FAULT_COUNTER_ROLLBACK,
                           NULL, elapsed_ms, "Stage-7 evidence counters moved backwards");
    }

    bzm_running_stats_t observed = subtract_stats(current, baseline);
    char detail[BZM_RUNNING_EVIDENCE_DETAIL_LENGTH];
    if (observed.dispatch_failures != 0) {
        snprintf(detail, sizeof(detail),
                 "mining dispatch failed after %llu complete batches",
                 (unsigned long long) observed.dispatch_batches);
        return result_with(BZM_RUNNING_EVIDENCE_BAD,
                           BZM_RUNNING_EVIDENCE_FAULT_DISPATCH,
                           &observed, elapsed_ms, detail);
    }
    if (observed.mapping_rejections != 0 && !config->allow_mapping_recovery) {
        snprintf(detail, sizeof(detail),
                 "%llu ASIC result frame(s) could not be attributed to current work; recovery is disabled",
                 (unsigned long long) observed.mapping_rejections);
        return result_with(BZM_RUNNING_EVIDENCE_BAD,
                           BZM_RUNNING_EVIDENCE_FAULT_MAPPING,
                           &observed, elapsed_ms, detail);
    }
    if (observed.mapping_rejection_streak >
        config->maximum_mapping_rejections) {
        snprintf(detail, sizeof(detail),
                 "%u consecutive ASIC result frame(s) could not be attributed to current work; total=%llu maximum streak=%u",
                 (unsigned) observed.mapping_rejection_streak,
                 (unsigned long long) observed.mapping_rejections,
                 (unsigned) config->maximum_mapping_rejections);
        return result_with(BZM_RUNNING_EVIDENCE_BAD,
                           BZM_RUNNING_EVIDENCE_FAULT_MAPPING,
                           &observed, elapsed_ms, detail);
    }
    if (observed.local_rejection_streak > config->maximum_local_rejections) {
        snprintf(detail, sizeof(detail),
                 "%u consecutive mapped result(s) failed local validation; total=%llu maximum streak=%u",
                 (unsigned) observed.local_rejection_streak,
                 (unsigned long long) observed.locally_rejected_results,
                 (unsigned) config->maximum_local_rejections);
        return result_with(BZM_RUNNING_EVIDENCE_BAD,
                           BZM_RUNNING_EVIDENCE_FAULT_LOCAL_VALIDATION,
                           &observed, elapsed_ms, detail);
    }
    if (observed.mapping_rejections != 0 && observed.mapping_recovery_pending) {
        snprintf(detail, sizeof(detail),
                 "waiting for valid proof after unattributed frames: total=%llu streak=%u/%u valid=%llu/%u",
                 (unsigned long long) observed.mapping_rejections,
                 (unsigned) observed.mapping_rejection_streak,
                 (unsigned) config->maximum_mapping_rejections,
                 (unsigned long long) observed.locally_valid_results,
                 (unsigned) config->minimum_valid_results);
        if (elapsed_ms >= config->proof_timeout_ms) {
            return result_with(BZM_RUNNING_EVIDENCE_BAD,
                               BZM_RUNNING_EVIDENCE_FAULT_MAPPING,
                               &observed, elapsed_ms, detail);
        }
        return result_with(BZM_RUNNING_EVIDENCE_PENDING,
                           BZM_RUNNING_EVIDENCE_FAULT_NONE,
                           &observed, elapsed_ms, detail);
    }
    if (observed.locally_rejected_results != 0 &&
        observed.local_recovery_pending) {
        snprintf(detail, sizeof(detail),
                 "waiting for valid proof after local rejection: total=%llu streak=%u/%u valid=%llu/%u",
                 (unsigned long long) observed.locally_rejected_results,
                 (unsigned) observed.local_rejection_streak,
                 (unsigned) config->maximum_local_rejections,
                 (unsigned long long) observed.locally_valid_results,
                 (unsigned) config->minimum_valid_results);
        if (elapsed_ms >= config->proof_timeout_ms) {
            return result_with(BZM_RUNNING_EVIDENCE_BAD,
                               BZM_RUNNING_EVIDENCE_FAULT_LOCAL_VALIDATION,
                               &observed, elapsed_ms, detail);
        }
        return result_with(BZM_RUNNING_EVIDENCE_PENDING,
                           BZM_RUNNING_EVIDENCE_FAULT_NONE,
                           &observed, elapsed_ms, detail);
    }

    if (proof_requirements_met(&observed, config)) {
        snprintf(detail, sizeof(detail),
                 "full dispatch: batches=%llu chipEngines=%llu mapped=%llu mappingRejected=%llu maxStreak=%u/%u valid=%llu localRejected=%llu maxStreak=%u/%u",
                 (unsigned long long) observed.dispatch_batches,
                 (unsigned long long) observed.dispatched_chip_engines,
                 (unsigned long long) observed.mapped_results,
                 (unsigned long long) observed.mapping_rejections,
                 (unsigned) observed.mapping_rejection_streak,
                 (unsigned) config->maximum_mapping_rejections,
                 (unsigned long long) observed.locally_valid_results,
                 (unsigned long long) observed.locally_rejected_results,
                 (unsigned) observed.local_rejection_streak,
                 (unsigned) config->maximum_local_rejections);
        return result_with(BZM_RUNNING_EVIDENCE_GOOD,
                           BZM_RUNNING_EVIDENCE_FAULT_NONE,
                           &observed, elapsed_ms, detail);
    }

    snprintf(detail, sizeof(detail),
             "waiting: batches=%llu chipEngines=%llu/%u mapped=%llu mappingRejected=%llu streak=%u/%u valid=%llu/%u localRejected=%llu streak=%u/%u",
             (unsigned long long) observed.dispatch_batches,
             (unsigned long long) observed.dispatched_chip_engines,
             (unsigned) config->required_chip_engine_writes,
             (unsigned long long) observed.mapped_results,
             (unsigned long long) observed.mapping_rejections,
             (unsigned) observed.mapping_rejection_streak,
             (unsigned) config->maximum_mapping_rejections,
             (unsigned long long) observed.locally_valid_results,
             (unsigned) config->minimum_valid_results,
             (unsigned long long) observed.locally_rejected_results,
             (unsigned) observed.local_rejection_streak,
             (unsigned) config->maximum_local_rejections);
    if (elapsed_ms >= config->proof_timeout_ms) {
        return result_with(BZM_RUNNING_EVIDENCE_BAD,
                           BZM_RUNNING_EVIDENCE_FAULT_TIMEOUT,
                           &observed, elapsed_ms, detail);
    }
    return result_with(BZM_RUNNING_EVIDENCE_PENDING,
                       BZM_RUNNING_EVIDENCE_FAULT_NONE,
                       &observed, elapsed_ms, detail);
}

void bzm_running_evidence_lifecycle_init(bzm_running_evidence_lifecycle_t * lifecycle)
{
    if (lifecycle != NULL)
        *lifecycle = (bzm_running_evidence_lifecycle_t){0};
}

bzm_running_evidence_result_t bzm_running_evidence_track(
    bzm_running_evidence_lifecycle_t * lifecycle,
    const bzm_running_stats_t * baseline,
    const bzm_running_stats_t * current,
    const bzm_running_evidence_config_t * config,
    uint64_t started_at_ms,
    uint64_t now_ms)
{
    uint64_t overall_elapsed_ms = now_ms >= started_at_ms ? now_ms - started_at_ms : UINT64_MAX;
    if (lifecycle == NULL) {
        return result_with(BZM_RUNNING_EVIDENCE_BAD,
                           BZM_RUNNING_EVIDENCE_FAULT_INVALID_CONFIGURATION,
                           NULL, overall_elapsed_ms,
                           "Stage-7 evidence lifecycle is unavailable");
    }

    /* Zero elapsed classifies immediate configuration/counter/streak faults
     * separately from the lifecycle timeout. */
    bzm_running_evidence_result_t result =
        bzm_running_evidence_evaluate(baseline, current, config, 0);
    result.elapsed_ms = overall_elapsed_ms;
    if (result.status == BZM_RUNNING_EVIDENCE_BAD)
        return result;
    if (!lifecycle->completed_once) {
        if (config != NULL && overall_elapsed_ms >= config->proof_timeout_ms) {
            return result_with(BZM_RUNNING_EVIDENCE_BAD,
                               BZM_RUNNING_EVIDENCE_FAULT_TIMEOUT,
                               &result.observed, overall_elapsed_ms,
                               "initial Stage-7 dispatch and local nonce proof missed its deadline");
        }
        if (result.status == BZM_RUNNING_EVIDENCE_GOOD) {
            lifecycle->completed_once = true;
            lifecycle->recovery_active = false;
            lifecycle->recovery_started_at_ms = 0;
            lifecycle->last_locally_valid_results = result.observed.locally_valid_results;
            return result;
        }
        if (proof_requirements_met(&result.observed, config)) {
            char pending[BZM_RUNNING_EVIDENCE_DETAIL_LENGTH];
            snprintf(pending, sizeof(pending), "%s", result.detail);
            lifecycle->completed_once = true;
            lifecycle->recovery_active = true;
            lifecycle->recovery_started_at_ms = now_ms;
            lifecycle->last_locally_valid_results = result.observed.locally_valid_results;
            result.status = BZM_RUNNING_EVIDENCE_GOOD;
            result.fault = BZM_RUNNING_EVIDENCE_FAULT_NONE;
            snprintf(result.detail, sizeof(result.detail),
                     "proof established; bounded recovery 0/%u ms: %.91s",
                     (unsigned) config->proof_timeout_ms, pending);
            return result;
        }
        return bzm_running_evidence_evaluate(baseline, current, config,
                                             overall_elapsed_ms);
    }

    if (result.status == BZM_RUNNING_EVIDENCE_GOOD) {
        lifecycle->recovery_active = false;
        lifecycle->recovery_started_at_ms = 0;
        lifecycle->last_locally_valid_results = result.observed.locally_valid_results;
        return result;
    }

    if (!lifecycle->recovery_active ||
        result.observed.locally_valid_results > lifecycle->last_locally_valid_results) {
        lifecycle->recovery_active = true;
        lifecycle->recovery_started_at_ms = now_ms;
    }
    lifecycle->last_locally_valid_results = result.observed.locally_valid_results;
    uint64_t recovery_elapsed_ms = now_ms >= lifecycle->recovery_started_at_ms
        ? now_ms - lifecycle->recovery_started_at_ms : UINT64_MAX;
    result = bzm_running_evidence_evaluate(baseline, current, config,
                                           recovery_elapsed_ms);
    result.elapsed_ms = overall_elapsed_ms;
    if (result.status == BZM_RUNNING_EVIDENCE_PENDING) {
        char pending[BZM_RUNNING_EVIDENCE_DETAIL_LENGTH];
        snprintf(pending, sizeof(pending), "%s", result.detail);
        result.status = BZM_RUNNING_EVIDENCE_GOOD;
        result.fault = BZM_RUNNING_EVIDENCE_FAULT_NONE;
        snprintf(result.detail, sizeof(result.detail),
                 "proof retained; bounded recovery %llu/%u ms: %.90s",
                 (unsigned long long) recovery_elapsed_ms,
                 config != NULL ? (unsigned) config->proof_timeout_ms : 0U,
                 pending);
    }
    return result;
}

const char * bzm_running_evidence_status_name(bzm_running_evidence_status_t status)
{
    switch (status) {
    case BZM_RUNNING_EVIDENCE_PENDING:
        return "PENDING";
    case BZM_RUNNING_EVIDENCE_GOOD:
        return "GOOD";
    case BZM_RUNNING_EVIDENCE_BAD:
        return "BAD";
    default:
        return "INVALID";
    }
}

const char * bzm_running_evidence_fault_name(bzm_running_evidence_fault_t fault)
{
    switch (fault) {
    case BZM_RUNNING_EVIDENCE_FAULT_NONE:
        return "NONE";
    case BZM_RUNNING_EVIDENCE_FAULT_COUNTER_ROLLBACK:
        return "COUNTER_ROLLBACK";
    case BZM_RUNNING_EVIDENCE_FAULT_DISPATCH:
        return "DISPATCH";
    case BZM_RUNNING_EVIDENCE_FAULT_MAPPING:
        return "MAPPING";
    case BZM_RUNNING_EVIDENCE_FAULT_LOCAL_VALIDATION:
        return "LOCAL_VALIDATION";
    case BZM_RUNNING_EVIDENCE_FAULT_TIMEOUT:
        return "TIMEOUT";
    case BZM_RUNNING_EVIDENCE_FAULT_INVALID_CONFIGURATION:
        return "INVALID_CONFIGURATION";
    default:
        return "INVALID";
    }
}
