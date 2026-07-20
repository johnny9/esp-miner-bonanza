#ifndef BZM_RUNTIME_CONTROL_H
#define BZM_RUNTIME_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "bzm_local_arm.h"
#include "bzm_validation.h"

#define BZM_RUNTIME_POWER_CONFIRMATION "ENERGIZE_BZM_1002"

typedef enum
{
    BZM_RUNTIME_GATE_ACCEPTED = 0,
    BZM_RUNTIME_GATE_INVALID_STAGE,
    BZM_RUNTIME_GATE_BUILD_CEILING,
    BZM_RUNTIME_GATE_POWER_NOT_COMPILED,
    BZM_RUNTIME_GATE_CONFIRMATION_REQUIRED,
    BZM_RUNTIME_GATE_LOCAL_ARM_REQUIRED,
    BZM_RUNTIME_GATE_LEASE_REQUIRED,
    BZM_RUNTIME_GATE_LEASE_TOO_LONG,
    BZM_RUNTIME_GATE_BUSY,
    BZM_RUNTIME_GATE_FAULT_LATCHED,
} bzm_runtime_gate_result_t;

typedef struct
{
    bzm_validation_stage_t target_stage;
    bool hold_after_success;
    uint32_t lease_ms;
    bool confirmation_valid;
    bool local_arm_present;
} bzm_runtime_request_t;

typedef struct
{
    bzm_validation_stage_t build_max_stage;
    bool powered_stages_compiled;
    uint32_t maximum_lease_ms;
    bool busy;
    bool fault_latched;
} bzm_runtime_gate_t;

bzm_runtime_gate_result_t bzm_runtime_validate_request(const bzm_runtime_gate_t * gate, const bzm_runtime_request_t * request);
bzm_runtime_gate_result_t bzm_runtime_validate_request_with_local_arm(const bzm_runtime_gate_t * gate,
                                                                      bzm_runtime_request_t * request, bzm_local_arm_t * local_arm,
                                                                      uint64_t now_ms);
const char * bzm_runtime_gate_result_name(bzm_runtime_gate_result_t result);

#endif // BZM_RUNTIME_CONTROL_H
