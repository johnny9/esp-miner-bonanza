#include <string.h>

#include "bzm_supervisor.h"
#include "unity.h"

typedef struct {
    size_t stage_calls;
    bzm_validation_stage_t fail_stage;
    size_t safe_off_calls;
    bool safe_off_fails;
} simulated_hardware_t;

static bzm_stage_result_t run_stage(void *context,
                                    bzm_validation_stage_t stage)
{
    simulated_hardware_t *hardware = context;
    hardware->stage_calls++;
    if (stage == hardware->fail_stage) {
        return bzm_validation_result(BZM_CHECK_BAD,
                                     BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "injected hardware failure");
    }
    return bzm_validation_result(BZM_CHECK_GOOD,
                                 BZM_VALIDATION_CODE_STAGE_OK,
                                 "fresh evidence is good");
}

static bzm_stage_result_t safe_off(void *context)
{
    simulated_hardware_t *hardware = context;
    hardware->safe_off_calls++;
    return bzm_validation_result(
        hardware->safe_off_fails ? BZM_CHECK_BAD : BZM_CHECK_GOOD,
        hardware->safe_off_fails
            ? BZM_VALIDATION_CODE_SAFE_OFF_FAILED
            : BZM_VALIDATION_CODE_STAGE_OK,
        hardware->safe_off_fails
            ? "rail stayed energized" : "rail is physically off");
}

static bzm_supervisor_t supervisor_for(simulated_hardware_t *hardware)
{
    bzm_supervisor_t supervisor;
    const bzm_supervisor_config_t config = {
        .build_max_stage = BZM_STAGE_RUNNING,
        .implemented_max_stage = BZM_STAGE_RUNNING,
        .powered_stages_compiled = true,
        .production_mode = true,
        .independent_kill_available = true,
        .maximum_lease_ms = 120000,
    };
    const bzm_validation_ops_t ops = {
        .run_stage = run_stage,
        .force_safe_off = safe_off,
    };
    TEST_ASSERT_TRUE(bzm_supervisor_init(
        &supervisor, &config, &ops, hardware));
    return supervisor;
}

TEST_CASE("BZM supervisor validates configuration before accepting ownership",
          "[asic][bzm][supervisor][init]")
{
    bzm_supervisor_t supervisor;
    simulated_hardware_t hardware = {.fail_stage = BZM_STAGE_COUNT};
    bzm_supervisor_config_t config = {
        .build_max_stage = BZM_STAGE_RUNNING,
        .implemented_max_stage = BZM_STAGE_POWER_RAIL,
        .maximum_lease_ms = 1000,
    };
    bzm_validation_ops_t ops = {
        .run_stage = run_stage,
        .force_safe_off = safe_off,
    };
    TEST_ASSERT_FALSE(bzm_supervisor_init(
        &supervisor, &config, &ops, &hardware));
    config.implemented_max_stage = BZM_STAGE_RUNNING;
    config.maximum_lease_ms = 0;
    TEST_ASSERT_FALSE(bzm_supervisor_init(
        &supervisor, &config, &ops, &hardware));
    config.maximum_lease_ms = 1000;
    TEST_ASSERT_TRUE(bzm_supervisor_init(
        &supervisor, &config, &ops, &hardware));
    TEST_ASSERT_EQUAL(BZM_SUPERVISOR_OWNER_NONE, supervisor.owner);
    TEST_ASSERT_FALSE(bzm_supervisor_owner_is_maintenance(
        BZM_SUPERVISOR_OWNER_NONE));
    TEST_ASSERT_EQUAL_STRING("BRIDGE_UPDATE",
                             bzm_supervisor_owner_name(
                                 BZM_SUPERVISOR_OWNER_BRIDGE_UPDATE));
}

