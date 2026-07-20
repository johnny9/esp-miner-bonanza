#include "bzm_supervisor.h"

#include <stdio.h>
#include <string.h>

static bool valid_stage(bzm_validation_stage_t stage)
{
    return stage >= BZM_STAGE_OFF_SAFE && stage < BZM_STAGE_COUNT;
}

static bool maintenance_owner(bzm_supervisor_owner_t owner)
{
    return owner == BZM_SUPERVISOR_OWNER_BRIDGE_UPDATE || owner == BZM_SUPERVISOR_OWNER_FACTORY_TEST ||
           owner == BZM_SUPERVISOR_OWNER_ESP_OTA || owner == BZM_SUPERVISOR_OWNER_ESP_RESTART;
}

bool bzm_supervisor_owner_is_maintenance(bzm_supervisor_owner_t owner)
{
    return maintenance_owner(owner);
}

static bool good(const bzm_stage_result_t * result)
{
    return result != NULL && result->status == BZM_CHECK_GOOD;
}

static void set_fault_detail(bzm_supervisor_t * supervisor, const char * detail)
{
    snprintf(supervisor->fault_detail, sizeof(supervisor->fault_detail), "%s",
             detail != NULL ? detail : "unspecified Bonanza fault");
}

const char * bzm_supervisor_owner_name(bzm_supervisor_owner_t owner)
{
    switch (owner) {
    case BZM_SUPERVISOR_OWNER_NONE:
        return "NONE";
    case BZM_SUPERVISOR_OWNER_VALIDATION:
        return "VALIDATION";
    case BZM_SUPERVISOR_OWNER_MINING:
        return "MINING";
    case BZM_SUPERVISOR_OWNER_BRIDGE_UPDATE:
        return "BRIDGE_UPDATE";
    case BZM_SUPERVISOR_OWNER_FACTORY_TEST:
        return "FACTORY_TEST";
    case BZM_SUPERVISOR_OWNER_ESP_OTA:
        return "ESP_OTA";
    case BZM_SUPERVISOR_OWNER_ESP_RESTART:
        return "ESP_RESTART";
    default:
        return "INVALID_OWNER";
    }
}

bool bzm_supervisor_init(bzm_supervisor_t * supervisor, const bzm_supervisor_config_t * config,
                         const bzm_validation_ops_t * validation_ops, void * ops_context)
{
    if (supervisor == NULL || config == NULL || validation_ops == NULL || validation_ops->run_stage == NULL ||
        validation_ops->force_safe_off == NULL || !valid_stage(config->build_max_stage) ||
        !valid_stage(config->implemented_max_stage) || config->build_max_stage > config->implemented_max_stage ||
        config->maximum_lease_ms == 0) {
        return false;
    }

    memset(supervisor, 0, sizeof(*supervisor));
    supervisor->config = *config;
    supervisor->validation_ops = *validation_ops;
    supervisor->ops_context = ops_context;
    supervisor->owner = BZM_SUPERVISOR_OWNER_NONE;
    supervisor->next_run_id = 1;
    supervisor->initialized = true;
    return true;
}

static void block_report(bzm_supervisor_t * supervisor, bzm_validation_stage_t target_stage, const char * detail)
{
    memset(&supervisor->report, 0, sizeof(supervisor->report));
    supervisor->report.schema_version = BZM_VALIDATION_SCHEMA_VERSION;
    supervisor->report.run_id = supervisor->next_run_id++;
    supervisor->report.requested_stage = target_stage;
    supervisor->report.build_max_stage = supervisor->config.build_max_stage;
    supervisor->report.implemented_max_stage = supervisor->config.implemented_max_stage;
    supervisor->report.overall = BZM_CHECK_BLOCKED;
    supervisor->report.state = BZM_VALIDATION_OFF_SAFE;
    supervisor->report.stages[target_stage] =
        bzm_validation_result(BZM_CHECK_BLOCKED, BZM_VALIDATION_CODE_PREREQUISITE_FAILED, detail);
    supervisor->report.final_safe_off =
        bzm_validation_result(BZM_CHECK_NOT_RUN, BZM_VALIDATION_CODE_NONE, "request was blocked before hardware execution");
}

