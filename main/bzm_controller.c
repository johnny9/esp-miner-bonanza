#include "bzm_controller.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "TPS546.h"
#include "asic_result_task.h"
#include "bzm_driver.h"
#include "bzm_lease_guard.h"
#include "bzm_power.h"
#include "bzm_running_evidence.h"
#include "bzm_runtime_health.h"
#include "create_jobs_task.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hashrate_monitor_task.h"
#include "protocol_coordinator.h"
#include "statistics_task.h"
#include "stratum_api.h"
#include "thermal.h"
#include "vcore.h"

#define BZM_MONITOR_PERIOD_MS 100U
#define BZM_HEALTH_PERIOD_MS 500U
#define BZM_SAFE_OFF_TIMEOUT_MS 1500U
#define BZM_SAFE_OFF_SAMPLE_MS 25U
#define BZM_CONTROLLER_WATCHDOG_MS 300000U

typedef struct
{
    pthread_mutex_t lock;
    GlobalState * global_state;
    bzm_supervisor_t supervisor;
    bzm_bridge_info_t bridge_info;
    bzm_bridge_safety_status_t bridge_status;
    bool active;
    bool initialized;
    bool monitor_running;
    bool bridge_info_valid;
    bool bridge_status_valid;
    bool mining_stack_ready;
    bool mining_tasks_started;
    bool protocol_coordinator_initialized;
    TaskHandle_t create_jobs_task_handle;
    TaskHandle_t asic_result_task_handle;
    TaskHandle_t hashrate_task_handle;
    TaskHandle_t statistics_task_handle;
    TaskHandle_t protocol_task_handle;
    atomic_bool dispatch_enabled;
    atomic_uint_fast64_t dispatch_deadline_ms;
    atomic_uint_fast64_t execution_deadline_ms;
    bool parser_baseline_valid;
    bzm_serial_parser_stats_t parser_baseline;
    bool parser_realign_valid;
    bzm_parser_realign_t parser_realign;
    bool health_valid;
    uint64_t health_sampled_at_ms;
    bzm_runtime_health_result_t health;
    bool tps_sample_valid;
    bzm_runtime_health_tps_sample_t tps_sample;
    bool parser_sample_valid;
    bzm_serial_parser_stats_t parser_sample;
    uint32_t parser_recovery_count;
    float board_temperature_c;
    bzm_ch2_confirmation_t ch2_confirmation;
    bzm_pll_lock_confirmation_t pll_lock_confirmation;
    bool running_evidence_requested;
    bool running_evidence_monitoring;
    uint64_t running_evidence_started_at_ms;
    bzm_running_stats_t running_evidence_baseline;
    bzm_running_evidence_lifecycle_t running_evidence_lifecycle;
    bzm_running_evidence_result_t running_evidence;
    uint16_t fan_rpm;
} bzm_runtime_state_t;

static const char * TAG = "bzm_controller";
static bool bridge_control_contract_compatible(
    const bzm_bridge_safety_status_t *status);
static bzm_runtime_state_t RUNTIME = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static bzm_bringup_telemetry_policy_t telemetry_policy(void);
static bool runtime_is_holding_locked(void);
static bzm_runtime_health_result_t sample_runtime_health_locked(void);
static void publish_driver_health_locked(asic_driver_lifecycle_t lifecycle);

static bzm_running_evidence_config_t running_evidence_config(void)
{
    return (bzm_running_evidence_config_t){
        .required_chip_engine_writes = BZM_ENGINES_PER_ASIC * BZM_BRINGUP_ASIC_COUNT,
        .minimum_valid_results = CONFIG_BZM_1002_MIN_VALID_RESULTS,
        .allow_mapping_recovery = true,
        .maximum_mapping_rejections = CONFIG_BZM_1002_MAX_MAPPING_REJECTIONS,
        .maximum_local_rejections = CONFIG_BZM_1002_MAX_LOCAL_REJECTIONS,
        .proof_timeout_ms = CONFIG_BZM_1002_PROOF_TIMEOUT_SECONDS * 1000U,
        .recovery_timeout_ms = CONFIG_BZM_1002_RESULT_RECOVERY_TIMEOUT_MS,
    };
}

static void reset_running_evidence_locked(bool requested)
{
    RUNTIME.running_evidence_requested = requested;
    RUNTIME.running_evidence_monitoring = false;
    RUNTIME.running_evidence_started_at_ms = 0;
    RUNTIME.running_evidence_baseline = (bzm_running_stats_t){0};
    bzm_running_evidence_lifecycle_init(&RUNTIME.running_evidence_lifecycle);
    RUNTIME.running_evidence = (bzm_running_evidence_result_t){
        .status = BZM_RUNNING_EVIDENCE_PENDING,
        .fault = BZM_RUNNING_EVIDENCE_FAULT_NONE,
    };
    snprintf(RUNTIME.running_evidence.detail, sizeof(RUNTIME.running_evidence.detail), "%s",
             requested ? "waiting for mining proof" : "mining proof is not active");
}

static bzm_running_evidence_result_t evaluate_running_evidence_locked(uint64_t current_ms)
{
    if (!RUNTIME.running_evidence_monitoring)
        return RUNTIME.running_evidence;

    bzm_running_stats_t current = {0};
    if (!BZM_running_stats_snapshot(&current)) {
        RUNTIME.running_evidence = (bzm_running_evidence_result_t){
            .status = BZM_RUNNING_EVIDENCE_BAD,
            .fault = BZM_RUNNING_EVIDENCE_FAULT_INVALID_CONFIGURATION,
        };
        snprintf(RUNTIME.running_evidence.detail, sizeof(RUNTIME.running_evidence.detail),
                 "mining driver evidence is unavailable");
        return RUNTIME.running_evidence;
    }
    bzm_running_evidence_config_t config = running_evidence_config();
    RUNTIME.running_evidence =
        bzm_running_evidence_track(&RUNTIME.running_evidence_lifecycle,
                                   &RUNTIME.running_evidence_baseline,
                                   &current, &config,
                                   RUNTIME.running_evidence_started_at_ms,
                                   current_ms);
    if (RUNTIME.running_evidence.status == BZM_RUNNING_EVIDENCE_GOOD && RUNTIME.parser_realign_valid &&
        RUNTIME.parser_realign.recovering) {
        snprintf(RUNTIME.running_evidence.detail, sizeof(RUNTIME.running_evidence.detail),
                 "proof retained; bounded parser realignment clean windows %u/%u",
                 (unsigned) RUNTIME.parser_realign.clean_windows, (unsigned) CONFIG_BZM_1002_PARSER_REALIGN_CLEAN_WINDOWS);
    }
    if (RUNTIME.running_evidence.status == BZM_RUNNING_EVIDENCE_GOOD) {
        snprintf(RUNTIME.supervisor.report.stages[BZM_STAGE_RUNNING].detail,
                 sizeof(RUNTIME.supervisor.report.stages[BZM_STAGE_RUNNING].detail), "RUNNING GOOD: %.140s",
                 RUNTIME.running_evidence.detail);
    } else if (RUNTIME.running_evidence.status == BZM_RUNNING_EVIDENCE_BAD) {
        RUNTIME.supervisor.report.stages[BZM_STAGE_RUNNING] =
            bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED, RUNTIME.running_evidence.detail);
    }
    return RUNTIME.running_evidence;
}

static bool runtime_restart_guard(void * context)
{
    (void) context;
    return bzm_controller_prepare_restart();
}

static uint64_t now_ms(void)
{
    return (uint64_t) (esp_timer_get_time() / 1000);
}

static asic_driver_lifecycle_t current_lifecycle_locked(void)
{
    if (RUNTIME.supervisor.fault_latched) return ASIC_DRIVER_FAULT;
    if (bzm_supervisor_owner_is_maintenance(RUNTIME.supervisor.owner)) {
        return ASIC_DRIVER_MAINTENANCE;
    }
    if (RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_MINING) {
        return ASIC_DRIVER_MINING;
    }
    if (RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_VALIDATION) {
        return ASIC_DRIVER_STARTING;
    }
    return ASIC_DRIVER_SAFE_OFF;
}

