#include <string.h>

#include "bzm_validation.h"
#include "unity.h"

typedef struct
{
    size_t stage_calls;
    bzm_validation_stage_t stages[BZM_STAGE_COUNT];
    bzm_validation_stage_t fail_stage;
    bzm_check_status_t fail_status;
    size_t safe_off_calls;
    bool safe_off_fails;
} simulated_validation_t;

static bzm_stage_result_t simulated_stage(void * context, bzm_validation_stage_t stage)
{
    simulated_validation_t * simulation = context;
    simulation->stages[simulation->stage_calls++] = stage;
    if (stage == simulation->fail_stage && simulation->fail_status != BZM_CHECK_GOOD) {
        return bzm_validation_result(simulation->fail_status, BZM_VALIDATION_CODE_STAGE_FAILED, "injected stage failure");
    }
    return bzm_validation_result(BZM_CHECK_GOOD, BZM_VALIDATION_CODE_STAGE_OK, "simulated evidence matched");
}

static bzm_stage_result_t simulated_safe_off(void * context)
{
    simulated_validation_t * simulation = context;
    simulation->safe_off_calls++;
    return bzm_validation_result(simulation->safe_off_fails ? BZM_CHECK_BAD : BZM_CHECK_GOOD,
                                 simulation->safe_off_fails ? BZM_VALIDATION_CODE_SAFE_OFF_FAILED : BZM_VALIDATION_CODE_STAGE_OK,
                                 simulation->safe_off_fails ? "VCORE did not discharge" : "safe outputs verified");
}

static const bzm_validation_ops_t SIMULATED_OPS = {
    .run_stage = simulated_stage,
    .force_safe_off = simulated_safe_off,
};

static bzm_validation_policy_t policy_for(bzm_validation_stage_t target)
{
    return (bzm_validation_policy_t){
        .run_id = 17,
        .requested_stage = target,
        .build_max_stage = BZM_STAGE_RUNNING,
        .implemented_max_stage = BZM_STAGE_RUNNING,
        .powered_stages_compiled = true,
        .local_arm_present = true,
        .production_mode = true,
        .independent_kill_available = true,
        .lease_ms = 120000,
    };
}

TEST_CASE("BZM internal startup steps have stable diagnostic names", "[asic][bzm][controller]")
{
    for (int stage = 0; stage < BZM_STAGE_COUNT; ++stage) {
        TEST_ASSERT_NOT_EQUAL(0, strlen(bzm_validation_stage_name(stage)));
    }
    TEST_ASSERT_EQUAL_STRING("INVALID_STAGE", bzm_validation_stage_name(BZM_STAGE_COUNT));
    TEST_ASSERT_EQUAL_STRING("GOOD", bzm_validation_status_name(BZM_CHECK_GOOD));
    TEST_ASSERT_EQUAL_STRING("INDEPENDENT_KILL_REQUIRED", bzm_validation_code_name(BZM_VALIDATION_CODE_INDEPENDENT_KILL_REQUIRED));
}

TEST_CASE("BZM validation executes every prerequisite in order", "[asic][bzm][validation][ordering]")
{
    simulated_validation_t simulation = {
        .fail_stage = BZM_STAGE_COUNT,
        .fail_status = BZM_CHECK_GOOD,
    };
    bzm_validation_policy_t policy = policy_for(BZM_STAGE_SENSORS);
    bzm_validation_report_t report;

    TEST_ASSERT_TRUE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL_UINT32(BZM_STAGE_SENSORS + 1, simulation.stage_calls);
    for (int stage = 0; stage <= BZM_STAGE_SENSORS; ++stage) {
        TEST_ASSERT_EQUAL(stage, simulation.stages[stage]);
        TEST_ASSERT_EQUAL(BZM_CHECK_GOOD, report.stages[stage].status);
    }
    TEST_ASSERT_EQUAL_UINT32(1, simulation.safe_off_calls);
    TEST_ASSERT_EQUAL(BZM_CHECK_GOOD, report.overall);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_OFF_SAFE, report.state);
    TEST_ASSERT_FALSE(report.energized);
    TEST_ASSERT_EQUAL(BZM_CHECK_GOOD, report.final_safe_off.status);
}