static bool stop_internal(bzm_supervisor_t * supervisor, const char * reason, bool preempt_maintenance)
{
    if (supervisor == NULL || !supervisor->initialized || (!preempt_maintenance && maintenance_owner(supervisor->owner))) {
        return false;
    }

    bzm_stage_result_t safe_off = supervisor->validation_ops.force_safe_off(supervisor->ops_context);
    supervisor->report.final_safe_off = safe_off;
    supervisor->lease_deadline_ms = 0;
    supervisor->owner = BZM_SUPERVISOR_OWNER_NONE;
    supervisor->report.energized = safe_off.status != BZM_CHECK_GOOD;

    if (safe_off.status != BZM_CHECK_GOOD) {
        supervisor->fault_latched = true;
        if (supervisor->fault_code == 0) {
            supervisor->fault_code = BZM_VALIDATION_CODE_SAFE_OFF_FAILED;
            set_fault_detail(supervisor, safe_off.detail[0] != '\0' ? safe_off.detail : reason);
        }
        supervisor->report.overall = BZM_CHECK_BAD;
        supervisor->report.state = BZM_VALIDATION_SHUTDOWN_UNVERIFIED;
        return false;
    }

    supervisor->report.state = supervisor->fault_latched ? BZM_VALIDATION_FAULT_LATCHED : BZM_VALIDATION_OFF_SAFE;
    return true;
}

bool bzm_supervisor_stop(bzm_supervisor_t * supervisor, const char * reason)
{
    return stop_internal(supervisor, reason, false);
}

bool bzm_supervisor_request_validation(bzm_supervisor_t * supervisor, bzm_validation_stage_t target_stage, bool local_arm_present,
                                       bool hold_after_success, uint32_t lease_ms, uint64_t now_ms)
{
    if (supervisor == NULL || !supervisor->initialized || !valid_stage(target_stage)) {
        return false;
    }
    if (maintenance_owner(supervisor->owner)) {
        block_report(supervisor, target_stage, "exclusive maintenance lease is active");
        return false;
    }
    if (supervisor->fault_latched && target_stage != BZM_STAGE_OFF_SAFE) {
        block_report(supervisor, target_stage, "latched fault requires a new OFF_SAFE proof and clear");
        bzm_supervisor_stop(supervisor, "fault-latched start rejected");
        return false;
    }
    if (lease_ms > supervisor->config.maximum_lease_ms) {
        block_report(supervisor, target_stage, "requested lease exceeds the compiled maximum");
        bzm_supervisor_stop(supervisor, "invalid validation lease");
        return false;
    }
    if (supervisor->owner != BZM_SUPERVISOR_OWNER_NONE && !bzm_supervisor_stop(supervisor, "replacing active validation")) {
        return false;
    }

    bzm_validation_policy_t policy = {
        .run_id = supervisor->next_run_id++,
        .requested_stage = target_stage,
        .build_max_stage = supervisor->config.build_max_stage,
        .implemented_max_stage = supervisor->config.implemented_max_stage,
        .powered_stages_compiled = supervisor->config.powered_stages_compiled,
        .local_arm_present = local_arm_present,
        .production_mode = supervisor->config.production_mode,
        .independent_kill_available = supervisor->config.independent_kill_available,
        .allow_esp_only_kill_in_lab = supervisor->config.allow_esp_only_kill_in_lab,
        .board_managed_safety = supervisor->config.board_managed_safety,
        .hold_after_success = hold_after_success,
        .lease_ms = lease_ms,
    };

    bool completed = bzm_validation_execute(&policy, &supervisor->validation_ops, supervisor->ops_context, &supervisor->report);
    if (!completed) {
        supervisor->owner = BZM_SUPERVISOR_OWNER_NONE;
        supervisor->lease_deadline_ms = 0;
        if (supervisor->report.overall == BZM_CHECK_BAD) {
            supervisor->fault_latched = true;
            if (supervisor->fault_code == 0) {
                supervisor->fault_code = BZM_VALIDATION_CODE_STAGE_FAILED;
                set_fault_detail(supervisor, "startup step failed");
            }
        }
        if (supervisor->fault_latched && supervisor->report.state != BZM_VALIDATION_SHUTDOWN_UNVERIFIED) {
            supervisor->report.state = BZM_VALIDATION_FAULT_LATCHED;
        }
        return false;
    }

    if (supervisor->report.state == BZM_VALIDATION_HOLDING) {
        supervisor->owner = target_stage == BZM_STAGE_RUNNING ? BZM_SUPERVISOR_OWNER_MINING : BZM_SUPERVISOR_OWNER_VALIDATION;
        supervisor->lease_deadline_ms = now_ms + lease_ms;
    } else {
        supervisor->owner = BZM_SUPERVISOR_OWNER_NONE;
        supervisor->lease_deadline_ms = 0;
    }
    if (supervisor->fault_latched) {
        supervisor->report.state = BZM_VALIDATION_FAULT_LATCHED;
    }
    return true;
}

