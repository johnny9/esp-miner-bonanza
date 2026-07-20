#include "unity.h"

#include <string.h>

#include "bzm_bringup.h"
#include "bzm_registers.h"

typedef struct
{
    uint32_t registers[BZM_BRINGUP_ASIC_COUNT][256];
    uint32_t assigned_values[BZM_BRINGUP_ASIC_COUNT];
    bool assigned[BZM_BRINGUP_ASIC_COUNT];
    uint8_t unaddressed;
    uint8_t broadcast_probe_count;
    uint8_t addressed_probe_count;
    uint8_t transient_unaddressed_noop_misses;
    bool extra_asic;
    bool mismatch;
    uint8_t mismatch_asic;
    uint8_t mismatch_offset;
    uint32_t mismatch_value;
    bool fail_reads_while_tdm_active;
    uint16_t reads_while_tdm_active;
    bool pll_locked[BZM_BRINGUP_ASIC_COUNT][2];
    uint64_t now_us;
    bool telemetry_missing;
    uint8_t telemetry_missing_id;
    bool telemetry_unsafe;
    uint8_t telemetry_unsafe_id;
    uint8_t telemetry_ch2_excursion_snapshots;
    uint8_t telemetry_ch2_excursion_id;
    bool telemetry_ch2_excursion_voltage_fault;
    uint8_t telemetry_pll_unlock_snapshots;
    uint8_t telemetry_pll_unlock_id;
    bool telemetry_old;
    uint16_t telemetry_old_snapshots;
    bool telemetry_clocks_locked;
    uint16_t telemetry_snapshot_count;
    uint16_t write_count;
    uint16_t all_asic_write_count;
    bool fail_all_asic_write;
    uint16_t batch_begin_count;
    uint16_t batch_end_count;
    int fail_batch_begin_at;
    int fail_batch_end_at;
    bool batch_active;
    uint16_t pair_commit_count;
    int fail_pair_commit_at;
    bool barrier_result;
    uint16_t barrier_count;
} bringup_mock_t;

static int asic_index(uint8_t asic_id)
{
    size_t index = 0;
    if (!bzm_topology_asic_index(asic_id, &index))
        return -1;
    return (int) index;
}

static bzm_bringup_probe_result_t mock_probe(void * context, uint8_t asic_id)
{
    bringup_mock_t * mock = context;
    if (asic_id == BZM_BROADCAST_ASIC) {
        ++mock->broadcast_probe_count;
        if (mock->transient_unaddressed_noop_misses != 0) {
            --mock->transient_unaddressed_noop_misses;
            return BZM_BRINGUP_PROBE_NO_RESPONSE;
        }
        return mock->unaddressed != 0 || mock->extra_asic ? BZM_BRINGUP_PROBE_RESPONSE : BZM_BRINGUP_PROBE_NO_RESPONSE;
    }
    ++mock->addressed_probe_count;
    int index = asic_index(asic_id);
    return index >= 0 && mock->assigned[index] ? BZM_BRINGUP_PROBE_RESPONSE : BZM_BRINGUP_PROBE_NO_RESPONSE;
}

static bool mock_write(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset, uint32_t value)
{
    bringup_mock_t * mock = context;
    TEST_ASSERT_EQUAL_HEX16(BZM_BRINGUP_CONTROL_ENGINE_ID, engine_id);
    ++mock->write_count;

    if (asic_id == BZM_BROADCAST_ASIC && offset == BZM_LOCAL_REG_ASIC_ID) {
        int index = asic_index(value & 0xffU);
        if (index < 0 || mock->unaddressed == 0)
            return false;
        mock->assigned[index] = true;
        mock->assigned_values[index] = value;
        mock->registers[index][offset] = value | 0x100U;
        --mock->unaddressed;
        return true;
    }

    if (asic_id == BZM_ALL_ASICS) {
        ++mock->all_asic_write_count;
        if (mock->fail_all_asic_write)
            return false;
        for (size_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
            mock->registers[index][offset] = value;
        }
        return true;
    }

    int index = asic_index(asic_id);
    if (index < 0)
        return false;
    mock->registers[index][offset] = value;
    return true;
}

static bool mock_read(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset, uint32_t * value)
{
    bringup_mock_t * mock = context;
    TEST_ASSERT_EQUAL_HEX16(BZM_BRINGUP_CONTROL_ENGINE_ID, engine_id);
    int index = asic_index(asic_id);
    if (index < 0 || value == NULL)
        return false;
    for (uint8_t item = 0; item < BZM_BRINGUP_ASIC_COUNT; ++item) {
        if ((mock->registers[item][BZM_LOCAL_REG_UART_TDM_CONTROL] & 1U) != 0) {
            ++mock->reads_while_tdm_active;
            if (mock->fail_reads_while_tdm_active && offset != BZM_LOCAL_REG_UART_TDM_CONTROL) {
                return false;
            }
            break;
        }
    }
    if (mock->mismatch && mock->mismatch_asic == asic_id && mock->mismatch_offset == offset) {
        *value = mock->mismatch_value;
        return true;
    }

    *value = mock->registers[index][offset];
    if (offset == BZM_LOCAL_REG_PLL0_ENABLE && mock->pll_locked[index][0]) {
        *value |= 0x04;
    } else if (offset == BZM_LOCAL_REG_PLL1_ENABLE && mock->pll_locked[index][1]) {
        *value |= 0x04;
    }
    return true;
}