static void publish_driver_health_locked(asic_driver_lifecycle_t lifecycle)
{
    bzm_bringup_state_t bringup = {0};
    bzm_running_stats_t running = {0};
    (void)BZM_staged_get_state(&bringup);
    (void)BZM_running_stats_snapshot(&running);

    asic_driver_health_t health = {
        .available = true,
        .lifecycle = lifecycle,
        .asic_count = bringup.chain_verified ? BZM_BRINGUP_ASIC_COUNT : 0,
        .expected_asic_count = BZM_BRINGUP_ASIC_COUNT,
        .active_engine_count = bringup.balanced_ramp_verified
            ? BZM_ENGINES_PER_ASIC * BZM_BRINGUP_ASIC_COUNT : 0,
        .expected_engine_count = BZM_ENGINES_PER_ASIC * BZM_BRINGUP_ASIC_COUNT,
        .fixed_frequency_mhz = 800,
        .fixed_voltage_mv = 2800,
        .measured_voltage_v = RUNTIME.tps_sample_valid
            ? RUNTIME.tps_sample.vout_v : 0.0f,
        .board_temperature_c = RUNTIME.board_temperature_c,
        .fan_percent = RUNTIME.bridge_status_valid
            ? RUNTIME.bridge_status.fan_percent : 0,
        .fan_rpm = RUNTIME.fan_rpm,
        .bridge_protocol_major = RUNTIME.bridge_info_valid
            ? RUNTIME.bridge_info.protocol_major : 0,
        .bridge_protocol_minor = RUNTIME.bridge_info_valid
            ? RUNTIME.bridge_info.protocol_minor : 0,
        .bridge_compatible = RUNTIME.bridge_info_valid &&
            RUNTIME.bridge_status_valid &&
            bzm_bridge_info_supports_data_link(&RUNTIME.bridge_info) &&
            bridge_control_contract_compatible(&RUNTIME.bridge_status),
        .parser_discarded_bytes = RUNTIME.parser_sample_valid
            ? RUNTIME.parser_sample.discarded_bytes : 0,
        .parser_recoveries = RUNTIME.parser_recovery_count,
        .address_mark_realignments = RUNTIME.parser_sample_valid
            ? RUNTIME.parser_sample.address_mark_realignments : 0,
        .transport_crc_failures = RUNTIME.parser_sample_valid
            ? RUNTIME.parser_sample.transport_crc_failures : 0,
        .transport_sequence_gaps = RUNTIME.parser_sample_valid
            ? RUNTIME.parser_sample.transport_sequence_gaps : 0,
        .transport_duplicate_frames = RUNTIME.parser_sample_valid
            ? RUNTIME.parser_sample.transport_duplicate_frames : 0,
        .transport_discarded_wire_bytes = RUNTIME.parser_sample_valid
            ? RUNTIME.parser_sample.transport_discarded_wire_bytes : 0,
        .bridge_pio_fifo_overflows = RUNTIME.parser_sample_valid
            ? RUNTIME.parser_sample.bridge_pio_fifo_overflows : 0,
        .bridge_software_ring_overflows = RUNTIME.parser_sample_valid
            ? RUNTIME.parser_sample.bridge_software_ring_overflows : 0,
        .mapped_results = running.mapped_results,
        .locally_valid_results = running.locally_valid_results,
        .mapping_rejections = running.mapping_rejections,
        .local_rejections = running.locally_rejected_results,
        .duplicate_results = running.duplicate_results,
        .dispatch_failures = running.dispatch_failures,
    };
    if (RUNTIME.bridge_info_valid) {
        snprintf(health.bridge_version, sizeof(health.bridge_version), "%s",
                 RUNTIME.bridge_info.version);
    }
    if (RUNTIME.supervisor.fault_latched) {
        health.last_fault_code = RUNTIME.supervisor.fault_code;
        snprintf(health.last_fault, sizeof(health.last_fault), "%s",
                 RUNTIME.supervisor.fault_detail);
        health.user_action_required = true;
        snprintf(health.recommended_action,
                 sizeof(health.recommended_action),
                 "Restart the miner; if the fault returns, inspect power, fan, and bridge diagnostics");
    } else if (lifecycle == ASIC_DRIVER_MAINTENANCE) {
        snprintf(health.recommended_action,
                 sizeof(health.recommended_action),
                 "Wait for maintenance to finish before mining resumes");
    }
    BZM_driver_health_publish(&health);
}

static void close_dispatch_locked(void)
{
    atomic_store_explicit(&RUNTIME.dispatch_enabled, false, memory_order_release);
    atomic_store_explicit(&RUNTIME.dispatch_deadline_ms, 0, memory_order_release);
}

static bool runtime_dispatch_authorizer(void * context)
{
    bzm_runtime_state_t * runtime = context;
    if (runtime == NULL || !atomic_load_explicit(&runtime->dispatch_enabled, memory_order_acquire)) {
        return false;
    }
    uint64_t deadline = atomic_load_explicit(&runtime->dispatch_deadline_ms, memory_order_acquire);
    return deadline != 0 && now_ms() < deadline;
}

static bool runtime_execution_authorizer(void * context)
{
    bzm_runtime_state_t * runtime = context;
    if (runtime == NULL) {
        return false;
    }
    uint64_t deadline = atomic_load_explicit(&runtime->execution_deadline_ms, memory_order_acquire);
    return bzm_lease_guard_deadline_allows(deadline, now_ms());
}

static void sync_dispatch_locked(void)
{
    uint64_t current_ms = now_ms();
    if (!bzm_supervisor_dispatch_allowed(&RUNTIME.supervisor, current_ms)) {
        close_dispatch_locked();
        publish_driver_health_locked(current_lifecycle_locked());
        return;
    }

    /* Publish the deadline before opening the gate. The driver re-evaluates
     * this callback before dispatch and before every engine write. */
    atomic_store_explicit(&RUNTIME.dispatch_deadline_ms, RUNTIME.supervisor.lease_deadline_ms, memory_order_release);
    atomic_store_explicit(&RUNTIME.dispatch_enabled, true, memory_order_release);
    publish_driver_health_locked(current_lifecycle_locked());
}

static bool start_mining_tasks_locked(void)
{
    GlobalState * state = RUNTIME.global_state;
    if (state == NULL || !RUNTIME.mining_stack_ready)
        return false;

    /* Every task starts while ASIC_initalized is false and the independent
     * dispatch gate is closed. Partial creation is safe and retryable. */
    if (RUNTIME.create_jobs_task_handle == NULL &&
        xTaskCreate(create_jobs_task, "stratum miner", 8192, state, 20, &RUNTIME.create_jobs_task_handle) != pdPASS) {
        return false;
    }
    if (RUNTIME.asic_result_task_handle == NULL &&
        xTaskCreate(ASIC_result_task, "asic result", 8192, state, 15, &RUNTIME.asic_result_task_handle) != pdPASS) {
        return false;
    }
    if (RUNTIME.hashrate_task_handle == NULL && xTaskCreateWithCaps(hashrate_monitor_task, "hashrate monitor", 8192, state, 5,
                                                                    &RUNTIME.hashrate_task_handle, MALLOC_CAP_SPIRAM) != pdPASS) {
        return false;
    }
    if (RUNTIME.statistics_task_handle == NULL &&
        xTaskCreateWithCaps(statistics_task, "statistics", 8192, state, 3, &RUNTIME.statistics_task_handle, MALLOC_CAP_SPIRAM) !=
            pdPASS) {
        return false;
    }
    if (!RUNTIME.protocol_coordinator_initialized) {
        protocol_coordinator_init(state);
        RUNTIME.protocol_coordinator_initialized = true;
    }
    if (RUNTIME.protocol_task_handle == NULL && xTaskCreateWithCaps(protocol_coordinator_task, "protocol coord", 3072, state, 5,
                                                                    &RUNTIME.protocol_task_handle, MALLOC_CAP_SPIRAM) != pdPASS) {
        return false;
    }
    RUNTIME.mining_tasks_started = true;
    return true;
}