bool bzm_supervisor_tick(bzm_supervisor_t * supervisor, uint64_t now_ms)
{
    if (supervisor == NULL || !supervisor->initialized)
        return false;
    if ((supervisor->owner == BZM_SUPERVISOR_OWNER_VALIDATION || supervisor->owner == BZM_SUPERVISOR_OWNER_MINING) &&
        supervisor->lease_deadline_ms != 0 && now_ms >= supervisor->lease_deadline_ms) {
        return bzm_supervisor_stop(supervisor, "validation lease expired");
    }
    return true;
}

bool bzm_supervisor_heartbeat(bzm_supervisor_t * supervisor, uint32_t lease_ms, uint64_t now_ms)
{
    if (supervisor == NULL || !supervisor->initialized ||
        (supervisor->owner != BZM_SUPERVISOR_OWNER_VALIDATION && supervisor->owner != BZM_SUPERVISOR_OWNER_MINING) ||
        supervisor->report.state != BZM_VALIDATION_HOLDING || lease_ms == 0 || lease_ms > supervisor->config.maximum_lease_ms ||
        now_ms >= supervisor->lease_deadline_ms) {
        return false;
    }
    supervisor->lease_deadline_ms = now_ms + lease_ms;
    supervisor->report.lease_ms = lease_ms;
    return true;
}

bool bzm_supervisor_latch_fault(bzm_supervisor_t * supervisor, uint32_t fault_code, const char * detail)
{
    if (supervisor == NULL || !supervisor->initialized || fault_code == 0) {
        return false;
    }
    if (!supervisor->fault_latched) {
        supervisor->fault_code = fault_code;
        set_fault_detail(supervisor, detail);
    }
    supervisor->fault_latched = true;
    bool off = stop_internal(supervisor, detail, true);
    supervisor->report.overall = BZM_CHECK_BAD;
    if (off)
        supervisor->report.state = BZM_VALIDATION_FAULT_LATCHED;
    return off;
}

bool bzm_supervisor_safe_off_verified(const bzm_supervisor_t * supervisor)
{
    return supervisor != NULL && supervisor->initialized && !supervisor->report.energized &&
           supervisor->report.stages[BZM_STAGE_OFF_SAFE].status == BZM_CHECK_GOOD && good(&supervisor->report.final_safe_off) &&
           (supervisor->report.state == BZM_VALIDATION_OFF_SAFE || supervisor->report.state == BZM_VALIDATION_FAULT_LATCHED);
}