static void mock_delay(void * context, uint32_t delay_ms)
{
    bringup_mock_t * mock = context;
    mock->now_us += (uint64_t) delay_ms * 1000U;
}

static uint64_t mock_now(void * context)
{
    bringup_mock_t * mock = context;
    return mock->now_us;
}

static bool mock_telemetry(void * context, bzm_telemetry_store_t * store)
{
    bringup_mock_t * mock = context;
    ++mock->telemetry_snapshot_count;
    bzm_telemetry_store_init(store);
    for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        uint8_t asic_id = bzm_asic_wire_ids[index];
        bzm_telemetry_sample_t * sample = &store->samples[index];
        bool ch2_excursion = mock->telemetry_ch2_excursion_id == asic_id &&
                             mock->telemetry_snapshot_count <= mock->telemetry_ch2_excursion_snapshots;
        *sample = (bzm_telemetry_sample_t){
            .asic_id = asic_id,
            .timestamp_us =
                mock->telemetry_old || mock->telemetry_snapshot_count <= mock->telemetry_old_snapshots ? 1 : mock->now_us,
            .received = !(mock->telemetry_missing && mock->telemetry_missing_id == asic_id),
            .temperature_c = 50.0f,
            .thermal_enabled = true,
            .thermal_validity = true,
            .thermal_valid = true,
            .ch0_mv = 350.0f,
            .ch1_mv = 350.0f,
            .ch2_mv = ch2_excursion ? 75.0f : 0.0f,
            .voltage_enabled = true,
            .voltage_fault = ch2_excursion && mock->telemetry_ch2_excursion_voltage_fault,
            .voltage_valid = !(ch2_excursion && mock->telemetry_ch2_excursion_voltage_fault),
            .pll_locked =
                mock->telemetry_clocks_locked && !(mock->telemetry_pll_unlock_id == asic_id &&
                                                   mock->telemetry_snapshot_count <= mock->telemetry_pll_unlock_snapshots),
            .valid = !(ch2_excursion && mock->telemetry_ch2_excursion_voltage_fault),
            .trip = mock->telemetry_unsafe && mock->telemetry_unsafe_id == asic_id,
        };
    }
    return true;
}

static bool mock_balanced_pair(void * context, uint8_t asic_id, const bzm_engine_pair_t * pair)
{
    bringup_mock_t * mock = context;
    TEST_ASSERT_NOT_NULL(pair);
    TEST_ASSERT_TRUE(mock->batch_active);
    TEST_ASSERT_TRUE(asic_index(asic_id) >= 0);
    TEST_ASSERT_EQUAL_UINT16(mock->pair_commit_count / BZM_BRINGUP_ASIC_COUNT, pair->pair_index);
    TEST_ASSERT_EQUAL(BZM_ENGINE_STACK_BOTTOM, pair->bottom.stack);
    TEST_ASSERT_EQUAL(BZM_ENGINE_STACK_TOP, pair->top.stack);
    if (mock->fail_pair_commit_at >= 0 && mock->pair_commit_count == mock->fail_pair_commit_at)
        return false;
    ++mock->pair_commit_count;
    return true;
}

static bool mock_batch_begin(void * context, uint16_t pair_index)
{
    bringup_mock_t * mock = context;
    TEST_ASSERT_FALSE(mock->batch_active);
    TEST_ASSERT_EQUAL_UINT16(mock->batch_begin_count, pair_index);
    if (mock->fail_batch_begin_at >= 0 && mock->batch_begin_count == mock->fail_batch_begin_at)
        return false;
    mock->batch_active = true;
    ++mock->batch_begin_count;
    return true;
}

static bool mock_batch_end(void * context, uint16_t pair_index)
{
    bringup_mock_t * mock = context;
    TEST_ASSERT_TRUE(mock->batch_active);
    TEST_ASSERT_EQUAL_UINT16(mock->batch_end_count, pair_index);
    if (mock->fail_batch_end_at >= 0 && mock->batch_end_count == mock->fail_batch_end_at)
        return false;
    mock->batch_active = false;
    ++mock->batch_end_count;
    return true;
}

static bool mock_barrier(void * context, size_t asic_count, size_t pairs_per_asic)
{
    bringup_mock_t * mock = context;
    TEST_ASSERT_EQUAL_UINT(BZM_BRINGUP_ASIC_COUNT, asic_count);
    TEST_ASSERT_EQUAL_UINT(BZM_TOPOLOGY_PAIR_COUNT, pairs_per_asic);
    ++mock->barrier_count;
    return mock->barrier_result;
}

static const bzm_bringup_ops_t MOCK_OPS = {
    .probe_noop = mock_probe,
    .write_u32 = mock_write,
    .read_u32 = mock_read,
    .delay_ms = mock_delay,
    .now_us = mock_now,
    .telemetry_snapshot = mock_telemetry,
    .balanced_batch_begin = mock_batch_begin,
    .balanced_pair_commit = mock_balanced_pair,
    .balanced_batch_end = mock_batch_end,
    .activation_barrier = mock_barrier,
};

static bzm_bringup_telemetry_policy_t telemetry_policy(void)
{
    return (bzm_bringup_telemetry_policy_t){
        .bounds =
            {
                .temperature_min_c = -20.0f,
                .temperature_max_c = 105.0f,
                .ch0_min_mv = 250.0f,
                .ch0_max_mv = 550.0f,
                .ch1_min_mv = 250.0f,
                .ch1_max_mv = 550.0f,
                .ch2_abs_max_mv = 50.0f,
                .max_stack_spread_mv = 100.0f,
            },
        .max_age_us = 1000000,
        .ch2_confirm_samples = 3,
    };
}