static bool start_production_mining_locked(void)
{
    if (!RUNTIME.initialized || !RUNTIME.mining_stack_ready ||
        RUNTIME.supervisor.owner != BZM_SUPERVISOR_OWNER_NONE ||
        RUNTIME.supervisor.fault_latched) {
        return false;
    }

    close_dispatch_locked();
    publish_driver_health_locked(ASIC_DRIVER_STARTING);
    reset_running_evidence_locked(true);
    bzm_ch2_confirmation_init(&RUNTIME.ch2_confirmation);
    bzm_pll_lock_confirmation_init(&RUNTIME.pll_lock_confirmation);

    const uint32_t lease_ms = RUNTIME.supervisor.config.maximum_lease_ms;
    const uint64_t started_ms = now_ms();
    uint64_t execution_deadline_ms = 0;
    if (!bzm_lease_guard_make_deadline(started_ms, lease_ms,
                                       &execution_deadline_ms)) {
        return false;
    }
    atomic_store_explicit(&RUNTIME.execution_deadline_ms,
                          execution_deadline_ms, memory_order_release);
    /* The local controller is the production authority for this locked
     * profile. The fresh arm is internal and cannot be supplied remotely. */
    bool completed = bzm_supervisor_request_validation(
        &RUNTIME.supervisor, BZM_STAGE_RUNNING, true, true, lease_ms,
        started_ms);
    atomic_store_explicit(&RUNTIME.execution_deadline_ms, 0,
                          memory_order_release);

    if (completed && runtime_is_holding_locked()) {
        bzm_runtime_health_result_t health = sample_runtime_health_locked();
        if (health.status == BZM_RUNTIME_HEALTH_BAD) {
            close_dispatch_locked();
            (void)bzm_supervisor_latch_fault(
                &RUNTIME.supervisor, (uint32_t)health.fault, health.detail);
            completed = false;
        }
    }
    if (!completed ||
        RUNTIME.supervisor.owner != BZM_SUPERVISOR_OWNER_MINING ||
        !start_mining_tasks_locked()) {
        if (!RUNTIME.supervisor.fault_latched) {
            (void)bzm_supervisor_latch_fault(
                &RUNTIME.supervisor, 0x1006,
                "production mining task stack could not start");
        }
        sync_dispatch_locked();
        return false;
    }

    RUNTIME.running_evidence_started_at_ms = now_ms();
    RUNTIME.running_evidence_monitoring = true;
    RUNTIME.global_state->ASIC_initalized = true;
    RUNTIME.global_state->SYSTEM_MODULE.mining_paused = false;
    (void)evaluate_running_evidence_locked(
        RUNTIME.running_evidence_started_at_ms);
    sync_dispatch_locked();
    return true;
}

static bool runtime_is_holding_locked(void)
{
    return RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_VALIDATION || RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_MINING;
}

