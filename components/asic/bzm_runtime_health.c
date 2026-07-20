#include "bzm_runtime_health.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bzm_runtime_health_result_t health_result(bzm_runtime_health_status_t status, bzm_runtime_health_fault_t fault,
                                                 const char * format, ...)
{
    bzm_runtime_health_result_t result = {
        .status = status,
        .fault = fault,
    };
    if (format != NULL) {
        va_list args;
        va_start(args, format);
        vsnprintf(result.detail, sizeof(result.detail), format, args);
        va_end(args);
    }
    return result;
}

static bzm_runtime_health_result_t good(const char * detail)
{
    return health_result(BZM_RUNTIME_HEALTH_GOOD, BZM_RUNTIME_HEALTH_FAULT_NONE, "%s", detail);
}

static bzm_runtime_health_result_t bad(bzm_runtime_health_fault_t fault, const char * format, ...)
{
    bzm_runtime_health_result_t result = {
        .status = BZM_RUNTIME_HEALTH_BAD,
        .fault = fault,
    };
    if (format != NULL) {
        va_list args;
        va_start(args, format);
        vsnprintf(result.detail, sizeof(result.detail), format, args);
        va_end(args);
    }
    return result;
}

static bool finite_range(float minimum, float maximum)
{
    return isfinite(minimum) && isfinite(maximum) && minimum <= maximum;
}

static bool in_range(float value, float minimum, float maximum)
{
    return isfinite(value) && finite_range(minimum, maximum) && value >= minimum && value <= maximum;
}

