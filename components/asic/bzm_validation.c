#include "bzm_validation.h"

#include <stdio.h>
#include <string.h>

static const bzm_stage_definition_t STAGES[BZM_STAGE_COUNT] = {
    [BZM_STAGE_OFF_SAFE] =
        {
            .stage = BZM_STAGE_OFF_SAFE,
            .name = "OFF_SAFE",
            .configuration = "targetStage=0",
            .good_criteria = "ASIC reset asserted; bridge 5V off; TPS enable and OPERATION "
                             "off; fresh PGOOD low; VCORE below the qualified discharge "
                             "threshold.",
            .bad_criteria = "Any energized/readback contradiction, failed shutdown action, "
                            "stale safety sample, or VCORE that does not discharge before "
                            "the deadline.",
        },
    [BZM_STAGE_CONTROLS] =
        {
            .stage = BZM_STAGE_CONTROLS,
            .name = "CONTROLS",
            .configuration = "buildMaxStage>=1; targetStage=1",
            .good_criteria = "Compatible bridge identity/protocol/capabilities; heartbeat "
                             "active; reset, 5V, and VCORE remain off; fan at 100 percent; "
                             "fresh tach above the qualified minimum.",
            .bad_criteria = "Bridge unavailable or incompatible, heartbeat absent, fan "
                            "command/tach invalid, or any controlled output differs from "
                            "its safe command.",
        },
    [BZM_STAGE_POWER_RAIL] =
        {
            .stage = BZM_STAGE_POWER_RAIL,
            .name = "POWER_RAIL",
            .configuration = "buildMaxStage>=2; powered stages compiled; local arm; "
                             "bounded lease; current-limited supply",
            .good_criteria = "TPS identity and qualified profile read back exactly; faults "
                             "clear; fixed 2.8V rail reaches PGOOD; VIN, VOUT, IOUT, and VR "
                             "temperature are fresh and within compiled limits while ASIC "
                             "reset remains asserted and 5V remains off.",
            .bad_criteria = "TPS/profile mismatch, stale or out-of-range telemetry, fault "
                            "status, unexpected reset/5V state, or PGOOD/VOUT timeout.",
            .energizes_asic_rail = true,
            .requires_local_arm = true,
        },
    [BZM_STAGE_CHAIN_4] =
        {
            .stage = BZM_STAGE_CHAIN_4,
            .name = "CHAIN_4",
            .configuration = "buildMaxStage>=3; targetStage=3; powered lease",
            .good_criteria = "Controlled 5V/reset release finds exactly four independently "
                             "responsive ASICs at spaced TDM IDs 0x0a, 0x14, 0x1e, and 0x28 and verifies each "
                             "control-register identity.",
            .bad_criteria = "Zero, partial, duplicate, extra, misaddressed, or subsequently "
                            "unresponsive ASICs.",
            .energizes_asic_rail = true,
            .requires_local_arm = true,
        },
    [BZM_STAGE_SENSORS] =
        {
            .stage = BZM_STAGE_SENSORS,
            .name = "SENSORS",
            .configuration = "buildMaxStage>=4; qualified thermal, stack-voltage, imbalance, "
                             "trip limits, and CH2 consecutive-confirmation count",
            .good_criteria =
                "Thermal/voltage sensor and trip configuration reads back on "
                "all four ASICs; four temperatures, eight CH0/CH1 stack voltages, "
                "and four CH2 inter-stack differentials have fresh valid samples "
                "inside the qualified limits. One all-ASIC TDM start preserves the "
                "spaced slots after the BIRDS first-ASIC 0x44464444 UART drive control "
                "reads back; fresh packets from all four ASICs prove that TDM start "
                "without injecting active-schedule register replies, and any bounded activation residue is followed by one "
                "full second with no parser errors. A finite CH2 excursion, including a "
                "voltage-fault bit in that same unchecksummed frame, may recover before "
                "the configured consecutive fresh-sample count.",
            .bad_criteria = "Any missing, stale, otherwise invalid, NaN, CH0/CH1 out-of-range or "
                            "imbalanced sample, readback mismatch, asserted trip, voltage fault without "
                            "a CH2 excursion, or CH2 anomaly that reaches the configured consecutive "
                            "fresh-sample count; a drive-control mismatch, failed synchronized TDM start, non-settling parser "
                            "residue, or any non-discard parser error.",
            .energizes_asic_rail = true,
            .requires_local_arm = true,
        },
    [BZM_STAGE_CLOCKS] =
        {
            .stage = BZM_STAGE_CLOCKS,
            .name = "CLOCKS",
            .configuration = "buildMaxStage>=5; validated 50MHz reference and conservative "
                             "800MHz startup PLL profile; live PLL-lock confirmation count",
            .good_criteria = "PLL0 and PLL1 on all four ASICs lock with symmetric settings; "
                             "DLL0/DLL1 controls read back disabled; TDM controls are re-proven; "
                             "the hard dispatch gate remains closed; fresh sensor checks and the "
                             "combined PLL-pair telemetry lock remain GOOD. An isolated clear in "
                             "the live unchecksummed telemetry bit may recover before the configured fresh-sample count.",
            .bad_criteria = "Initial direct lock timeout/loss, asymmetric or mismatched clock settings, "
                            "nonzero DLL control, an open dispatch gate, missing TDM/combined-PLL evidence, continuous live "
                            "combined-PLL clears reaching the configured count, or sensor degradation. There is no documented "
                            "active-engine-count register.",
            .energizes_asic_rail = true,
            .requires_local_arm = true,
        },
    [BZM_STAGE_BALANCED_RAMP] =
        {
            .stage = BZM_STAGE_BALANCED_RAMP,
            .name = "BALANCED_RAMP",
            .configuration = "buildMaxStage>=6; CONFIG_BZM_1002_STAGE6_BALANCED_RAMP=y; "
                             "deterministic sentinel work; bounded telemetry confirmation",
            .good_criteria = "236 valid engines per ASIC and 944 total activate as 472 acknowledged "
                             "higher-voltage-first bottom/top pair commits; transient skew is at most "
                             "one engine; every engine reads back busy plus writable config 0x04, "
                             "allowing hardware active bit 0x10; TDM pause/resume and clean engine "
                             "parser windows are proven with result reporting disabled; every batch receives newer safe telemetry; "
                             "isolated TDM anomalies recover within the configured confirmation count; "
                             "the final parser barrier has no sentinel result or fault.",
            .bad_criteria = "Toggle disabled, engine reset or TDM readback failure, invalid coordinate, "
                            "partial pair, missing busy/config acknowledgement, stack imbalance, stale "
                            "telemetry, an immediate trip or continuous telemetry anomaly, lease loss, "
                            "engine-window parser fault, or escaped sentinel result.",
            .energizes_asic_rail = true,
            .requires_local_arm = true,
        },
    [BZM_STAGE_RUNNING] =
        {
            .stage = BZM_STAGE_RUNNING,
            .name = "RUNNING",
            .configuration = "buildMaxStage>=7; CONFIG_BZM_1002_STAGE7_MINING=y; "
                             "qualified 800MHz/2.8V profile; leased proof timeout; "
                             "CONFIG_BZM_1002_STAGE7_LEAD_ZEROS ASIC result filter; "
                             "CONFIG_BZM_1002_SENSOR_TDM_GAP_COUNT telemetry cadence; "
                             "CONFIG_BZM_1002_STAGE7_DISPATCH_GAP_US bridge drain cadence; optional bounded mapping and parser recovery",
            .good_criteria = "Real templates start only after stages 0 through 6; at least one "
                             "complete 236-engine x four-ASIC dispatch and the configured number "
                             "of current-work nonces meet the local difficulty floor; TDM-framed result reporting is enabled and "
                             "read back only at Stage 7, followed by a full clean one-second parser settling window; clean-job "
                             "barriers, per-engine TX/RX checkpoints service the fixed bridge safety lease at a maximum 250 ms "
                             "cadence; pause, shutdown, and all live monitors remain GOOD; locally "
                             "invalid current-work results "
                             "remain at or below the configured transient bound; a complete dispatch plus a locally valid nonce "
                             "establishes proof even when bounded recovery is pending, and that recovery receives an independent "
                             "timeout; after proof, completion is retained during later bounded recovery; enabled parser realignment remains within all configured "
                             "byte and window limits, with the event count bounding new discard bursts inside one unresolved episode.",
            .bad_criteria =
                "Toggle disabled, failed bridge-lease or transport checkpoint, partial/asymmetric or failed dispatch, stale or "
                "disabled, excessive, or unrecovered result attribution corruption, local work/nonce rejections above the "
                "configured bound, initial proof or independent recovery timeout, or "
                "disabled, out-of-bounds, continuous, or non-discard parser corruption; continuous live PLL-lock clears; or any chain, fan, bridge, TPS, sensor, "
                "clock, trip, or lease fault.",
            .energizes_asic_rail = true,
            .requires_local_arm = true,
        },
};

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

