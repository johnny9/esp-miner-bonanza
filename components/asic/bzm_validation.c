#include "bzm_validation.h"

#include <stdio.h>
#include <string.h>

static void copy_detail(char destination[BZM_VALIDATION_DETAIL_LENGTH], const char * detail)
{
    if (detail == NULL)
        detail = "";
    snprintf(destination, BZM_VALIDATION_DETAIL_LENGTH, "%s", detail);
}

static bool valid_stage(bzm_validation_stage_t stage)
{
    return stage >= BZM_STAGE_OFF_SAFE && stage < BZM_STAGE_COUNT;
}

const char * bzm_validation_stage_name(bzm_validation_stage_t stage)
{
    switch (stage) {
    case BZM_STAGE_OFF_SAFE: return "OFF_SAFE";
    case BZM_STAGE_CONTROLS: return "CONTROLS";
    case BZM_STAGE_POWER_RAIL: return "POWER_RAIL";
    case BZM_STAGE_CHAIN_4: return "CHAIN_4";
    case BZM_STAGE_SENSORS: return "SENSORS";
    case BZM_STAGE_CLOCKS: return "CLOCKS";
    case BZM_STAGE_BALANCED_RAMP: return "BALANCED_RAMP";
    case BZM_STAGE_RUNNING: return "RUNNING";
    default: return "INVALID_STAGE";
    }
}

const char * bzm_validation_status_name(bzm_check_status_t status)
{
    switch (status) {
    case BZM_CHECK_NOT_RUN:
        return "NOT_RUN";
    case BZM_CHECK_RUNNING:
        return "RUNNING";
    case BZM_CHECK_GOOD:
        return "GOOD";
    case BZM_CHECK_BAD:
        return "BAD";
    case BZM_CHECK_BLOCKED:
        return "BLOCKED";
    case BZM_CHECK_SKIPPED:
        return "SKIPPED";
    default:
        return "INVALID_STATUS";
    }
}

const char * bzm_validation_code_name(bzm_validation_code_t code)
{
    switch (code) {
    case BZM_VALIDATION_CODE_NONE:
        return "NONE";
    case BZM_VALIDATION_CODE_STAGE_OK:
        return "STAGE_OK";
    case BZM_VALIDATION_CODE_STAGE_FAILED:
        return "STAGE_FAILED";
    case BZM_VALIDATION_CODE_INVALID_CONFIGURATION:
        return "INVALID_CONFIGURATION";
    case BZM_VALIDATION_CODE_BUILD_CEILING:
        return "BUILD_CEILING";
    case BZM_VALIDATION_CODE_NOT_IMPLEMENTED:
        return "NOT_IMPLEMENTED";
    case BZM_VALIDATION_CODE_POWER_NOT_COMPILED:
        return "POWER_NOT_COMPILED";
    case BZM_VALIDATION_CODE_LOCAL_ARM_REQUIRED:
        return "LOCAL_ARM_REQUIRED";
    case BZM_VALIDATION_CODE_INDEPENDENT_KILL_REQUIRED:
        return "INDEPENDENT_KILL_REQUIRED";
    case BZM_VALIDATION_CODE_POWER_LEASE_REQUIRED:
        return "POWER_LEASE_REQUIRED";
    case BZM_VALIDATION_CODE_PREREQUISITE_FAILED:
        return "PREREQUISITE_FAILED";
    case BZM_VALIDATION_CODE_SAFE_OFF_FAILED:
        return "SAFE_OFF_FAILED";
    default:
        return "INVALID_CODE";
    }
}

bzm_stage_result_t bzm_validation_result(bzm_check_status_t status, bzm_validation_code_t code, const char * detail)
{
    bzm_stage_result_t result = {
        .status = status,
        .code = code,
    };
    copy_detail(result.detail, detail);
    return result;
}

static bzm_stage_result_t blocked(bzm_validation_code_t code, const char * detail)
{
    return bzm_validation_result(BZM_CHECK_BLOCKED, code, detail);
}