static bzm_runtime_health_result_t check_bridge_and_fan(const bzm_runtime_health_input_t * input)
{
    const bzm_bridge_safety_status_t * status = &input->bridge_status;
    const uint16_t required_capabilities = BZM_BRIDGE_SAFETY_CAP_5V_CONTROL | BZM_BRIDGE_SAFETY_CAP_ASIC_RESET_CONTROL |
                                           BZM_BRIDGE_SAFETY_CAP_FAN_FORCE_FULL | BZM_BRIDGE_SAFETY_CAP_TRIP_INPUT_SAMPLED;
    const uint16_t required_evidence =
        BZM_BRIDGE_SAFETY_EVIDENCE_LEASE_VALID | BZM_BRIDGE_SAFETY_EVIDENCE_TRIP_CLEAR | BZM_BRIDGE_SAFETY_EVIDENCE_FAULT_CLEAR;

    if (!input->bridge_status_available || !status->valid) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_BRIDGE_UNAVAILABLE, "bridge safety status is unavailable");
    }
    if (status->schema_version != BZM_BRIDGE_SAFETY_STATUS_SCHEMA_VERSION || status->stage < BZM_BRIDGE_SAFETY_STAGE_LEASE) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_BRIDGE_STATUS_INVALID, "bridge status schema=%u safetyStage=%u is not lease-capable",
                   (unsigned) status->schema_version, (unsigned) status->stage);
    }
    if ((status->capabilities & required_capabilities) != required_capabilities) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_BRIDGE_CAPABILITY_MISSING, "bridge capabilities=0x%04x missing required=0x%04x",
                   (unsigned) status->capabilities, (unsigned) required_capabilities);
    }
    if (input->require_independent_kill) {
        const uint16_t production_capabilities = BZM_BRIDGE_SAFETY_CAP_CORE_POWER_CUTOFF |
                                                 BZM_BRIDGE_SAFETY_CAP_FAN_TACH_INTERLOCK |
                                                 BZM_BRIDGE_SAFETY_CAP_INDEPENDENT_TRIP_MONITOR;
        const uint16_t production_evidence = BZM_BRIDGE_SAFETY_EVIDENCE_CORE_CUTOFF_AVAILABLE |
                                             BZM_BRIDGE_SAFETY_EVIDENCE_FAN_TACH_INTERLOCK_AVAILABLE |
                                             BZM_BRIDGE_SAFETY_EVIDENCE_INDEPENDENT_TRIP_MONITOR_AVAILABLE;
        if (status->stage != BZM_BRIDGE_SAFETY_STAGE_TRIP_LATCH ||
            status->production_verdict != BZM_BRIDGE_SAFETY_PRODUCTION_GOOD ||
            (status->capabilities & production_capabilities) != production_capabilities ||
            (status->evidence & production_evidence) != production_evidence) {
            return bad(
                BZM_RUNTIME_HEALTH_FAULT_BRIDGE_CAPABILITY_MISSING,
                "live production independent-kill proof is absent: stage=%u verdict=0x%02x capabilities=0x%04x evidence=0x%04x",
                (unsigned) status->stage, (unsigned) status->production_verdict, (unsigned) status->capabilities,
                (unsigned) status->evidence);
        }
    }
    if (status->state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_BRIDGE_STATE, "bridge state=%u expected CONTROLLED", (unsigned) status->state);
    }
    if (status->fault != BZM_BRIDGE_SAFETY_FAULT_NONE || status->trip_input_asserted ||
        status->runtime_verdict != BZM_BRIDGE_SAFETY_RUNTIME_GOOD_CONTROLLED) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_BRIDGE_FAULT, "bridge fault=%u trip=%u runtimeVerdict=0x%02x", (unsigned) status->fault,
                   (unsigned) status->trip_input_asserted, (unsigned) status->runtime_verdict);
    }
    if (status->lease_remaining_ms == 0 || (status->evidence & required_evidence) != required_evidence) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_BRIDGE_LEASE, "bridge lease=%lu ms evidence=0x%04x required=0x%04x",
                   (unsigned long) status->lease_remaining_ms, (unsigned) status->evidence, (unsigned) required_evidence);
    }

    bool outputs_safe = !status->five_volt_enabled && status->asic_reset_asserted && status->fan_full && status->fan_percent == 100;
    bool outputs_safe_evidence = (status->evidence & BZM_BRIDGE_SAFETY_EVIDENCE_OUTPUTS_SAFE) != 0;
    if (outputs_safe != outputs_safe_evidence) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_BRIDGE_STATUS_INVALID, "bridge output-safe evidence contradicts output readback");
    }
    if (!status->fan_full || status->fan_percent != 100) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_FAN_NOT_FULL, "bridge fan full=%u percent=%u expected 100 percent",
                   (unsigned) status->fan_full, (unsigned) status->fan_percent);
    }
    if (!input->fan_tach_available) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_FAN_TACH_UNAVAILABLE, "fresh fan tach is unavailable");
    }
    if (input->fan_min_rpm == 0) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT, "fan minimum RPM must be nonzero");
    }
    if (input->fan_rpm < input->fan_min_rpm) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_FAN_TACH_LOW, "fan tach=%u RPM below minimum=%u RPM", (unsigned) input->fan_rpm,
                   (unsigned) input->fan_min_rpm);
    }
    return good("bridge lease/status and full-speed fan tach are GOOD");
}

