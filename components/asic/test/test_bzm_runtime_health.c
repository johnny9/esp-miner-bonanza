#include <math.h>
#include <string.h>

#include "bzm_runtime_health.h"
#include "unity.h"

static void set_stage(bzm_runtime_health_input_t * input, bzm_validation_stage_t stage)
{
    input->reached_stage = stage;
    bool five_volt_enabled = stage >= BZM_STAGE_CHAIN_4;
    input->bridge_status.five_volt_enabled = five_volt_enabled;
    input->bridge_status.asic_reset_asserted = !five_volt_enabled;
    bool outputs_safe = !five_volt_enabled && input->bridge_status.asic_reset_asserted && input->bridge_status.fan_full &&
                        input->bridge_status.fan_percent == 100;
    input->bridge_status.evidence &= (uint16_t) ~BZM_BRIDGE_SAFETY_EVIDENCE_OUTPUTS_SAFE;
    if (outputs_safe) {
        input->bridge_status.evidence |= BZM_BRIDGE_SAFETY_EVIDENCE_OUTPUTS_SAFE;
    }
}

static bzm_runtime_health_input_t good_input(bzm_validation_stage_t stage)
{
    bzm_runtime_health_input_t input;
    memset(&input, 0, sizeof(input));
    input.holding = true;
    input.bridge_status_available = true;
    input.bridge_status = (bzm_bridge_safety_status_t){
        .valid = true,
        .schema_version = BZM_BRIDGE_SAFETY_STATUS_SCHEMA_VERSION,
        .stage = BZM_BRIDGE_SAFETY_STAGE_LEASE,
        .state = BZM_BRIDGE_SAFETY_STATE_CONTROLLED,
        .fault = BZM_BRIDGE_SAFETY_FAULT_NONE,
        .runtime_verdict = BZM_BRIDGE_SAFETY_RUNTIME_GOOD_CONTROLLED,
        .production_verdict = BZM_BRIDGE_SAFETY_PRODUCTION_BAD_STAGE_DISABLED,
        .capabilities = BZM_BRIDGE_SAFETY_CAP_5V_CONTROL | BZM_BRIDGE_SAFETY_CAP_ASIC_RESET_CONTROL |
                        BZM_BRIDGE_SAFETY_CAP_FAN_FORCE_FULL | BZM_BRIDGE_SAFETY_CAP_TRIP_INPUT_SAMPLED,
        .evidence =
            BZM_BRIDGE_SAFETY_EVIDENCE_LEASE_VALID | BZM_BRIDGE_SAFETY_EVIDENCE_TRIP_CLEAR | BZM_BRIDGE_SAFETY_EVIDENCE_FAULT_CLEAR,
        .lease_remaining_ms = 5000,
        .fan_full = true,
        .fan_percent = 100,
    };
    input.fan_tach_available = true;
    input.fan_rpm = 2200;
    input.fan_min_rpm = 1000;

    input.tps = (bzm_runtime_health_tps_sample_t){
        .available = true,
        .pgood = true,
        .operation = BZM_RUNTIME_HEALTH_TPS_OPERATION_ON_MASK,
        .status_word = 0,
        .vout_command_v = 2.8f,
        .vout_command_matches_expected = true,
        .vin_v = 12.0f,
        .vout_v = 2.8f,
        .iout_a = 10.0f,
        .temperature_c = 55.0f,
    };
    input.tps_bounds = (bzm_runtime_health_tps_bounds_t){
        .vin_min_v = 10.5f,
        .vin_max_v = 15.0f,
        .vout_command_v = 2.8f,
        .vout_command_tolerance_v = 0.002f,
        .vout_min_v = 2.65f,
        .vout_max_v = 2.95f,
        .iout_min_a = -1.0f,
        .iout_max_a = 50.0f,
        .temperature_min_c = -40.0f,
        .temperature_max_c = 105.0f,
    };

    input.telemetry_available = true;
    input.telemetry_bounds = (bzm_telemetry_bounds_t){
        .temperature_min_c = -20.0f,
        .temperature_max_c = 105.0f,
        .ch0_min_mv = 300.0f,
        .ch0_max_mv = 800.0f,
        .ch1_min_mv = 300.0f,
        .ch1_max_mv = 800.0f,
        .ch2_abs_max_mv = 50.0f,
        .max_stack_spread_mv = 100.0f,
    };
    input.telemetry_now_us = 1000000;
    input.telemetry_max_age_us = 200000;
    bzm_telemetry_store_init(&input.telemetry);
    for (size_t index = 0; index < BZM_MAX_ASIC_COUNT; ++index) {
        input.telemetry.samples[index] = (bzm_telemetry_sample_t){
            .asic_id = bzm_asic_wire_ids[index],
            .timestamp_us = 900000,
            .received = true,
            .temperature_c = 50.0f,
            .thermal_enabled = true,
            .thermal_validity = true,
            .thermal_valid = true,
            .ch0_mv = 400.0f,
            .ch1_mv = 410.0f,
            .ch2_mv = 1.0f,
            .voltage_enabled = true,
            .voltage_valid = true,
            .pll_locked = true,
            .valid = true,
        };
    }
    input.parser_stats_available = true;
    input.parser_baseline.emitted_frames = 10;
    input.parser_current.emitted_frames = 12;
    set_stage(&input, stage);
    return input;
}

