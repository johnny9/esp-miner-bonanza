#include "bzm_bringup.h"

#include <math.h>
#include <stddef.h>

#include "bzm.h"
#include "bzm_registers.h"

enum
{
    BZM_REFERENCE_SLOW_CLOCK_DIVIDER = 2,
    BZM_REFERENCE_TDM_SLOT_BIT_COUNT = 0x7f,
    BZM_REFERENCE_TDM_SLOT_COUNT = 100,
    BZM_REFERENCE_TDM_DELAY = 1,
    BZM_REFERENCE_SENSOR_DIVIDER = 8,
    BZM_REFERENCE_THRESHOLD_COUNT = 10,
    BZM_RESULT_REPORT_ENABLED = 0,
    BZM_RESULT_REPORT_DISABLED = 1,
    BZM_REFERENCE_THERMAL_TRIP_CODE_115C = 2650,
    BZM_REFERENCE_VOLTAGE_SHUTDOWN_CODE_500MV = 7561,
    BZM_REFERENCE_SENSOR_SETTLE_MS = 10,
    BZM_TDM_TELEMETRY_FRAME_BIT_PERIODS = 100,
    BZM_CHAIN_NOOP_ATTEMPTS = 5,
    BZM_CHAIN_NOOP_RETRY_DELAY_MS = 200,
    BZM_TELEMETRY_ACQUIRE_ATTEMPTS = 5,
    BZM_TELEMETRY_RETRY_DELAY_MS = 5,
    BZM_PLL_COUNT = 2,
    BZM_PLL_LOCK_MASK = 0x05,
    BZM_PLL_ENABLE_VALUE = 0x01,
};

static const uint32_t BZM_REFERENCE_BIRDS_IO_PEPS_DRIVE_STRENGTH = 0x44464444U;

typedef struct
{
    uint8_t offset;
    uint32_t value;
} bzm_register_value_t;

static bzm_bringup_outcome_t set_report(bzm_bringup_report_t * report, bzm_bringup_outcome_t outcome, bzm_bringup_reason_t reason,
                                        uint8_t asic_id, uint8_t pll_index, uint8_t register_offset, uint32_t expected,
                                        uint32_t actual, uint16_t completed_items)
{
    if (report != NULL) {
        *report = (bzm_bringup_report_t){
            .outcome = outcome,
            .reason = reason,
            .asic_id = asic_id,
            .pll_index = pll_index,
            .register_offset = register_offset,
            .expected = expected,
            .actual = actual,
            .completed_items = completed_items,
        };
    }
    return outcome;
}

static void clear_after_chain(bzm_bringup_state_t * state)
{
    state->chain_verified = false;
    state->sensors_verified = false;
    state->clocks_verified = false;
    state->balanced_ramp_verified = false;
    state->running_verified = false;
    state->sensors_configured_us = 0;
    state->clocks_configured_us = 0;
}

static void clear_after_sensors(bzm_bringup_state_t * state)
{
    state->sensors_verified = false;
    state->clocks_verified = false;
    state->balanced_ramp_verified = false;
    state->running_verified = false;
    state->sensors_configured_us = 0;
    state->clocks_configured_us = 0;
}

static void clear_after_clocks(bzm_bringup_state_t * state)
{
    state->clocks_verified = false;
    state->balanced_ramp_verified = false;
    state->running_verified = false;
    state->clocks_configured_us = 0;
}

void bzm_bringup_init(bzm_bringup_state_t * state)
{
    if (state != NULL) {
        *state = (bzm_bringup_state_t){0};
    }
}

void bzm_bringup_reference_sensor_profile(bzm_bringup_sensor_profile_t * profile)
{
    if (profile == NULL)
        return;
    *profile = (bzm_bringup_sensor_profile_t){
        .slow_clock_divider = BZM_REFERENCE_SLOW_CLOCK_DIVIDER,
        .tdm_slot_bit_count = BZM_REFERENCE_TDM_SLOT_BIT_COUNT,
        .tdm_slot_count = BZM_REFERENCE_TDM_SLOT_COUNT,
        .tdm_delay = BZM_REFERENCE_TDM_DELAY,
        .sensor_clock_divider = BZM_REFERENCE_SENSOR_DIVIDER,
        .tdm_gap_count = CONFIG_BZM_1002_SENSOR_TDM_GAP_COUNT,
        .thermal_threshold_count = BZM_REFERENCE_THRESHOLD_COUNT,
        .voltage_threshold_count = BZM_REFERENCE_THRESHOLD_COUNT,
        .thermal_trip_code = BZM_REFERENCE_THERMAL_TRIP_CODE_115C,
        .voltage_ch0_shutdown_code = BZM_REFERENCE_VOLTAGE_SHUTDOWN_CODE_500MV,
        .voltage_ch1_shutdown_code = BZM_REFERENCE_VOLTAGE_SHUTDOWN_CODE_500MV,
        .settle_delay_ms = BZM_REFERENCE_SENSOR_SETTLE_MS,
    };
}

void bzm_bringup_pll_800_profile(bzm_bringup_pll_profile_t * profile)
{
    if (profile == NULL)
        return;
    *profile = (bzm_bringup_pll_profile_t){
        .target_mhz = 800,
        .reference_mhz = 50,
        .reference_divider = 2,
        .post1_divider = 1,
        .post2_divider = 1,
        .feedback_divider = 128,
        .postdiv_register = 0x1242,
        .lock_attempts = 30,
        .lock_poll_delay_ms = 100,
    };
}

uint32_t bzm_bringup_reference_tdm_control(void)
{
    return ((uint32_t) BZM_REFERENCE_TDM_SLOT_BIT_COUNT << 9) | ((uint32_t) BZM_REFERENCE_TDM_SLOT_COUNT << 1) | 1U;
}

const char * bzm_bringup_outcome_name(bzm_bringup_outcome_t outcome)
{
    switch (outcome) {
    case BZM_BRINGUP_GOOD:
        return "GOOD";
    case BZM_BRINGUP_BAD:
        return "BAD";
    case BZM_BRINGUP_BLOCKED:
        return "BLOCKED";
    default:
        return "UNKNOWN";
    }
}