TEST_CASE("BZM validation cannot cross its build or implementation ceiling", "[asic][bzm][validation][ceiling]")
{
    simulated_validation_t simulation = {0};
    bzm_validation_report_t report;
    bzm_validation_policy_t policy = policy_for(BZM_STAGE_CHAIN_4);
    policy.build_max_stage = BZM_STAGE_POWER_RAIL;

    TEST_ASSERT_FALSE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL_UINT32(0, simulation.stage_calls);
    TEST_ASSERT_EQUAL_UINT32(1, simulation.safe_off_calls);
    TEST_ASSERT_EQUAL(BZM_CHECK_BLOCKED, report.stages[BZM_STAGE_CHAIN_4].status);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_CODE_BUILD_CEILING, report.stages[BZM_STAGE_CHAIN_4].code);

    memset(&simulation, 0, sizeof(simulation));
    policy.build_max_stage = BZM_STAGE_CHAIN_4;
    policy.implemented_max_stage = BZM_STAGE_POWER_RAIL;
    TEST_ASSERT_FALSE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL(BZM_VALIDATION_CODE_NOT_IMPLEMENTED, report.stages[BZM_STAGE_CHAIN_4].code);
}

TEST_CASE("BZM powered stages require compile authorization arm and kill path", "[asic][bzm][validation][interlock]")
{
    simulated_validation_t simulation = {0};
    bzm_validation_report_t report;
    bzm_validation_policy_t policy = policy_for(BZM_STAGE_POWER_RAIL);

    policy.powered_stages_compiled = false;
    TEST_ASSERT_FALSE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL(BZM_VALIDATION_CODE_POWER_NOT_COMPILED, report.stages[BZM_STAGE_POWER_RAIL].code);

    policy.powered_stages_compiled = true;
    policy.local_arm_present = false;
    TEST_ASSERT_FALSE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL(BZM_VALIDATION_CODE_LOCAL_ARM_REQUIRED, report.stages[BZM_STAGE_POWER_RAIL].code);

    policy.local_arm_present = true;
    policy.independent_kill_available = false;
    TEST_ASSERT_FALSE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL(BZM_VALIDATION_CODE_INDEPENDENT_KILL_REQUIRED, report.stages[BZM_STAGE_POWER_RAIL].code);
}

TEST_CASE("BZM ESP-only kill override is restricted to explicitly armed lab policy", "[asic][bzm][validation][lab]")
{
    simulated_validation_t simulation = {
        .fail_stage = BZM_STAGE_COUNT,
        .fail_status = BZM_CHECK_GOOD,
    };
    bzm_validation_report_t report;
    bzm_validation_policy_t policy = policy_for(BZM_STAGE_POWER_RAIL);
    policy.production_mode = false;
    policy.independent_kill_available = false;
    policy.allow_esp_only_kill_in_lab = true;

    TEST_ASSERT_TRUE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL(BZM_CHECK_GOOD, report.overall);

    memset(&simulation, 0, sizeof(simulation));
    policy.production_mode = true;
    TEST_ASSERT_FALSE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL(BZM_VALIDATION_CODE_INDEPENDENT_KILL_REQUIRED, report.stages[BZM_STAGE_POWER_RAIL].code);
}

TEST_CASE("BZM production accepts explicit board-managed fixed-voltage safety", "[asic][bzm][validation][production]")
{
    simulated_validation_t simulation = {
        .fail_stage = BZM_STAGE_COUNT,
        .fail_status = BZM_CHECK_GOOD,
    };
    bzm_validation_report_t report;
    bzm_validation_policy_t policy = policy_for(BZM_STAGE_POWER_RAIL);
    policy.independent_kill_available = false;
    policy.board_managed_safety = true;

    TEST_ASSERT_TRUE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL(BZM_CHECK_GOOD, report.overall);
    TEST_ASSERT_EQUAL_UINT32(BZM_STAGE_POWER_RAIL + 1, simulation.stage_calls);

    memset(&simulation, 0, sizeof(simulation));
    policy.board_managed_safety = false;
    TEST_ASSERT_FALSE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL(BZM_VALIDATION_CODE_INDEPENDENT_KILL_REQUIRED, report.stages[BZM_STAGE_POWER_RAIL].code);
}