static bringup_mock_t good_mock(void)
{
    bringup_mock_t mock = {
        .unaddressed = BZM_BRINGUP_ASIC_COUNT,
        .now_us = 1000000,
        .telemetry_clocks_locked = true,
        .fail_batch_begin_at = -1,
        .fail_batch_end_at = -1,
        .fail_pair_commit_at = -1,
        .barrier_result = true,
    };
    for (size_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        mock.registers[index][BZM_LOCAL_REG_BANDGAP] = 0xabcd1234;
        mock.registers[index][BZM_LOCAL_REG_UART_TDM_CONTROL] = 0x0000fec9;
        mock.registers[index][BZM_LOCAL_REG_SLOW_CLOCK_DIVIDER] = 2;
        mock.registers[index][BZM_LOCAL_REG_TDM_DELAY] = 1;
        mock.registers[index][BZM_LOCAL_REG_UART_TX] = 0x0f;
        mock.registers[index][BZM_LOCAL_REG_SENSOR_TDM_GAP_COUNT] = CONFIG_BZM_1002_SENSOR_TDM_GAP_COUNT;
        mock.registers[index][BZM_LOCAL_REG_SENSOR_CLOCK_DIVIDER] = 0x108;
        mock.pll_locked[index][0] = true;
        mock.pll_locked[index][1] = true;
    }
    return mock;
}

static void state_at_sensors(bzm_bringup_state_t * state)
{
    bzm_bringup_init(state);
    state->chain_verified = true;
}

static void state_at_clocks(bzm_bringup_state_t * state)
{
    state_at_sensors(state);
    state->sensors_verified = true;
}

TEST_CASE("bzm bringup reference profiles are exact", "[bzm_bringup]")
{
    bzm_bringup_sensor_profile_t sensors;
    bzm_bringup_pll_profile_t pll;
    bzm_bringup_reference_sensor_profile(&sensors);
    bzm_bringup_pll_800_profile(&pll);

    TEST_ASSERT_EQUAL_UINT8(2, sensors.slow_clock_divider);
    TEST_ASSERT_EQUAL_UINT8(0x7f, sensors.tdm_slot_bit_count);
    TEST_ASSERT_EQUAL_UINT8(100, sensors.tdm_slot_count);
    TEST_ASSERT_EQUAL_UINT8(1, sensors.tdm_delay);
    TEST_ASSERT_EQUAL_UINT8(8, sensors.sensor_clock_divider);
    TEST_ASSERT_EQUAL_UINT8(CONFIG_BZM_1002_SENSOR_TDM_GAP_COUNT, sensors.tdm_gap_count);
    TEST_ASSERT_EQUAL_UINT16(2650, sensors.thermal_trip_code);
    TEST_ASSERT_EQUAL_UINT16(7561, sensors.voltage_ch0_shutdown_code);
    TEST_ASSERT_EQUAL_UINT16(800, pll.target_mhz);
    TEST_ASSERT_EQUAL_UINT16(128, pll.feedback_divider);
    TEST_ASSERT_EQUAL_HEX32(0x1242, pll.postdiv_register);
    TEST_ASSERT_EQUAL_HEX32(0x0000fec9, bzm_bringup_reference_tdm_control());
    TEST_ASSERT_EQUAL_STRING("GOOD", bzm_bringup_outcome_name(BZM_BRINGUP_GOOD));
    TEST_ASSERT_EQUAL_STRING("balanced_pair_unavailable", bzm_bringup_reason_name(BZM_BRINGUP_REASON_BALANCED_PAIR_UNAVAILABLE));
    TEST_ASSERT_EQUAL_STRING("balanced_batch", bzm_bringup_reason_name(BZM_BRINGUP_REASON_BALANCED_BATCH));
}

TEST_CASE("bzm chain4 proves the four spaced TDM IDs", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_init(&state);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_stage_chain4(&state, &MOCK_OPS, &mock, &report));
    TEST_ASSERT_TRUE(state.chain_verified);
    TEST_ASSERT_EQUAL_UINT16(4, report.completed_items);
    TEST_ASSERT_EQUAL_UINT8(0, mock.unaddressed);
    TEST_ASSERT_EQUAL_UINT8(BZM_BRINGUP_ASIC_COUNT + 1, mock.broadcast_probe_count);
    TEST_ASSERT_EQUAL_UINT8(0, mock.addressed_probe_count);
    for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        uint32_t programmed = bzm_asic_wire_ids[index] | (index == 0 ? 0U : 0x100U);
        uint32_t readback = bzm_asic_wire_ids[index] | 0x100U;
        TEST_ASSERT_TRUE(mock.assigned[index]);
        TEST_ASSERT_EQUAL_HEX32(programmed, mock.assigned_values[index]);
        TEST_ASSERT_EQUAL_HEX32(readback, mock.registers[index][BZM_LOCAL_REG_ASIC_ID]);
    }
}

TEST_CASE("bzm chain4 bounds power-up NOOP retries before exact ID proof", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.transient_unaddressed_noop_misses = 2;
    uint64_t started_us = mock.now_us;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_init(&state);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_stage_chain4(&state, &MOCK_OPS, &mock, &report));
    TEST_ASSERT_TRUE(state.chain_verified);
    TEST_ASSERT_EQUAL_UINT8(BZM_BRINGUP_ASIC_COUNT + 3, mock.broadcast_probe_count);
    TEST_ASSERT_EQUAL_UINT32((uint32_t) (started_us + 1200000U), (uint32_t) mock.now_us);
}