static void assert_fault(const bzm_runtime_health_input_t * input, bzm_runtime_health_fault_t fault)
{
    bzm_runtime_health_result_t result = bzm_runtime_health_evaluate(input);
    TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_BAD, result.status);
    TEST_ASSERT_EQUAL_HEX16(fault, result.fault);
    TEST_ASSERT_NOT_EQUAL('\0', result.detail[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', result.detail[sizeof(result.detail) - 1]);
}

TEST_CASE("BZM runtime health skips OFF_SAFE and non-holding snapshots", "[asic][bzm][runtime-health]")
{
    bzm_runtime_health_input_t input;
    memset(&input, 0xa5, sizeof(input));
    input.holding = false;
    input.reached_stage = BZM_STAGE_COUNT;
    bzm_runtime_health_result_t result = bzm_runtime_health_evaluate(&input);
    TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_GOOD, result.status);
    TEST_ASSERT_EQUAL_HEX16(BZM_RUNTIME_HEALTH_FAULT_NONE, result.fault);

    input.holding = true;
    input.reached_stage = BZM_STAGE_OFF_SAFE;
    result = bzm_runtime_health_evaluate(&input);
    TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_GOOD, result.status);
    TEST_ASSERT_EQUAL_HEX16(BZM_RUNTIME_HEALTH_FAULT_NONE, result.fault);
}

TEST_CASE("BZM runtime health applies only checks reached by each stage", "[asic][bzm][runtime-health]")
{
    bzm_runtime_health_input_t controls = good_input(BZM_STAGE_CONTROLS);
    controls.tps.available = false;
    controls.parser_stats_available = false;
    controls.telemetry_available = false;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_GOOD, bzm_runtime_health_evaluate(&controls).status);

    bzm_runtime_health_input_t power = good_input(BZM_STAGE_POWER_RAIL);
    power.parser_stats_available = false;
    power.telemetry_available = false;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_GOOD, bzm_runtime_health_evaluate(&power).status);

    bzm_runtime_health_input_t chain = good_input(BZM_STAGE_CHAIN_4);
    chain.telemetry_available = false;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_GOOD, bzm_runtime_health_evaluate(&chain).status);

    bzm_runtime_health_input_t sensors = good_input(BZM_STAGE_SENSORS);
    for (size_t i = 0; i < BZM_MAX_ASIC_COUNT; ++i) {
        sensors.telemetry.samples[i].pll_locked = false;
    }
    TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_GOOD, bzm_runtime_health_evaluate(&sensors).status);

    for (int stage = BZM_STAGE_CLOCKS; stage < BZM_STAGE_COUNT; ++stage) {
        bzm_runtime_health_input_t input = good_input((bzm_validation_stage_t) stage);
        bzm_runtime_health_result_t result = bzm_runtime_health_evaluate(&input);
        TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_GOOD, result.status);
        TEST_ASSERT_EQUAL_HEX16(BZM_RUNTIME_HEALTH_FAULT_NONE, result.fault);
    }
}