const char * bzm_bringup_reason_name(bzm_bringup_reason_t reason)
{
    switch (reason) {
    case BZM_BRINGUP_REASON_NONE:
        return "none";
    case BZM_BRINGUP_REASON_INVALID_ARGUMENT:
        return "invalid_argument";
    case BZM_BRINGUP_REASON_PREREQUISITE:
        return "prerequisite";
    case BZM_BRINGUP_REASON_CAPABILITY_UNAVAILABLE:
        return "capability_unavailable";
    case BZM_BRINGUP_REASON_IO:
        return "io";
    case BZM_BRINGUP_REASON_CHAIN_MISSING:
        return "chain_missing";
    case BZM_BRINGUP_REASON_CHAIN_ID_MISMATCH:
        return "chain_id_mismatch";
    case BZM_BRINGUP_REASON_CHAIN_EXTRA_ASIC:
        return "chain_extra_asic";
    case BZM_BRINGUP_REASON_REGISTER_READBACK:
        return "register_readback";
    case BZM_BRINGUP_REASON_TELEMETRY_MISSING:
        return "telemetry_missing";
    case BZM_BRINGUP_REASON_TELEMETRY_PRECONFIG:
        return "telemetry_preconfig";
    case BZM_BRINGUP_REASON_TELEMETRY_STALE:
        return "telemetry_stale";
    case BZM_BRINGUP_REASON_TELEMETRY_UNSAFE:
        return "telemetry_unsafe";
    case BZM_BRINGUP_REASON_PLL_UNLOCKED:
        return "pll_unlocked";
    case BZM_BRINGUP_REASON_TOPOLOGY:
        return "topology";
    case BZM_BRINGUP_REASON_BALANCED_PAIR_UNAVAILABLE:
        return "balanced_pair_unavailable";
    case BZM_BRINGUP_REASON_BALANCED_PAIR_COMMIT:
        return "balanced_pair_commit";
    case BZM_BRINGUP_REASON_ACTIVATION_BARRIER:
        return "activation_barrier";
    case BZM_BRINGUP_REASON_BALANCED_BATCH:
        return "balanced_batch";
    default:
        return "unknown";
    }
}

static bool basic_ops_are_valid(const bzm_bringup_ops_t * ops)
{
    return ops != NULL && ops->write_u32 != NULL && ops->read_u32 != NULL;
}

static bool telemetry_policy_is_valid(const bzm_bringup_telemetry_policy_t * policy)
{
    return policy != NULL && policy->max_age_us != 0 && policy->ch2_confirm_samples != 0 &&
           policy->ch2_confirm_samples <= BZM_CH2_CONFIRM_MAX_SAMPLES && isfinite(policy->bounds.temperature_min_c) &&
           isfinite(policy->bounds.temperature_max_c) && isfinite(policy->bounds.ch0_min_mv) &&
           isfinite(policy->bounds.ch0_max_mv) && isfinite(policy->bounds.ch1_min_mv) && isfinite(policy->bounds.ch1_max_mv) &&
           isfinite(policy->bounds.ch2_abs_max_mv) && isfinite(policy->bounds.max_stack_spread_mv) &&
           policy->bounds.temperature_min_c <= policy->bounds.temperature_max_c &&
           policy->bounds.ch0_min_mv <= policy->bounds.ch0_max_mv && policy->bounds.ch1_min_mv <= policy->bounds.ch1_max_mv &&
           policy->bounds.ch2_abs_max_mv >= 0.0f && policy->bounds.max_stack_spread_mv >= 0.0f;
}

bzm_bringup_outcome_t bzm_bringup_stage_chain4(bzm_bringup_state_t * state, const bzm_bringup_ops_t * ops, void * ops_context,
                                               bzm_bringup_report_t * report)
{
    if (state == NULL || !basic_ops_are_valid(ops) || ops->probe_noop == NULL) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_INVALID_ARGUMENT, 0, 0, 0, 0, 0, 0);
    }

    clear_after_chain(state);
    for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        uint8_t asic_id = bzm_asic_wire_ids[index];
        bzm_bringup_probe_result_t probe = BZM_BRINGUP_PROBE_NO_RESPONSE;
        for (uint8_t attempt = 0; attempt < BZM_CHAIN_NOOP_ATTEMPTS; ++attempt) {
            probe = ops->probe_noop(ops_context, BZM_BROADCAST_ASIC);
            if (probe != BZM_BRINGUP_PROBE_NO_RESPONSE) {
                break;
            }
            /* BIRDS retries the default-address power-up NOOP because an ASIC
             * may not answer the first request after reset release. Keep this
             * bounded well below its 30-attempt reference allowance. A
             * malformed frame/I/O error is never retried. */
            if (attempt + 1U < BZM_CHAIN_NOOP_ATTEMPTS && ops->delay_ms != NULL) {
                ops->delay_ms(ops_context, BZM_CHAIN_NOOP_RETRY_DELAY_MS);
            }
        }
        if (probe != BZM_BRINGUP_PROBE_RESPONSE) {
            bzm_bringup_reason_t reason =
                probe == BZM_BRINGUP_PROBE_NO_RESPONSE ? BZM_BRINGUP_REASON_CHAIN_MISSING : BZM_BRINGUP_REASON_IO;
            return set_report(report, BZM_BRINGUP_BAD, reason, asic_id, 0, BZM_LOCAL_REG_ASIC_ID, BZM_BRINGUP_PROBE_RESPONSE, probe,
                              index);
        }

        uint32_t programmed_id = asic_id | (index == 0 ? 0U : (1U << 8));
        if (!ops->write_u32(ops_context, BZM_BROADCAST_ASIC, BZM_BRINGUP_CONTROL_ENGINE_ID, BZM_LOCAL_REG_ASIC_ID, programmed_id)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, asic_id, 0, BZM_LOCAL_REG_ASIC_ID, programmed_id, 0,
                              index);
        }
        if (ops->delay_ms != NULL) {
            ops->delay_ms(ops_context, 200);
        }

        uint32_t readback = 0;
        uint32_t expected_readback = asic_id | (1U << 8);
        if (!ops->read_u32(ops_context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, BZM_LOCAL_REG_ASIC_ID, &readback)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, asic_id, 0, BZM_LOCAL_REG_ASIC_ID, expected_readback,
                              0, index);
        }
        if (readback != expected_readback) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_CHAIN_ID_MISMATCH, asic_id, 0, BZM_LOCAL_REG_ASIC_ID,
                              expected_readback, readback, index);
        }

        /* BIRDS uses NOOP only at the default/unassigned address during
         * discovery. The exact addressed register readback above proves the
         * newly assigned ASIC; an addressed NOOP is not a supported identity
         * check on this chain. */
    }

    bzm_bringup_probe_result_t extra_probe = ops->probe_noop(ops_context, BZM_BROADCAST_ASIC);
    if (extra_probe == BZM_BRINGUP_PROBE_RESPONSE) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_CHAIN_EXTRA_ASIC, BZM_BROADCAST_ASIC, 0,
                          BZM_LOCAL_REG_ASIC_ID, BZM_BRINGUP_PROBE_NO_RESPONSE, extra_probe, BZM_BRINGUP_ASIC_COUNT);
    }
    if (extra_probe == BZM_BRINGUP_PROBE_IO_ERROR) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, BZM_BROADCAST_ASIC, 0, BZM_LOCAL_REG_ASIC_ID,
                          BZM_BRINGUP_PROBE_NO_RESPONSE, extra_probe, BZM_BRINGUP_ASIC_COUNT);
    }

    state->chain_verified = true;
    return set_report(report, BZM_BRINGUP_GOOD, BZM_BRINGUP_REASON_NONE, BZM_LAST_ASIC_ID, 0, BZM_LOCAL_REG_ASIC_ID,
                      BZM_BRINGUP_ASIC_COUNT, BZM_BRINGUP_ASIC_COUNT, BZM_BRINGUP_ASIC_COUNT);
}