TEST_CASE("bzm chain4 fails after bounded missing-chain retries", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.unaddressed = 0;
    uint64_t started_us = mock.now_us;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_init(&state);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_chain4(&state, &MOCK_OPS, &mock, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_CHAIN_MISSING, report.reason);
    TEST_ASSERT_EQUAL_UINT8(5, mock.broadcast_probe_count);
    TEST_ASSERT_EQUAL_UINT16(0, report.completed_items);
    TEST_ASSERT_EQUAL_UINT32((uint32_t) (started_us + 800000U), (uint32_t) mock.now_us);
    TEST_ASSERT_FALSE(state.chain_verified);
}

TEST_CASE("bzm chain4 rejects an ID readback mismatch", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.mismatch = true;
    mock.mismatch_asic = 0x1e;
    mock.mismatch_offset = BZM_LOCAL_REG_ASIC_ID;
    mock.mismatch_value = 0x00000114;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_init(&state);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_chain4(&state, &MOCK_OPS, &mock, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_CHAIN_ID_MISMATCH, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x1e, report.asic_id);
    TEST_ASSERT_FALSE(state.chain_verified);
}

TEST_CASE("bzm chain4 requires the hardware chain-status readback bit", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.mismatch = true;
    mock.mismatch_asic = BZM_BRINGUP_FIRST_ASIC_ID;
    mock.mismatch_offset = BZM_LOCAL_REG_ASIC_ID;
    mock.mismatch_value = BZM_BRINGUP_FIRST_ASIC_ID;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_init(&state);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_chain4(&state, &MOCK_OPS, &mock, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_CHAIN_ID_MISMATCH, report.reason);
    TEST_ASSERT_EQUAL_HEX32(BZM_BRINGUP_FIRST_ASIC_ID | 0x100U, report.expected);
    TEST_ASSERT_EQUAL_HEX32(BZM_BRINGUP_FIRST_ASIC_ID, report.actual);
    TEST_ASSERT_FALSE(state.chain_verified);
}

TEST_CASE("bzm chain4 rejects a fifth unaddressed ASIC", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.extra_asic = true;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_init(&state);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_chain4(&state, &MOCK_OPS, &mock, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_CHAIN_EXTRA_ASIC, report.reason);
    TEST_ASSERT_FALSE(state.chain_verified);
}

TEST_CASE("bzm sensors write and read back every ASIC then require fresh telemetry", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_TRUE(state.sensors_verified);
    TEST_ASSERT_FALSE(state.clocks_verified);
    TEST_ASSERT_EQUAL_UINT16(1, mock.all_asic_write_count);
    TEST_ASSERT_EQUAL_HEX32(0x44464444, mock.registers[0][BZM_LOCAL_REG_IO_PEPS_DRIVE_STRENGTH]);
    for (size_t index = 1; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        TEST_ASSERT_EQUAL_HEX32(0, mock.registers[index][BZM_LOCAL_REG_IO_PEPS_DRIVE_STRENGTH]);
    }
    for (size_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        TEST_ASSERT_EQUAL_HEX32(0x0f, mock.registers[index][BZM_LOCAL_REG_UART_TX]);
        TEST_ASSERT_EQUAL_HEX32(2, mock.registers[index][BZM_LOCAL_REG_SLOW_CLOCK_DIVIDER]);
        TEST_ASSERT_EQUAL_HEX32(1, mock.registers[index][BZM_LOCAL_REG_TDM_DELAY]);
        TEST_ASSERT_EQUAL_HEX32(0x0000fec9, mock.registers[index][BZM_LOCAL_REG_UART_TDM_CONTROL]);
        TEST_ASSERT_EQUAL_HEX32(0x108, mock.registers[index][BZM_LOCAL_REG_SENSOR_CLOCK_DIVIDER]);
        TEST_ASSERT_EQUAL_HEX32(CONFIG_BZM_1002_SENSOR_TDM_GAP_COUNT, mock.registers[index][BZM_LOCAL_REG_SENSOR_TDM_GAP_COUNT]);
        TEST_ASSERT_EQUAL_HEX32(0x100, mock.registers[index][BZM_LOCAL_REG_DTS_RESET_POWERDOWN]);
        TEST_ASSERT_EQUAL_HEX32(0x000a000a, mock.registers[index][BZM_LOCAL_REG_SENSOR_THRESHOLD_COUNT]);
        TEST_ASSERT_EQUAL_HEX32(0x8001U | (2650U << 1), mock.registers[index][BZM_LOCAL_REG_TEMPERATURE_TUNE_CODE]);
        TEST_ASSERT_EQUAL_HEX32(0xabcd1233, mock.registers[index][BZM_LOCAL_REG_BANDGAP]);
        TEST_ASSERT_EQUAL_HEX32(0x81000000, mock.registers[index][BZM_LOCAL_REG_VSENSOR_CONFIG]);
        TEST_ASSERT_EQUAL_HEX32(0x1d893b13, mock.registers[index][BZM_LOCAL_REG_VSENSOR_CONTROL]);
    }
}

TEST_CASE("bzm sensors finish all readback before enabling TDM traffic", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    for (size_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        mock.registers[index][BZM_LOCAL_REG_UART_TDM_CONTROL] = 0;
    }
    mock.fail_reads_while_tdm_active = true;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL_UINT16(1, mock.all_asic_write_count);
    TEST_ASSERT_EQUAL_UINT16(0, mock.reads_while_tdm_active);
    for (size_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        TEST_ASSERT_EQUAL_HEX32(0x0000fec9, mock.registers[index][BZM_LOCAL_REG_UART_TDM_CONTROL]);
    }
}