static void initialize_report(const bzm_validation_policy_t * policy, bzm_validation_report_t * report)
{
    memset(report, 0, sizeof(*report));
    report->schema_version = BZM_VALIDATION_SCHEMA_VERSION;
    report->run_id = policy != NULL ? policy->run_id : 0;
    report->requested_stage = policy != NULL ? policy->requested_stage : BZM_STAGE_OFF_SAFE;
    report->build_max_stage = policy != NULL ? policy->build_max_stage : BZM_STAGE_OFF_SAFE;
    report->implemented_max_stage = policy != NULL ? policy->implemented_max_stage : BZM_STAGE_OFF_SAFE;
    report->reached_stage = BZM_STAGE_OFF_SAFE;
    report->overall = BZM_CHECK_NOT_RUN;
    report->state = BZM_VALIDATION_IDLE;
    report->lease_ms = policy != NULL ? policy->lease_ms : 0;
    for (size_t i = 0; i < BZM_STAGE_COUNT; ++i) {
        report->stages[i] = bzm_validation_result(BZM_CHECK_NOT_RUN, BZM_VALIDATION_CODE_NONE, "not requested");
    }
    report->final_safe_off = bzm_validation_result(BZM_CHECK_NOT_RUN, BZM_VALIDATION_CODE_NONE, "final safe-off has not run");
}

static bzm_stage_result_t normalize_stage_result(bzm_stage_result_t result)
{
    if (result.status == BZM_CHECK_GOOD) {
        if (result.code == BZM_VALIDATION_CODE_NONE) {
            result.code = BZM_VALIDATION_CODE_STAGE_OK;
        }
        return result;
    }
    if (result.status == BZM_CHECK_BAD || result.status == BZM_CHECK_BLOCKED) {
        if (result.code == BZM_VALIDATION_CODE_NONE) {
            result.code = BZM_VALIDATION_CODE_STAGE_FAILED;
        }
        return result;
    }
    return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                 "stage runner returned an invalid terminal status");
}

static bool force_safe_off(const bzm_validation_ops_t * ops, void * ops_context, bzm_validation_report_t * report)
{
    bzm_stage_result_t result =
        ops != NULL && ops->force_safe_off != NULL
            ? normalize_stage_result(ops->force_safe_off(ops_context))
            : bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_SAFE_OFF_FAILED, "safe-off operation is unavailable");
    if (result.status != BZM_CHECK_GOOD) {
        result.status = BZM_CHECK_BAD;
        result.code = BZM_VALIDATION_CODE_SAFE_OFF_FAILED;
        if (result.detail[0] == '\0') {
            copy_detail(result.detail, "safe-off could not be verified");
        }
        report->final_safe_off = result;
        report->overall = BZM_CHECK_BAD;
        report->state = BZM_VALIDATION_SHUTDOWN_UNVERIFIED;
        report->energized = true;
        return false;
    }
    report->final_safe_off = result;
    report->state = report->overall == BZM_CHECK_BAD ? BZM_VALIDATION_FAULT_LATCHED : BZM_VALIDATION_OFF_SAFE;
    report->energized = false;
    return true;
}

static void skip_after(bzm_validation_report_t * report, bzm_validation_stage_t failed_stage)
{
    for (int stage = failed_stage + 1; stage <= report->requested_stage && stage < BZM_STAGE_COUNT; ++stage) {
        report->stages[stage] = bzm_validation_result(BZM_CHECK_SKIPPED, BZM_VALIDATION_CODE_PREREQUISITE_FAILED,
                                                      "an earlier startup step did not complete GOOD");
    }
}