static bzm_runtime_health_result_t check_power(const bzm_runtime_health_input_t * input)
{
    const bzm_bridge_safety_status_t * bridge = &input->bridge_status;
    bool expect_5v = input->reached_stage >= BZM_STAGE_CHAIN_4;
    bool expect_reset = !expect_5v;
    if (bridge->five_volt_enabled != expect_5v || bridge->asic_reset_asserted != expect_reset) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_BRIDGE_OUTPUT, "bridge 5V=%u resetAsserted=%u expected 5V=%u resetAsserted=%u for %s",
                   (unsigned) bridge->five_volt_enabled, (unsigned) bridge->asic_reset_asserted, (unsigned) expect_5v,
                   (unsigned) expect_reset, bzm_validation_stage_name(input->reached_stage));
    }

    if (!finite_range(input->tps_bounds.vin_min_v, input->tps_bounds.vin_max_v) ||
        !isfinite(input->tps_bounds.vout_command_v) || !isfinite(input->tps_bounds.vout_command_tolerance_v) ||
        input->tps_bounds.vout_command_tolerance_v < 0.0f ||
        !finite_range(input->tps_bounds.vout_min_v, input->tps_bounds.vout_max_v) ||
        !finite_range(input->tps_bounds.iout_min_a, input->tps_bounds.iout_max_a) ||
        !finite_range(input->tps_bounds.temperature_min_c, input->tps_bounds.temperature_max_c)) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT, "TPS health bounds are invalid");
    }
    if (!input->tps.available) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_TPS_UNAVAILABLE, "fresh TPS status and telemetry are unavailable");
    }
    if (!input->tps.pgood) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_TPS_PGOOD_LOW, "TPS PGOOD is low");
    }
    if ((input->tps.operation & BZM_RUNTIME_HEALTH_TPS_OPERATION_ON_MASK) == 0) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_TPS_OPERATION_OFF, "TPS OPERATION=0x%02x does not command conversion on",
                   (unsigned) input->tps.operation);
    }
    uint16_t fault_bits = input->tps.status_word & BZM_RUNTIME_HEALTH_TPS_STATUS_FAULT_MASK;
    if (fault_bits != 0) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_TPS_STATUS, "TPS STATUS_WORD=0x%04x faultBits=0x%04x",
                   (unsigned) input->tps.status_word, (unsigned) fault_bits);
    }
    if (!input->tps.vout_command_matches_expected ||
        !isfinite(input->tps.vout_command_v) ||
        fabsf(input->tps.vout_command_v - input->tps_bounds.vout_command_v) >
            input->tps_bounds.vout_command_tolerance_v) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_TPS_VOUT_COMMAND,
                   "TPS VOUT_COMMAND=%.3f V exactRaw=%u expected %.3f +/- %.3f V",
                   input->tps.vout_command_v,
                   (unsigned) input->tps.vout_command_matches_expected,
                   input->tps_bounds.vout_command_v,
                   input->tps_bounds.vout_command_tolerance_v);
    }
    if (!in_range(input->tps.vin_v, input->tps_bounds.vin_min_v, input->tps_bounds.vin_max_v)) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_TPS_VIN_RANGE, "TPS VIN=%.3f V outside [%.3f, %.3f] V", input->tps.vin_v,
                   input->tps_bounds.vin_min_v, input->tps_bounds.vin_max_v);
    }
    if (!in_range(input->tps.vout_v, input->tps_bounds.vout_min_v, input->tps_bounds.vout_max_v)) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_TPS_VOUT_RANGE, "TPS VOUT=%.3f V outside [%.3f, %.3f] V", input->tps.vout_v,
                   input->tps_bounds.vout_min_v, input->tps_bounds.vout_max_v);
    }
    if (!in_range(input->tps.iout_a, input->tps_bounds.iout_min_a, input->tps_bounds.iout_max_a)) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_TPS_IOUT_RANGE, "TPS IOUT=%.3f A outside [%.3f, %.3f] A", input->tps.iout_a,
                   input->tps_bounds.iout_min_a, input->tps_bounds.iout_max_a);
    }
    if (!in_range(input->tps.temperature_c, input->tps_bounds.temperature_min_c, input->tps_bounds.temperature_max_c)) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_TPS_TEMPERATURE_RANGE, "TPS temperature=%.1f C outside [%.1f, %.1f] C",
                   input->tps.temperature_c, input->tps_bounds.temperature_min_c, input->tps_bounds.temperature_max_c);
    }
    return good("TPS fixed VOUT command, PGOOD, operation, status, VIN, VOUT and temperature are GOOD");
}

static bzm_runtime_health_result_t parser_regression(const char * name, uint32_t baseline, uint32_t current)
{
    return bad(BZM_RUNTIME_HEALTH_FAULT_PARSER_COUNTER_REGRESSED, "parser counter %s regressed baseline=%lu current=%lu", name,
               (unsigned long) baseline, (unsigned long) current);
}

static bzm_runtime_health_result_t parser_delta(const char * name, uint32_t baseline, uint32_t current)
{
    return bad(BZM_RUNTIME_HEALTH_FAULT_PARSER_ERROR_DELTA, "parser error counter %s increased baseline=%lu current=%lu", name,
               (unsigned long) baseline, (unsigned long) current);
}