TEST_CASE("BZM runtime health rejects invalid active input and stage", "[asic][bzm][runtime-health]")
{
    assert_fault(NULL, BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT);
    bzm_runtime_health_input_t input = good_input(BZM_STAGE_CONTROLS);
    input.reached_stage = BZM_STAGE_COUNT;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_INVALID_STAGE);

    input = good_input(BZM_STAGE_CONTROLS);
    input.fan_min_rpm = 0;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT);
}

TEST_CASE("BZM runtime health fails closed on bridge safety evidence", "[asic][bzm][runtime-health]")
{
    bzm_runtime_health_input_t input = good_input(BZM_STAGE_CLOCKS);
    input.bridge_status_available = false;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_UNAVAILABLE);

    input = good_input(BZM_STAGE_CLOCKS);
    input.bridge_status.schema_version++;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_STATUS_INVALID);

    input = good_input(BZM_STAGE_CLOCKS);
    input.bridge_status.stage = BZM_BRIDGE_SAFETY_STAGE_BOOT_SAFE;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_STATUS_INVALID);

    input = good_input(BZM_STAGE_CLOCKS);
    input.bridge_status.capabilities &= (uint16_t) ~BZM_BRIDGE_SAFETY_CAP_TRIP_INPUT_SAMPLED;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_CAPABILITY_MISSING);

    input = good_input(BZM_STAGE_CLOCKS);
    input.bridge_status.state = BZM_BRIDGE_SAFETY_STATE_SAFE_OFF;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_STATE);

    input = good_input(BZM_STAGE_CLOCKS);
    input.bridge_status.fault = BZM_BRIDGE_SAFETY_FAULT_LEASE_EXPIRED;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_FAULT);

    input = good_input(BZM_STAGE_CLOCKS);
    input.bridge_status.trip_input_asserted = true;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_FAULT);

    input = good_input(BZM_STAGE_CLOCKS);
    input.bridge_status.lease_remaining_ms = 0;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_LEASE);

    input = good_input(BZM_STAGE_CLOCKS);
    input.bridge_status.evidence &= (uint16_t) ~BZM_BRIDGE_SAFETY_EVIDENCE_TRIP_CLEAR;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_LEASE);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.bridge_status.evidence &= (uint16_t) ~BZM_BRIDGE_SAFETY_EVIDENCE_OUTPUTS_SAFE;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_STATUS_INVALID);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.require_independent_kill = true;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_CAPABILITY_MISSING);

    input.bridge_status.stage = BZM_BRIDGE_SAFETY_STAGE_TRIP_LATCH;
    input.bridge_status.production_verdict = BZM_BRIDGE_SAFETY_PRODUCTION_GOOD;
    input.bridge_status.capabilities |= BZM_BRIDGE_SAFETY_CAP_CORE_POWER_CUTOFF | BZM_BRIDGE_SAFETY_CAP_FAN_TACH_INTERLOCK |
                                        BZM_BRIDGE_SAFETY_CAP_INDEPENDENT_TRIP_MONITOR;
    input.bridge_status.evidence |= BZM_BRIDGE_SAFETY_EVIDENCE_CORE_CUTOFF_AVAILABLE |
                                    BZM_BRIDGE_SAFETY_EVIDENCE_FAN_TACH_INTERLOCK_AVAILABLE |
                                    BZM_BRIDGE_SAFETY_EVIDENCE_INDEPENDENT_TRIP_MONITOR_AVAILABLE;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_GOOD, bzm_runtime_health_evaluate(&input).status);
}

TEST_CASE("BZM runtime health enforces fan command and tach threshold", "[asic][bzm][runtime-health]")
{
    bzm_runtime_health_input_t input = good_input(BZM_STAGE_CLOCKS);
    input.bridge_status.fan_full = false;
    input.bridge_status.fan_percent = 99;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_FAN_NOT_FULL);

    input = good_input(BZM_STAGE_CLOCKS);
    input.fan_tach_available = false;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_FAN_TACH_UNAVAILABLE);

    input = good_input(BZM_STAGE_CLOCKS);
    input.fan_rpm = input.fan_min_rpm - 1;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_FAN_TACH_LOW);

    input.fan_rpm = input.fan_min_rpm;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_GOOD, bzm_runtime_health_evaluate(&input).status);
}