static bool sensor_profile_is_valid(const bzm_bringup_sensor_profile_t * profile)
{
    return profile != NULL &&
           (profile->slow_clock_divider == 2 || profile->slow_clock_divider == 4 || profile->slow_clock_divider == 8 ||
            profile->slow_clock_divider == 16) &&
           profile->tdm_slot_bit_count >= BZM_TDM_TELEMETRY_FRAME_BIT_PERIODS && profile->tdm_slot_count > BZM_LAST_ASIC_ID &&
           profile->tdm_delay <= 3 && profile->sensor_clock_divider >= 2 && profile->sensor_clock_divider <= 16 &&
           profile->tdm_gap_count <= 63 && profile->thermal_trip_code <= BZM_TEMPERATURE_CODE_MASK &&
           profile->voltage_ch0_shutdown_code <= BZM_VOLTAGE_CODE_MASK &&
           profile->voltage_ch1_shutdown_code <= BZM_VOLTAGE_CODE_MASK;
}

static bzm_bringup_outcome_t write_and_verify_registers(const bzm_bringup_ops_t * ops, void * ops_context, uint8_t asic_id,
                                                        const bzm_register_value_t * registers, size_t register_count,
                                                        uint16_t completed_items, bzm_bringup_report_t * report)
{
    for (size_t i = 0; i < register_count; ++i) {
        if (!ops->write_u32(ops_context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, registers[i].offset, registers[i].value)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, asic_id, 0, registers[i].offset, registers[i].value,
                              0, completed_items);
        }
    }
    for (size_t i = 0; i < register_count; ++i) {
        uint32_t actual = 0;
        if (!ops->read_u32(ops_context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, registers[i].offset, &actual)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, asic_id, 0, registers[i].offset, registers[i].value,
                              0, completed_items);
        }
        if (actual != registers[i].value) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_REGISTER_READBACK, asic_id, 0, registers[i].offset,
                              registers[i].value, actual, completed_items);
        }
    }
    return BZM_BRINGUP_GOOD;
}

static bzm_bringup_outcome_t verify_telemetry_snapshot(const bzm_telemetry_store_t * store, uint64_t now_us,
                                                       uint64_t configured_after_us, const bzm_bringup_telemetry_policy_t * policy,
                                                       bool require_clock_locks, bzm_bringup_report_t * report)
{
    for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        uint8_t asic_id = bzm_asic_wire_ids[index];
        const bzm_telemetry_sample_t * sample = bzm_telemetry_store_get(store, asic_id);
        if (sample == NULL || !sample->received) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_TELEMETRY_MISSING, asic_id, 0, 0, 1, 0, index);
        }
        if (sample->timestamp_us < configured_after_us) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_TELEMETRY_PRECONFIG, asic_id, 0, 0,
                              (uint32_t) configured_after_us, (uint32_t) sample->timestamp_us, index);
        }
        if (!bzm_telemetry_sample_is_fresh(sample, now_us, policy->max_age_us)) {
            uint64_t age_us = now_us >= sample->timestamp_us ? now_us - sample->timestamp_us : UINT64_MAX;
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_TELEMETRY_STALE, asic_id, 0, 0,
                              (uint32_t) policy->max_age_us, (uint32_t) age_us, index);
        }
        /* Only a finite CH2 value outside its absolute bound is eligible for
         * confirmation. A voltage-fault bit in that same unchecksummed frame
         * is qualified with the CH2 excursion. Trips, every other invalid
         * state, clock loss, temperature, CH0, CH1, and stack-spread
         * violations remain immediate failures. */
        if (!bzm_telemetry_sample_is_safe_except_ch2(sample, now_us, policy->max_age_us, &policy->bounds, require_clock_locks)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_TELEMETRY_UNSAFE, asic_id, 0, 0, 1, 0, index);
        }
    }
    return BZM_BRINGUP_GOOD;
}