bool bzm_runtime_health_parser_window_is_clean(const bzm_serial_parser_stats_t * baseline,
                                               const bzm_serial_parser_stats_t * current)
{
    return baseline != NULL && current != NULL && current->emitted_frames >= baseline->emitted_frames &&
           current->discarded_bytes == baseline->discarded_bytes &&
           current->unexpected_register_headers == baseline->unexpected_register_headers &&
           current->dropped_results == baseline->dropped_results &&
           current->rejected_result_frames == baseline->rejected_result_frames &&
           current->unmatched_register_frames == baseline->unmatched_register_frames &&
           current->unsolicited_noop_frames == baseline->unsolicited_noop_frames &&
           current->invalid_noop_frames == baseline->invalid_noop_frames &&
           current->telemetry_decode_failures == baseline->telemetry_decode_failures && current->buffered_bytes == 0 &&
           current->queued_results == 0;
}

bool bzm_runtime_health_parser_window_is_discard_transition(const bzm_serial_parser_stats_t * baseline,
                                                            const bzm_serial_parser_stats_t * current)
{
    return baseline != NULL && current != NULL && current->emitted_frames >= baseline->emitted_frames &&
           current->discarded_bytes >= baseline->discarded_bytes &&
           current->unexpected_register_headers == baseline->unexpected_register_headers &&
           current->dropped_results == baseline->dropped_results &&
           current->rejected_result_frames == baseline->rejected_result_frames &&
           current->unmatched_register_frames == baseline->unmatched_register_frames &&
           current->unsolicited_noop_frames == baseline->unsolicited_noop_frames &&
           current->invalid_noop_frames == baseline->invalid_noop_frames &&
           current->telemetry_decode_failures == baseline->telemetry_decode_failures && current->buffered_bytes == 0 &&
           current->queued_results == 0;
}

bool bzm_parser_settling_snapshot_at_frame_boundary(const bzm_serial_parser_stats_t * stats)
{
    return stats != NULL && stats->buffered_bytes == 0;
}

void bzm_parser_settling_init(bzm_parser_settling_t * settling, const bzm_serial_parser_stats_t * baseline)
{
    if (settling == NULL)
        return;
    *settling = (bzm_parser_settling_t){0};
    if (baseline != NULL && baseline->queued_results == 0 && baseline->buffered_bytes == 0) {
        settling->baseline = *baseline;
        settling->initialized = true;
    }
}

bzm_parser_settling_result_t bzm_parser_settling_observe(bzm_parser_settling_t * settling,
                                                         const bzm_serial_parser_stats_t * current, uint8_t required_clean_windows)
{
    if (settling == NULL || !settling->initialized || current == NULL || required_clean_windows == 0 ||
        !bzm_runtime_health_parser_window_is_discard_transition(&settling->baseline, current)) {
        return BZM_PARSER_SETTLING_BAD;
    }

    if (current->discarded_bytes == settling->baseline.discarded_bytes) {
        if (settling->clean_windows < UINT8_MAX)
            ++settling->clean_windows;
    } else {
        settling->clean_windows = 0;
    }
    settling->baseline = *current;
    return settling->clean_windows >= required_clean_windows ? BZM_PARSER_SETTLING_COMPLETE : BZM_PARSER_SETTLING_PENDING;
}

void bzm_parser_realign_init(bzm_parser_realign_t * realign, const bzm_serial_parser_stats_t * baseline)
{
    if (realign == NULL)
        return;
    *realign = (bzm_parser_realign_t){0};
    if (baseline != NULL && baseline->queued_results == 0 && baseline->buffered_bytes == 0) {
        realign->accepted = *baseline;
        realign->last = *baseline;
        realign->initialized = true;
    }
}