TEST_CASE("BZM runtime health derives exact bridge outputs from stage", "[asic][bzm][runtime-health]")
{
    bzm_runtime_health_input_t input = good_input(BZM_STAGE_POWER_RAIL);
    input.bridge_status.five_volt_enabled = true;
    input.bridge_status.evidence &= (uint16_t) ~BZM_BRIDGE_SAFETY_EVIDENCE_OUTPUTS_SAFE;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_OUTPUT);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.bridge_status.asic_reset_asserted = false;
    input.bridge_status.evidence &= (uint16_t) ~BZM_BRIDGE_SAFETY_EVIDENCE_OUTPUTS_SAFE;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_OUTPUT);

    input = good_input(BZM_STAGE_CHAIN_4);
    input.bridge_status.five_volt_enabled = false;
    input.bridge_status.asic_reset_asserted = true;
    input.bridge_status.evidence |= BZM_BRIDGE_SAFETY_EVIDENCE_OUTPUTS_SAFE;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_OUTPUT);

    input = good_input(BZM_STAGE_CHAIN_4);
    input.bridge_status.asic_reset_asserted = true;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_OUTPUT);
}

TEST_CASE("BZM runtime health validates every TPS runtime invariant", "[asic][bzm][runtime-health]")
{
    bzm_runtime_health_input_t input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.available = false;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_UNAVAILABLE);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.pgood = false;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_PGOOD_LOW);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.operation = 0;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_OPERATION_OFF);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.status_word = 0x0040;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_STATUS);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.status_word = 0x1000;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_STATUS);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.vout_command_matches_expected = false;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_VOUT_COMMAND);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.vout_command_v = 2.81f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_VOUT_COMMAND);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.vout_command_v = NAN;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_VOUT_COMMAND);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.vin_v = NAN;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_VIN_RANGE);
    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.vin_v = input.tps_bounds.vin_max_v + 0.1f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_VIN_RANGE);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.vout_v = input.tps_bounds.vout_min_v - 0.01f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_VOUT_RANGE);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.iout_a = input.tps_bounds.iout_max_a + 0.1f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_IOUT_RANGE);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.iout_a = NAN;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_IOUT_RANGE);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps.temperature_c = input.tps_bounds.temperature_max_c + 1.0f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TPS_TEMPERATURE_RANGE);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps_bounds.vout_min_v = input.tps_bounds.vout_max_v + 1.0f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT);

    input = good_input(BZM_STAGE_POWER_RAIL);
    input.tps_bounds.vout_command_tolerance_v = -0.001f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT);
}

#define ASSERT_PARSER_DELTA(field)                                                                                                 \
    do {                                                                                                                           \
        bzm_runtime_health_input_t input = good_input(BZM_STAGE_CHAIN_4);                                                          \
        input.parser_current.field = input.parser_baseline.field + 1;                                                              \
        assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_PARSER_ERROR_DELTA);                                                         \
    } while (0)

TEST_CASE("BZM runtime health rejects every parser error delta and reset", "[asic][bzm][runtime-health]")
{
    bzm_runtime_health_input_t input = good_input(BZM_STAGE_CHAIN_4);
    input.parser_stats_available = false;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_PARSER_UNAVAILABLE);

    ASSERT_PARSER_DELTA(discarded_bytes);
    ASSERT_PARSER_DELTA(unexpected_register_headers);
    ASSERT_PARSER_DELTA(dropped_results);
    ASSERT_PARSER_DELTA(rejected_result_frames);
    ASSERT_PARSER_DELTA(unmatched_register_frames);
    ASSERT_PARSER_DELTA(unsolicited_noop_frames);
    ASSERT_PARSER_DELTA(invalid_noop_frames);
    ASSERT_PARSER_DELTA(telemetry_decode_failures);

    input = good_input(BZM_STAGE_CHAIN_4);
    input.parser_baseline.emitted_frames = 20;
    input.parser_current.emitted_frames = 19;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_PARSER_COUNTER_REGRESSED);

    input = good_input(BZM_STAGE_CHAIN_4);
    input.parser_baseline.discarded_bytes = 2;
    input.parser_current.discarded_bytes = 1;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_PARSER_COUNTER_REGRESSED);
}