static bzm_runtime_health_result_t sample_runtime_health_locked(void)
{
    bzm_runtime_health_input_t input = {
        .reached_stage = RUNTIME.supervisor.report.reached_stage,
        .holding = runtime_is_holding_locked(),
        .bridge_status_available = RUNTIME.bridge_status_valid,
        .bridge_status = RUNTIME.bridge_status,
        .require_independent_kill =
            RUNTIME.supervisor.config.production_mode && !RUNTIME.supervisor.config.board_managed_safety &&
            RUNTIME.supervisor.report.reached_stage >= BZM_STAGE_POWER_RAIL,
        .fan_min_rpm = CONFIG_BZM_1002_FAN_MIN_RPM,
        .tps_bounds =
            {
                .vin_min_v = BZM_TPS546_BIRDS_PROFILE.vin_off,
                .vin_max_v = BZM_TPS546_BIRDS_PROFILE.vin_ov_fault_limit,
                .vout_command_v = BZM_TPS546_FIXED_VOUT_V,
                .vout_command_tolerance_v = BZM_TPS546_VOUT_READBACK_TOLERANCE_V,
                .vout_min_v = 2.65f,
                .vout_max_v = 2.95f,
                .iout_min_a = -1.0f,
                .iout_max_a = BZM_TPS546_BIRDS_PROFILE.iout_oc_warn_limit,
                .temperature_min_c = -40.0f,
                .temperature_max_c = (float) BZM_TPS546_BIRDS_PROFILE.ot_warn_limit - 0.1f,
            },
        .telemetry_bounds = telemetry_policy().bounds,
        .telemetry_now_us = (uint64_t) esp_timer_get_time(),
        .telemetry_max_age_us = telemetry_policy().max_age_us,
        .defer_ch2_bounds = true,
        .defer_clock_locks = true,
        .parser_stats_available = RUNTIME.parser_baseline_valid,
        .parser_baseline = RUNTIME.parser_baseline,
    };

    if (input.holding) {
        bzm_bridge_safety_status_t status;
        input.bridge_status_available = BZM_bridge_safety_heartbeat(&status) == ESP_OK && status.valid;
        if (input.bridge_status_available) {
            input.bridge_status = status;
            RUNTIME.bridge_status = status;
            RUNTIME.bridge_status_valid = true;
        } else {
            RUNTIME.bridge_status_valid = false;
        }
        input.fan_tach_available = BZM_bridge_get_fan_rpm(&input.fan_rpm) == ESP_OK;
        if (input.fan_tach_available)
            RUNTIME.fan_rpm = input.fan_rpm;
    }

    if (input.holding && input.reached_stage >= BZM_STAGE_POWER_RAIL) {
        TPS546_StatusSnapshot power = {0};
        bool pgood = false;
        input.tps.available = VCORE_bzm_snapshot(&power, &pgood) == ESP_OK;
        if (input.tps.available) {
            input.tps.pgood = pgood;
            input.tps.operation = power.operation;
            input.tps.status_word = power.status_word;
            input.tps.vout_command_v = power.vout_command;
            input.tps.vout_command_matches_expected = power.vout_command_matches_active_config;
            input.tps.vin_v = power.read_vin;
            input.tps.vout_v = power.read_vout;
            input.tps.iout_a = power.read_iout;
            input.tps.temperature_c = power.read_temp1;
        }
        RUNTIME.tps_sample = input.tps;
        RUNTIME.tps_sample_valid = input.tps.available;
    }

    if (input.holding && input.reached_stage >= BZM_STAGE_CHAIN_4) {
        (void) BZM_staged_poll(25);
        input.parser_stats_available = RUNTIME.parser_baseline_valid && BZM_get_parser_stats(&input.parser_current);
    }

    bzm_parser_realign_result_t parser_realign_result = BZM_PARSER_REALIGN_CLEAN;
    uint32_t parser_realign_discarded = 0;
    uint32_t parser_realign_unexpected_registers = 0;
    if (input.holding && input.reached_stage == BZM_STAGE_RUNNING && input.parser_stats_available && RUNTIME.parser_realign_valid) {
        parser_realign_result = bzm_parser_realign_observe(
            &RUNTIME.parser_realign, &input.parser_current, CONFIG_BZM_1002_PARSER_REALIGN_MAX_DISCARDS,
            CONFIG_BZM_1002_PARSER_REALIGN_CLEAN_WINDOWS, CONFIG_BZM_1002_PARSER_REALIGN_MAX_WINDOWS,
            CONFIG_BZM_1002_PARSER_REALIGN_MAX_EVENTS);
        if (RUNTIME.parser_realign.recovering || parser_realign_result == BZM_PARSER_REALIGN_RECOVERED) {
            parser_realign_discarded = input.parser_current.discarded_bytes - RUNTIME.parser_realign.burst_discard_baseline;
            parser_realign_unexpected_registers = input.parser_current.unexpected_register_headers -
                                                  RUNTIME.parser_realign.burst_unexpected_register_baseline;
        }
        if (parser_realign_result == BZM_PARSER_REALIGN_PENDING || parser_realign_result == BZM_PARSER_REALIGN_RECOVERED) {
            /* The realignment state machine proved that every rejected
             * register-header increment accompanied discarded bytes and that
             * every other parser counter remained exact. Suppress only this
             * bounded realignment episode while independent checks continue. */
            input.parser_baseline.discarded_bytes = input.parser_current.discarded_bytes;
            input.parser_baseline.unexpected_register_headers = input.parser_current.unexpected_register_headers;
        }
        if (parser_realign_result == BZM_PARSER_REALIGN_RECOVERED) {
            RUNTIME.parser_baseline = RUNTIME.parser_realign.accepted;
            RUNTIME.parser_recovery_count++;
            ESP_LOGW(TAG, "Mining parser realigned after %lu discarded bytes and %lu rejected register headers; valid frame resumed and %u clean windows passed",
                     (unsigned long) parser_realign_discarded, (unsigned long) parser_realign_unexpected_registers,
                     (unsigned) CONFIG_BZM_1002_PARSER_REALIGN_CLEAN_WINDOWS);
        }
    }

    if (input.holding && input.reached_stage >= BZM_STAGE_SENSORS) {
        input.telemetry_available = BZM_get_telemetry_snapshot(&input.telemetry);
        /* Timestamp the completed snapshot, not the start of the copy. The
         * transport can publish a fresh sample while this task is waiting for
         * its mutex; taking `now` first made that valid sample appear a few
         * hundred microseconds in the future and falsely latched safe-off. */
        input.telemetry_now_us = (uint64_t) esp_timer_get_time();
        if (input.telemetry_available) {
            float temperature_sum = 0.0f;
            uint8_t temperature_count = 0;
            for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
                const bzm_telemetry_sample_t *sample =
                    bzm_telemetry_store_get(&input.telemetry,
                                            bzm_asic_wire_ids[index]);
                if (sample != NULL && isfinite(sample->temperature_c)) {
                    temperature_sum += sample->temperature_c;
                    temperature_count++;
                }
            }
            if (temperature_count != 0) {
                RUNTIME.board_temperature_c =
                    temperature_sum / temperature_count;
            }
        }
    }

    RUNTIME.parser_sample = input.parser_current;
    RUNTIME.parser_sample_valid = input.parser_stats_available;

    RUNTIME.health = bzm_runtime_health_evaluate(&input);
    if (RUNTIME.health.status == BZM_RUNTIME_HEALTH_GOOD && parser_realign_result == BZM_PARSER_REALIGN_PENDING) {
        snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                 "Mining parser realignment pending: discarded=%lu/%u rejectedHeaders=%lu bursts=%u/%u cleanWindows=%u/%u windows=%u/%u",
                 (unsigned long) parser_realign_discarded, (unsigned) CONFIG_BZM_1002_PARSER_REALIGN_MAX_DISCARDS,
                 (unsigned long) parser_realign_unexpected_registers,
                 (unsigned) RUNTIME.parser_realign.episode_bursts,
                 (unsigned) CONFIG_BZM_1002_PARSER_REALIGN_MAX_EVENTS,
                 (unsigned) RUNTIME.parser_realign.clean_windows, (unsigned) CONFIG_BZM_1002_PARSER_REALIGN_CLEAN_WINDOWS,
                 (unsigned) RUNTIME.parser_realign.observed_windows, (unsigned) CONFIG_BZM_1002_PARSER_REALIGN_MAX_WINDOWS);
    } else if (RUNTIME.health.status == BZM_RUNTIME_HEALTH_GOOD && parser_realign_result == BZM_PARSER_REALIGN_RECOVERED) {
        snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                 "Mining parser realigned: discarded=%lu rejectedHeaders=%lu valid frame resumed cleanWindows=%u",
                 (unsigned long) parser_realign_discarded, (unsigned long) parser_realign_unexpected_registers,
                 (unsigned) RUNTIME.parser_realign.clean_windows);
    }
    if (RUNTIME.health.status == BZM_RUNTIME_HEALTH_GOOD && input.holding && input.reached_stage >= BZM_STAGE_CLOCKS &&
        input.telemetry_available) {
        uint8_t culprit_asic_id = 0;
        uint8_t observed_samples = 0;
        bzm_ch2_confirmation_result_t confirmation = bzm_pll_lock_confirmation_observe(
            &RUNTIME.pll_lock_confirmation, &input.telemetry, input.telemetry_now_us, input.telemetry_max_age_us,
            CONFIG_BZM_1002_PLL_LOCK_CONFIRM_SAMPLES, &culprit_asic_id, &observed_samples);
        if (confirmation == BZM_CH2_CONFIRMATION_CONTINUOUS || confirmation == BZM_CH2_CONFIRMATION_INVALID) {
            RUNTIME.health.status = BZM_RUNTIME_HEALTH_BAD;
            RUNTIME.health.fault = BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_CLOCK_UNLOCKED;
            if (confirmation == BZM_CH2_CONFIRMATION_CONTINUOUS) {
                snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                         "ASIC 0x%02x combined PLL lock clear continuously for %u/%u fresh samples",
                         (unsigned) culprit_asic_id, (unsigned) observed_samples,
                         (unsigned) CONFIG_BZM_1002_PLL_LOCK_CONFIRM_SAMPLES);
            } else {
                snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                         "combined PLL lock confirmation input is invalid");
            }
        } else if (observed_samples != 0) {
            RUNTIME.health.fault = BZM_RUNTIME_HEALTH_FAULT_NONE;
            snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                     confirmation == BZM_CH2_CONFIRMATION_NO_NEW_SAMPLE
                         ? "ASIC 0x%02x combined PLL lock clear pending a fresh sample at %u/%u"
                         : "ASIC 0x%02x combined PLL lock clear pending confirmation at %u/%u",
                     (unsigned) culprit_asic_id, (unsigned) observed_samples,
                     (unsigned) CONFIG_BZM_1002_PLL_LOCK_CONFIRM_SAMPLES);
        }
    }
    if (RUNTIME.health.status == BZM_RUNTIME_HEALTH_GOOD && input.holding && input.reached_stage >= BZM_STAGE_SENSORS &&
        input.telemetry_available) {
        uint8_t culprit_asic_id = 0;
        uint8_t observed_samples = 0;
        bzm_ch2_confirmation_result_t confirmation =
            bzm_ch2_confirmation_observe(&RUNTIME.ch2_confirmation, &input.telemetry, &input.telemetry_bounds,
                                         CONFIG_BZM_1002_CH2_CONFIRM_SAMPLES, &culprit_asic_id, &observed_samples);
        if (confirmation == BZM_CH2_CONFIRMATION_CONTINUOUS || confirmation == BZM_CH2_CONFIRMATION_INVALID) {
            RUNTIME.health.status = BZM_RUNTIME_HEALTH_BAD;
            RUNTIME.health.fault = BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_BOUNDS;
            if (confirmation == BZM_CH2_CONFIRMATION_CONTINUOUS) {
                const bzm_telemetry_sample_t * sample = bzm_telemetry_store_get(&input.telemetry, culprit_asic_id);
                snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                         "ASIC 0x%02x CH2 excursion continuous for %u/%u fresh samples: %.1f mV limit=+/-%.1f mV",
                         (unsigned) culprit_asic_id, (unsigned) observed_samples, (unsigned) CONFIG_BZM_1002_CH2_CONFIRM_SAMPLES,
                         sample != NULL ? sample->ch2_mv : NAN, input.telemetry_bounds.ch2_abs_max_mv);
            } else {
                snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail), "CH2 confirmation input is invalid");
            }
        } else if (observed_samples != 0) {
            RUNTIME.health.fault = BZM_RUNTIME_HEALTH_FAULT_NONE;
            snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail),
                     confirmation == BZM_CH2_CONFIRMATION_NO_NEW_SAMPLE
                         ? "ASIC 0x%02x CH2 excursion pending a fresh sample at %u/%u"
                         : "ASIC 0x%02x CH2 excursion pending confirmation at %u/%u",
                     (unsigned) culprit_asic_id, (unsigned) observed_samples, (unsigned) CONFIG_BZM_1002_CH2_CONFIRM_SAMPLES);
        }
    }
    RUNTIME.health_valid = true;
    RUNTIME.health_sampled_at_ms = now_ms();
    publish_driver_health_locked(current_lifecycle_locked());
    return RUNTIME.health;
}

static bool bridge_status_runtime_good(const bzm_bridge_safety_status_t * status)
{
    return status != NULL && status->valid && status->fault == BZM_BRIDGE_SAFETY_FAULT_NONE && !status->trip_input_asserted &&
           (status->runtime_verdict == BZM_BRIDGE_SAFETY_RUNTIME_GOOD_SAFE_OFF ||
            status->runtime_verdict == BZM_BRIDGE_SAFETY_RUNTIME_GOOD_CONTROLLED);
}