static bool parser_realign_transition_is_valid(const bzm_serial_parser_stats_t * baseline,
                                               const bzm_serial_parser_stats_t * current)
{
    if (baseline == NULL || current == NULL || current->discarded_bytes < baseline->discarded_bytes ||
        current->unexpected_register_headers < baseline->unexpected_register_headers) {
        return false;
    }

    uint32_t discarded_delta = current->discarded_bytes - baseline->discarded_bytes;
    uint32_t unexpected_register_delta =
        current->unexpected_register_headers - baseline->unexpected_register_headers;
    return current->emitted_frames >= baseline->emitted_frames &&
           unexpected_register_delta <= discarded_delta &&
           current->dropped_results == baseline->dropped_results &&
           current->rejected_result_frames == baseline->rejected_result_frames &&
           current->unmatched_register_frames == baseline->unmatched_register_frames &&
           current->unsolicited_noop_frames == baseline->unsolicited_noop_frames &&
           current->invalid_noop_frames == baseline->invalid_noop_frames &&
           current->telemetry_decode_failures == baseline->telemetry_decode_failures;
}

bzm_parser_realign_result_t bzm_parser_realign_observe(bzm_parser_realign_t * realign, const bzm_serial_parser_stats_t * current,
                                                       uint32_t maximum_discarded_bytes, uint8_t required_clean_windows,
                                                       uint8_t maximum_windows, uint8_t maximum_events)
{
    if (realign == NULL || !realign->initialized || current == NULL || maximum_discarded_bytes == 0 ||
        required_clean_windows == 0 || maximum_windows < required_clean_windows || maximum_events == 0 ||
        !parser_realign_transition_is_valid(&realign->accepted, current) ||
        !parser_realign_transition_is_valid(&realign->last, current)) {
        return BZM_PARSER_REALIGN_BAD;
    }

    if (!realign->recovering && current->discarded_bytes == realign->accepted.discarded_bytes &&
        current->unexpected_register_headers == realign->accepted.unexpected_register_headers) {
        realign->accepted = *current;
        realign->last = *current;
        realign->episode_bursts = 0;
        return BZM_PARSER_REALIGN_CLEAN;
    }

    if (!realign->recovering) {
        realign->recovering = true;
        realign->burst_discard_baseline = realign->accepted.discarded_bytes;
        realign->burst_unexpected_register_baseline = realign->accepted.unexpected_register_headers;
        realign->episode_bursts = 1;
        realign->clean_windows = 0;
        realign->observed_windows = 1;
        realign->last = *current;
    } else {
        if (realign->observed_windows < UINT8_MAX)
            ++realign->observed_windows;
        if (current->discarded_bytes != realign->last.discarded_bytes ||
            current->unexpected_register_headers != realign->last.unexpected_register_headers) {
            if (realign->episode_bursts >= maximum_events)
                return BZM_PARSER_REALIGN_BAD;
            ++realign->episode_bursts;
            realign->clean_windows = 0;
        } else if (current->buffered_bytes == 0 &&
                   current->emitted_frames > current->emitted_frames_at_last_discard) {
            if (realign->clean_windows < UINT8_MAX)
                ++realign->clean_windows;
        } else {
            realign->clean_windows = 0;
        }
        realign->last = *current;
    }

    uint32_t discarded = current->discarded_bytes - realign->burst_discard_baseline;
    if (discarded > maximum_discarded_bytes || realign->observed_windows > maximum_windows) {
        return BZM_PARSER_REALIGN_BAD;
    }
    if (realign->clean_windows >= required_clean_windows) {
        realign->accepted = *current;
        realign->last = *current;
        realign->recovering = false;
        realign->episode_bursts = 0;
        return BZM_PARSER_REALIGN_RECOVERED;
    }
    return BZM_PARSER_REALIGN_PENDING;
}