TEST_CASE("BZM supervisor lease expiry always performs safe shutdown",
          "[asic][bzm][supervisor][lease]")
{
    simulated_hardware_t hardware = {.fail_stage = BZM_STAGE_COUNT};
    bzm_supervisor_t supervisor = supervisor_for(&hardware);

    TEST_ASSERT_TRUE(bzm_supervisor_request_validation(
        &supervisor, BZM_STAGE_POWER_RAIL, true, true, 5000, 1000));
    TEST_ASSERT_EQUAL(BZM_SUPERVISOR_OWNER_VALIDATION, supervisor.owner);
    TEST_ASSERT_EQUAL_UINT32(6000, (uint32_t) supervisor.lease_deadline_ms);
    TEST_ASSERT_TRUE(bzm_supervisor_tick(&supervisor, 5999));
    TEST_ASSERT_EQUAL_UINT32(0, hardware.safe_off_calls);
    TEST_ASSERT_TRUE(bzm_supervisor_tick(&supervisor, 6000));
    TEST_ASSERT_EQUAL_UINT32(1, hardware.safe_off_calls);
    TEST_ASSERT_EQUAL(BZM_SUPERVISOR_OWNER_NONE, supervisor.owner);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_OFF_SAFE, supervisor.report.state);
}

TEST_CASE("BZM supervisor heartbeat renews only a live bounded lease",
          "[asic][bzm][supervisor][heartbeat]")
{
    simulated_hardware_t hardware = {.fail_stage = BZM_STAGE_COUNT};
    bzm_supervisor_t supervisor = supervisor_for(&hardware);
    TEST_ASSERT_FALSE(bzm_supervisor_heartbeat(&supervisor, 1000, 0));
    TEST_ASSERT_TRUE(bzm_supervisor_request_validation(
        &supervisor, BZM_STAGE_POWER_RAIL, true, true, 5000, 1000));
    TEST_ASSERT_TRUE(bzm_supervisor_heartbeat(&supervisor, 4000, 5000));
    TEST_ASSERT_EQUAL_UINT32(9000, (uint32_t) supervisor.lease_deadline_ms);
    TEST_ASSERT_FALSE(bzm_supervisor_heartbeat(&supervisor, 4000, 9000));
    TEST_ASSERT_FALSE(bzm_supervisor_heartbeat(&supervisor, 120001, 6000));
}

TEST_CASE("BZM supervisor only opens dispatch after the complete RUNNING stage",
          "[asic][bzm][supervisor][dispatch]")
{
    simulated_hardware_t hardware = {.fail_stage = BZM_STAGE_COUNT};
    bzm_supervisor_t supervisor = supervisor_for(&hardware);
    TEST_ASSERT_TRUE(bzm_supervisor_request_validation(
        &supervisor, BZM_STAGE_BALANCED_RAMP, true, true, 5000, 0));
    TEST_ASSERT_FALSE(bzm_supervisor_dispatch_allowed(&supervisor, 1));
    TEST_ASSERT_TRUE(bzm_supervisor_stop(&supervisor, "next stage"));
    TEST_ASSERT_TRUE(bzm_supervisor_request_validation(
        &supervisor, BZM_STAGE_RUNNING, true, true, 5000, 100));
    TEST_ASSERT_TRUE(bzm_supervisor_dispatch_allowed(&supervisor, 101));
    TEST_ASSERT_FALSE(bzm_supervisor_dispatch_allowed(&supervisor, 5100));
}

TEST_CASE("BZM supervisor latches stage BAD and rejects higher starts",
          "[asic][bzm][supervisor][fault]")
{
    simulated_hardware_t hardware = {.fail_stage = BZM_STAGE_CHAIN_4};
    bzm_supervisor_t supervisor = supervisor_for(&hardware);
    TEST_ASSERT_FALSE(bzm_supervisor_request_validation(
        &supervisor, BZM_STAGE_SENSORS, true, false, 5000, 0));
    TEST_ASSERT_TRUE(supervisor.fault_latched);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_FAULT_LATCHED,
                      supervisor.report.state);

    hardware.fail_stage = BZM_STAGE_COUNT;
    size_t previous_calls = hardware.stage_calls;
    TEST_ASSERT_FALSE(bzm_supervisor_request_validation(
        &supervisor, BZM_STAGE_CONTROLS, false, false, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(previous_calls, hardware.stage_calls);
}

TEST_CASE("BZM fault clear requires a fresh successful OFF_SAFE proof",
          "[asic][bzm][supervisor][clear]")
{
    simulated_hardware_t hardware = {.fail_stage = BZM_STAGE_COUNT};
    bzm_supervisor_t supervisor = supervisor_for(&hardware);
    TEST_ASSERT_TRUE(bzm_supervisor_latch_fault(
        &supervisor, 42, "injected fan stall"));
    TEST_ASSERT_EQUAL(BZM_CHECK_BAD, supervisor.report.overall);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_FAULT_LATCHED,
                      supervisor.report.state);
    TEST_ASSERT_FALSE(bzm_supervisor_clear_fault(&supervisor));

    TEST_ASSERT_TRUE(bzm_supervisor_request_validation(
        &supervisor, BZM_STAGE_OFF_SAFE, false, false, 0, 100));
    TEST_ASSERT_TRUE(supervisor.fault_latched);
    TEST_ASSERT_TRUE(bzm_supervisor_safe_off_verified(&supervisor));
    TEST_ASSERT_TRUE(bzm_supervisor_clear_fault(&supervisor));
    TEST_ASSERT_FALSE(supervisor.fault_latched);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_OFF_SAFE, supervisor.report.state);
}