static bzm_bringup_outcome_t verify_telemetry(const bzm_bringup_ops_t * ops, void * ops_context,
                                              const bzm_bringup_telemetry_policy_t * policy, uint64_t configured_after_us,
                                              bool require_clock_locks, bzm_bringup_report_t * report)
{
    if (ops->telemetry_snapshot == NULL || ops->now_us == NULL) {
        return set_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_CAPABILITY_UNAVAILABLE, 0, 0, 0, 0, 0, 0);
    }

    bzm_telemetry_store_t store = {0};
    bool all_post_configuration = false;
    for (uint8_t attempt = 0; attempt < BZM_TELEMETRY_ACQUIRE_ATTEMPTS; ++attempt) {
        if (!ops->telemetry_snapshot(ops_context, &store)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, 0, 0, 0, 0, 0, 0);
        }

        all_post_configuration = true;
        for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
            uint8_t asic_id = bzm_asic_wire_ids[index];
            const bzm_telemetry_sample_t * sample = bzm_telemetry_store_get(&store, asic_id);
            if (sample == NULL || !sample->received || sample->timestamp_us < configured_after_us) {
                all_post_configuration = false;
                break;
            }
        }
        if (all_post_configuration) {
            break;
        }
        if (attempt + 1U < BZM_TELEMETRY_ACQUIRE_ATTEMPTS && ops->delay_ms != NULL) {
            ops->delay_ms(ops_context, BZM_TELEMETRY_RETRY_DELAY_MS);
        }
    }

    bzm_telemetry_confirmation_t confirmation;
    bzm_telemetry_confirmation_init(&confirmation);
    uint8_t culprit_asic_id = 0;
    uint8_t observed_samples = 0;
    uint16_t attempt_limit = (uint16_t) policy->ch2_confirm_samples * BZM_BRINGUP_ASIC_COUNT;
    for (uint16_t attempt = 0; attempt <= attempt_limit; ++attempt) {
        uint64_t snapshot_now_us = ops->now_us(ops_context);
        bzm_bringup_outcome_t outcome =
            verify_telemetry_snapshot(&store, snapshot_now_us, configured_after_us, policy, require_clock_locks, report);

        /* CH2 excursions already pass the strict non-CH2 checks above. A
         * combined-PLL bit loss is also confirmable when every non-clock
         * field is otherwise safe. Missing, stale, pre-configuration,
         * malformed, and all other unsafe samples remain immediate failures. */
        bool confirmable = outcome == BZM_BRINGUP_GOOD;
        if (!confirmable && outcome == BZM_BRINGUP_BAD && report != NULL && report->reason == BZM_BRINGUP_REASON_TELEMETRY_UNSAFE &&
            require_clock_locks) {
            const bzm_telemetry_sample_t * sample = bzm_telemetry_store_get(&store, report->asic_id);
            confirmable =
                sample != NULL && !sample->pll_locked && !bzm_telemetry_sample_has_immediate_trip(sample) &&
                bzm_telemetry_sample_is_safe_except_ch2(sample, snapshot_now_us, policy->max_age_us, &policy->bounds, false);
        }
        if (!confirmable)
            return outcome;

        bzm_ch2_confirmation_result_t confirmation_result = bzm_telemetry_confirmation_observe(
            &confirmation, &store, snapshot_now_us, policy->max_age_us, &policy->bounds, require_clock_locks,
            policy->ch2_confirm_samples, &culprit_asic_id, &observed_samples);
        if (confirmation_result == BZM_CH2_CONFIRMATION_GOOD)
            return BZM_BRINGUP_GOOD;
        if (confirmation_result == BZM_CH2_CONFIRMATION_CONTINUOUS || confirmation_result == BZM_CH2_CONFIRMATION_INVALID) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_TELEMETRY_UNSAFE, culprit_asic_id, 0, 0,
                              policy->ch2_confirm_samples, observed_samples, 0);
        }
        if (ops->delay_ms == NULL) {
            return set_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_CAPABILITY_UNAVAILABLE, culprit_asic_id, 0, 0,
                              policy->ch2_confirm_samples, observed_samples, 0);
        }
        if (attempt == attempt_limit)
            break;

        /* Require recovery or the configured number of consecutive fresh
         * same-ASIC anomalies. The four-ASIC attempt multiplier gives each
         * TDM slot a bounded chance to produce a newer sample without letting
         * migrating one-off corruption hold powered validation indefinitely. */
        ops->delay_ms(ops_context, BZM_TELEMETRY_RETRY_DELAY_MS);
        if (!ops->telemetry_snapshot(ops_context, &store)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, culprit_asic_id, 0, 0, policy->ch2_confirm_samples,
                              observed_samples, 0);
        }
    }

    return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_TELEMETRY_UNSAFE, culprit_asic_id, 0, 0,
                      policy->ch2_confirm_samples, observed_samples, 0);
}