#define CHECK_PARSER_REGRESSION(field)                                                                                             \
    do {                                                                                                                           \
        if (current->field < baseline->field) {                                                                                    \
            return parser_regression(#field, baseline->field, current->field);                                                     \
        }                                                                                                                          \
    } while (0)

#define CHECK_PARSER_DELTA(field)                                                                                                  \
    do {                                                                                                                           \
        if (current->field != baseline->field) {                                                                                   \
            return parser_delta(#field, baseline->field, current->field);                                                          \
        }                                                                                                                          \
    } while (0)

static bzm_runtime_health_result_t check_parser(const bzm_runtime_health_input_t * input)
{
    if (!input->parser_stats_available) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_PARSER_UNAVAILABLE, "parser baseline/current snapshots are unavailable");
    }
    const bzm_serial_parser_stats_t * baseline = &input->parser_baseline;
    const bzm_serial_parser_stats_t * current = &input->parser_current;

    CHECK_PARSER_REGRESSION(emitted_frames);
    CHECK_PARSER_REGRESSION(discarded_bytes);
    CHECK_PARSER_REGRESSION(unexpected_register_headers);
    CHECK_PARSER_REGRESSION(dropped_results);
    CHECK_PARSER_REGRESSION(rejected_result_frames);
    CHECK_PARSER_REGRESSION(unmatched_register_frames);
    CHECK_PARSER_REGRESSION(unsolicited_noop_frames);
    CHECK_PARSER_REGRESSION(invalid_noop_frames);
    CHECK_PARSER_REGRESSION(telemetry_decode_failures);

    CHECK_PARSER_DELTA(unexpected_register_headers);
    CHECK_PARSER_DELTA(dropped_results);
    CHECK_PARSER_DELTA(rejected_result_frames);
    CHECK_PARSER_DELTA(unmatched_register_frames);
    CHECK_PARSER_DELTA(unsolicited_noop_frames);
    CHECK_PARSER_DELTA(invalid_noop_frames);
    CHECK_PARSER_DELTA(telemetry_decode_failures);

    if (current->discarded_bytes != baseline->discarded_bytes) {
        char recent[3U * BZM_FRAME_PARSER_DISCARD_TRACE_SIZE + 1U] = {0};
        size_t offset = 0;
        for (size_t index = 0; index < current->recent_discarded_length && offset + 3U < sizeof(recent); ++index) {
            int written = snprintf(recent + offset, sizeof(recent) - offset, "%s%02x", index == 0 ? "" : " ",
                                   current->recent_discarded_bytes[index]);
            if (written <= 0 || (size_t) written >= sizeof(recent) - offset) {
                break;
            }
            offset += (size_t) written;
        }
        return bad(BZM_RUNTIME_HEALTH_FAULT_PARSER_ERROR_DELTA,
                   "parser error counter discarded_bytes increased baseline=%lu current=%lu recent=[%s]",
                   (unsigned long) baseline->discarded_bytes, (unsigned long) current->discarded_bytes, recent);
    }

    return good("UART parser error counters have no runtime delta");
}

#undef CHECK_PARSER_DELTA
#undef CHECK_PARSER_REGRESSION

static bool valid_telemetry_bounds(const bzm_telemetry_bounds_t * bounds)
{
    return bounds != NULL && finite_range(bounds->temperature_min_c, bounds->temperature_max_c) &&
           finite_range(bounds->ch0_min_mv, bounds->ch0_max_mv) && finite_range(bounds->ch1_min_mv, bounds->ch1_max_mv) &&
           isfinite(bounds->ch2_abs_max_mv) && bounds->ch2_abs_max_mv >= 0.0f && isfinite(bounds->max_stack_spread_mv) &&
           bounds->max_stack_spread_mv >= 0.0f;
}