TEST_CASE("BZM pre-dispatch parser window accepts frames but no errors or queued results", "[asic][bzm][runtime-health]")
{
    bzm_serial_parser_stats_t baseline = {
        .emitted_frames = 10,
        .discarded_bytes = 5,
    };
    bzm_serial_parser_stats_t current = baseline;
    current.emitted_frames = 20;
    TEST_ASSERT_TRUE(bzm_runtime_health_parser_window_is_clean(&baseline, &current));

    current.discarded_bytes++;
    TEST_ASSERT_FALSE(bzm_runtime_health_parser_window_is_clean(&baseline, &current));
    current = baseline;
    current.queued_results = 1;
    TEST_ASSERT_FALSE(bzm_runtime_health_parser_window_is_clean(&baseline, &current));
    current = baseline;
    current.buffered_bytes = 1;
    TEST_ASSERT_FALSE(bzm_runtime_health_parser_window_is_clean(&baseline, &current));
    current = baseline;
    current.emitted_frames--;
    TEST_ASSERT_FALSE(bzm_runtime_health_parser_window_is_clean(&baseline, &current));
    TEST_ASSERT_FALSE(bzm_runtime_health_parser_window_is_clean(NULL, &current));
    TEST_ASSERT_FALSE(bzm_runtime_health_parser_window_is_clean(&baseline, NULL));
}

TEST_CASE("BZM stage-entry settling accepts only discarded transition residue", "[asic][bzm][runtime-health]")
{
    bzm_serial_parser_stats_t baseline = {
        .emitted_frames = 10,
        .discarded_bytes = 5,
    };
    bzm_serial_parser_stats_t current = baseline;
    current.emitted_frames = 20;
    current.discarded_bytes = 17;
    TEST_ASSERT_TRUE(bzm_runtime_health_parser_window_is_discard_transition(&baseline, &current));

    current.unexpected_register_headers = 1;
    TEST_ASSERT_FALSE(bzm_runtime_health_parser_window_is_discard_transition(&baseline, &current));
    current = baseline;
    current.discarded_bytes = 4;
    TEST_ASSERT_FALSE(bzm_runtime_health_parser_window_is_discard_transition(&baseline, &current));
    current = baseline;
    current.queued_results = 1;
    TEST_ASSERT_FALSE(bzm_runtime_health_parser_window_is_discard_transition(&baseline, &current));
    current = baseline;
    current.buffered_bytes = 1;
    TEST_ASSERT_FALSE(bzm_runtime_health_parser_window_is_discard_transition(&baseline, &current));
    TEST_ASSERT_FALSE(bzm_runtime_health_parser_window_is_discard_transition(NULL, &current));
    TEST_ASSERT_FALSE(bzm_runtime_health_parser_window_is_discard_transition(&baseline, NULL));
}

TEST_CASE("BZM parser settling restarts its clean-window proof after transition residue", "[asic][bzm][runtime-health]")
{
    bzm_serial_parser_stats_t baseline = {.emitted_frames = 10};
    bzm_serial_parser_stats_t current = baseline;
    bzm_parser_settling_t settling;
    bzm_parser_settling_init(&settling, &baseline);

    current.emitted_frames = 20;
    TEST_ASSERT_EQUAL(BZM_PARSER_SETTLING_PENDING, bzm_parser_settling_observe(&settling, &current, 2));
    TEST_ASSERT_EQUAL_UINT8(1, settling.clean_windows);

    current.emitted_frames = 30;
    current.discarded_bytes = 9;
    TEST_ASSERT_EQUAL(BZM_PARSER_SETTLING_PENDING, bzm_parser_settling_observe(&settling, &current, 2));
    TEST_ASSERT_EQUAL_UINT8(0, settling.clean_windows);

    current.emitted_frames = 40;
    TEST_ASSERT_EQUAL(BZM_PARSER_SETTLING_PENDING, bzm_parser_settling_observe(&settling, &current, 2));
    current.emitted_frames = 50;
    TEST_ASSERT_EQUAL(BZM_PARSER_SETTLING_COMPLETE, bzm_parser_settling_observe(&settling, &current, 2));
    TEST_ASSERT_EQUAL_UINT8(2, settling.clean_windows);

    current.unexpected_register_headers = 1;
    TEST_ASSERT_EQUAL(BZM_PARSER_SETTLING_BAD, bzm_parser_settling_observe(&settling, &current, 2));
    bzm_parser_settling_init(&settling, NULL);
    TEST_ASSERT_EQUAL(BZM_PARSER_SETTLING_BAD, bzm_parser_settling_observe(&settling, &current, 2));

    baseline.buffered_bytes = 1;
    bzm_parser_settling_init(&settling, &baseline);
    TEST_ASSERT_EQUAL(BZM_PARSER_SETTLING_BAD, bzm_parser_settling_observe(&settling, &current, 2));
}