TEST_CASE("BZM maintenance obtains exclusive ownership only after fresh safe off",
          "[asic][bzm][supervisor][maintenance]")
{
    const bzm_supervisor_owner_t owners[] = {
        BZM_SUPERVISOR_OWNER_BRIDGE_UPDATE,
        BZM_SUPERVISOR_OWNER_FACTORY_TEST,
        BZM_SUPERVISOR_OWNER_ESP_OTA,
        BZM_SUPERVISOR_OWNER_ESP_RESTART,
    };
    for (size_t index = 0; index < sizeof(owners) / sizeof(owners[0]);
         ++index) {
        simulated_hardware_t hardware = {.fail_stage = BZM_STAGE_COUNT};
        bzm_supervisor_t supervisor = supervisor_for(&hardware);
        TEST_ASSERT_TRUE(bzm_supervisor_acquire_maintenance(
            &supervisor, owners[index], 0));
        TEST_ASSERT_TRUE(bzm_supervisor_owner_is_maintenance(owners[index]));
        TEST_ASSERT_EQUAL(owners[index], supervisor.owner);
        TEST_ASSERT_TRUE(bzm_supervisor_safe_off_verified(&supervisor));
        TEST_ASSERT_EQUAL_UINT32(1, hardware.safe_off_calls);
        bzm_supervisor_owner_t wrong_owner =
            owners[(index + 1) % (sizeof(owners) / sizeof(owners[0]))];
        TEST_ASSERT_FALSE(bzm_supervisor_acquire_maintenance(
            &supervisor, wrong_owner, 1));
        TEST_ASSERT_FALSE(bzm_supervisor_release_maintenance(
            &supervisor, wrong_owner));
        TEST_ASSERT_FALSE(bzm_supervisor_heartbeat(
            &supervisor, 1000, 1));
        TEST_ASSERT_TRUE(bzm_supervisor_tick(&supervisor, UINT64_MAX));
        TEST_ASSERT_FALSE(bzm_supervisor_request_validation(
            &supervisor, BZM_STAGE_CONTROLS, false, false, 0, 0));
        TEST_ASSERT_EQUAL(owners[index], supervisor.owner);
        TEST_ASSERT_FALSE(bzm_supervisor_stop(
            &supervisor, "operator stop during maintenance"));
        TEST_ASSERT_EQUAL(owners[index], supervisor.owner);
        TEST_ASSERT_TRUE(bzm_supervisor_release_maintenance(
            &supervisor, owners[index]));
        TEST_ASSERT_EQUAL(BZM_SUPERVISOR_OWNER_NONE, supervisor.owner);
        TEST_ASSERT_EQUAL_UINT32(2, hardware.safe_off_calls);
    }
}