static bzm_runtime_health_result_t check_telemetry(const bzm_runtime_health_input_t * input)
{
    if (!input->telemetry_available) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_UNAVAILABLE, "four-ASIC telemetry snapshot is unavailable");
    }
    if (!valid_telemetry_bounds(&input->telemetry_bounds)) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT, "ASIC telemetry bounds are invalid");
    }

    bool require_clock_locks = input->reached_stage >= BZM_STAGE_CLOCKS;
    for (size_t index = 0; index < BZM_MAX_ASIC_COUNT; ++index) {
        const bzm_telemetry_sample_t * sample = &input->telemetry.samples[index];
        uint8_t expected_id = bzm_asic_wire_ids[index];
        if (!sample->received || sample->asic_id != expected_id) {
            return bad(BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_MISSING, "ASIC 0x%02x telemetry missing or misaddressed as 0x%02x",
                       (unsigned) expected_id, (unsigned) sample->asic_id);
        }
        if (sample->trip || sample->thermal_trip || sample->voltage_trip) {
            return bad(BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_TRIP, "ASIC 0x%02x telemetry trip is asserted", (unsigned) expected_id);
        }
        if (!bzm_telemetry_sample_is_fresh(sample, input->telemetry_now_us, input->telemetry_max_age_us)) {
            return bad(BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_STALE, "ASIC 0x%02x telemetry timestamp=%llu now=%llu maxAge=%llu us",
                       (unsigned) expected_id, (unsigned long long) sample->timestamp_us,
                       (unsigned long long) input->telemetry_now_us, (unsigned long long) input->telemetry_max_age_us);
        }
        bool safe_with_deferred_ch2 = input->defer_ch2_bounds && bzm_telemetry_sample_is_safe_except_ch2(
                                                                     sample, input->telemetry_now_us, input->telemetry_max_age_us,
                                                                     &input->telemetry_bounds, false);
        if ((!sample->valid || !sample->thermal_valid || !sample->voltage_valid) && !safe_with_deferred_ch2) {
            return bad(BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_INVALID, "ASIC 0x%02x telemetry validity failed", (unsigned) expected_id);
        }
        bool within_bounds = input->defer_ch2_bounds ? safe_with_deferred_ch2
                                                     : bzm_telemetry_sample_is_within_bounds(sample, &input->telemetry_bounds);
        if (!within_bounds) {
            return bad(BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_BOUNDS,
                       "ASIC 0x%02x telemetry outside qualified bounds: temp=%.1f C ch=[%.1f,%.1f,%.1f] mV", (unsigned) expected_id,
                       sample->temperature_c, sample->ch0_mv, sample->ch1_mv, sample->ch2_mv);
        }
        if (require_clock_locks && !input->defer_clock_locks && !sample->pll_locked) {
            return bad(BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_CLOCK_UNLOCKED, "ASIC 0x%02x combined PLL0/PLL1 telemetry lock is clear",
                       (unsigned) expected_id);
        }
    }
    return good(require_clock_locks ? "four fresh safe ASIC samples and all clock locks are GOOD"
                                    : "four fresh safe ASIC samples are GOOD");
}

bzm_runtime_health_result_t bzm_runtime_health_evaluate(const bzm_runtime_health_input_t * input)
{
    if (input == NULL) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT, "runtime health input is NULL");
    }
    if (!input->holding || input->reached_stage == BZM_STAGE_OFF_SAFE) {
        return good("no runtime health checks apply while OFF_SAFE or not holding");
    }
    if (input->reached_stage < BZM_STAGE_OFF_SAFE || input->reached_stage >= BZM_STAGE_COUNT) {
        return bad(BZM_RUNTIME_HEALTH_FAULT_INVALID_STAGE, "reached stage=%d is outside the validation schema",
                   (int) input->reached_stage);
    }

    bzm_runtime_health_result_t result = check_bridge_and_fan(input);
    if (result.status != BZM_RUNTIME_HEALTH_GOOD)
        return result;

    if (input->reached_stage >= BZM_STAGE_POWER_RAIL) {
        result = check_power(input);
        if (result.status != BZM_RUNTIME_HEALTH_GOOD)
            return result;
    }
    if (input->reached_stage >= BZM_STAGE_CHAIN_4) {
        result = check_parser(input);
        if (result.status != BZM_RUNTIME_HEALTH_GOOD)
            return result;
    }
    if (input->reached_stage >= BZM_STAGE_SENSORS) {
        result = check_telemetry(input);
        if (result.status != BZM_RUNTIME_HEALTH_GOOD)
            return result;
    }

    return health_result(BZM_RUNTIME_HEALTH_GOOD, BZM_RUNTIME_HEALTH_FAULT_NONE, "%s runtime health checks are GOOD",
                         bzm_validation_stage_name(input->reached_stage));
}

const char * bzm_runtime_health_status_name(bzm_runtime_health_status_t status)
{
    switch (status) {
    case BZM_RUNTIME_HEALTH_GOOD:
        return "GOOD";
    case BZM_RUNTIME_HEALTH_BAD:
        return "BAD";
    default:
        return "INVALID_STATUS";
    }
}