static bzm_stage_result_t validate_policy(const bzm_validation_policy_t * policy)
{
    if (policy == NULL || !valid_stage(policy->requested_stage) || !valid_stage(policy->build_max_stage) ||
        !valid_stage(policy->implemented_max_stage)) {
        return blocked(BZM_VALIDATION_CODE_INVALID_CONFIGURATION, "stage values are outside the validation schema");
    }
    if (policy->build_max_stage > policy->implemented_max_stage) {
        return blocked(BZM_VALIDATION_CODE_NOT_IMPLEMENTED, "build ceiling exceeds the implemented stage ceiling");
    }
    if (policy->requested_stage > policy->build_max_stage) {
        return blocked(BZM_VALIDATION_CODE_BUILD_CEILING, "requested stage exceeds this image's build ceiling");
    }
    if (policy->requested_stage >= BZM_STAGE_POWER_RAIL) {
        if (!policy->powered_stages_compiled) {
            return blocked(BZM_VALIDATION_CODE_POWER_NOT_COMPILED, "this image cannot energize the ASIC rail");
        }
        if (!policy->local_arm_present) {
            return blocked(BZM_VALIDATION_CODE_LOCAL_ARM_REQUIRED, "powered startup requires local controller authority");
        }
        bool attended_lab_authority = !policy->production_mode && policy->allow_esp_only_kill_in_lab;
        if (!policy->independent_kill_available && !policy->board_managed_safety && !attended_lab_authority) {
            return blocked(BZM_VALIDATION_CODE_INDEPENDENT_KILL_REQUIRED,
                           "neither independent kill nor accepted board-managed fixed-voltage safety is configured");
        }
        if (policy->lease_ms == 0) {
            return blocked(BZM_VALIDATION_CODE_POWER_LEASE_REQUIRED, "powered startup requires a bounded watchdog");
        }
    }
    return bzm_validation_result(BZM_CHECK_GOOD, BZM_VALIDATION_CODE_STAGE_OK, "validation policy accepted");
}

bool bzm_validation_execute(const bzm_validation_policy_t * policy, const bzm_validation_ops_t * ops, void * ops_context,
                            bzm_validation_report_t * report)
{
    if (report == NULL)
        return false;
    initialize_report(policy, report);

    if (ops == NULL || ops->run_stage == NULL) {
        report->overall = BZM_CHECK_BLOCKED;
        report->stages[BZM_STAGE_OFF_SAFE] =
            blocked(BZM_VALIDATION_CODE_INVALID_CONFIGURATION, "startup step runner is unavailable");
        force_safe_off(ops, ops_context, report);
        return false;
    }

    bzm_stage_result_t policy_result = validate_policy(policy);
    if (policy_result.status != BZM_CHECK_GOOD) {
        bzm_validation_stage_t blocked_stage = BZM_STAGE_OFF_SAFE;
        if (policy != NULL && valid_stage(policy->requested_stage)) {
            blocked_stage = policy->requested_stage;
        }
        report->overall = BZM_CHECK_BLOCKED;
        report->stages[blocked_stage] = policy_result;
        force_safe_off(ops, ops_context, report);
        return false;
    }

    report->state = BZM_VALIDATION_EXECUTING;
    report->overall = BZM_CHECK_GOOD;
    for (int stage = BZM_STAGE_OFF_SAFE; stage <= policy->requested_stage; ++stage) {
        report->stages[stage] = bzm_validation_result(BZM_CHECK_RUNNING, BZM_VALIDATION_CODE_NONE, "stage is executing");
        bzm_stage_result_t result = normalize_stage_result(ops->run_stage(ops_context, (bzm_validation_stage_t) stage));
        report->stages[stage] = result;
        if (result.status != BZM_CHECK_GOOD) {
            report->overall = result.status;
            skip_after(report, (bzm_validation_stage_t) stage);
            force_safe_off(ops, ops_context, report);
            return false;
        }
        report->reached_stage = (bzm_validation_stage_t) stage;
        if (stage >= BZM_STAGE_POWER_RAIL)
            report->energized = true;
    }

    if (policy->requested_stage >= BZM_STAGE_POWER_RAIL && policy->hold_after_success) {
        report->state = BZM_VALIDATION_HOLDING;
        report->energized = true;
        return true;
    }

    return force_safe_off(ops, ops_context, report);
}