bzm_bringup_outcome_t bzm_bringup_stage_sensors(bzm_bringup_state_t * state, const bzm_bringup_ops_t * ops, void * ops_context,
                                                const bzm_bringup_sensor_profile_t * profile,
                                                const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                                bzm_bringup_report_t * report)
{
    if (state == NULL || !basic_ops_are_valid(ops) || !sensor_profile_is_valid(profile) ||
        !telemetry_policy_is_valid(telemetry_policy)) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_INVALID_ARGUMENT, 0, 0, 0, 0, 0, 0);
    }
    if (!state->chain_verified) {
        return set_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE, 0, 0, 0, 1, 0, 0);
    }
    if (ops->now_us == NULL || ops->telemetry_snapshot == NULL) {
        return set_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_CAPABILITY_UNAVAILABLE, 0, 0, 0, 0, 0, 0);
    }
    clear_after_sensors(state);

    uint32_t tdm_control = ((uint32_t) profile->tdm_slot_bit_count << 9) | ((uint32_t) profile->tdm_slot_count << 1) | 1U;
    for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        uint8_t asic_id = bzm_asic_wire_ids[index];
        uint32_t bandgap = 0;
        if (!ops->read_u32(ops_context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, BZM_LOCAL_REG_BANDGAP, &bandgap)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, asic_id, 0, BZM_LOCAL_REG_BANDGAP, 0, 0, index);
        }

        const bzm_register_value_t registers[] = {
            /* Quiesce unsolicited traffic before any control readback. */
            {BZM_LOCAL_REG_UART_TDM_CONTROL, tdm_control & ~1U},
            {BZM_LOCAL_REG_UART_TX, 0x0f},
            {BZM_LOCAL_REG_SLOW_CLOCK_DIVIDER, profile->slow_clock_divider},
            {BZM_LOCAL_REG_TDM_DELAY, profile->tdm_delay},
            {BZM_LOCAL_REG_SENSOR_CLOCK_DIVIDER, ((uint32_t) profile->sensor_clock_divider << 5) | profile->sensor_clock_divider},
            {BZM_LOCAL_REG_DTS_RESET_POWERDOWN, 1U << 8},
            {BZM_LOCAL_REG_SENSOR_TDM_GAP_COUNT, profile->tdm_gap_count},
            {BZM_LOCAL_REG_DTS_CONFIG, 0},
            {BZM_LOCAL_REG_SENSOR_THRESHOLD_COUNT,
             ((uint32_t) profile->thermal_threshold_count << 16) | profile->voltage_threshold_count},
            {BZM_LOCAL_REG_TEMPERATURE_TUNE_CODE, 0x8001U | ((uint32_t) profile->thermal_trip_code << 1)},
            {BZM_LOCAL_REG_BANDGAP, (bandgap & ~0x0fU) | 0x03U},
            {BZM_LOCAL_REG_VSENSOR_RESET_POWERDOWN, 1U << 8},
            {BZM_LOCAL_REG_VSENSOR_CONFIG, (8U << 28) | (1U << 24)},
            {BZM_LOCAL_REG_VSENSOR_CONTROL,
             ((uint32_t) profile->voltage_ch1_shutdown_code << 16) | ((uint32_t) profile->voltage_ch0_shutdown_code << 1) | 1U},
        };
        bzm_bringup_outcome_t outcome = write_and_verify_registers(ops, ops_context, asic_id, registers,
                                                                   sizeof(registers) / sizeof(registers[0]), index, report);
        if (outcome != BZM_BRINGUP_GOOD) {
            return outcome;
        }
    }

    /* BIRDS drives the shared chain from the first ASIC in its only stack.
     * Its production sequence programs this exact value specifically to fix
     * UART unknown-message corruption. Prove it before unsolicited TDM
     * traffic begins so the parser barrier validates a conditioned link. */
    const bzm_register_value_t drive_strength[] = {
        {BZM_LOCAL_REG_IO_PEPS_DRIVE_STRENGTH, BZM_REFERENCE_BIRDS_IO_PEPS_DRIVE_STRENGTH},
    };
    bzm_bringup_outcome_t drive_outcome = write_and_verify_registers(ops, ops_context, BZM_FIRST_ASIC_ID, drive_strength,
                                                                     sizeof(drive_strength) / sizeof(drive_strength[0]), 0, report);
    if (drive_outcome != BZM_BRINGUP_GOOD) {
        return drive_outcome;
    }

    /* UART_TX only selects which packet classes may be transmitted. TDM is
     * a separate enable in register 0x07. Start all four transmitters with a
     * single all-ASIC write only after every other sensor control has read
     * back. Besides matching the BIRDS reference sequence, one shared epoch
     * preserves the intentional ten-slot spacing between wire IDs instead of
     * creating four separately phased schedules. The slot count deliberately
     * exceeds the highest wire ID (0x28); a count of four would only allocate
     * slots for IDs 0..3 and these ASICs would never win a transmit slot. */
    if (!ops->write_u32(ops_context, BZM_ALL_ASICS, BZM_BRINGUP_CONTROL_ENGINE_ID, BZM_LOCAL_REG_UART_TDM_CONTROL, tdm_control)) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, BZM_ALL_ASICS, 0, BZM_LOCAL_REG_UART_TDM_CONTROL,
                          tdm_control, 0, 0);
    }

    state->sensors_configured_us = ops->now_us(ops_context);
    if (ops->delay_ms != NULL) {
        ops->delay_ms(ops_context, profile->settle_delay_ms);
    }
    bzm_bringup_outcome_t outcome =
        verify_telemetry(ops, ops_context, telemetry_policy, state->sensors_configured_us, false, report);
    if (outcome != BZM_BRINGUP_GOOD) {
        return outcome;
    }

    state->sensors_verified = true;
    return set_report(report, BZM_BRINGUP_GOOD, BZM_BRINGUP_REASON_NONE, BZM_LAST_ASIC_ID, 0, 0, BZM_BRINGUP_ASIC_COUNT,
                      BZM_BRINGUP_ASIC_COUNT, BZM_BRINGUP_ASIC_COUNT);
}

static bool pll_profile_is_exact_800(const bzm_bringup_pll_profile_t * profile)
{
    return profile != NULL && profile->target_mhz == 800 && profile->reference_mhz == 50 && profile->reference_divider == 2 &&
           profile->post1_divider == 1 && profile->post2_divider == 1 && profile->feedback_divider == 128 &&
           profile->postdiv_register == 0x1242 && profile->lock_attempts != 0;
}

static uint8_t pll_register(uint8_t pll, uint8_t pll0_register)
{
    return pll == 0 ? pll0_register : (uint8_t) (pll0_register + 0x0aU);
}