const char * bzm_runtime_health_fault_name(bzm_runtime_health_fault_t fault)
{
    switch (fault) {
    case BZM_RUNTIME_HEALTH_FAULT_NONE:
        return "NONE";
    case BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT:
        return "INVALID_INPUT";
    case BZM_RUNTIME_HEALTH_FAULT_INVALID_STAGE:
        return "INVALID_STAGE";
    case BZM_RUNTIME_HEALTH_FAULT_BRIDGE_UNAVAILABLE:
        return "BRIDGE_UNAVAILABLE";
    case BZM_RUNTIME_HEALTH_FAULT_BRIDGE_STATUS_INVALID:
        return "BRIDGE_STATUS_INVALID";
    case BZM_RUNTIME_HEALTH_FAULT_BRIDGE_CAPABILITY_MISSING:
        return "BRIDGE_CAPABILITY_MISSING";
    case BZM_RUNTIME_HEALTH_FAULT_BRIDGE_STATE:
        return "BRIDGE_STATE";
    case BZM_RUNTIME_HEALTH_FAULT_BRIDGE_FAULT:
        return "BRIDGE_FAULT";
    case BZM_RUNTIME_HEALTH_FAULT_BRIDGE_LEASE:
        return "BRIDGE_LEASE";
    case BZM_RUNTIME_HEALTH_FAULT_BRIDGE_OUTPUT:
        return "BRIDGE_OUTPUT";
    case BZM_RUNTIME_HEALTH_FAULT_FAN_NOT_FULL:
        return "FAN_NOT_FULL";
    case BZM_RUNTIME_HEALTH_FAULT_FAN_TACH_UNAVAILABLE:
        return "FAN_TACH_UNAVAILABLE";
    case BZM_RUNTIME_HEALTH_FAULT_FAN_TACH_LOW:
        return "FAN_TACH_LOW";
    case BZM_RUNTIME_HEALTH_FAULT_TPS_UNAVAILABLE:
        return "TPS_UNAVAILABLE";
    case BZM_RUNTIME_HEALTH_FAULT_TPS_PGOOD_LOW:
        return "TPS_PGOOD_LOW";
    case BZM_RUNTIME_HEALTH_FAULT_TPS_OPERATION_OFF:
        return "TPS_OPERATION_OFF";
    case BZM_RUNTIME_HEALTH_FAULT_TPS_STATUS:
        return "TPS_STATUS";
    case BZM_RUNTIME_HEALTH_FAULT_TPS_VIN_RANGE:
        return "TPS_VIN_RANGE";
    case BZM_RUNTIME_HEALTH_FAULT_TPS_VOUT_RANGE:
        return "TPS_VOUT_RANGE";
    case BZM_RUNTIME_HEALTH_FAULT_TPS_TEMPERATURE_RANGE:
        return "TPS_TEMPERATURE_RANGE";
    case BZM_RUNTIME_HEALTH_FAULT_TPS_IOUT_RANGE:
        return "TPS_IOUT_RANGE";
    case BZM_RUNTIME_HEALTH_FAULT_TPS_VOUT_COMMAND:
        return "TPS_VOUT_COMMAND";
    case BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_UNAVAILABLE:
        return "TELEMETRY_UNAVAILABLE";
    case BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_MISSING:
        return "TELEMETRY_MISSING";
    case BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_INVALID:
        return "TELEMETRY_INVALID";
    case BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_TRIP:
        return "TELEMETRY_TRIP";
    case BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_STALE:
        return "TELEMETRY_STALE";
    case BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_BOUNDS:
        return "TELEMETRY_BOUNDS";
    case BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_CLOCK_UNLOCKED:
        return "TELEMETRY_CLOCK_UNLOCKED";
    case BZM_RUNTIME_HEALTH_FAULT_PARSER_UNAVAILABLE:
        return "PARSER_UNAVAILABLE";
    case BZM_RUNTIME_HEALTH_FAULT_PARSER_COUNTER_REGRESSED:
        return "PARSER_COUNTER_REGRESSED";
    case BZM_RUNTIME_HEALTH_FAULT_PARSER_ERROR_DELTA:
        return "PARSER_ERROR_DELTA";
    default:
        return "INVALID_FAULT";
    }
}