bool bzm_supervisor_clear_fault(bzm_supervisor_t * supervisor)
{
    if (supervisor == NULL || !supervisor->initialized || !supervisor->fault_latched ||
        supervisor->report.requested_stage != BZM_STAGE_OFF_SAFE || !bzm_supervisor_safe_off_verified(supervisor)) {
        return false;
    }
    supervisor->fault_latched = false;
    supervisor->fault_code = 0;
    supervisor->fault_detail[0] = '\0';
    supervisor->report.state = BZM_VALIDATION_OFF_SAFE;
    return true;
}

bool bzm_supervisor_acquire_maintenance(bzm_supervisor_t * supervisor, bzm_supervisor_owner_t owner, uint64_t now_ms)
{
    if (supervisor == NULL || !supervisor->initialized || !maintenance_owner(owner) ||
        maintenance_owner(supervisor->owner)) {
        return false;
    }

    /* OTA and factory operations are normal production transitions. If the
     * miner currently owns the hardware, first revoke dispatch and complete
     * the same verified safe-off used by stop/restart. A maintenance owner is
     * never preempted. Starting from NONE still runs a fresh OFF_SAFE stage so
     * an old report cannot authorize maintenance. */
    bool safe = supervisor->owner == BZM_SUPERVISOR_OWNER_NONE
        ? bzm_supervisor_request_validation(supervisor, BZM_STAGE_OFF_SAFE,
                                            false, false, 0, now_ms)
        : bzm_supervisor_stop(supervisor, "maintenance requested");
    if (!safe || !bzm_supervisor_safe_off_verified(supervisor)) {
        return false;
    }
    /* A latched runtime fault must not make a physically safe unit
     * unmaintainable. Clear it only after this acquisition has forced and
     * freshly verified OFF_SAFE; shutdown-unverified faults still fail above. */
    if (supervisor->fault_latched && !bzm_supervisor_clear_fault(supervisor)) {
        return false;
    }
    supervisor->owner = owner;
    return true;
}

bool bzm_supervisor_release_maintenance(bzm_supervisor_t * supervisor, bzm_supervisor_owner_t owner)
{
    if (supervisor == NULL || !supervisor->initialized || !maintenance_owner(owner) || supervisor->owner != owner) {
        return false;
    }
    bool off = stop_internal(supervisor, "maintenance released", true);
    supervisor->owner = BZM_SUPERVISOR_OWNER_NONE;
    return off;
}

bool bzm_supervisor_prepare_restart(bzm_supervisor_t * supervisor)
{
    if (supervisor == NULL || !supervisor->initialized || maintenance_owner(supervisor->owner)) {
        return false;
    }

    /*
     * A restart may preempt validation or mining, but never another updater or
     * factory operation. Keep exclusive ownership until reset so no request
     * can re-energize the rail in the response-to-reboot window.
     */
    if (!bzm_supervisor_stop(supervisor, "ESP restart requested") || !bzm_supervisor_safe_off_verified(supervisor)) {
        return false;
    }
    supervisor->owner = BZM_SUPERVISOR_OWNER_ESP_RESTART;
    return true;
}

bool bzm_supervisor_dispatch_allowed(const bzm_supervisor_t * supervisor, uint64_t now_ms)
{
    return supervisor != NULL && supervisor->initialized && !supervisor->fault_latched &&
           supervisor->owner == BZM_SUPERVISOR_OWNER_MINING && supervisor->report.state == BZM_VALIDATION_HOLDING &&
           supervisor->report.overall == BZM_CHECK_GOOD && supervisor->report.reached_stage == BZM_STAGE_RUNNING &&
           supervisor->lease_deadline_ms > now_ms;
}

const bzm_validation_report_t * bzm_supervisor_report(const bzm_supervisor_t * supervisor)
{
    return supervisor != NULL && supervisor->initialized ? &supervisor->report : NULL;
}