static bool bridge_control_contract_compatible(
    const bzm_bridge_safety_status_t *status)
{
    const uint16_t required = BZM_BRIDGE_SAFETY_CAP_5V_CONTROL |
                              BZM_BRIDGE_SAFETY_CAP_ASIC_RESET_CONTROL |
                              BZM_BRIDGE_SAFETY_CAP_FAN_FORCE_FULL |
                              BZM_BRIDGE_SAFETY_CAP_TRIP_INPUT_SAMPLED;
    return bridge_status_runtime_good(status) &&
           status->stage == BZM_BRIDGE_SAFETY_STAGE_TRIP_LATCH &&
           (status->capabilities & required) == required;
}

static bool bridge_has_independent_kill(const bzm_bridge_safety_status_t * status)
{
    const uint16_t required = BZM_BRIDGE_SAFETY_CAP_CORE_POWER_CUTOFF | BZM_BRIDGE_SAFETY_CAP_FAN_TACH_INTERLOCK |
                              BZM_BRIDGE_SAFETY_CAP_INDEPENDENT_TRIP_MONITOR;
    return bridge_status_runtime_good(status) && status->stage == BZM_BRIDGE_SAFETY_STAGE_TRIP_LATCH &&
           status->production_verdict == BZM_BRIDGE_SAFETY_PRODUCTION_GOOD && (status->capabilities & required) == required &&
           (status->evidence & required) == required;
}

static void refresh_bridge_evidence_locked(void)
{
    RUNTIME.bridge_info_valid =
        BZM_bridge_get_info(&RUNTIME.bridge_info) == ESP_OK && bzm_bridge_info_supports_data_link(&RUNTIME.bridge_info);
    RUNTIME.bridge_status_valid =
        RUNTIME.bridge_info_valid && BZM_bridge_get_safety_status(&RUNTIME.bridge_status) == ESP_OK && RUNTIME.bridge_status.valid;
    RUNTIME.supervisor.config.independent_kill_available =
        RUNTIME.bridge_status_valid && bridge_has_independent_kill(&RUNTIME.bridge_status);
}

static bzm_stage_result_t runtime_force_safe_off(void * context)
{
    GlobalState * state = context;
    bool commands_ok = state != NULL;
    bzm_bridge_safety_status_t status;

    close_dispatch_locked();
    RUNTIME.running_evidence_monitoring = false;
    RUNTIME.parser_baseline_valid = false;
    RUNTIME.parser_realign_valid = false;

    if (state == NULL) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_SAFE_OFF_FAILED,
                                     "global state is unavailable; shutdown cannot be verified");
    }

    state->SYSTEM_MODULE.mining_paused = true;
    state->ASIC_initalized = false;
    (void) BZM_staged_hold_reset();
    BZM_staged_set_dispatch_authorizer(runtime_dispatch_authorizer, &RUNTIME);

    /* Disarm is best-effort for recovery from pre-1.1 bridge firmware. */
    (void) BZM_bridge_disarm_safety(&status);
    commands_ok = BZM_bridge_set_asic_reset(false) == ESP_OK && commands_ok;
    commands_ok = BZM_bridge_set_5v_enabled(false) == ESP_OK && commands_ok;
    commands_ok = Thermal_set_fan_percent(&state->DEVICE_CONFIG, 1.0f) == ESP_OK && commands_ok;
    commands_ok = VCORE_bzm_set_rail_enabled(state, false) == ESP_OK && commands_ok;

    TPS546_StatusSnapshot power = {0};
    bool pgood = true;
    bool electrical_safe = false;
    for (uint32_t waited = 0; waited <= BZM_SAFE_OFF_TIMEOUT_MS; waited += BZM_SAFE_OFF_SAMPLE_MS) {
        if (VCORE_bzm_snapshot(&power, &pgood) == ESP_OK && !pgood && (power.operation & OPERATION_ON) == 0 &&
            power.read_vout * 1000.0f <= (float) CONFIG_BZM_1002_SAFE_OFF_VCORE_MV) {
            electrical_safe = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(BZM_SAFE_OFF_SAMPLE_MS));
    }

    RUNTIME.bridge_status_valid = BZM_bridge_get_safety_status(&RUNTIME.bridge_status) == ESP_OK && RUNTIME.bridge_status.valid;
    if (electrical_safe && RUNTIME.bridge_status_valid &&
        RUNTIME.bridge_status.fault != BZM_BRIDGE_SAFETY_FAULT_NONE &&
        bzm_bridge_safety_status_allows_fault_clear(&RUNTIME.bridge_status)) {
        /* The RP2040 intentionally retains lease/trip faults across an ESP
         * reboot. Clear that latch only after this boot has independently
         * forced safe bridge outputs and proved the TPS rail off and
         * discharged, then evaluate the returned status from scratch. */
        bzm_bridge_safety_status_t cleared = {0};
        esp_err_t clear_err = BZM_bridge_clear_safety_fault(&cleared);
        commands_ok = clear_err == ESP_OK && commands_ok;
        RUNTIME.bridge_status = cleared;
        RUNTIME.bridge_status_valid = clear_err == ESP_OK && cleared.valid;
        if (RUNTIME.bridge_status_valid) {
            ESP_LOGW(TAG, "Cleared retained bridge safety fault after verified electrical safe-off");
        } else {
            ESP_LOGE(TAG, "Retained bridge safety fault clear failed: %s", esp_err_to_name(clear_err));
        }
    }
    const uint16_t required_safe_evidence =
        BZM_BRIDGE_SAFETY_EVIDENCE_OUTPUTS_SAFE | BZM_BRIDGE_SAFETY_EVIDENCE_TRIP_CLEAR | BZM_BRIDGE_SAFETY_EVIDENCE_FAULT_CLEAR;
    bool bridge_safe = RUNTIME.bridge_status_valid && RUNTIME.bridge_status.state == BZM_BRIDGE_SAFETY_STATE_SAFE_OFF &&
                       RUNTIME.bridge_status.fault == BZM_BRIDGE_SAFETY_FAULT_NONE &&
                       RUNTIME.bridge_status.runtime_verdict == BZM_BRIDGE_SAFETY_RUNTIME_GOOD_SAFE_OFF &&
                       RUNTIME.bridge_status.lease_remaining_ms == 0 && !RUNTIME.bridge_status.five_volt_enabled &&
                       RUNTIME.bridge_status.asic_reset_asserted && RUNTIME.bridge_status.fan_full &&
                       RUNTIME.bridge_status.fan_percent == 100 && !RUNTIME.bridge_status.trip_input_asserted &&
                       (RUNTIME.bridge_status.evidence & required_safe_evidence) == required_safe_evidence;

    if (!commands_ok || !electrical_safe || !bridge_safe) {
        char detail[BZM_VALIDATION_DETAIL_LENGTH];
        snprintf(detail, sizeof(detail),
                 "safe-off failed: commands=%s pgood=%s vout=%.3fV "
                 "operation=0x%02x bridge=%s",
                 commands_ok ? "ok" : "bad", pgood ? "high" : "low", power.read_vout, power.operation,
                 bridge_safe ? "safe" : (RUNTIME.bridge_status_valid ? "unsafe" : "unavailable"));
        ESP_LOGE(TAG, "%s", detail);
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_SAFE_OFF_FAILED, detail);
    }

    return bzm_validation_result(BZM_CHECK_GOOD, BZM_VALIDATION_CODE_STAGE_OK,
                                 "reset asserted, 5 V and TPS off, fan full, PGOOD low, "
                                 "VCORE discharged, bridge safety readback coherent");
}