TEST_CASE("BZM parser settling snapshots require only a complete frame boundary", "[asic][bzm][runtime-health]")
{
    bzm_serial_parser_stats_t stats = {
        .emitted_frames = 100,
        .queued_results = 0,
        .buffered_bytes = 0,
    };
    TEST_ASSERT_TRUE(bzm_parser_settling_snapshot_at_frame_boundary(&stats));

    stats.emitted_frames = 101;
    TEST_ASSERT_TRUE(bzm_parser_settling_snapshot_at_frame_boundary(&stats));

    stats.buffered_bytes = 1;
    TEST_ASSERT_FALSE(bzm_parser_settling_snapshot_at_frame_boundary(&stats));
    TEST_ASSERT_FALSE(bzm_parser_settling_snapshot_at_frame_boundary(NULL));
}

TEST_CASE("BZM Stage 7 parser realignment accepts queued complete results but requires a frame boundary",
          "[asic][bzm][runtime-health]")
{
    bzm_serial_parser_stats_t baseline = {.emitted_frames = 100, .discarded_bytes = 4};
    bzm_serial_parser_stats_t current = baseline;
    bzm_parser_realign_t realign;
    bzm_parser_realign_init(&realign, &baseline);

    current.discarded_bytes = 22;
    current.emitted_frames_at_last_discard = 100;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_PENDING, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));
    TEST_ASSERT_TRUE(realign.recovering);
    TEST_ASSERT_EQUAL_UINT8(1, realign.episode_bursts);

    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_PENDING, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));
    TEST_ASSERT_EQUAL_UINT8(0, realign.clean_windows);

    current.emitted_frames = 101;
    current.queued_results = 3;
    current.buffered_bytes = 1;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_PENDING, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));
    TEST_ASSERT_EQUAL_UINT8(0, realign.clean_windows);

    current.buffered_bytes = 0;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_PENDING, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));
    TEST_ASSERT_EQUAL_UINT8(1, realign.clean_windows);
    current.emitted_frames = 102;
    current.queued_results = 4;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_RECOVERED, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));
    TEST_ASSERT_FALSE(realign.recovering);
    TEST_ASSERT_EQUAL_UINT8(0, realign.episode_bursts);
    TEST_ASSERT_EQUAL_UINT32(22, realign.accepted.discarded_bytes);

    current.emitted_frames = 103;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_CLEAN, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));
}

TEST_CASE("BZM Stage 7 parser realignment rejects continuous and unrelated corruption", "[asic][bzm][runtime-health]")
{
    bzm_serial_parser_stats_t baseline = {.emitted_frames = 100, .discarded_bytes = 4};
    bzm_serial_parser_stats_t current = baseline;
    bzm_parser_realign_t realign;
    bzm_parser_realign_init(&realign, &baseline);

    current.discarded_bytes = 37;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_BAD, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));

    bzm_parser_realign_init(&realign, &baseline);
    current = baseline;
    current.unexpected_register_headers = 1;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_BAD, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));

    bzm_parser_realign_init(&realign, &baseline);
    current = baseline;
    current.discarded_bytes = 5;
    current.unexpected_register_headers = 2;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_BAD, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));

    bzm_parser_realign_init(&realign, NULL);
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_BAD, bzm_parser_realign_observe(&realign, &baseline, 32, 2, 6, 2));
}