TEST_CASE("BZM maintenance safe-off failures latch closed",
          "[asic][bzm][supervisor][maintenance]")
{
    simulated_hardware_t acquire_hardware = {
        .fail_stage = BZM_STAGE_COUNT,
        .safe_off_fails = true,
    };
    bzm_supervisor_t acquire = supervisor_for(&acquire_hardware);
    TEST_ASSERT_FALSE(bzm_supervisor_acquire_maintenance(
        &acquire, BZM_SUPERVISOR_OWNER_ESP_OTA, 0));
    TEST_ASSERT_EQUAL(BZM_SUPERVISOR_OWNER_NONE, acquire.owner);
    TEST_ASSERT_TRUE(acquire.fault_latched);
    TEST_ASSERT_TRUE(acquire.report.energized);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_SHUTDOWN_UNVERIFIED,
                      acquire.report.state);

    simulated_hardware_t release_hardware = {
        .fail_stage = BZM_STAGE_COUNT,
    };
    bzm_supervisor_t release = supervisor_for(&release_hardware);
    TEST_ASSERT_TRUE(bzm_supervisor_acquire_maintenance(
        &release, BZM_SUPERVISOR_OWNER_BRIDGE_UPDATE, 0));
    release_hardware.safe_off_fails = true;
    TEST_ASSERT_FALSE(bzm_supervisor_release_maintenance(
        &release, BZM_SUPERVISOR_OWNER_BRIDGE_UPDATE));
    TEST_ASSERT_EQUAL(BZM_SUPERVISOR_OWNER_NONE, release.owner);
    TEST_ASSERT_TRUE(release.fault_latched);
    TEST_ASSERT_TRUE(release.report.energized);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_SHUTDOWN_UNVERIFIED,
                      release.report.state);
    TEST_ASSERT_FALSE(bzm_supervisor_request_validation(
        &release, BZM_STAGE_POWER_RAIL, true, true, 1000, 1));
}

TEST_CASE("BZM maintenance preempts mining only after verified safe off",
          "[asic][bzm][supervisor][maintenance][mining]")
{
    simulated_hardware_t hardware = {.fail_stage = BZM_STAGE_COUNT};
    bzm_supervisor_t supervisor = supervisor_for(&hardware);
    TEST_ASSERT_TRUE(bzm_supervisor_request_validation(
        &supervisor, BZM_STAGE_RUNNING, true, true, 5000, 100));
    TEST_ASSERT_TRUE(bzm_supervisor_dispatch_allowed(&supervisor, 101));

    TEST_ASSERT_TRUE(bzm_supervisor_acquire_maintenance(
        &supervisor, BZM_SUPERVISOR_OWNER_ESP_OTA, 102));
    TEST_ASSERT_EQUAL_UINT32(1, hardware.safe_off_calls);
    TEST_ASSERT_EQUAL(BZM_SUPERVISOR_OWNER_ESP_OTA, supervisor.owner);
    TEST_ASSERT_TRUE(bzm_supervisor_safe_off_verified(&supervisor));
    TEST_ASSERT_FALSE(bzm_supervisor_dispatch_allowed(&supervisor, 103));
}

TEST_CASE("BZM maintenance cannot own a mining unit with unverified shutdown",
          "[asic][bzm][supervisor][maintenance][mining]")
{
    simulated_hardware_t hardware = {.fail_stage = BZM_STAGE_COUNT};
    bzm_supervisor_t supervisor = supervisor_for(&hardware);
    TEST_ASSERT_TRUE(bzm_supervisor_request_validation(
        &supervisor, BZM_STAGE_RUNNING, true, true, 5000, 100));
    hardware.safe_off_fails = true;

    TEST_ASSERT_FALSE(bzm_supervisor_acquire_maintenance(
        &supervisor, BZM_SUPERVISOR_OWNER_BRIDGE_UPDATE, 102));
    TEST_ASSERT_EQUAL(BZM_SUPERVISOR_OWNER_NONE, supervisor.owner);
    TEST_ASSERT_TRUE(supervisor.fault_latched);
    TEST_ASSERT_TRUE(supervisor.report.energized);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_SHUTDOWN_UNVERIFIED,
                      supervisor.report.state);
}