bzm_bringup_outcome_t bzm_bringup_stage_clocks(bzm_bringup_state_t * state, const bzm_bringup_ops_t * ops, void * ops_context,
                                               const bzm_bringup_pll_profile_t * profile,
                                               const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                               bzm_bringup_report_t * report)
{
    if (state == NULL || !basic_ops_are_valid(ops) || !pll_profile_is_exact_800(profile) ||
        !telemetry_policy_is_valid(telemetry_policy)) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_INVALID_ARGUMENT, 0, 0, 0, 800,
                          profile == NULL ? 0 : profile->target_mhz, 0);
    }
    if (!state->sensors_verified) {
        return set_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE, 0, 0, 0, 1, 0, 0);
    }
    if (ops->now_us == NULL || ops->telemetry_snapshot == NULL) {
        return set_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_CAPABILITY_UNAVAILABLE, 0, 0, 0, 0, 0, 0);
    }
    clear_after_clocks(state);

    const uint32_t tdm_control =
        ((uint32_t) BZM_REFERENCE_TDM_SLOT_BIT_COUNT << 9) | ((uint32_t) BZM_REFERENCE_TDM_SLOT_COUNT << 1) | 1U;
    /* Stop unsolicited telemetry on every ASIC before reading any control
     * register. Otherwise early ASICs can fill the UART while a later ASIC's
     * reply is still waiting for its TDM slot. */
    if (!ops->write_u32(ops_context, BZM_ALL_ASICS, BZM_BRINGUP_CONTROL_ENGINE_ID, BZM_LOCAL_REG_UART_TDM_CONTROL,
                        tdm_control & ~1U)) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, BZM_ALL_ASICS, 0, BZM_LOCAL_REG_UART_TDM_CONTROL,
                          tdm_control & ~1U, 0, 0);
    }
    for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        uint8_t asic_id = bzm_asic_wire_ids[index];
        uint32_t actual = 0;
        if (!ops->read_u32(ops_context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, BZM_LOCAL_REG_UART_TDM_CONTROL, &actual)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, asic_id, 0, BZM_LOCAL_REG_UART_TDM_CONTROL,
                              tdm_control & ~1U, 0, index);
        }
        if (actual != (tdm_control & ~1U)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_REGISTER_READBACK, asic_id, 0,
                              BZM_LOCAL_REG_UART_TDM_CONTROL, tdm_control & ~1U, actual, index);
        }
    }

    for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        uint8_t asic_id = bzm_asic_wire_ids[index];
        for (uint8_t pll = 0; pll < BZM_PLL_COUNT; ++pll) {
            uint8_t enable_register = pll_register(pll, BZM_LOCAL_REG_PLL0_ENABLE);
            if (!ops->write_u32(ops_context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, enable_register, 0)) {
                return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, asic_id, pll, enable_register, 0, 0, index);
            }
        }
        for (uint8_t pll = 0; pll < BZM_PLL_COUNT; ++pll) {
            uint8_t feedback_register = pll_register(pll, BZM_LOCAL_REG_PLL0_FBDIV);
            uint8_t postdiv_register = pll_register(pll, BZM_LOCAL_REG_PLL0_POSTDIV);
            if (!ops->write_u32(ops_context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, feedback_register,
                                profile->feedback_divider) ||
                !ops->write_u32(ops_context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, postdiv_register, profile->postdiv_register)) {
                return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, asic_id, pll, feedback_register,
                                  profile->feedback_divider, 0, index);
            }
        }
        if (ops->delay_ms != NULL) {
            ops->delay_ms(ops_context, 1);
        }
        for (uint8_t pll = 0; pll < BZM_PLL_COUNT; ++pll) {
            uint8_t enable_register = pll_register(pll, BZM_LOCAL_REG_PLL0_ENABLE);
            if (!ops->write_u32(ops_context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, enable_register, BZM_PLL_ENABLE_VALUE)) {
                return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, asic_id, pll, enable_register,
                                  BZM_PLL_ENABLE_VALUE, 0, index);
            }
        }

        for (uint8_t pll = 0; pll < BZM_PLL_COUNT; ++pll) {
            const bzm_register_value_t expected[] = {
                {pll_register(pll, BZM_LOCAL_REG_PLL0_FBDIV), profile->feedback_divider},
                {pll_register(pll, BZM_LOCAL_REG_PLL0_POSTDIV), profile->postdiv_register},
            };
            for (size_t item = 0; item < sizeof(expected) / sizeof(expected[0]); ++item) {
                uint32_t actual = 0;
                if (!ops->read_u32(ops_context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, expected[item].offset, &actual)) {
                    return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, asic_id, pll, expected[item].offset,
                                      expected[item].value, 0, index);
                }
                if (actual != expected[item].value) {
                    return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_REGISTER_READBACK, asic_id, pll,
                                      expected[item].offset, expected[item].value, actual, index);
                }
            }
        }

        bool locked[BZM_PLL_COUNT] = {false, false};
        uint32_t enable_values[BZM_PLL_COUNT] = {0, 0};
        for (uint8_t attempt = 0; attempt < profile->lock_attempts && !(locked[0] && locked[1]); ++attempt) {
            for (uint8_t pll = 0; pll < BZM_PLL_COUNT; ++pll) {
                uint8_t enable_register = pll_register(pll, BZM_LOCAL_REG_PLL0_ENABLE);
                if (!ops->read_u32(ops_context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, enable_register, &enable_values[pll])) {
                    return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, asic_id, pll, enable_register,
                                      BZM_PLL_LOCK_MASK, 0, index);
                }
                locked[pll] = (enable_values[pll] & BZM_PLL_LOCK_MASK) == BZM_PLL_LOCK_MASK;
            }
            if (!(locked[0] && locked[1]) && ops->delay_ms != NULL) {
                ops->delay_ms(ops_context, profile->lock_poll_delay_ms);
            }
        }
        for (uint8_t pll = 0; pll < BZM_PLL_COUNT; ++pll) {
            if (!locked[pll]) {
                uint8_t enable_register = pll_register(pll, BZM_LOCAL_REG_PLL0_ENABLE);
                return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_PLL_UNLOCKED, asic_id, pll, enable_register,
                                  BZM_PLL_LOCK_MASK, enable_values[pll] & BZM_PLL_LOCK_MASK, index);
            }
        }

        const bzm_register_value_t dll_disabled[] = {
            {BZM_LOCAL_REG_DLL0_CONTROL_5, 0},
            {BZM_LOCAL_REG_DLL1_CONTROL_5, 0},
        };
        bzm_bringup_outcome_t dll_outcome = write_and_verify_registers(
            ops, ops_context, asic_id, dll_disabled, sizeof(dll_disabled) / sizeof(dll_disabled[0]), index, report);
        if (dll_outcome != BZM_BRINGUP_GOOD) {
            return dll_outcome;
        }

        /* PLL programming must not disturb the sensor/TDM path used to
         * produce the lock and voltage evidence below. Re-prove the known
         * reference controls after both PLLs have locked. */
        const bzm_register_value_t sensor_controls[] = {
            {BZM_LOCAL_REG_UART_TDM_CONTROL, tdm_control & ~1U},
            {BZM_LOCAL_REG_SLOW_CLOCK_DIVIDER, BZM_REFERENCE_SLOW_CLOCK_DIVIDER},
            {BZM_LOCAL_REG_TDM_DELAY, BZM_REFERENCE_TDM_DELAY},
            {BZM_LOCAL_REG_UART_TX, 0x0f},
            {BZM_LOCAL_REG_SENSOR_TDM_GAP_COUNT, CONFIG_BZM_1002_SENSOR_TDM_GAP_COUNT},
            {BZM_LOCAL_REG_SENSOR_CLOCK_DIVIDER, (BZM_REFERENCE_SENSOR_DIVIDER << 5) | BZM_REFERENCE_SENSOR_DIVIDER},
        };
        for (size_t item = 0; item < sizeof(sensor_controls) / sizeof(sensor_controls[0]); ++item) {
            uint32_t actual = 0;
            if (!ops->read_u32(ops_context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, sensor_controls[item].offset, &actual)) {
                return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, asic_id, 0, sensor_controls[item].offset,
                                  sensor_controls[item].value, 0, index);
            }
            if (actual != sensor_controls[item].value) {
                return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_REGISTER_READBACK, asic_id, 0,
                                  sensor_controls[item].offset, sensor_controls[item].value, actual, index);
            }
        }
    }

    if (!ops->write_u32(ops_context, BZM_ALL_ASICS, BZM_BRINGUP_CONTROL_ENGINE_ID, BZM_LOCAL_REG_UART_TDM_CONTROL, tdm_control)) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO, BZM_ALL_ASICS, 0, BZM_LOCAL_REG_UART_TDM_CONTROL,
                          tdm_control, 0, 0);
    }

    state->clocks_configured_us = ops->now_us(ops_context);
    bzm_bringup_outcome_t outcome = verify_telemetry(ops, ops_context, telemetry_policy, state->clocks_configured_us, true, report);
    if (outcome != BZM_BRINGUP_GOOD) {
        return outcome;
    }

    state->clocks_verified = true;
    return set_report(report, BZM_BRINGUP_GOOD, BZM_BRINGUP_REASON_NONE, BZM_LAST_ASIC_ID, 1, 0, 800, 800, BZM_BRINGUP_ASIC_COUNT);
}