static bzm_stage_result_t run_controls(GlobalState * state)
{
    bzm_bridge_safety_status_t status;
    if (BZM_bridge_get_info(&RUNTIME.bridge_info) != ESP_OK || !bzm_bridge_info_supports_data_link(&RUNTIME.bridge_info)) {
        RUNTIME.bridge_info_valid = false;
        return bzm_validation_result(BZM_CHECK_BLOCKED, BZM_VALIDATION_CODE_NOT_IMPLEMENTED,
                                     "bridge protocol 1.2 safety and protected data transport are required");
    }
    RUNTIME.bridge_info_valid = true;

    if (BZM_bridge_get_safety_status(&status) != ESP_OK || !status.valid ||
        status.state != BZM_BRIDGE_SAFETY_STATE_SAFE_OFF ||
        !bridge_control_contract_compatible(&status)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "bridge is unsafe, incompatible, or lacks required lease/trip control paths");
    }
    if (BZM_bridge_arm_safety(&status) != ESP_OK || status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED ||
        status.lease_remaining_ms == 0 || BZM_bridge_safety_heartbeat(&status) != ESP_OK ||
        status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED || status.lease_remaining_ms == 0 ||
        !bridge_status_runtime_good(&status)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED, "bridge arm/heartbeat lease proof failed");
    }
    if (Thermal_set_fan_percent(&state->DEVICE_CONFIG, 1.0f) != ESP_OK || BZM_bridge_get_fan_rpm(&RUNTIME.fan_rpm) != ESP_OK ||
        RUNTIME.fan_rpm < CONFIG_BZM_1002_FAN_MIN_RPM) {
        (void) BZM_bridge_disarm_safety(&status);
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "fan full-speed command or fresh tach threshold failed");
    }
    if (BZM_bridge_disarm_safety(&status) != ESP_OK || !status.valid || status.state != BZM_BRIDGE_SAFETY_STATE_SAFE_OFF ||
        !status.asic_reset_asserted || status.five_volt_enabled || !status.fan_full) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "bridge did not return to coherent safe outputs after lease proof");
    }
    RUNTIME.bridge_status = status;
    RUNTIME.bridge_status_valid = true;
    return bzm_validation_result(BZM_CHECK_GOOD, BZM_VALIDATION_CODE_STAGE_OK,
                                 "protocol 1.1, safety status, lease heartbeat, trip-clear state and fan tach are GOOD");
}

static bzm_stage_result_t run_power_rail(GlobalState * state)
{
    bzm_bridge_safety_status_t status;
    if (BZM_bridge_arm_safety(&status) != ESP_OK || !status.valid || status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED ||
        status.lease_remaining_ms == 0 || status.five_volt_enabled || !status.asic_reset_asserted || !status.fan_full ||
        !bridge_status_runtime_good(&status)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "bridge lease could not be armed with reset asserted, 5 V off and fan full");
    }
    if (VCORE_bzm_set_rail_enabled(state, true) != ESP_OK) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "TPS rail enable or live power validation failed");
    }

    char profile_detail[128];
    if (TPS546_verify_active_config(profile_detail, sizeof(profile_detail)) != ESP_OK) {
        char detail[BZM_VALIDATION_DETAIL_LENGTH];
        snprintf(detail, sizeof(detail), "TPS profile readback mismatch: %s", profile_detail);
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED, detail);
    }

    TPS546_StatusSnapshot power;
    bool pgood = false;
    if (VCORE_bzm_snapshot(&power, &pgood) != ESP_OK || !pgood || (power.operation & OPERATION_ON) == 0 ||
        !power.vout_command_matches_active_config || !isfinite(power.vout_command) ||
        fabsf(power.vout_command - BZM_TPS546_FIXED_VOUT_V) > BZM_TPS546_VOUT_READBACK_TOLERANCE_V ||
        !isfinite(power.read_vin) || !isfinite(power.read_vout) || !isfinite(power.read_iout) ||
        power.read_vin < BZM_TPS546_BIRDS_PROFILE.vin_off || power.read_vout < 2.65f || power.read_vout > 2.95f ||
        power.read_iout < -1.0f || power.read_iout > BZM_TPS546_BIRDS_PROFILE.iout_oc_warn_limit ||
        power.read_temp1 >= BZM_TPS546_BIRDS_PROFILE.ot_warn_limit || power.status_word != 0) {
        char detail[BZM_VALIDATION_DETAIL_LENGTH];
        snprintf(detail, sizeof(detail),
                 "TPS stage-2 bad: PGOOD=%u OP=0x%02x STATUS=0x%04x CMD=%.3fV RAW=0x%04x EXACT=%u VIN=%.2fV VOUT=%.3fV IOUT=%.2fA TEMP=%dC",
                 (unsigned) pgood, (unsigned) power.operation, (unsigned) power.status_word,
                 power.vout_command, (unsigned) power.vout_command_raw,
                 (unsigned) power.vout_command_matches_active_config, power.read_vin,
                 power.read_vout, power.read_iout, power.read_temp1);
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED, detail);
    }
    if (BZM_bridge_get_safety_status(&status) != ESP_OK || !status.valid || status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED ||
        status.five_volt_enabled || !status.asic_reset_asserted || !status.fan_full || !bridge_status_runtime_good(&status)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "bridge outputs changed unexpectedly while the TPS rail was validated");
    }
    RUNTIME.bridge_status = status;
    RUNTIME.bridge_status_valid = true;
    return bzm_validation_result(
        BZM_CHECK_GOOD, BZM_VALIDATION_CODE_STAGE_OK,
        "TPS command is locked at 2.8 V with PGOOD and fresh telemetry; reset remains asserted and bridge 5 V remains off");
}

static bzm_bringup_telemetry_policy_t telemetry_policy(void)
{
    return (bzm_bringup_telemetry_policy_t){
        .bounds =
            {
                .temperature_min_c = (float) CONFIG_BZM_1002_TEMP_MIN_C,
                .temperature_max_c = (float) CONFIG_BZM_1002_TEMP_MAX_C,
                .ch0_min_mv = (float) CONFIG_BZM_1002_STACK_MV_MIN,
                .ch0_max_mv = (float) CONFIG_BZM_1002_STACK_MV_MAX,
                .ch1_min_mv = (float) CONFIG_BZM_1002_STACK_MV_MIN,
                .ch1_max_mv = (float) CONFIG_BZM_1002_STACK_MV_MAX,
                .ch2_abs_max_mv = (float) CONFIG_BZM_1002_INTERSTACK_DIFF_ABS_MAX_MV,
                .max_stack_spread_mv = (float) CONFIG_BZM_1002_STACK_MAX_SPREAD_MV,
            },
        .max_age_us = (uint64_t) CONFIG_BZM_1002_TELEMETRY_MAX_AGE_MS * 1000U,
        .ch2_confirm_samples = CONFIG_BZM_1002_CH2_CONFIRM_SAMPLES,
    };
}

static bzm_stage_result_t bringup_stage_result(const char * stage_name, bzm_bringup_outcome_t outcome,
                                               const bzm_bringup_report_t * report)
{
    char detail[BZM_VALIDATION_DETAIL_LENGTH];
    snprintf(detail, sizeof(detail),
             "%s %s: reason=%s asic=0x%02x pll=%u reg=0x%02x "
             "expected=%lu actual=%lu completed=%u",
             stage_name, bzm_bringup_outcome_name(outcome),
             report != NULL ? bzm_bringup_reason_name(report->reason) : "missing_report", report != NULL ? report->asic_id : 0,
             report != NULL ? report->pll_index : 0, report != NULL ? report->register_offset : 0,
             (unsigned long) (report != NULL ? report->expected : 0), (unsigned long) (report != NULL ? report->actual : 0),
             report != NULL ? report->completed_items : 0);
    if (outcome == BZM_BRINGUP_GOOD) {
        return bzm_validation_result(BZM_CHECK_GOOD, BZM_VALIDATION_CODE_STAGE_OK, detail);
    }
    if (outcome == BZM_BRINGUP_BLOCKED) {
        bzm_validation_code_t code = report != NULL && report->reason == BZM_BRINGUP_REASON_BALANCED_PAIR_UNAVAILABLE
                                         ? BZM_VALIDATION_CODE_NOT_IMPLEMENTED
                                         : BZM_VALIDATION_CODE_PREREQUISITE_FAILED;
        return bzm_validation_result(BZM_CHECK_BLOCKED, code, detail);
    }
    return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED, detail);
}