TEST_CASE("BZM maintenance clears a fault only after fresh safe off",
          "[asic][bzm][supervisor][maintenance][fault]")
{
    simulated_hardware_t hardware = {.fail_stage = BZM_STAGE_COUNT};
    bzm_supervisor_t supervisor = supervisor_for(&hardware);
    TEST_ASSERT_TRUE(bzm_supervisor_latch_fault(
        &supervisor, 42, "injected runtime fault"));
    TEST_ASSERT_TRUE(supervisor.fault_latched);
    TEST_ASSERT_EQUAL_UINT32(1, hardware.safe_off_calls);

    TEST_ASSERT_TRUE(bzm_supervisor_acquire_maintenance(
        &supervisor, BZM_SUPERVISOR_OWNER_ESP_OTA, 100));
    TEST_ASSERT_FALSE(supervisor.fault_latched);
    TEST_ASSERT_EQUAL(BZM_SUPERVISOR_OWNER_ESP_OTA, supervisor.owner);
    TEST_ASSERT_TRUE(bzm_supervisor_safe_off_verified(&supervisor));
    TEST_ASSERT_EQUAL_UINT32(2, hardware.safe_off_calls);
}

TEST_CASE("BZM restart preempts mining only after verified safe off",
          "[asic][bzm][supervisor][restart]")
{
    simulated_hardware_t hardware = {.fail_stage = BZM_STAGE_COUNT};
    bzm_supervisor_t supervisor = supervisor_for(&hardware);
    TEST_ASSERT_TRUE(bzm_supervisor_request_validation(
        &supervisor, BZM_STAGE_RUNNING, true, true, 5000, 100));
    TEST_ASSERT_TRUE(bzm_supervisor_dispatch_allowed(&supervisor, 101));

    TEST_ASSERT_TRUE(bzm_supervisor_prepare_restart(&supervisor));
    TEST_ASSERT_EQUAL_UINT32(1, hardware.safe_off_calls);
    TEST_ASSERT_EQUAL(BZM_SUPERVISOR_OWNER_ESP_RESTART, supervisor.owner);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)supervisor.lease_deadline_ms);
    TEST_ASSERT_TRUE(bzm_supervisor_safe_off_verified(&supervisor));
    TEST_ASSERT_FALSE(bzm_supervisor_dispatch_allowed(&supervisor, 102));
    TEST_ASSERT_FALSE(bzm_supervisor_prepare_restart(&supervisor));
}

TEST_CASE("BZM restart is blocked by maintenance or unverified shutdown",
          "[asic][bzm][supervisor][restart]")
{
    simulated_hardware_t busy_hardware = {.fail_stage = BZM_STAGE_COUNT};
    bzm_supervisor_t busy = supervisor_for(&busy_hardware);
    TEST_ASSERT_TRUE(bzm_supervisor_acquire_maintenance(
        &busy, BZM_SUPERVISOR_OWNER_BRIDGE_UPDATE, 0));
    size_t safe_off_calls = busy_hardware.safe_off_calls;
    TEST_ASSERT_FALSE(bzm_supervisor_prepare_restart(&busy));
    TEST_ASSERT_EQUAL_UINT32(safe_off_calls, busy_hardware.safe_off_calls);
    TEST_ASSERT_EQUAL(BZM_SUPERVISOR_OWNER_BRIDGE_UPDATE, busy.owner);

    simulated_hardware_t failed_hardware = {
        .fail_stage = BZM_STAGE_COUNT,
        .safe_off_fails = true,
    };
    bzm_supervisor_t failed = supervisor_for(&failed_hardware);
    TEST_ASSERT_FALSE(bzm_supervisor_prepare_restart(&failed));
    TEST_ASSERT_EQUAL(BZM_SUPERVISOR_OWNER_NONE, failed.owner);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_SHUTDOWN_UNVERIFIED,
                      failed.report.state);
    TEST_ASSERT_TRUE(failed.fault_latched);
}

TEST_CASE("BZM safe-off failure becomes shutdown-unverified",
          "[asic][bzm][supervisor][shutdown]")
{
    simulated_hardware_t hardware = {
        .fail_stage = BZM_STAGE_COUNT,
        .safe_off_fails = true,
    };
    bzm_supervisor_t supervisor = supervisor_for(&hardware);
    TEST_ASSERT_FALSE(bzm_supervisor_latch_fault(
        &supervisor, 99, "injected trip"));
    TEST_ASSERT_TRUE(supervisor.fault_latched);
    TEST_ASSERT_EQUAL(BZM_VALIDATION_SHUTDOWN_UNVERIFIED,
                      supervisor.report.state);
    TEST_ASSERT_TRUE(supervisor.report.energized);
    TEST_ASSERT_FALSE(bzm_supervisor_clear_fault(&supervisor));
}