TEST_CASE("BZM Stage 7 parser realignment accepts a rejected header inside a bounded discard episode",
          "[asic][bzm][runtime-health]")
{
    bzm_serial_parser_stats_t baseline = {.emitted_frames = 100, .discarded_bytes = 4};
    bzm_serial_parser_stats_t current = baseline;
    bzm_parser_realign_t realign;
    bzm_parser_realign_init(&realign, &baseline);

    /* This is the counter relationship captured on the device: the false
     * register header is rejected while the byte-at-a-time parser discards
     * the same damaged nine-byte region. */
    current.discarded_bytes = 13;
    current.unexpected_register_headers = 1;
    current.emitted_frames_at_last_discard = current.emitted_frames;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_PENDING, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));
    TEST_ASSERT_EQUAL_UINT32(4, realign.burst_discard_baseline);
    TEST_ASSERT_EQUAL_UINT32(0, realign.burst_unexpected_register_baseline);

    ++current.emitted_frames;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_PENDING, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));
    ++current.emitted_frames;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_RECOVERED, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));
    TEST_ASSERT_EQUAL_UINT32(13, realign.accepted.discarded_bytes);
    TEST_ASSERT_EQUAL_UINT32(1, realign.accepted.unexpected_register_headers);

    /* A later header classification without another discard is independent
     * corruption and still fails immediately. */
    ++current.unexpected_register_headers;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_BAD, bzm_parser_realign_observe(&realign, &current, 32, 2, 6, 2));
}

TEST_CASE("BZM Stage 7 parser realignment permits later independently recovered episodes", "[asic][bzm][runtime-health]")
{
    bzm_serial_parser_stats_t baseline = {.emitted_frames = 10};
    bzm_serial_parser_stats_t current = baseline;
    bzm_parser_realign_t realign;
    bzm_parser_realign_init(&realign, &baseline);

    for (uint32_t event = 1; event <= 4; ++event) {
        current.discarded_bytes = event;
        current.emitted_frames_at_last_discard = current.emitted_frames;
        TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_PENDING, bzm_parser_realign_observe(&realign, &current, 32, 1, 3, 2));
        TEST_ASSERT_EQUAL_UINT8(1, realign.episode_bursts);
        ++current.emitted_frames;
        TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_RECOVERED, bzm_parser_realign_observe(&realign, &current, 32, 1, 3, 2));
        TEST_ASSERT_EQUAL_UINT8(0, realign.episode_bursts);
    }
}

TEST_CASE("BZM Stage 7 parser realignment limits discard bursts inside one unresolved episode", "[asic][bzm][runtime-health]")
{
    bzm_serial_parser_stats_t baseline = {.emitted_frames = 10};
    bzm_serial_parser_stats_t current = baseline;
    bzm_parser_realign_t realign;
    bzm_parser_realign_init(&realign, &baseline);

    current.discarded_bytes = 1;
    current.emitted_frames_at_last_discard = current.emitted_frames;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_PENDING, bzm_parser_realign_observe(&realign, &current, 32, 1, 6, 2));
    TEST_ASSERT_EQUAL_UINT8(1, realign.episode_bursts);

    current.discarded_bytes = 2;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_PENDING, bzm_parser_realign_observe(&realign, &current, 32, 1, 6, 2));
    TEST_ASSERT_EQUAL_UINT8(2, realign.episode_bursts);

    current.discarded_bytes = 3;
    TEST_ASSERT_EQUAL(BZM_PARSER_REALIGN_BAD, bzm_parser_realign_observe(&realign, &current, 32, 1, 3, 2));
}

#undef ASSERT_PARSER_DELTA