TEST_CASE("bzm sensors fail closed when the synchronized TDM start fails", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.fail_all_asic_write = true;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_IO, report.reason);
    TEST_ASSERT_EQUAL_HEX8(BZM_ALL_ASICS, report.asic_id);
    TEST_ASSERT_EQUAL_HEX8(BZM_LOCAL_REG_UART_TDM_CONTROL, report.register_offset);
    TEST_ASSERT_EQUAL_UINT16(1, mock.all_asic_write_count);
    TEST_ASSERT_FALSE(state.sensors_verified);
}

TEST_CASE("bzm sensors require the BIRDS first-ASIC drive-strength readback", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.mismatch = true;
    mock.mismatch_asic = BZM_FIRST_ASIC_ID;
    mock.mismatch_offset = BZM_LOCAL_REG_IO_PEPS_DRIVE_STRENGTH;
    mock.mismatch_value = 0;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_REGISTER_READBACK, report.reason);
    TEST_ASSERT_EQUAL_HEX8(BZM_FIRST_ASIC_ID, report.asic_id);
    TEST_ASSERT_EQUAL_HEX8(BZM_LOCAL_REG_IO_PEPS_DRIVE_STRENGTH, report.register_offset);
    TEST_ASSERT_EQUAL_HEX32(0x44464444, report.expected);
    TEST_ASSERT_EQUAL_HEX32(0, report.actual);
    TEST_ASSERT_EQUAL_UINT16(0, mock.all_asic_write_count);
    TEST_ASSERT_FALSE(state.sensors_verified);
}

TEST_CASE("bzm sensors reject a TDM slot count that cannot reach wire IDs 10 through 40", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);
    profile.tdm_slot_count = BZM_BRINGUP_ASIC_COUNT;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_INVALID_ARGUMENT, report.reason);
    TEST_ASSERT_EQUAL_UINT16(0, mock.write_count);
}

TEST_CASE("bzm sensors are blocked until chain4 is good", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    bzm_bringup_init(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BLOCKED, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_PREREQUISITE, report.reason);
    TEST_ASSERT_EQUAL_UINT16(0, mock.write_count);
}

TEST_CASE("bzm sensors fail on per ASIC register readback", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.mismatch = true;
    mock.mismatch_asic = 0x28;
    mock.mismatch_offset = BZM_LOCAL_REG_VSENSOR_CONFIG;
    mock.mismatch_value = 0;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_REGISTER_READBACK, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x28, report.asic_id);
    TEST_ASSERT_FALSE(state.sensors_verified);
}

TEST_CASE("bzm sensors reject telemetry captured before configuration", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_old = true;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_TELEMETRY_PRECONFIG, report.reason);
    TEST_ASSERT_EQUAL_UINT16(5, mock.telemetry_snapshot_count);
}

TEST_CASE("bzm sensors retry a queued pre-configuration sample", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_old_snapshots = 1;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_TRUE(state.sensors_verified);
    TEST_ASSERT_EQUAL_UINT16(2, mock.telemetry_snapshot_count);
}

TEST_CASE("bzm sensors ignore one CH2 excursion only after a fresh recovery sample", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_ch2_excursion_id = 0x0a;
    mock.telemetry_ch2_excursion_snapshots = 1;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_TRUE(state.sensors_verified);
    TEST_ASSERT_EQUAL_UINT16(2, mock.telemetry_snapshot_count);
}

TEST_CASE("bzm sensors reject a continuous CH2 excursion at the configured count", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_ch2_excursion_id = 0x14;
    mock.telemetry_ch2_excursion_snapshots = 3;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_TELEMETRY_UNSAFE, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x14, report.asic_id);
    TEST_ASSERT_EQUAL_UINT32(3, report.expected);
    TEST_ASSERT_EQUAL_UINT32(3, report.actual);
    TEST_ASSERT_EQUAL_UINT16(3, mock.telemetry_snapshot_count);
    TEST_ASSERT_FALSE(state.sensors_verified);
}

TEST_CASE("bzm sensors qualify a transient CH2 excursion with voltage-fault bit", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_ch2_excursion_id = 0x0a;
    mock.telemetry_ch2_excursion_snapshots = 1;
    mock.telemetry_ch2_excursion_voltage_fault = true;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_TRUE(state.sensors_verified);
    TEST_ASSERT_EQUAL_UINT16(2, mock.telemetry_snapshot_count);
}

TEST_CASE("bzm sensors reject continuous CH2 excursions with voltage-fault bit", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_ch2_excursion_id = 0x0a;
    mock.telemetry_ch2_excursion_snapshots = 3;
    mock.telemetry_ch2_excursion_voltage_fault = true;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_TELEMETRY_UNSAFE, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x0a, report.asic_id);
    TEST_ASSERT_EQUAL_UINT32(3, report.expected);
    TEST_ASSERT_EQUAL_UINT32(3, report.actual);
    TEST_ASSERT_EQUAL_UINT16(3, mock.telemetry_snapshot_count);
    TEST_ASSERT_FALSE(state.sensors_verified);
}