static bzm_stage_result_t run_chain4(GlobalState * state)
{
    bzm_bridge_safety_status_t status;
    if (BZM_bridge_set_asic_reset(false) != ESP_OK || BZM_bridge_set_5v_enabled(true) != ESP_OK ||
        BZM_bridge_get_safety_status(&status) != ESP_OK || !status.valid || status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED ||
        !status.five_volt_enabled || !status.asic_reset_asserted || !status.fan_full || !bridge_status_runtime_good(&status)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "chain power/reset precondition failed: expected leased 5 V on, reset asserted and fan full");
    }

    bzm_bringup_report_t report;
    bzm_bringup_outcome_t outcome = BZM_staged_initialize(state, &report);
    if (outcome == BZM_BRINGUP_GOOD) {
        /* BZM_staged_initialize reconstructs the transport and closes its
         * gate. Reattach only the lock-free runtime predicate while the
         * predicate itself remains false. */
        BZM_staged_set_dispatch_authorizer(runtime_dispatch_authorizer, &RUNTIME);
        RUNTIME.parser_baseline_valid = BZM_get_parser_stats(&RUNTIME.parser_baseline);
        if (!RUNTIME.parser_baseline_valid) {
            outcome = BZM_BRINGUP_BAD;
            report = (bzm_bringup_report_t){
                .outcome = BZM_BRINGUP_BAD,
                .reason = BZM_BRINGUP_REASON_IO,
            };
        } else {
            outcome = BZM_staged_chain4(&report);
        }
    }
    if (outcome == BZM_BRINGUP_GOOD &&
        (BZM_bridge_get_safety_status(&status) != ESP_OK || !status.valid || status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED ||
         !status.five_volt_enabled || status.asic_reset_asserted || !status.fan_full || !bridge_status_runtime_good(&status))) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "four-chip probe passed but bridge power/reset readback is incoherent");
    }
    RUNTIME.bridge_status = status;
    RUNTIME.bridge_status_valid = status.valid;
    return bringup_stage_result("CHAIN_4", outcome, &report);
}

static bzm_stage_result_t run_sensors(void)
{
    bzm_bringup_sensor_profile_t profile;
    bzm_bringup_reference_sensor_profile(&profile);
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    bzm_bringup_report_t report;
    bzm_bringup_outcome_t outcome = BZM_staged_sensors(&profile, &policy, &report);
    if (outcome == BZM_BRINGUP_GOOD) {
        /* Telemetry startup begins the four TDM transmitters together and proves a
         * full clean parser interval after any bounded activation residue.
         * Runtime monitoring begins at that accepted boundary. */
        RUNTIME.parser_baseline_valid = BZM_staged_get_sensor_parser_baseline(&RUNTIME.parser_baseline);
        if (!RUNTIME.parser_baseline_valid) {
            return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                         "telemetry startup parser baseline is unavailable");
        }
    }
    return bringup_stage_result("SENSORS", outcome, &report);
}

static bzm_stage_result_t run_clocks(void)
{
    bzm_bringup_pll_profile_t profile;
    bzm_bringup_pll_800_profile(&profile);
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    bzm_bringup_report_t report;
    bzm_bringup_outcome_t outcome = BZM_staged_clocks(&profile, &policy, &report);
    return bringup_stage_result("CLOCKS", outcome, &report);
}

static bzm_stage_result_t run_balanced_ramp(void)
{
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    bzm_bringup_report_t report;
    bzm_bringup_outcome_t outcome = BZM_staged_balanced_ramp(&policy, &report);
    if (outcome == BZM_BRINGUP_GOOD) {
        /* Engine activation deliberately pauses/resumes TDM and proves those transition
         * discards separately from every clean engine window. Start runtime
         * parser monitoring at that accepted boundary so proven transition
         * traffic is not misclassified as a live fault. */
        RUNTIME.parser_baseline_valid = BZM_staged_get_parser_baseline(&RUNTIME.parser_baseline);
        if (!RUNTIME.parser_baseline_valid) {
            return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                         "engine activation parser baseline is unavailable");
        }
    }
    return bringup_stage_result("BALANCED_RAMP", outcome, &report);
}

static bzm_stage_result_t run_running(GlobalState * state)
{
    if (!RUNTIME.mining_stack_ready) {
        return bzm_validation_result(BZM_CHECK_BLOCKED, BZM_VALIDATION_CODE_PREREQUISITE_FAILED,
                                     "network mining queue is not initialized yet");
    }
    bzm_bringup_telemetry_policy_t policy = telemetry_policy();
    bzm_bringup_report_t report;
    bzm_bringup_outcome_t outcome = BZM_staged_running(state, &policy, &report);
    if (outcome == BZM_BRINGUP_GOOD) {
        RUNTIME.parser_baseline_valid = BZM_staged_get_running_parser_baseline(&RUNTIME.parser_baseline);
        if (!RUNTIME.parser_baseline_valid) {
            return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                         "pre-mining quiet parser baseline is unavailable");
        }
        bzm_parser_realign_init(&RUNTIME.parser_realign, &RUNTIME.parser_baseline);
        RUNTIME.parser_realign_valid = RUNTIME.parser_realign.initialized;
        if (!RUNTIME.parser_realign_valid) {
            return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                         "mining parser realignment baseline is unavailable");
        }
    }
    if (outcome == BZM_BRINGUP_GOOD && !BZM_running_stats_snapshot(&RUNTIME.running_evidence_baseline)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "mining driver evidence baseline is unavailable");
    }
    return bringup_stage_result("RUNNING", outcome, &report);
}

static bzm_stage_result_t runtime_run_stage(void * context, bzm_validation_stage_t stage)
{
    GlobalState * state = context;
    if (stage >= BZM_STAGE_POWER_RAIL && !runtime_execution_authorizer(&RUNTIME)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "startup watchdog expired before step entry");
    }
    bzm_stage_result_t result;
    switch (stage) {
    case BZM_STAGE_OFF_SAFE:
        return runtime_force_safe_off(context);
    case BZM_STAGE_CONTROLS:
        result = run_controls(state);
        break;
    case BZM_STAGE_POWER_RAIL:
        result = run_power_rail(state);
        break;
    case BZM_STAGE_CHAIN_4:
        result = run_chain4(state);
        break;
    case BZM_STAGE_SENSORS:
        result = run_sensors();
        break;
    case BZM_STAGE_CLOCKS:
        result = run_clocks();
        break;
    case BZM_STAGE_BALANCED_RAMP:
        result = run_balanced_ramp();
        break;
    case BZM_STAGE_RUNNING:
        result = run_running(state);
        break;
    default:
        return bzm_validation_result(BZM_CHECK_BLOCKED, BZM_VALIDATION_CODE_INVALID_CONFIGURATION,
                                     "unknown Bonanza startup step");
    }
    if (stage >= BZM_STAGE_POWER_RAIL && !runtime_execution_authorizer(&RUNTIME)) {
        return bzm_validation_result(BZM_CHECK_BAD, BZM_VALIDATION_CODE_STAGE_FAILED,
                                     "startup watchdog expired during step");
    }
    return result;
}

