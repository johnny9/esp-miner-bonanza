#include "bzm_runtime_control.h"
#include "unity.h"

static bzm_runtime_gate_t default_gate(void)
{
    return (bzm_runtime_gate_t){
        .build_max_stage = BZM_STAGE_RUNNING,
        .powered_stages_compiled = true,
        .maximum_lease_ms = 30000,
    };
}

static bzm_runtime_request_t powered_request(void)
{
    return (bzm_runtime_request_t){
        .target_stage = BZM_STAGE_POWER_RAIL,
        .hold_after_success = true,
        .lease_ms = 5000,
        .confirmation_valid = true,
        .local_arm_present = true,
    };
}

TEST_CASE("BZM runtime gate keeps unpowered stages lease-free", "[asic][bzm][runtime][gate]")
{
    bzm_runtime_gate_t gate = default_gate();
    bzm_runtime_request_t request = {.target_stage = BZM_STAGE_CONTROLS};
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_ACCEPTED, bzm_runtime_validate_request(&gate, &request));
    request.lease_ms = 1;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_LEASE_TOO_LONG, bzm_runtime_validate_request(&gate, &request));
}

TEST_CASE("BZM powered runtime request requires every independent gate", "[asic][bzm][runtime][gate]")
{
    bzm_runtime_gate_t gate = default_gate();
    bzm_runtime_request_t request = powered_request();
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_ACCEPTED, bzm_runtime_validate_request(&gate, &request));

    gate.powered_stages_compiled = false;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_POWER_NOT_COMPILED, bzm_runtime_validate_request(&gate, &request));
    gate.powered_stages_compiled = true;
    request.confirmation_valid = false;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_CONFIRMATION_REQUIRED, bzm_runtime_validate_request(&gate, &request));
    request.confirmation_valid = true;
    request.local_arm_present = false;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_LOCAL_ARM_REQUIRED, bzm_runtime_validate_request(&gate, &request));
    request.local_arm_present = true;
    request.lease_ms = 0;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_LEASE_REQUIRED, bzm_runtime_validate_request(&gate, &request));
    request.lease_ms = 30001;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_LEASE_TOO_LONG, bzm_runtime_validate_request(&gate, &request));
}

TEST_CASE("BZM runtime ceiling rejects rather than clamps requests", "[asic][bzm][runtime][ceiling]")
{
    bzm_runtime_gate_t gate = default_gate();
    gate.build_max_stage = BZM_STAGE_CONTROLS;
    bzm_runtime_request_t request = powered_request();
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_BUILD_CEILING, bzm_runtime_validate_request(&gate, &request));
}

TEST_CASE("BZM OFF_SAFE remains available while busy or fault latched", "[asic][bzm][runtime][off]")
{
    bzm_runtime_gate_t gate = default_gate();
    gate.busy = true;
    gate.fault_latched = true;
    bzm_runtime_request_t request = {
        .target_stage = BZM_STAGE_OFF_SAFE,
        .lease_ms = 999999,
    };
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_ACCEPTED, bzm_runtime_validate_request(&gate, &request));
}

TEST_CASE("BZM runtime conflict and fault gates higher starts", "[asic][bzm][runtime][conflict]")
{
    bzm_runtime_gate_t gate = default_gate();
    bzm_runtime_request_t request = {.target_stage = BZM_STAGE_CONTROLS};
    gate.busy = true;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_BUSY, bzm_runtime_validate_request(&gate, &request));
    gate.busy = false;
    gate.fault_latched = true;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_FAULT_LATCHED, bzm_runtime_validate_request(&gate, &request));
    TEST_ASSERT_EQUAL_STRING("FAULT_LATCHED", bzm_runtime_gate_result_name(BZM_RUNTIME_GATE_FAULT_LATCHED));
}

TEST_CASE("BZM local arm is consumed only by an otherwise valid powered request", "[asic][bzm][runtime][local-arm]")
{
    bzm_runtime_gate_t gate = default_gate();
    bzm_runtime_request_t request = powered_request();
    request.local_arm_present = false;
    bzm_local_arm_t arm;
    bzm_local_arm_init(&arm);
    TEST_ASSERT_EQUAL(BZM_LOCAL_ARM_ACCEPTED,
                      bzm_local_arm_issue(&arm, BZM_RUNTIME_POWER_CONFIRMATION, BZM_RUNTIME_POWER_CONFIRMATION, 1000, 30000));

    request.lease_ms = 0;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_LEASE_REQUIRED, bzm_runtime_validate_request_with_local_arm(&gate, &request, &arm, 1000));
    TEST_ASSERT_EQUAL_UINT32(30000, bzm_local_arm_remaining_ms(&arm, 1000));

    request.lease_ms = 5000;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_ACCEPTED, bzm_runtime_validate_request_with_local_arm(&gate, &request, &arm, 1000));
    TEST_ASSERT_TRUE(request.local_arm_present);
    TEST_ASSERT_EQUAL_UINT32(0, bzm_local_arm_remaining_ms(&arm, 1000));

    request.local_arm_present = false;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_GATE_LOCAL_ARM_REQUIRED,
                      bzm_runtime_validate_request_with_local_arm(&gate, &request, &arm, 1000));
}