TEST_CASE("bzm sensors keep trip faults immediate while CH2 confirmation is enabled", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_unsafe = true;
    mock.telemetry_unsafe_id = 0x1e;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_TELEMETRY_UNSAFE, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x1e, report.asic_id);
    TEST_ASSERT_EQUAL_UINT16(1, mock.telemetry_snapshot_count);
}

TEST_CASE("bzm sensors allow configuration to restore immediate CH2 shutdown", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_ch2_excursion_id = 0x28;
    mock.telemetry_ch2_excursion_snapshots = 1;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    policy.ch2_confirm_samples = 1;
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL_HEX8(0x28, report.asic_id);
    TEST_ASSERT_EQUAL_UINT16(1, mock.telemetry_snapshot_count);
}

TEST_CASE("bzm sensors require telemetry from every ASIC", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_missing = true;
    mock.telemetry_missing_id = 0x14;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_sensors(&state);
    bzm_bringup_reference_sensor_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_sensors(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_TELEMETRY_MISSING, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x14, report.asic_id);
    TEST_ASSERT_EQUAL_UINT16(5, mock.telemetry_snapshot_count);
}

TEST_CASE("bzm clocks configure both 800 MHz PLLs symmetrically", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_pll_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    bzm_bringup_pll_800_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_stage_clocks(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_TRUE(state.clocks_verified);
    TEST_ASSERT_EQUAL_UINT16(2, mock.all_asic_write_count);
    for (size_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        TEST_ASSERT_EQUAL_HEX32(128, mock.registers[index][BZM_LOCAL_REG_PLL0_FBDIV]);
        TEST_ASSERT_EQUAL_HEX32(128, mock.registers[index][BZM_LOCAL_REG_PLL1_FBDIV]);
        TEST_ASSERT_EQUAL_HEX32(0x1242, mock.registers[index][BZM_LOCAL_REG_PLL0_POSTDIV]);
        TEST_ASSERT_EQUAL_HEX32(0x1242, mock.registers[index][BZM_LOCAL_REG_PLL1_POSTDIV]);
        TEST_ASSERT_EQUAL_HEX32(1, mock.registers[index][BZM_LOCAL_REG_PLL0_ENABLE]);
        TEST_ASSERT_EQUAL_HEX32(1, mock.registers[index][BZM_LOCAL_REG_PLL1_ENABLE]);
        TEST_ASSERT_EQUAL_HEX32(0, mock.registers[index][BZM_LOCAL_REG_DLL0_CONTROL_5]);
        TEST_ASSERT_EQUAL_HEX32(0, mock.registers[index][BZM_LOCAL_REG_DLL1_CONTROL_5]);
    }
}

TEST_CASE("bzm clocks quiesce every TDM sender before control readback", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.fail_reads_while_tdm_active = true;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_pll_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    bzm_bringup_pll_800_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_stage_clocks(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL_UINT16(2, mock.all_asic_write_count);
    for (size_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        TEST_ASSERT_EQUAL_HEX32(0x0000fec9, mock.registers[index][BZM_LOCAL_REG_UART_TDM_CONTROL]);
    }
}

TEST_CASE("bzm clocks fail if either PLL does not lock", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.pll_locked[2][1] = false;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_pll_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    bzm_bringup_pll_800_profile(&profile);
    profile.lock_attempts = 2;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_clocks(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_PLL_UNLOCKED, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x1e, report.asic_id);
    TEST_ASSERT_EQUAL_UINT8(1, report.pll_index);
    TEST_ASSERT_FALSE(state.clocks_verified);
}

TEST_CASE("bzm clocks require fresh recovery from one combined-lock frame anomaly", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_pll_unlock_id = 0x14;
    mock.telemetry_pll_unlock_snapshots = 1;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_pll_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    bzm_bringup_pll_800_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_stage_clocks(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL_UINT16(2, mock.telemetry_snapshot_count);
    TEST_ASSERT_TRUE(state.clocks_verified);
}

TEST_CASE("bzm clocks reject continuous combined-lock frame anomalies", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_pll_unlock_id = 0x14;
    mock.telemetry_pll_unlock_snapshots = 3;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_pll_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    bzm_bringup_pll_800_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_clocks(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_TELEMETRY_UNSAFE, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x14, report.asic_id);
    TEST_ASSERT_EQUAL_UINT32(3, report.expected);
    TEST_ASSERT_EQUAL_UINT32(3, report.actual);
    TEST_ASSERT_EQUAL_UINT16(3, mock.telemetry_snapshot_count);
    TEST_ASSERT_FALSE(state.clocks_verified);
}

TEST_CASE("bzm clocks reject an asymmetric PLL readback", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.mismatch = true;
    mock.mismatch_asic = 0x14;
    mock.mismatch_offset = BZM_LOCAL_REG_PLL1_FBDIV;
    mock.mismatch_value = 127;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_pll_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    bzm_bringup_pll_800_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_clocks(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_REGISTER_READBACK, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x14, report.asic_id);
    TEST_ASSERT_EQUAL_UINT8(1, report.pll_index);
    TEST_ASSERT_EQUAL_HEX8(BZM_LOCAL_REG_PLL1_FBDIV, report.register_offset);
}

TEST_CASE("bzm clocks reprove sensor TDM controls after PLL programming", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.mismatch = true;
    mock.mismatch_asic = 0x1e;
    mock.mismatch_offset = BZM_LOCAL_REG_SENSOR_CLOCK_DIVIDER;
    mock.mismatch_value = 0x100;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_pll_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    bzm_bringup_pll_800_profile(&profile);

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_clocks(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_REGISTER_READBACK, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x1e, report.asic_id);
    TEST_ASSERT_EQUAL_HEX8(BZM_LOCAL_REG_SENSOR_CLOCK_DIVIDER, report.register_offset);
    TEST_ASSERT_EQUAL_HEX32(0x108, report.expected);
}

TEST_CASE("bzm clocks reject any profile other than exact 800 MHz", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_pll_profile_t profile;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    bzm_bringup_pll_800_profile(&profile);
    profile.target_mhz = 799;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_clocks(&state, &MOCK_OPS, &mock, &profile, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_INVALID_ARGUMENT, report.reason);
    TEST_ASSERT_EQUAL_UINT16(0, mock.write_count);
}

TEST_CASE("bzm balanced ramp is blocked without the sequential pair adapter", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;
    bzm_bringup_ops_t serial_only = MOCK_OPS;
    serial_only.balanced_batch_begin = NULL;
    serial_only.balanced_pair_commit = NULL;
    serial_only.balanced_batch_end = NULL;
    serial_only.activation_barrier = NULL;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BLOCKED, bzm_bringup_stage_balanced_ramp(&state, &serial_only, &mock, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_BALANCED_PAIR_UNAVAILABLE, report.reason);
    TEST_ASSERT_EQUAL_UINT16(0, mock.pair_commit_count);
    TEST_ASSERT_FALSE(state.balanced_ramp_verified);
}

TEST_CASE("bzm balanced ramp commits all 236 engines as bounded-skew stack pairs", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_stage_balanced_ramp(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_TRUE(state.balanced_ramp_verified);
    TEST_ASSERT_EQUAL_UINT16(BZM_TOPOLOGY_PAIR_COUNT, mock.batch_begin_count);
    TEST_ASSERT_EQUAL_UINT16(BZM_TOPOLOGY_PAIR_COUNT, mock.batch_end_count);
    TEST_ASSERT_FALSE(mock.batch_active);
    TEST_ASSERT_EQUAL_UINT16(118U * 4U, mock.pair_commit_count);
    TEST_ASSERT_EQUAL_UINT16(118U * 4U, report.completed_items);
    TEST_ASSERT_EQUAL_UINT16(1, mock.barrier_count);
    TEST_ASSERT_EQUAL_UINT16(BZM_TOPOLOGY_PAIR_COUNT, mock.telemetry_snapshot_count);
    for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        TEST_ASSERT_EQUAL_HEX32(1, mock.registers[index][BZM_LOCAL_REG_RESULT_STATUS_CONTROL]);
    }
}

TEST_CASE("bzm balanced ramp requires fresh recovery from one combined-lock frame anomaly", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_pll_unlock_id = 0x0a;
    mock.telemetry_pll_unlock_snapshots = 1;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_stage_balanced_ramp(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_TRUE(state.balanced_ramp_verified);
    TEST_ASSERT_EQUAL_UINT16(118U * 4U, mock.pair_commit_count);
    TEST_ASSERT_EQUAL_UINT16(BZM_TOPOLOGY_PAIR_COUNT + 1U, mock.telemetry_snapshot_count);
}

TEST_CASE("bzm balanced ramp rejects continuous same-ASIC combined-lock frame anomalies", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_pll_unlock_id = 0x0a;
    mock.telemetry_pll_unlock_snapshots = 3;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_balanced_ramp(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_TELEMETRY_UNSAFE, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x0a, report.asic_id);
    TEST_ASSERT_EQUAL_UINT32(3, report.expected);
    TEST_ASSERT_EQUAL_UINT32(3, report.actual);
    TEST_ASSERT_EQUAL_UINT16(BZM_BRINGUP_ASIC_COUNT, report.completed_items);
    TEST_ASSERT_EQUAL_UINT16(3, mock.telemetry_snapshot_count);
    TEST_ASSERT_FALSE(state.balanced_ramp_verified);
}

TEST_CASE("bzm balanced ramp disables and verifies result reports before activation", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.mismatch = true;
    mock.mismatch_asic = 0x1e;
    mock.mismatch_offset = BZM_LOCAL_REG_RESULT_STATUS_CONTROL;
    mock.mismatch_value = 0;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_balanced_ramp(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_REGISTER_READBACK, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x1e, report.asic_id);
    TEST_ASSERT_EQUAL_HEX8(BZM_LOCAL_REG_RESULT_STATUS_CONTROL, report.register_offset);
    TEST_ASSERT_EQUAL_UINT32(1, report.expected);
    TEST_ASSERT_EQUAL_UINT32(0, report.actual);
    TEST_ASSERT_EQUAL_UINT16(0, mock.pair_commit_count);
    TEST_ASSERT_FALSE(state.balanced_ramp_verified);
}

TEST_CASE("bzm balanced ramp fails closed before activation when a batch cannot begin", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.fail_batch_begin_at = 0;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_balanced_ramp(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_BALANCED_BATCH, report.reason);
    TEST_ASSERT_EQUAL_UINT16(0, report.completed_items);
    TEST_ASSERT_EQUAL_UINT16(0, mock.pair_commit_count);
    TEST_ASSERT_FALSE(mock.batch_active);
    TEST_ASSERT_FALSE(state.balanced_ramp_verified);
}

TEST_CASE("bzm balanced ramp fails closed when telemetry cannot resume after a batch", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.fail_batch_end_at = 0;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_balanced_ramp(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_BALANCED_BATCH, report.reason);
    TEST_ASSERT_EQUAL_UINT16(BZM_BRINGUP_ASIC_COUNT, report.completed_items);
    TEST_ASSERT_EQUAL_UINT16(BZM_BRINGUP_ASIC_COUNT, mock.pair_commit_count);
    TEST_ASSERT_TRUE(mock.batch_active);
    TEST_ASSERT_FALSE(state.balanced_ramp_verified);
}

TEST_CASE("bzm balanced ramp rejects telemetry captured before each pair batch", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_old = true;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_balanced_ramp(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_TELEMETRY_PRECONFIG, report.reason);
    TEST_ASSERT_EQUAL_UINT16(BZM_BRINGUP_ASIC_COUNT, mock.pair_commit_count);
    TEST_ASSERT_EQUAL_UINT16(BZM_BRINGUP_ASIC_COUNT, report.completed_items);
    TEST_ASSERT_EQUAL_UINT16(5, mock.telemetry_snapshot_count);
    TEST_ASSERT_EQUAL_UINT16(0, mock.barrier_count);
    TEST_ASSERT_FALSE(state.balanced_ramp_verified);
}

TEST_CASE("bzm balanced ramp requires the final activation barrier", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.barrier_result = false;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_stage_balanced_ramp(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_ACTIVATION_BARRIER, report.reason);
    TEST_ASSERT_EQUAL_UINT16(118U * 4U, mock.pair_commit_count);
    TEST_ASSERT_FALSE(state.balanced_ramp_verified);
}

TEST_CASE("bzm running requires post clock fresh safe telemetry", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;
    state.clocks_configured_us = mock.now_us;
    state.balanced_ramp_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_check_running(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_TRUE(state.running_verified);
    for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        TEST_ASSERT_EQUAL_HEX32(0, mock.registers[index][BZM_LOCAL_REG_RESULT_STATUS_CONTROL]);
    }

    mock.telemetry_unsafe = true;
    mock.telemetry_unsafe_id = 0x14;
    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_check_running(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_TELEMETRY_UNSAFE, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x14, report.asic_id);
    TEST_ASSERT_FALSE(state.running_verified);
}

TEST_CASE("bzm running verifies result reporting is enabled only after Stage 6", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        mock.registers[index][BZM_LOCAL_REG_RESULT_STATUS_CONTROL] = 1;
    }
    mock.mismatch = true;
    mock.mismatch_asic = 0x14;
    mock.mismatch_offset = BZM_LOCAL_REG_RESULT_STATUS_CONTROL;
    mock.mismatch_value = 1;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;
    state.clocks_configured_us = mock.now_us;
    state.balanced_ramp_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_check_running(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_REGISTER_READBACK, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x14, report.asic_id);
    TEST_ASSERT_EQUAL_HEX8(BZM_LOCAL_REG_RESULT_STATUS_CONTROL, report.register_offset);
    TEST_ASSERT_EQUAL_UINT32(0, report.expected);
    TEST_ASSERT_EQUAL_UINT32(1, report.actual);
    TEST_ASSERT_FALSE(state.running_verified);
}

TEST_CASE("bzm running requires a fresh recovery after one PLL status-frame anomaly", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_pll_unlock_id = 0x0a;
    mock.telemetry_pll_unlock_snapshots = 1;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;
    state.clocks_configured_us = mock.now_us;
    state.balanced_ramp_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_GOOD, bzm_bringup_check_running(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_EQUAL_UINT16(2, mock.telemetry_snapshot_count);
    TEST_ASSERT_TRUE(state.running_verified);
}

TEST_CASE("bzm running rejects a continuous PLL status-frame anomaly", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_pll_unlock_id = 0x14;
    mock.telemetry_pll_unlock_snapshots = 3;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;
    state.clocks_configured_us = mock.now_us;
    state.balanced_ramp_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_check_running(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_TELEMETRY_UNSAFE, report.reason);
    TEST_ASSERT_EQUAL_HEX8(0x14, report.asic_id);
    TEST_ASSERT_EQUAL_UINT32(3, report.expected);
    TEST_ASSERT_EQUAL_UINT32(3, report.actual);
    TEST_ASSERT_EQUAL_UINT16(3, mock.telemetry_snapshot_count);
    TEST_ASSERT_FALSE(state.running_verified);
}

TEST_CASE("bzm running keeps a PLL anomaly with trip immediate", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    mock.telemetry_pll_unlock_id = 0x1e;
    mock.telemetry_pll_unlock_snapshots = 1;
    mock.telemetry_unsafe = true;
    mock.telemetry_unsafe_id = 0x1e;
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;
    state.clocks_configured_us = mock.now_us;
    state.balanced_ramp_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BAD, bzm_bringup_check_running(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_TELEMETRY_UNSAFE, report.reason);
    TEST_ASSERT_EQUAL_UINT16(1, mock.telemetry_snapshot_count);
    TEST_ASSERT_FALSE(state.running_verified);
}

TEST_CASE("bzm running is blocked until balanced ramp is good", "[bzm_bringup]")
{
    bringup_mock_t mock = good_mock();
    bzm_bringup_state_t state;
    bzm_bringup_report_t report;
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    state_at_clocks(&state);
    state.clocks_verified = true;

    TEST_ASSERT_EQUAL(BZM_BRINGUP_BLOCKED, bzm_bringup_check_running(&state, &MOCK_OPS, &mock, &policy, &report));
    TEST_ASSERT_EQUAL(BZM_BRINGUP_REASON_PREREQUISITE, report.reason);
    TEST_ASSERT_FALSE(state.running_verified);
}