static bool topology_is_exact(void)
{
    bool physical_seen[BZM_TOPOLOGY_COLUMNS * 64U] = {false};
    uint16_t stack_counts[BZM_ENGINE_STACK_COUNT] = {0};

    for (uint16_t index = 0; index < BZM_TOPOLOGY_ENGINE_COUNT; ++index) {
        bzm_engine_location_t engine;
        if (!bzm_topology_engine_at(index, &engine) || engine.topology_index != index ||
            engine.physical_id >= sizeof(physical_seen) / sizeof(physical_seen[0]) || physical_seen[engine.physical_id] ||
            !bzm_topology_coordinate_is_valid(engine.row, engine.column) || engine.stack >= BZM_ENGINE_STACK_COUNT) {
            return false;
        }
        physical_seen[engine.physical_id] = true;
        ++stack_counts[engine.stack];
    }
    if (stack_counts[BZM_ENGINE_STACK_BOTTOM] != BZM_TOPOLOGY_STACK_ENGINE_COUNT ||
        stack_counts[BZM_ENGINE_STACK_TOP] != BZM_TOPOLOGY_STACK_ENGINE_COUNT) {
        return false;
    }

    for (uint16_t index = 0; index < BZM_TOPOLOGY_PAIR_COUNT; ++index) {
        bzm_engine_pair_t pair;
        if (!bzm_topology_balanced_pair_at(index, &pair) || pair.pair_index != index ||
            pair.bottom.stack != BZM_ENGINE_STACK_BOTTOM || pair.top.stack != BZM_ENGINE_STACK_TOP ||
            pair.bottom.stack_index != index || pair.top.stack_index != index || pair.bottom.physical_id == pair.top.physical_id) {
            return false;
        }
    }
    return BZM_TOPOLOGY_ENGINE_COUNT == 236U && BZM_TOPOLOGY_PAIR_COUNT == 118U;
}