static void runtime_monitor_task(void * parameter)
{
    (void) parameter;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BZM_MONITOR_PERIOD_MS));
        pthread_mutex_lock(&RUNTIME.lock);
        uint64_t current_ms = now_ms();
        /* This is an internal controller watchdog, not an operator lease.
         * Renew it locally while production mining is healthy; the RP2040
         * bridge retains its shorter independent output lease below. */
        if (RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_MINING &&
            RUNTIME.supervisor.lease_deadline_ms > current_ms &&
            RUNTIME.supervisor.lease_deadline_ms - current_ms <=
                RUNTIME.supervisor.config.maximum_lease_ms / 2U) {
            (void)bzm_supervisor_heartbeat(
                &RUNTIME.supervisor,
                RUNTIME.supervisor.config.maximum_lease_ms, current_ms);
        }
        if (RUNTIME.supervisor.lease_deadline_ms != 0 && current_ms >= RUNTIME.supervisor.lease_deadline_ms) {
            close_dispatch_locked();
        }
        if (!bzm_supervisor_tick(&RUNTIME.supervisor, current_ms)) {
            ESP_LOGE(TAG, "local controller watchdog expired; safe-off requested");
        }

        bool holding = runtime_is_holding_locked();
        /* BIRDS uses a dedicated UART TDM reader. The ESP safety task must
         * provide the same continuous-drain property once the chain is live;
         * waiting for the 500 ms health sample can overflow the 2 KiB UART RX
         * ring at four telemetry frames per TDM cycle. */
        if (holding && RUNTIME.supervisor.report.reached_stage >= BZM_STAGE_CHAIN_4) {
            (void) BZM_staged_poll(1);
        }
        if (holding) {
            bzm_bridge_safety_status_t status = {0};
            esp_err_t heartbeat_err = BZM_bridge_safety_heartbeat(&status);
            if (heartbeat_err != ESP_OK || !status.valid ||
                status.state != BZM_BRIDGE_SAFETY_STATE_CONTROLLED || status.lease_remaining_ms == 0 ||
                !bridge_status_runtime_good(&status)) {
                close_dispatch_locked();
                RUNTIME.health = (bzm_runtime_health_result_t){
                    .status = BZM_RUNTIME_HEALTH_BAD,
                    .fault = BZM_RUNTIME_HEALTH_FAULT_BRIDGE_UNAVAILABLE,
                };
                snprintf(RUNTIME.health.detail, sizeof(RUNTIME.health.detail), "bridge heartbeat/status interlock failed");
                RUNTIME.health_valid = true;
                RUNTIME.health_sampled_at_ms = current_ms;
                ESP_LOGE(TAG,
                         "runtime bridge interlock BAD: err=%s valid=%u state=%u lease=%lu fault=%u trip=%u verdict=0x%02x",
                         esp_err_to_name(heartbeat_err), (unsigned)status.valid,
                         (unsigned)status.state,
                         (unsigned long)status.lease_remaining_ms,
                         (unsigned)status.fault,
                         (unsigned)status.trip_input_asserted,
                         (unsigned)status.runtime_verdict);
                (void) bzm_supervisor_latch_fault(&RUNTIME.supervisor, BZM_RUNTIME_HEALTH_FAULT_BRIDGE_UNAVAILABLE,
                                                  RUNTIME.health.detail);
            } else {
                RUNTIME.bridge_status = status;
                RUNTIME.bridge_status_valid = true;
            }
        }
        if (runtime_is_holding_locked() &&
            (RUNTIME.health_sampled_at_ms == 0 || current_ms - RUNTIME.health_sampled_at_ms >= BZM_HEALTH_PERIOD_MS)) {
            bzm_runtime_health_result_t health = sample_runtime_health_locked();
            if (health.status == BZM_RUNTIME_HEALTH_BAD) {
                close_dispatch_locked();
                ESP_LOGE(TAG, "runtime health BAD fault=%s(%u): %s",
                         bzm_runtime_health_fault_name(health.fault),
                         (unsigned)health.fault, health.detail);
                (void) bzm_supervisor_latch_fault(&RUNTIME.supervisor, (uint32_t) health.fault, health.detail);
            }
        }
        if (RUNTIME.supervisor.owner == BZM_SUPERVISOR_OWNER_MINING && RUNTIME.running_evidence_monitoring) {
            bzm_running_evidence_result_t evidence = evaluate_running_evidence_locked(current_ms);
            if (evidence.status == BZM_RUNNING_EVIDENCE_BAD) {
                close_dispatch_locked();
                ESP_LOGE(TAG, "runtime mining evidence BAD fault=%s(%u): %s",
                         bzm_running_evidence_fault_name(evidence.fault),
                         (unsigned)evidence.fault, evidence.detail);
                (void) bzm_supervisor_latch_fault(&RUNTIME.supervisor, 0x1007, evidence.detail);
            }
        }
        sync_dispatch_locked();
        pthread_mutex_unlock(&RUNTIME.lock);
    }
}

esp_err_t bzm_controller_init(GlobalState * global_state)
{
    if (global_state == NULL)
        return ESP_ERR_INVALID_ARG;
    if (!global_state->DEVICE_CONFIG.bonanza_bridge || global_state->DEVICE_CONFIG.family.asic.id != BZM) {
        return ESP_OK;
    }

    pthread_mutex_lock(&RUNTIME.lock);
    if (RUNTIME.initialized) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return ESP_OK;
    }
    RUNTIME.global_state = global_state;
    RUNTIME.active = true;
    reset_running_evidence_locked(false);
    bzm_ch2_confirmation_init(&RUNTIME.ch2_confirmation);
    bzm_pll_lock_confirmation_init(&RUNTIME.pll_lock_confirmation);
    atomic_init(&RUNTIME.dispatch_enabled, false);
    atomic_init(&RUNTIME.dispatch_deadline_ms, 0);
    atomic_init(&RUNTIME.execution_deadline_ms, 0);
    BZM_staged_set_operation_authorizer(runtime_execution_authorizer, &RUNTIME);

    bzm_validation_ops_t ops = {
        .run_stage = runtime_run_stage,
        .force_safe_off = runtime_force_safe_off,
    };
    bzm_supervisor_config_t config = {
        .build_max_stage = BZM_STAGE_RUNNING,
        .implemented_max_stage = BZM_STAGE_RUNNING,
        .powered_stages_compiled = true,
        .production_mode = true,
        .independent_kill_available = false,
        .allow_esp_only_kill_in_lab = false,
        .board_managed_safety = true,
        .maximum_lease_ms = BZM_CONTROLLER_WATCHDOG_MS,
    };
    if (!bzm_supervisor_init(&RUNTIME.supervisor, &config, &ops, global_state)) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return ESP_ERR_INVALID_STATE;
    }
    RUNTIME.initialized = true;
    STRATUM_V1_set_restart_guard(runtime_restart_guard, NULL);
    refresh_bridge_evidence_locked();
    bool safe = bzm_supervisor_request_validation(&RUNTIME.supervisor, BZM_STAGE_OFF_SAFE, false, false, 0, now_ms());
    publish_driver_health_locked(safe ? ASIC_DRIVER_SAFE_OFF
                                      : ASIC_DRIVER_FAULT);
    pthread_mutex_unlock(&RUNTIME.lock);
    if (!safe)
        return ESP_FAIL;

    if (xTaskCreate(runtime_monitor_task, "bzm_safety", 6144, NULL, 18, NULL) != pdPASS) {
        pthread_mutex_lock(&RUNTIME.lock);
        close_dispatch_locked();
        (void) bzm_supervisor_latch_fault(&RUNTIME.supervisor, 0x1002, "Bonanza safety monitor task could not start");
        pthread_mutex_unlock(&RUNTIME.lock);
        return ESP_ERR_NO_MEM;
    }
    pthread_mutex_lock(&RUNTIME.lock);
    RUNTIME.monitor_running = true;
    pthread_mutex_unlock(&RUNTIME.lock);
    ESP_LOGI(TAG, "Bonanza production controller initialized at safe-off");
    return ESP_OK;
}

bool bzm_controller_mining_stack_ready(void)
{
    pthread_mutex_lock(&RUNTIME.lock);
    bool started = false;
    if (RUNTIME.active && RUNTIME.initialized) {
        RUNTIME.mining_stack_ready = true;
        started = start_production_mining_locked();
    }
    pthread_mutex_unlock(&RUNTIME.lock);
    if (started) {
        ESP_LOGI(TAG, "Bonanza reached MINING on the fixed production profile");
    } else {
        ESP_LOGE(TAG, "Bonanza automatic startup failed closed");
    }
    return started;
}

bool bzm_controller_active(void)
{
    pthread_mutex_lock(&RUNTIME.lock);
    bool active = RUNTIME.active;
    pthread_mutex_unlock(&RUNTIME.lock);
    return active;
}

bool bzm_controller_dispatch_allowed(void)
{
    return runtime_dispatch_authorizer(&RUNTIME);
}

bool bzm_controller_acquire_maintenance(bzm_supervisor_owner_t owner)
{
    pthread_mutex_lock(&RUNTIME.lock);
    close_dispatch_locked();
    bool ok = RUNTIME.initialized && bzm_supervisor_acquire_maintenance(&RUNTIME.supervisor, owner, now_ms());
    sync_dispatch_locked();
    pthread_mutex_unlock(&RUNTIME.lock);
    return ok;
}

bool bzm_controller_release_maintenance(bzm_supervisor_owner_t owner)
{
    pthread_mutex_lock(&RUNTIME.lock);
    close_dispatch_locked();
    bool ok = RUNTIME.initialized && bzm_supervisor_release_maintenance(&RUNTIME.supervisor, owner);
    sync_dispatch_locked();
    pthread_mutex_unlock(&RUNTIME.lock);
    return ok;
}

bool bzm_controller_prepare_restart(void)
{
    pthread_mutex_lock(&RUNTIME.lock);
    if (!RUNTIME.active) {
        pthread_mutex_unlock(&RUNTIME.lock);
        return true;
    }

    close_dispatch_locked();
    bool ok = RUNTIME.initialized && bzm_supervisor_prepare_restart(&RUNTIME.supervisor);
    sync_dispatch_locked();
    pthread_mutex_unlock(&RUNTIME.lock);
    return ok;
}
