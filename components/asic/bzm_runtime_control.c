#include "bzm_runtime_control.h"

static bool valid_stage(bzm_validation_stage_t stage)
{
    return stage >= BZM_STAGE_OFF_SAFE && stage < BZM_STAGE_COUNT;
}

bzm_runtime_gate_result_t bzm_runtime_validate_request(const bzm_runtime_gate_t * gate, const bzm_runtime_request_t * request)
{
    if (gate == NULL || request == NULL || !valid_stage(gate->build_max_stage) || gate->maximum_lease_ms == 0 ||
        !valid_stage(request->target_stage)) {
        return BZM_RUNTIME_GATE_INVALID_STAGE;
    }
    if (request->target_stage > gate->build_max_stage) {
        return BZM_RUNTIME_GATE_BUILD_CEILING;
    }

    /* Lowering to OFF_SAFE must always be possible, even during a fault. */
    if (request->target_stage == BZM_STAGE_OFF_SAFE) {
        return BZM_RUNTIME_GATE_ACCEPTED;
    }
    if (gate->busy)
        return BZM_RUNTIME_GATE_BUSY;
    if (gate->fault_latched)
        return BZM_RUNTIME_GATE_FAULT_LATCHED;

    if (request->target_stage < BZM_STAGE_POWER_RAIL) {
        return request->lease_ms == 0 ? BZM_RUNTIME_GATE_ACCEPTED : BZM_RUNTIME_GATE_LEASE_TOO_LONG;
    }
    if (!gate->powered_stages_compiled) {
        return BZM_RUNTIME_GATE_POWER_NOT_COMPILED;
    }
    if (!request->confirmation_valid) {
        return BZM_RUNTIME_GATE_CONFIRMATION_REQUIRED;
    }
    if (!request->local_arm_present) {
        return BZM_RUNTIME_GATE_LOCAL_ARM_REQUIRED;
    }
    if (request->lease_ms == 0) {
        return BZM_RUNTIME_GATE_LEASE_REQUIRED;
    }
    if (request->lease_ms > gate->maximum_lease_ms) {
        return BZM_RUNTIME_GATE_LEASE_TOO_LONG;
    }
    return BZM_RUNTIME_GATE_ACCEPTED;
}

bzm_runtime_gate_result_t bzm_runtime_validate_request_with_local_arm(const bzm_runtime_gate_t * gate,
                                                                      bzm_runtime_request_t * request, bzm_local_arm_t * local_arm,
                                                                      uint64_t now_ms)
{
    if (request == NULL)
        return BZM_RUNTIME_GATE_INVALID_STAGE;
    if (request->target_stage < BZM_STAGE_POWER_RAIL) {
        return bzm_runtime_validate_request(gate, request);
    }

    /* First prove every non-arm precondition so invalid network requests do
     * not consume the local one-shot token. */
    bzm_runtime_request_t preflight = *request;
    preflight.local_arm_present = true;
    bzm_runtime_gate_result_t result = bzm_runtime_validate_request(gate, &preflight);
    if (result != BZM_RUNTIME_GATE_ACCEPTED)
        return result;

    request->local_arm_present = bzm_local_arm_consume(local_arm, now_ms);
    return bzm_runtime_validate_request(gate, request);
}

const char * bzm_runtime_gate_result_name(bzm_runtime_gate_result_t result)
{
    switch (result) {
    case BZM_RUNTIME_GATE_ACCEPTED:
        return "ACCEPTED";
    case BZM_RUNTIME_GATE_INVALID_STAGE:
        return "INVALID_STAGE";
    case BZM_RUNTIME_GATE_BUILD_CEILING:
        return "BUILD_CEILING";
    case BZM_RUNTIME_GATE_POWER_NOT_COMPILED:
        return "POWER_NOT_COMPILED";
    case BZM_RUNTIME_GATE_CONFIRMATION_REQUIRED:
        return "CONFIRMATION_REQUIRED";
    case BZM_RUNTIME_GATE_LOCAL_ARM_REQUIRED:
        return "LOCAL_ARM_REQUIRED";
    case BZM_RUNTIME_GATE_LEASE_REQUIRED:
        return "LEASE_REQUIRED";
    case BZM_RUNTIME_GATE_LEASE_TOO_LONG:
        return "LEASE_TOO_LONG";
    case BZM_RUNTIME_GATE_BUSY:
        return "BUSY";
    case BZM_RUNTIME_GATE_FAULT_LATCHED:
        return "FAULT_LATCHED";
    default:
        return "INVALID_GATE_RESULT";
    }
}