bzm_bringup_outcome_t bzm_bringup_stage_balanced_ramp(bzm_bringup_state_t * state, const bzm_bringup_ops_t * ops,
                                                      void * ops_context, const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                                      bzm_bringup_report_t * report)
{
    if (state == NULL || ops == NULL) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_INVALID_ARGUMENT, 0, 0, 0, 0, 0, 0);
    }
    state->balanced_ramp_verified = false;
    state->running_verified = false;
    if (!state->clocks_verified) {
        return set_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE, 0, 0, 0, 1, 0, 0);
    }
    if (!topology_is_exact()) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_TOPOLOGY, 0, 0, 0, BZM_TOPOLOGY_ENGINE_COUNT, 0, 0);
    }
    if (ops->balanced_batch_begin == NULL || ops->balanced_pair_commit == NULL || ops->balanced_batch_end == NULL ||
        ops->activation_barrier == NULL) {
        return set_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_BALANCED_PAIR_UNAVAILABLE, 0, 0, 0, 1, 0, 0);
    }
    /* Validate every evidence capability before the first engine can be
     * activated. This preserves an all-or-nothing pre-activation block for
     * incomplete production adapters. */
    if (!telemetry_policy_is_valid(telemetry_policy)) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_INVALID_ARGUMENT, 0, 0, 0, 0, 0, 0);
    }
    if (ops->write_u32 == NULL || ops->read_u32 == NULL || ops->now_us == NULL || ops->telemetry_snapshot == NULL) {
        return set_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_CAPABILITY_UNAVAILABLE, 0, 0, 0, 0, 0, 0);
    }

    /* BIRDS disables the result FSM before turning TDM off. Stage 6 pauses
     * TDM around every sentinel activation batch, so leaving result reports
     * enabled here permits raw, headerless eight-byte result/status packets
     * to escape while the framed transport is paused. Keep reports disabled
     * for the complete ramp; Stage 7 enables them only after TDM is restored. */
    const bzm_register_value_t result_reporting_disabled[] = {
        {BZM_LOCAL_REG_RESULT_STATUS_CONTROL, BZM_RESULT_REPORT_DISABLED},
    };
    for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        uint8_t asic_id = bzm_asic_wire_ids[index];
        bzm_bringup_outcome_t result_status =
            write_and_verify_registers(ops, ops_context, asic_id, result_reporting_disabled,
                                       sizeof(result_reporting_disabled) / sizeof(result_reporting_disabled[0]), 0, report);
        if (result_status != BZM_BRINGUP_GOOD) {
            return result_status;
        }
    }

    uint16_t completed = 0;
    for (uint16_t pair_index = 0; pair_index < BZM_TOPOLOGY_PAIR_COUNT; ++pair_index) {
        bzm_engine_pair_t pair;
        if (!bzm_topology_balanced_pair_at(pair_index, &pair)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_TOPOLOGY, 0, 0, 0, pair_index, 0, completed);
        }
        if (!ops->balanced_batch_begin(ops_context, pair_index)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_BALANCED_BATCH, 0, 0, 0, 1, 0, completed);
        }
        for (uint8_t asic_index = 0; asic_index < BZM_BRINGUP_ASIC_COUNT; ++asic_index) {
            uint8_t asic_id = bzm_asic_wire_ids[asic_index];
            if (!ops->balanced_pair_commit(ops_context, asic_id, &pair)) {
                return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_BALANCED_PAIR_COMMIT, asic_id, 0, 0, pair_index, 0,
                                  completed);
            }
            ++completed;
        }
        if (!ops->balanced_batch_end(ops_context, pair_index)) {
            return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_BALANCED_BATCH, 0, 0, 0, 1, 0, completed);
        }

        /* Timestamp only after all four ASIC commits in this balanced
         * batch. A cached sample from before any commit must not prove that
         * the newly activated pair is electrically safe and clock locked. */
        uint64_t committed_after_us = ops->now_us(ops_context);
        bzm_bringup_outcome_t telemetry_outcome =
            verify_telemetry(ops, ops_context, telemetry_policy, committed_after_us, true, report);
        if (telemetry_outcome != BZM_BRINGUP_GOOD) {
            if (report != NULL) {
                report->completed_items = completed;
            }
            return telemetry_outcome;
        }
    }
    if (!ops->activation_barrier(ops_context, BZM_BRINGUP_ASIC_COUNT, BZM_TOPOLOGY_PAIR_COUNT)) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_ACTIVATION_BARRIER, 0, 0, 0,
                          BZM_TOPOLOGY_PAIR_COUNT * BZM_BRINGUP_ASIC_COUNT, completed, completed);
    }

    state->balanced_ramp_verified = true;
    return set_report(report, BZM_BRINGUP_GOOD, BZM_BRINGUP_REASON_NONE, BZM_LAST_ASIC_ID, 0, 0,
                      BZM_TOPOLOGY_PAIR_COUNT * BZM_BRINGUP_ASIC_COUNT, completed, completed);
}

bzm_bringup_outcome_t bzm_bringup_check_running(bzm_bringup_state_t * state, const bzm_bringup_ops_t * ops, void * ops_context,
                                                const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                                bzm_bringup_report_t * report)
{
    if (state == NULL || ops == NULL || !telemetry_policy_is_valid(telemetry_policy)) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_INVALID_ARGUMENT, 0, 0, 0, 0, 0, 0);
    }
    state->running_verified = false;
    if (!state->balanced_ramp_verified) {
        return set_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE, 0, 0, 0, 1, 0, 0);
    }
    if (!topology_is_exact()) {
        return set_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_TOPOLOGY, 0, 0, 0, BZM_TOPOLOGY_ENGINE_COUNT, 0, 0);
    }
    if (ops->write_u32 == NULL || ops->read_u32 == NULL || ops->now_us == NULL || ops->telemetry_snapshot == NULL) {
        return set_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_CAPABILITY_UNAVAILABLE, 0, 0, 0, 0, 0, 0);
    }

    /* TDM is now continuously enabled. Only at this boundary may the ASIC
     * result FSM be opened, so every mining result uses the addressed TDM
     * frame consumed by bzm_frame_parser. */
    const bzm_register_value_t result_reporting_enabled[] = {
        {BZM_LOCAL_REG_RESULT_STATUS_CONTROL, BZM_RESULT_REPORT_ENABLED},
    };
    for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
        uint8_t asic_id = bzm_asic_wire_ids[index];
        bzm_bringup_outcome_t outcome =
            write_and_verify_registers(ops, ops_context, asic_id, result_reporting_enabled,
                                       sizeof(result_reporting_enabled) / sizeof(result_reporting_enabled[0]), index, report);
        if (outcome != BZM_BRINGUP_GOOD) {
            return outcome;
        }
    }
    uint64_t result_reporting_enabled_us = ops->now_us(ops_context);
    bzm_bringup_outcome_t outcome = verify_telemetry(ops, ops_context, telemetry_policy, result_reporting_enabled_us, true, report);
    if (outcome != BZM_BRINGUP_GOOD) {
        return outcome;
    }
    state->running_verified = true;
    return set_report(report, BZM_BRINGUP_GOOD, BZM_BRINGUP_REASON_NONE, BZM_LAST_ASIC_ID, 0, 0, 1, 1, BZM_BRINGUP_ASIC_COUNT);
}