TEST_CASE("BZM stage failure remains BAD and skips every higher stage", "[asic][bzm][validation][failure]")
{
    simulated_validation_t simulation = {
        .fail_stage = BZM_STAGE_CHAIN_4,
        .fail_status = BZM_CHECK_BAD,
    };
    bzm_validation_policy_t policy = policy_for(BZM_STAGE_CLOCKS);
    bzm_validation_report_t report;

    TEST_ASSERT_FALSE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL_UINT32(BZM_STAGE_CHAIN_4 + 1, simulation.stage_calls);
    TEST_ASSERT_EQUAL(BZM_CHECK_BAD, report.stages[BZM_STAGE_CHAIN_4].status);
    TEST_ASSERT_EQUAL(BZM_CHECK_SKIPPED, report.stages[BZM_STAGE_SENSORS].status);
    TEST_ASSERT_EQUAL(BZM_CHECK_SKIPPED, report.stages[BZM_STAGE_CLOCKS].status);
    TEST_ASSERT_EQUAL(BZM_CHECK_BAD, report.overall);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_FAULT_LATCHED, report.state);
    TEST_ASSERT_EQUAL_UINT32(1, simulation.safe_off_calls);
}

TEST_CASE("BZM BLOCKED evidence is distinct from a hardware BAD result", "[asic][bzm][validation][blocked]")
{
    simulated_validation_t simulation = {
        .fail_stage = BZM_STAGE_SENSORS,
        .fail_status = BZM_CHECK_BLOCKED,
    };
    bzm_validation_policy_t policy = policy_for(BZM_STAGE_SENSORS);
    bzm_validation_report_t report;

    TEST_ASSERT_FALSE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL(BZM_CHECK_BLOCKED, report.stages[BZM_STAGE_SENSORS].status);
    TEST_ASSERT_EQUAL(BZM_CHECK_BLOCKED, report.overall);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_OFF_SAFE, report.state);
}

TEST_CASE("BZM unverified shutdown overrides prior stage evidence", "[asic][bzm][validation][shutdown]")
{
    simulated_validation_t simulation = {
        .fail_stage = BZM_STAGE_COUNT,
        .fail_status = BZM_CHECK_GOOD,
        .safe_off_fails = true,
    };
    bzm_validation_policy_t policy = policy_for(BZM_STAGE_CONTROLS);
    bzm_validation_report_t report;

    TEST_ASSERT_FALSE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL(BZM_CHECK_GOOD, report.stages[BZM_STAGE_CONTROLS].status);
    TEST_ASSERT_EQUAL(BZM_CHECK_BAD, report.overall);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_SHUTDOWN_UNVERIFIED, report.state);
    TEST_ASSERT_TRUE(report.energized);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_CODE_SAFE_OFF_FAILED, report.final_safe_off.code);
}

TEST_CASE("BZM powered hold requires a lease and leaves an explicit HOLDING state", "[asic][bzm][validation][lease]")
{
    simulated_validation_t simulation = {
        .fail_stage = BZM_STAGE_COUNT,
        .fail_status = BZM_CHECK_GOOD,
    };
    bzm_validation_policy_t policy = policy_for(BZM_STAGE_POWER_RAIL);
    bzm_validation_report_t report;
    policy.hold_after_success = true;
    policy.lease_ms = 0;

    TEST_ASSERT_FALSE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL(BZM_VALIDATION_CODE_POWER_LEASE_REQUIRED, report.stages[BZM_STAGE_POWER_RAIL].code);

    memset(&simulation, 0, sizeof(simulation));
    simulation.fail_stage = BZM_STAGE_COUNT;
    simulation.fail_status = BZM_CHECK_GOOD;
    policy.lease_ms = 30000;
    TEST_ASSERT_TRUE(bzm_validation_execute(&policy, &SIMULATED_OPS, &simulation, &report));
    TEST_ASSERT_EQUAL(BZM_VALIDATION_HOLDING, report.state);
    TEST_ASSERT_TRUE(report.energized);
    TEST_ASSERT_EQUAL_UINT32(0, simulation.safe_off_calls);
    TEST_ASSERT_EQUAL(BZM_CHECK_NOT_RUN, report.final_safe_off.status);
}