const bzm_stage_definition_t * bzm_validation_stage_definition(bzm_validation_stage_t stage)
{
    return valid_stage(stage) ? &STAGES[stage] : NULL;
}

const char * bzm_validation_stage_name(bzm_validation_stage_t stage)
{
    const bzm_stage_definition_t * definition = bzm_validation_stage_definition(stage);
    return definition != NULL ? definition->name : "INVALID_STAGE";
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
                                                      "a lower validation stage did not complete GOOD");
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
            return blocked(BZM_VALIDATION_CODE_LOCAL_ARM_REQUIRED, "powered validation requires a fresh local arm");
        }
        bool attended_lab_authority = !policy->production_mode && policy->allow_esp_only_kill_in_lab;
        if (!policy->independent_kill_available && !policy->board_managed_safety && !attended_lab_authority) {
            return blocked(BZM_VALIDATION_CODE_INDEPENDENT_KILL_REQUIRED,
                           "neither independent kill nor accepted board-managed fixed-voltage safety is configured");
        }
        if (policy->lease_ms == 0) {
            return blocked(BZM_VALIDATION_CODE_POWER_LEASE_REQUIRED, "powered validation requires a bounded lease");
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
            blocked(BZM_VALIDATION_CODE_INVALID_CONFIGURATION, "validation stage runner is unavailable");
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
        if (STAGES[stage].energizes_asic_rail)
            report->energized = true;
    }

    if (policy->requested_stage >= BZM_STAGE_POWER_RAIL && policy->hold_after_success) {
        report->state = BZM_VALIDATION_HOLDING;
        report->energized = true;
        return true;
    }

    return force_safe_off(ops, ops_context, report);
}