TEST_CASE("BZM runtime health requires four fresh valid bounded ASIC samples", "[asic][bzm][runtime-health]")
{
    bzm_runtime_health_input_t input = good_input(BZM_STAGE_SENSORS);
    input.telemetry_available = false;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_UNAVAILABLE);

    input = good_input(BZM_STAGE_SENSORS);
    input.telemetry.samples[2].received = false;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_MISSING);

    input = good_input(BZM_STAGE_SENSORS);
    input.telemetry.samples[1].asic_id = BZM_FIRST_ASIC_ID;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_MISSING);

    input = good_input(BZM_STAGE_SENSORS);
    input.telemetry.samples[0].trip = true;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_TRIP);

    input = good_input(BZM_STAGE_SENSORS);
    input.telemetry.samples[0].valid = false;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_INVALID);

    input = good_input(BZM_STAGE_SENSORS);
    input.telemetry.samples[3].timestamp_us = input.telemetry_now_us - input.telemetry_max_age_us - 1;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_STALE);

    input = good_input(BZM_STAGE_SENSORS);
    input.telemetry.samples[3].timestamp_us = input.telemetry_now_us + 1;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_STALE);

    input = good_input(BZM_STAGE_SENSORS);
    input.telemetry.samples[2].ch2_mv = input.telemetry_bounds.ch2_abs_max_mv + 1.0f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_BOUNDS);

    input = good_input(BZM_STAGE_SENSORS);
    input.defer_ch2_bounds = true;
    input.telemetry.samples[2].ch2_mv = input.telemetry_bounds.ch2_abs_max_mv + 1.0f;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_GOOD, bzm_runtime_health_evaluate(&input).status);

    input.telemetry.samples[2].voltage_fault = true;
    input.telemetry.samples[2].voltage_valid = false;
    input.telemetry.samples[2].valid = false;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_GOOD, bzm_runtime_health_evaluate(&input).status);

    input.telemetry.samples[2].ch2_mv = 0.0f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_INVALID);

    input = good_input(BZM_STAGE_SENSORS);
    input.defer_ch2_bounds = true;
    input.telemetry.samples[2].ch0_mv = input.telemetry_bounds.ch0_min_mv - 1.0f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_BOUNDS);

    input = good_input(BZM_STAGE_SENSORS);
    input.telemetry.samples[2].ch0_mv = 300.0f;
    input.telemetry.samples[2].ch1_mv = 400.1f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_BOUNDS);

    input = good_input(BZM_STAGE_SENSORS);
    input.telemetry.samples[2].temperature_c = NAN;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_BOUNDS);

    input = good_input(BZM_STAGE_SENSORS);
    input.telemetry_bounds.ch1_min_mv = input.telemetry_bounds.ch1_max_mv + 1.0f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT);

    input = good_input(BZM_STAGE_SENSORS);
    input.telemetry_bounds.max_stack_spread_mv = -1.0f;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT);
}

TEST_CASE("BZM runtime health requires combined PLL telemetry from CLOCKS", "[asic][bzm][runtime-health]")
{
    bzm_runtime_health_input_t input = good_input(BZM_STAGE_CLOCKS);
    input.telemetry.samples[0].pll_locked = false;
    assert_fault(&input, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_CLOCK_UNLOCKED);

    input.defer_clock_locks = true;
    TEST_ASSERT_EQUAL(BZM_RUNTIME_HEALTH_GOOD, bzm_runtime_health_evaluate(&input).status);
}

TEST_CASE("BZM runtime health fault codes and names remain stable", "[asic][bzm][runtime-health]")
{
    TEST_ASSERT_EQUAL_STRING("GOOD", bzm_runtime_health_status_name(BZM_RUNTIME_HEALTH_GOOD));
    TEST_ASSERT_EQUAL_STRING("BAD", bzm_runtime_health_status_name(BZM_RUNTIME_HEALTH_BAD));
    TEST_ASSERT_EQUAL_STRING("INVALID_STATUS", bzm_runtime_health_status_name((bzm_runtime_health_status_t) 99));
    TEST_ASSERT_EQUAL_HEX16(0x0000, BZM_RUNTIME_HEALTH_FAULT_NONE);
    TEST_ASSERT_EQUAL_HEX16(0x0106, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_OUTPUT);
    TEST_ASSERT_EQUAL_HEX16(0x0303, BZM_RUNTIME_HEALTH_FAULT_TPS_STATUS);
    TEST_ASSERT_EQUAL_HEX16(0x0307, BZM_RUNTIME_HEALTH_FAULT_TPS_IOUT_RANGE);
    TEST_ASSERT_EQUAL_HEX16(0x0406, BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_CLOCK_UNLOCKED);
    TEST_ASSERT_EQUAL_HEX16(0x0502, BZM_RUNTIME_HEALTH_FAULT_PARSER_ERROR_DELTA);
    TEST_ASSERT_EQUAL_STRING("BRIDGE_OUTPUT", bzm_runtime_health_fault_name(BZM_RUNTIME_HEALTH_FAULT_BRIDGE_OUTPUT));
    TEST_ASSERT_EQUAL_STRING("TELEMETRY_CLOCK_UNLOCKED",
                             bzm_runtime_health_fault_name(BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_CLOCK_UNLOCKED));
    TEST_ASSERT_EQUAL_STRING("INVALID_FAULT", bzm_runtime_health_fault_name((bzm_runtime_health_fault_t) 0xffff));
}
