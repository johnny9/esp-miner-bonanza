#include "bzm_driver.h"

#include <pthread.h>
#include <stdatomic.h>

#include "bzm_bridge.h"
#include "bzm_balanced_ramp.h"
#include "bzm_bringup.h"
#include "bzm_dispatch_gate.h"
#include "bzm_lease_guard.h"
#include "bzm_reactor.h"
#include "bzm_registers.h"
#include "bzm_runtime_health.h"
#include "bzm_transport.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "serial.h"

static const char * TAG = "bzm";
static bzm_reactor_t REACTOR;
static bzm_serial_transport_t TRANSPORT;
static pthread_mutex_t REACTOR_LOCK = PTHREAD_MUTEX_INITIALIZER;
static bool INITIALIZED;
static bool STAGED_TRANSPORT_READY;
/* Sticky for one staged session. A failed bridge renewal makes every later
 * adapter operation fail I/O until initialize reconstructs the session. */
static bool STAGED_LEASE_IO_OK;
static bzm_lease_guard_schedule_t STAGED_LEASE_SCHEDULE;
static bzm_dispatch_authorizer_t STAGED_OPERATION_AUTHORIZE;
static void * STAGED_OPERATION_AUTHORIZE_CONTEXT;
static bzm_bringup_state_t STAGED_BRINGUP;
static bzm_balanced_ramp_t STAGED_BALANCED_RAMP;
static bzm_serial_parser_stats_t STAGED_ENGINE_WINDOW_BASELINE;
static bzm_serial_parser_stats_t STAGED_SENSOR_PARSER_BASELINE;
static bool STAGED_SENSOR_PARSER_BASELINE_VALID;
static bzm_serial_parser_stats_t STAGED_RUNNING_PARSER_BASELINE;
static bool STAGED_RUNNING_PARSER_BASELINE_VALID;
static bool STAGED_ENGINE_WINDOW_ACTIVE;
static bzm_bringup_telemetry_policy_t STAGED_RAMP_TELEMETRY_POLICY;
static bool STAGED_RAMP_TELEMETRY_POLICY_READY;
static bzm_telemetry_store_t STAGED_BATCH_TELEMETRY;
static bool STAGED_BATCH_TELEMETRY_ACTIVE;
static bzm_dispatch_gate_t STAGED_DISPATCH_GATE;
static float LAST_TEMPERATURE = -1.0f;
static int64_t LAST_TEMPERATURE_US;
static atomic_uint_fast64_t RUNNING_DISPATCH_BATCHES;
static atomic_uint_fast64_t RUNNING_DISPATCHED_LOGICAL_ENGINES;
static atomic_uint_fast64_t RUNNING_DISPATCHED_CHIP_ENGINES;
static atomic_uint_fast64_t RUNNING_DISPATCH_FAILURES;
static atomic_uint_fast64_t RUNNING_MAPPED_RESULTS;
static atomic_uint_fast64_t RUNNING_MAPPING_REJECTIONS;
static atomic_uint_fast32_t RUNNING_MAPPING_REJECTION_STREAK;
static atomic_bool RUNNING_MAPPING_RECOVERY_PENDING;
static atomic_uint_fast64_t RUNNING_LOCALLY_VALID_RESULTS;
static atomic_uint_fast64_t RUNNING_LOCALLY_REJECTED_RESULTS;
static atomic_uint_fast32_t RUNNING_LOCAL_REJECTION_STREAK;
static atomic_bool RUNNING_LOCAL_RECOVERY_PENDING;

#ifdef CONFIG_BZM_1002_LAB_VALIDATION
static void lab_format_hex(const uint8_t *bytes, size_t length,
                           char *hex, size_t hex_capacity)
{
    static const char digits[] = "0123456789abcdef";
    if (bytes == NULL || hex == NULL || hex_capacity < length * 2 + 1) {
        return;
    }
    for (size_t i = 0; i < length; ++i) {
        hex[i * 2] = digits[bytes[i] >> 4];
        hex[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    hex[length * 2] = '\0';
}

static void lab_log_stage7_template(const mining_template_t *template)
{
    if (template == NULL) {
        return;
    }
    asic_work_t source = {
        .handle = 1,
        .template = template,
    };
    bzm_work_t work;
    if (!bzm_work_build(&source, 0, 0, 16, CONFIG_BZM_1002_STAGE7_LEAD_ZEROS,
                        true, &work)) {
        ESP_LOGE(TAG, "Stage 7 template trace could not build diagnostic work");
        return;
    }

    char prev_hash[65];
    char merkle_root[65];
    char midstates[BZM_VERSION_VARIANTS][65];
    lab_format_hex(template->prev_block_hash, 32, prev_hash,
                   sizeof(prev_hash));
    lab_format_hex(template->merkle_root, 32, merkle_root,
                   sizeof(merkle_root));
    for (size_t i = 0; i < work.midstate_count; ++i) {
        lab_format_hex(work.midstates[i], 32, midstates[i],
                       sizeof(midstates[i]));
    }

    ESP_LOGI(TAG,
             "Stage 7 template: job=%s extranonce2=%s version=%08lx mask=%08lx ntime=%08lx nbits=%08lx "
             "merkleResidue=%08lx",
             template->share.job_id ? template->share.job_id : "",
             template->share.extranonce2 ? template->share.extranonce2 : "",
             (unsigned long) template->version,
             (unsigned long) template->version_mask,
             (unsigned long) template->ntime,
             (unsigned long) template->target,
             (unsigned long) work.merkle_residue);
    ESP_LOGI(TAG, "Stage 7 template prev=%s merkle=%s", prev_hash,
             merkle_root);
    for (size_t i = 0; i < work.midstate_count; ++i) {
        ESP_LOGI(TAG, "Stage 7 template midstate[%u] version=%08lx value=%s",
                 (unsigned) i, (unsigned long) work.versions[i],
                 midstates[i]);
    }
}
#endif

enum
{
    BZM_REG_DTS_SRST_PD = 0x2e,
    BZM_REG_TEMPSENSOR_TUNECODE = 0x30,
    BZM_REG_TEMPSENSOR_TEMP_CODE_STATUS = 0x32,
    BZM_REG_SENSOR_CLK_DIV = 0x3d,
    BZM_DEFAULT_THERMAL_SENSOR_DIV = 8,
    BZM_PARSER_SETTLE_WINDOW_MS = 100,
    BZM_PARSER_SETTLE_REQUIRED_CLEAN_WINDOWS = 10,
    BZM_PARSER_SETTLE_MAX_WINDOWS = 30,
};

static bool staged_settle_parser(bzm_serial_transport_t * transport, const char * stage_name,
                                 const bzm_serial_parser_stats_t * initial_baseline,
                                 bzm_serial_parser_stats_t * accepted_baseline);

static void staged_report(bzm_bringup_report_t * report, bzm_bringup_outcome_t outcome, bzm_bringup_reason_t reason)
{
    if (report != NULL) {
        *report = (bzm_bringup_report_t){
            .outcome = outcome,
            .reason = reason,
        };
    }
}

int BZM_set_max_baud(void)
{
    return SERIAL_set_baud(BZM_BAUD_RATE) == ESP_OK ? BZM_BAUD_RATE : 0;
}

uint8_t BZM_init(GlobalState * state)
{
    if (state == NULL || SERIAL_set_baud(BZM_set_max_baud()) != ESP_OK) {
        ESP_LOGE(TAG, "Could not select the 5 Mbaud BZM data link");
        return 0;
    }

    INITIALIZED = false;
    STAGED_TRANSPORT_READY = false;
    STAGED_LEASE_IO_OK = false;
    bzm_bringup_init(&STAGED_BRINGUP);
    bzm_balanced_ramp_init(&STAGED_BALANCED_RAMP);
    STAGED_ENGINE_WINDOW_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_ENGINE_WINDOW_ACTIVE = false;
    STAGED_SENSOR_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_SENSOR_PARSER_BASELINE_VALID = false;
    STAGED_RUNNING_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_RUNNING_PARSER_BASELINE_VALID = false;
    STAGED_RAMP_TELEMETRY_POLICY = (bzm_bringup_telemetry_policy_t){0};
    STAGED_RAMP_TELEMETRY_POLICY_READY = false;
    bzm_telemetry_store_init(&STAGED_BATCH_TELEMETRY);
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    bzm_dispatch_gate_init(&STAGED_DISPATCH_GATE);
    LAST_TEMPERATURE = -1.0f;
    LAST_TEMPERATURE_US = 0;
    atomic_store_explicit(&RUNNING_DISPATCH_BATCHES, 0, memory_order_relaxed);
    atomic_store_explicit(&RUNNING_DISPATCHED_LOGICAL_ENGINES, 0, memory_order_relaxed);
    atomic_store_explicit(&RUNNING_DISPATCHED_CHIP_ENGINES, 0, memory_order_relaxed);
    atomic_store_explicit(&RUNNING_DISPATCH_FAILURES, 0, memory_order_relaxed);
    atomic_store_explicit(&RUNNING_MAPPED_RESULTS, 0, memory_order_relaxed);
    atomic_store_explicit(&RUNNING_MAPPING_REJECTIONS, 0, memory_order_relaxed);
    atomic_store_explicit(&RUNNING_MAPPING_REJECTION_STREAK, 0, memory_order_seq_cst);
    atomic_store_explicit(&RUNNING_MAPPING_RECOVERY_PENDING, false, memory_order_seq_cst);
    atomic_store_explicit(&RUNNING_LOCALLY_VALID_RESULTS, 0, memory_order_relaxed);
    atomic_store_explicit(&RUNNING_LOCALLY_REJECTED_RESULTS, 0, memory_order_relaxed);
    atomic_store_explicit(&RUNNING_LOCAL_REJECTION_STREAK, 0, memory_order_seq_cst);
    atomic_store_explicit(&RUNNING_LOCAL_RECOVERY_PENDING, false, memory_order_seq_cst);

    uint16_t engine_count = state->DEVICE_CONFIG.family.asic.core_count;
    if (engine_count != BZM_ENGINES_PER_ASIC) {
        ESP_LOGE(TAG, "BZM requires exactly %u usable engines, got %u", BZM_ENGINES_PER_ASIC, engine_count);
        return 0;
    }

    size_t expected_asics = state->DEVICE_CONFIG.family.asic_count;
    if (expected_asics == 0 || expected_asics > BZM_MAX_ASIC_COUNT) {
        ESP_LOGE(TAG, "Unsupported BZM ASIC count: %u", (unsigned) expected_asics);
        return 0;
    }

    bzm_serial_transport_deinit(&TRANSPORT);
    TRANSPORT = (bzm_serial_transport_t){
        .engine_count = engine_count,
        .enhanced_mode = true,
    };
    if (!bzm_serial_transport_init(&TRANSPORT)) {
        ESP_LOGE(TAG, "Unable to initialize the BZM serial frame owner");
        return 0;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    size_t detected_asics = bzm_serial_discover_chain(&TRANSPORT, expected_asics);
    if (detected_asics == 0) {
        ESP_LOGE(TAG, "No BZM ASICs responded during chain addressing");
        return 0;
    }
    for (size_t i = 0; i < detected_asics; ++i) {
        ESP_LOGI(TAG, "BZM ASIC %u addressed at 0x%02x", (unsigned) i, TRANSPORT.asic_ids[i]);
    }
    if (detected_asics != expected_asics) {
        ESP_LOGE(TAG, "Detected %u BZM ASICs, expected %u", (unsigned) detected_asics, (unsigned) expected_asics);
    }
    bzm_reactor_config_t config = {
        .engine_count = engine_count,
        /* BIRDS leaves each independent engine job enough ntime budget to
         * remain productive throughout the paced 236-engine rotation. */
        .timestamp_count = 60,
        .lead_zeros = CONFIG_BZM_1002_STAGE7_LEAD_ZEROS,
        .nonce_offset = BZM_NONCE_GAP_1002,
        .enhanced_mode = true,
    };

    pthread_mutex_lock(&REACTOR_LOCK);
    INITIALIZED = bzm_reactor_init(&REACTOR, &state->asic_job_store, &config, &BZM_SERIAL_TRANSPORT_OPS, &TRANSPORT);
    pthread_mutex_unlock(&REACTOR_LOCK);
    if (!INITIALIZED)
        return 0;

    ESP_LOGI(TAG, "BZM reactor ready for %u engines across %u ASICs", engine_count, (unsigned) detected_asics);
    return detected_asics;
}

bool BZM_send_work(GlobalState * state, const mining_template_t * template)
{
    (void) state;
    if (!INITIALIZED || template == NULL) {
        return false;
    }

    if (STAGED_TRANSPORT_READY) {
        pthread_mutex_lock(&REACTOR_LOCK);
        bool running = STAGED_BRINGUP.running_verified;
        bzm_dispatch_gate_t gate = STAGED_DISPATCH_GATE;
        pthread_mutex_unlock(&REACTOR_LOCK);
        if (!running || !bzm_dispatch_gate_is_authorized(&gate))
            return false;
    }

#ifdef CONFIG_BZM_1002_LAB_VALIDATION
    lab_log_stage7_template(template);
#endif

    pthread_mutex_lock(&REACTOR_LOCK);
    if (template->clean_jobs) {
        if (!bzm_reactor_clear_work(&REACTOR)) {
            pthread_mutex_unlock(&REACTOR_LOCK);
            atomic_fetch_add_explicit(&RUNNING_DISPATCH_FAILURES, 1, memory_order_relaxed);
            ESP_LOGE(TAG, "Unable to flush engines for clean work");
            return false;
        }
    }

    bzm_work_t assigned_work;
    bzm_assign_status_t status =
        bzm_reactor_assign(&REACTOR, template, &assigned_work);
    if (status == BZM_ASSIGN_FLUSH_REQUIRED) {
        if (!bzm_reactor_clear_work(&REACTOR)) {
            pthread_mutex_unlock(&REACTOR_LOCK);
            atomic_fetch_add_explicit(&RUNNING_DISPATCH_FAILURES, 1, memory_order_relaxed);
            ESP_LOGE(TAG, "Unable to complete BZM flush barrier");
            return false;
        }
        status = bzm_reactor_assign(&REACTOR, template, &assigned_work);
    }
    bool rotation_complete = status == BZM_ASSIGN_OK &&
        REACTOR.next_engine == 0;
    size_t chip_engines = status == BZM_ASSIGN_OK
        ? TRANSPORT.asic_count : 0;
    pthread_mutex_unlock(&REACTOR_LOCK);

    if (status != BZM_ASSIGN_OK) {
        atomic_fetch_add_explicit(&RUNNING_DISPATCH_FAILURES, 1, memory_order_relaxed);
        ESP_LOGE(TAG, "BZM work assignment failed (%d)", status);
        return false;
    }
    if (rotation_complete) {
        atomic_fetch_add_explicit(&RUNNING_DISPATCH_BATCHES, 1,
                                  memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&RUNNING_DISPATCHED_LOGICAL_ENGINES, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&RUNNING_DISPATCHED_CHIP_ENGINES, chip_engines, memory_order_relaxed);
    return true;
}

bool BZM_clear_work(GlobalState * state)
{
    (void) state;
    pthread_mutex_lock(&REACTOR_LOCK);
    bool cleared = !INITIALIZED || bzm_reactor_clear_work(&REACTOR);
    pthread_mutex_unlock(&REACTOR_LOCK);
    if (!cleared) {
        atomic_fetch_add_explicit(&RUNNING_DISPATCH_FAILURES, 1,
                                  memory_order_relaxed);
        ESP_LOGE(TAG, "BZM clean-job barrier failed");
    }
    return cleared;
}

asic_event_t * BZM_process_work(GlobalState * state)
{
    (void) state;
    if (!INITIALIZED || (STAGED_TRANSPORT_READY && !STAGED_BRINGUP.running_verified)) {
        return NULL;
    }

    bzm_raw_result_t raw;
    static asic_event_t event;
    pthread_mutex_lock(&REACTOR_LOCK);
    bool received = bzm_serial_read_result(&TRANSPORT, &raw, 20);
    bool nonce_frame = received && bzm_raw_result_has_valid_nonce(&raw);
    bool mapped = nonce_frame && bzm_reactor_map_result(&REACTOR, &raw, &event);
    pthread_mutex_unlock(&REACTOR_LOCK);
    if (mapped) {
        atomic_fetch_add_explicit(&RUNNING_MAPPED_RESULTS, 1, memory_order_relaxed);
#ifdef CONFIG_BZM_1002_LAB_VALIDATION
        ESP_LOGI(TAG,
                 "Stage 7 result map: asic=0x%02x engine=0x%03x status=0x%x rawNonce=0x%08lx rawSeq=%u rawTime=%u "
                 "handle=0x%llx nonce=0x%08lx finalVersion=0x%08lx versionBits=0x%08lx finalNtime=0x%08lx "
                 "logicalSeq=%u micro=%u",
                 raw.asic_id, raw.engine_id, raw.status, (unsigned long) raw.nonce, raw.sequence_id, raw.time,
                 (unsigned long long) event.data.share.work_handle, (unsigned long) event.data.share.nonce,
                 (unsigned long) event.data.share.final_version, (unsigned long) event.data.share.version_bits,
                 (unsigned long) event.data.share.final_ntime, event.data.share.sequence_id,
                 event.data.share.micro_job_id);
#endif
    } else if (nonce_frame) {
        /* An unchecksummed frame that looks like a nonce can contain a
         * corrupted ASIC/engine/sequence field. Recovery is proven only when
         * a later mapped result passes full local hash validation. */
        atomic_store_explicit(&RUNNING_MAPPING_RECOVERY_PENDING, true,
                              memory_order_seq_cst);
        atomic_fetch_add_explicit(&RUNNING_MAPPING_REJECTIONS, 1,
                                  memory_order_seq_cst);
        atomic_fetch_add_explicit(&RUNNING_MAPPING_REJECTION_STREAK, 1,
                                  memory_order_seq_cst);
#ifdef CONFIG_BZM_1002_LAB_VALIDATION
        ESP_LOGW(TAG,
                 "Stage 7 result attribution rejected: asic=0x%02x engine=0x%03x status=0x%x rawNonce=0x%08lx rawSeq=%u rawTime=%u",
                 raw.asic_id, raw.engine_id, raw.status,
                 (unsigned long) raw.nonce, raw.sequence_id, raw.time);
#endif
    }
    return mapped ? &event : NULL;
}

bool BZM_running_stats_snapshot(bzm_running_stats_t * stats)
{
    if (stats == NULL)
        return false;
    *stats = (bzm_running_stats_t){
        .dispatch_batches = atomic_load_explicit(&RUNNING_DISPATCH_BATCHES, memory_order_relaxed),
        .dispatched_logical_engines =
            atomic_load_explicit(&RUNNING_DISPATCHED_LOGICAL_ENGINES, memory_order_relaxed),
        .dispatched_chip_engines =
            atomic_load_explicit(&RUNNING_DISPATCHED_CHIP_ENGINES, memory_order_relaxed),
        .dispatch_failures = atomic_load_explicit(&RUNNING_DISPATCH_FAILURES, memory_order_relaxed),
        .mapped_results = atomic_load_explicit(&RUNNING_MAPPED_RESULTS, memory_order_relaxed),
        .mapping_rejections = atomic_load_explicit(&RUNNING_MAPPING_REJECTIONS, memory_order_relaxed),
        .mapping_rejection_streak = atomic_load_explicit(
            &RUNNING_MAPPING_REJECTION_STREAK, memory_order_seq_cst),
        .mapping_recovery_pending = atomic_load_explicit(
            &RUNNING_MAPPING_RECOVERY_PENDING, memory_order_seq_cst),
        .locally_valid_results =
            atomic_load_explicit(&RUNNING_LOCALLY_VALID_RESULTS, memory_order_relaxed),
        .locally_rejected_results =
            atomic_load_explicit(&RUNNING_LOCALLY_REJECTED_RESULTS, memory_order_relaxed),
        .local_rejection_streak = atomic_load_explicit(
            &RUNNING_LOCAL_REJECTION_STREAK, memory_order_seq_cst),
        .local_recovery_pending = atomic_load_explicit(
            &RUNNING_LOCAL_RECOVERY_PENDING, memory_order_seq_cst),
    };
    return true;
}

void BZM_running_record_proof(void)
{
    atomic_fetch_add_explicit(&RUNNING_LOCALLY_VALID_RESULTS, 1,
                              memory_order_seq_cst);
    atomic_store_explicit(&RUNNING_MAPPING_RECOVERY_PENDING, false,
                          memory_order_seq_cst);
    atomic_store_explicit(&RUNNING_MAPPING_REJECTION_STREAK, 0,
                          memory_order_seq_cst);
    atomic_store_explicit(&RUNNING_LOCAL_RECOVERY_PENDING, false,
                          memory_order_seq_cst);
    atomic_store_explicit(&RUNNING_LOCAL_REJECTION_STREAK, 0,
                          memory_order_seq_cst);
}

void BZM_running_record_rejection(void)
{
    atomic_store_explicit(&RUNNING_LOCAL_RECOVERY_PENDING, true,
                          memory_order_seq_cst);
    atomic_fetch_add_explicit(&RUNNING_LOCALLY_REJECTED_RESULTS, 1,
                              memory_order_seq_cst);
    atomic_fetch_add_explicit(&RUNNING_LOCAL_REJECTION_STREAK, 1,
                              memory_order_seq_cst);
}

static bool read_u32(uint8_t asic_id, uint8_t offset, uint32_t * value)
{
    uint8_t bytes[4];
    if (value == NULL || !bzm_serial_read_register(&TRANSPORT, asic_id, BZM_CONTROL_ENGINE_ID, offset, bytes, sizeof(bytes))) {
        return false;
    }
    *value = (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) | ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
    return true;
}

static bool write_u32(uint8_t asic_id, uint8_t offset, uint32_t value)
{
    uint8_t bytes[4] = {
        value & 0xff,
        (value >> 8) & 0xff,
        (value >> 16) & 0xff,
        (value >> 24) & 0xff,
    };
    return bzm_serial_write_register_to(&TRANSPORT, asic_id, BZM_CONTROL_ENGINE_ID, offset, bytes, sizeof(bytes));
}

static bool read_chip_temperature(uint8_t asic_id, float * temperature)
{
    uint32_t original_clk_div;
    uint32_t original_dts_config;
    uint32_t original_temp_config;
    uint32_t temp_status = 0;
    bool success = read_u32(asic_id, BZM_REG_SENSOR_CLK_DIV, &original_clk_div) &&
                   read_u32(asic_id, BZM_REG_DTS_SRST_PD, &original_dts_config) &&
                   read_u32(asic_id, BZM_REG_TEMPSENSOR_TUNECODE, &original_temp_config);
    if (!success)
        return false;

    uint32_t configured_clk_div = (original_clk_div & ~(0x1fu << 5)) | (BZM_DEFAULT_THERMAL_SENSOR_DIV << 5);
    uint32_t configured_dts_config = (original_dts_config | (1u << 8)) & ~1u;
    uint32_t configured_temp_config = original_temp_config | 1u;

    success = write_u32(asic_id, BZM_REG_SENSOR_CLK_DIV, configured_clk_div) &&
              write_u32(asic_id, BZM_REG_DTS_SRST_PD, configured_dts_config) &&
              write_u32(asic_id, BZM_REG_TEMPSENSOR_TUNECODE, configured_temp_config);
    if (success) {
        vTaskDelay(pdMS_TO_TICKS(10));
        success = read_u32(asic_id, BZM_REG_TEMPSENSOR_TEMP_CODE_STATUS, &temp_status);
    }

    bool restored = write_u32(asic_id, BZM_REG_TEMPSENSOR_TUNECODE, original_temp_config);
    restored = write_u32(asic_id, BZM_REG_DTS_SRST_PD, original_dts_config) && restored;
    restored = write_u32(asic_id, BZM_REG_SENSOR_CLK_DIV, original_clk_div) && restored;
    if (!success || !restored || (temp_status & (1u << 12)) != 0) {
        return false;
    }
    *temperature = bzm_temperature_from_code(temp_status & 0x0fff);
    return true;
}

float BZM_read_temperature(GlobalState * state)
{
    (void) state;
    if (!INITIALIZED || TRANSPORT.asic_count == 0)
        return -1.0f;

    int64_t now = esp_timer_get_time();
    if (LAST_TEMPERATURE_US != 0 && now - LAST_TEMPERATURE_US < 2000000) {
        return LAST_TEMPERATURE;
    }

    pthread_mutex_lock(&REACTOR_LOCK);
    float total = 0.0f;
    size_t valid = 0;
    for (size_t i = 0; i < TRANSPORT.asic_count; ++i) {
        float temperature;
        if (read_chip_temperature(TRANSPORT.asic_ids[i], &temperature)) {
            total += temperature;
            valid++;
        }
    }
    pthread_mutex_unlock(&REACTOR_LOCK);

    if (valid != 0) {
        LAST_TEMPERATURE = total / valid;
    }
    LAST_TEMPERATURE_US = now;
    return LAST_TEMPERATURE;
}

bool BZM_get_telemetry(uint8_t asic_id, bzm_telemetry_sample_t * sample)
{
    return bzm_serial_get_telemetry(&TRANSPORT, asic_id, sample);
}

bool BZM_get_telemetry_snapshot(bzm_telemetry_store_t * snapshot)
{
    return bzm_serial_get_telemetry_snapshot(&TRANSPORT, snapshot);
}

bool BZM_get_parser_stats(bzm_serial_parser_stats_t * stats)
{
    return bzm_serial_get_parser_stats(&TRANSPORT, stats);
}

bool BZM_staged_get_parser_baseline(bzm_serial_parser_stats_t * stats)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    bool available = STAGED_TRANSPORT_READY &&
                     bzm_balanced_ramp_get_parser_baseline(&STAGED_BALANCED_RAMP, stats);
    pthread_mutex_unlock(&REACTOR_LOCK);
    return available;
}

bool BZM_staged_get_sensor_parser_baseline(bzm_serial_parser_stats_t * stats)
{
    if (stats == NULL) {
        return false;
    }
    pthread_mutex_lock(&REACTOR_LOCK);
    bool available = STAGED_TRANSPORT_READY && STAGED_SENSOR_PARSER_BASELINE_VALID;
    if (available) {
        *stats = STAGED_SENSOR_PARSER_BASELINE;
    }
    pthread_mutex_unlock(&REACTOR_LOCK);
    return available;
}

bool BZM_staged_get_running_parser_baseline(bzm_serial_parser_stats_t * stats)
{
    if (stats == NULL) {
        return false;
    }
    pthread_mutex_lock(&REACTOR_LOCK);
    bool available = STAGED_TRANSPORT_READY && STAGED_RUNNING_PARSER_BASELINE_VALID;
    if (available) {
        *stats = STAGED_RUNNING_PARSER_BASELINE;
    }
    pthread_mutex_unlock(&REACTOR_LOCK);
    return available;
}

static bool staged_lease_renew(void * context)
{
    (void) context;
    if (STAGED_OPERATION_AUTHORIZE == NULL || !STAGED_OPERATION_AUTHORIZE(STAGED_OPERATION_AUTHORIZE_CONTEXT)) {
        return false;
    }
    bzm_bridge_safety_status_t status;
    bool renewed = BZM_bridge_safety_heartbeat(&status) == ESP_OK && bzm_lease_guard_status_is_controlled(&status);
    if (renewed) {
        STAGED_LEASE_SCHEDULE.renewed = true;
        STAGED_LEASE_SCHEDULE.last_renewal_ms = (uint64_t) (esp_timer_get_time() / 1000);
    }
    return renewed;
}

static bool staged_lease_check(void)
{
    if (!STAGED_LEASE_IO_OK || !staged_lease_renew(NULL)) {
        STAGED_LEASE_IO_OK = false;
        return false;
    }
    return true;
}

static bool staged_mining_lease_renew(void * context)
{
    (void) context;
    bzm_bridge_safety_status_t status;
    bool renewed = BZM_bridge_safety_heartbeat(&status) == ESP_OK && bzm_lease_guard_status_is_controlled(&status);
    if (renewed) {
        STAGED_LEASE_SCHEDULE.renewed = true;
        STAGED_LEASE_SCHEDULE.last_renewal_ms = (uint64_t) (esp_timer_get_time() / 1000);
    }
    return renewed;
}

static bool staged_mining_lease_service_due(void)
{
    uint64_t current_ms = (uint64_t) (esp_timer_get_time() / 1000);
    bool dispatch_authorized = STAGED_TRANSPORT_READY && STAGED_BRINGUP.running_verified &&
                               bzm_dispatch_gate_is_authorized(&STAGED_DISPATCH_GATE);
    if (!STAGED_LEASE_IO_OK ||
        !bzm_lease_guard_service_authorized(&STAGED_LEASE_SCHEDULE, dispatch_authorized, current_ms,
                                            BZM_LEASE_GUARD_MAX_DELAY_CHUNK_MS, staged_mining_lease_renew, NULL)) {
        STAGED_LEASE_IO_OK = false;
        return false;
    }
    return true;
}

#ifdef CONFIG_BZM_1002_STAGE6_BALANCED_RAMP
static bool staged_operation_check(void)
{
    if (!STAGED_LEASE_IO_OK || STAGED_OPERATION_AUTHORIZE == NULL ||
        !STAGED_OPERATION_AUTHORIZE(STAGED_OPERATION_AUTHORIZE_CONTEXT)) {
        STAGED_LEASE_IO_OK = false;
        return false;
    }
    return true;
}
#endif

static void staged_sleep(void * context, uint32_t delay_ms)
{
    (void) context;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static bzm_bringup_probe_result_t staged_probe_noop(void * context, uint8_t asic_id)
{
    if (!staged_lease_check()) {
        return BZM_BRINGUP_PROBE_IO_ERROR;
    }
    bzm_serial_probe_result_t result = bzm_serial_probe_noop(context, asic_id);
    switch (result) {
    case BZM_SERIAL_PROBE_RESPONSE:
        return BZM_BRINGUP_PROBE_RESPONSE;
    case BZM_SERIAL_PROBE_NO_RESPONSE:
        return BZM_BRINGUP_PROBE_NO_RESPONSE;
    default:
        return BZM_BRINGUP_PROBE_IO_ERROR;
    }
}

static bool staged_write_u32(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset, uint32_t value)
{
    if (!staged_lease_check()) {
        return false;
    }
    bzm_serial_transport_t * transport = context;
    uint8_t bytes[4] = {
        value & 0xff,
        (value >> 8) & 0xff,
        (value >> 16) & 0xff,
        (value >> 24) & 0xff,
    };
    return bzm_serial_write_register_to(transport, asic_id, engine_id, offset, bytes, sizeof(bytes));
}

static bool staged_read_u32(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset, uint32_t * value)
{
    if (!staged_lease_check()) {
        return false;
    }
    bzm_serial_transport_t * transport = context;
    uint8_t bytes[4];
    if (value == NULL || !bzm_serial_read_register(transport, asic_id, engine_id, offset, bytes, sizeof(bytes))) {
        return false;
    }
    *value = (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) | ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
    return true;
}

static void staged_delay_ms(void * context, uint32_t delay_ms)
{
    (void) context;
    if (!STAGED_LEASE_IO_OK || !bzm_lease_guard_delay(delay_ms, staged_lease_renew, staged_sleep, NULL)) {
        STAGED_LEASE_IO_OK = false;
    }
}

static uint64_t staged_now_us(void * context)
{
    (void) context;
    return (uint64_t) esp_timer_get_time();
}

static bool staged_telemetry_snapshot(void * context, bzm_telemetry_store_t * snapshot)
{
    if (!staged_lease_check()) {
        return false;
    }
    bzm_serial_transport_t * transport = context;
    /* Pump at least one receive window so stale pre-configuration evidence
     * cannot pass merely because it was already present in the store. */
    (void) bzm_serial_poll(transport, 100);
    return bzm_serial_get_telemetry_snapshot(transport, snapshot);
}

static bool staged_ramp_telemetry_snapshot(void * context, bzm_telemetry_store_t * snapshot)
{
    if (!staged_lease_check()) {
        return false;
    }
    bzm_serial_transport_t * transport = context;
    /* BIRDS waits 30 ms after an activation batch for fresh voltage data. */
    (void) bzm_serial_poll(transport, 30);
    return bzm_serial_get_telemetry_snapshot(transport, snapshot);
}

#ifdef CONFIG_BZM_1002_STAGE6_BALANCED_RAMP
static bool staged_ramp_begin_engine(void * context, uint8_t asic_id, uint16_t engine_id)
{
    (void) context;
    (void) asic_id;
    (void) engine_id;
    return staged_operation_check();
}

static bool staged_ramp_write_control_u32(void * context, uint8_t asic_id, uint8_t offset, uint32_t value)
{
    uint8_t bytes[4] = {
        value & 0xff,
        (value >> 8) & 0xff,
        (value >> 16) & 0xff,
        (value >> 24) & 0xff,
    };
    return staged_operation_check() &&
           bzm_serial_write_register_to(context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, offset, bytes, sizeof(bytes));
}

static bool staged_ramp_read_control_u32(void * context, uint8_t asic_id, uint8_t offset, uint32_t * value)
{
    uint8_t bytes[4];
    if (value == NULL || !staged_operation_check() ||
        !bzm_serial_read_register(context, asic_id, BZM_BRINGUP_CONTROL_ENGINE_ID, offset, bytes, sizeof(bytes))) {
        return false;
    }
    *value = (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) | ((uint32_t) bytes[2] << 16) |
             ((uint32_t) bytes[3] << 24);
    return true;
}

static bool staged_ramp_wait_tdm_idle(bzm_serial_transport_t * transport)
{
    /* ASICs occupy slots 10, 20, 30, and 40 in a 100-slot TDM cycle. Finish
     * the current response and require a one-millisecond byte-free interval
     * with no partial parser frame before changing TDM state. */
    for (uint8_t attempt = 0; attempt < 64; ++attempt) {
        size_t frames = bzm_serial_poll(transport, 1);
        bzm_serial_parser_stats_t stats;
        if (!bzm_serial_get_parser_stats(transport, &stats)) {
            return false;
        }
        if (frames == 0 && stats.buffered_bytes == 0) {
            return true;
        }
    }
    bzm_serial_parser_stats_t stats = {0};
    (void) bzm_serial_get_parser_stats(transport, &stats);
    ESP_LOGE(TAG, "Stage 6 could not reach TDM idle gap: frames=%lu discarded=%lu buffered=%u",
             (unsigned long) stats.emitted_frames, (unsigned long) stats.discarded_bytes, (unsigned) stats.buffered_bytes);
    return false;
}

static bool staged_ramp_capture_batch_telemetry(bzm_serial_transport_t * transport)
{
    if (!STAGED_RAMP_TELEMETRY_POLICY_READY) {
        return false;
    }
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    bzm_telemetry_confirmation_t confirmation;
    bzm_telemetry_confirmation_init(&confirmation);
    uint8_t attempts = STAGED_RAMP_TELEMETRY_POLICY.ch2_confirm_samples * BZM_BRINGUP_ASIC_COUNT;
    for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
        if (attempt != 0) {
            (void) bzm_serial_poll(transport, 30);
        }
        if (!staged_ramp_wait_tdm_idle(transport)) {
            return false;
        }

        bzm_telemetry_store_t snapshot;
        if (!bzm_serial_get_telemetry_snapshot(transport, &snapshot)) {
            return false;
        }
        uint64_t now_us = (uint64_t) esp_timer_get_time();
        bool hard_fault = false;
        for (size_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
            uint8_t asic_id = bzm_asic_wire_ids[index];
            const bzm_telemetry_sample_t * sample = bzm_telemetry_store_get(&snapshot, asic_id);
            if (bzm_telemetry_sample_has_immediate_trip(sample)) {
                hard_fault = true;
            }
        }
        uint8_t culprit = 0;
        uint8_t observed = 0;
        bzm_ch2_confirmation_result_t confirmation_result = bzm_telemetry_confirmation_observe(
            &confirmation, &snapshot, now_us, STAGED_RAMP_TELEMETRY_POLICY.max_age_us,
            &STAGED_RAMP_TELEMETRY_POLICY.bounds, true, STAGED_RAMP_TELEMETRY_POLICY.ch2_confirm_samples,
            &culprit, &observed);
        if (confirmation_result == BZM_CH2_CONFIRMATION_GOOD) {
            STAGED_BATCH_TELEMETRY = snapshot;
            STAGED_BATCH_TELEMETRY_ACTIVE = true;
            return true;
        }

        const bzm_telemetry_sample_t * culprit_sample = bzm_telemetry_store_get(&snapshot, culprit);

        ESP_LOGW(TAG,
                 "Stage 6 pre-activation telemetry retry: attempt=%u/%u asic=0x%02x consecutive=%u/%u received=%u valid=%u "
                 "thermal(en=%u validity=%u fault=%u trip=%u) voltage(en=%u fault=%u trip=%u) pll=%u "
                 "ch0=%.2f ch1=%.2f ch2=%.2f hard=%u",
                 attempt + 1U, attempts, culprit, observed, STAGED_RAMP_TELEMETRY_POLICY.ch2_confirm_samples,
                 culprit_sample != NULL && culprit_sample->received,
                 culprit_sample != NULL && culprit_sample->valid, culprit_sample != NULL && culprit_sample->thermal_enabled,
                 culprit_sample != NULL && culprit_sample->thermal_validity,
                 culprit_sample != NULL && culprit_sample->thermal_fault,
                 culprit_sample != NULL && culprit_sample->thermal_trip,
                 culprit_sample != NULL && culprit_sample->voltage_enabled,
                 culprit_sample != NULL && culprit_sample->voltage_fault,
                 culprit_sample != NULL && culprit_sample->voltage_trip, culprit_sample != NULL && culprit_sample->pll_locked,
                 culprit_sample != NULL ? culprit_sample->ch0_mv : 0.0f,
                 culprit_sample != NULL ? culprit_sample->ch1_mv : 0.0f,
                 culprit_sample != NULL ? culprit_sample->ch2_mv : 0.0f, hard_fault);
        if (hard_fault || confirmation_result == BZM_CH2_CONFIRMATION_CONTINUOUS ||
            confirmation_result == BZM_CH2_CONFIRMATION_INVALID) {
            return false;
        }
    }
    return false;
}

static bool staged_ramp_set_tdm(void * context, bool enabled)
{
    bzm_serial_transport_t * transport = context;
    uint32_t control = bzm_bringup_reference_tdm_control();
    if (!enabled) {
        control &= ~1U;
    }
    if (!staged_lease_check()) {
        return false;
    }
    if (!enabled && !staged_ramp_capture_batch_telemetry(transport)) {
        return false;
    }
    /* One broadcast reaches every ASIC inside the same TDM idle window. Four
     * sequential unacknowledged writes can otherwise leave the chain in a
     * mixed transport mode if the later write overlaps the next slot burst. */
    if (!staged_ramp_write_control_u32(transport, BZM_ALL_ASICS, BZM_LOCAL_REG_UART_TDM_CONTROL, control)) {
        return false;
    }
    if (SERIAL_wait_tx_done(100) != ESP_OK) {
        return false;
    }
    if (!enabled) {
        for (size_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
            uint8_t asic_id = bzm_asic_wire_ids[index];
            uint32_t actual = 0;
            bool read_ok = false;
            for (uint8_t attempt = 0; attempt < 3 && !read_ok; ++attempt) {
                read_ok = staged_ramp_read_control_u32(transport, asic_id, BZM_LOCAL_REG_UART_TDM_CONTROL, &actual);
                if (!read_ok && attempt < 2) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
            if (!read_ok || actual != control) {
                ESP_LOGE(TAG,
                         "Stage 6 TDM pause readback failed: asic=0x%02x expected=0x%08lx actual=0x%08lx read_ok=%u",
                         asic_id, (unsigned long) control, (unsigned long) actual, read_ok);
                return false;
            }
        }
    }
    return staged_operation_check();
}

static bool staged_balanced_batch_begin(void * context, uint16_t pair_index)
{
    (void) pair_index;
    STAGED_ENGINE_WINDOW_ACTIVE = false;
    if (!staged_ramp_set_tdm(context, false) ||
        !bzm_serial_get_parser_stats(context, &STAGED_ENGINE_WINDOW_BASELINE) ||
        STAGED_ENGINE_WINDOW_BASELINE.queued_results != 0 || STAGED_ENGINE_WINDOW_BASELINE.buffered_bytes != 0) {
        return false;
    }
    STAGED_ENGINE_WINDOW_ACTIVE = true;
    return true;
}

static bool staged_balanced_batch_end(void * context, uint16_t pair_index)
{
    (void) pair_index;
    bzm_serial_parser_stats_t current = {0};
    if (!STAGED_ENGINE_WINDOW_ACTIVE || !bzm_serial_get_parser_stats(context, &current) ||
        !bzm_balanced_ramp_parser_window_is_clean(&STAGED_ENGINE_WINDOW_BASELINE, &current)) {
        ESP_LOGE(TAG,
                 "Stage 6 engine parser window failed: discarded=%lu/%lu unexpected=%lu/%lu dropped=%lu/%lu "
                 "rejected=%lu/%lu unmatched=%lu/%lu telemetry_decode=%lu/%lu queued=%u buffered=%u",
                 (unsigned long) current.discarded_bytes,
                 (unsigned long) STAGED_ENGINE_WINDOW_BASELINE.discarded_bytes,
                 (unsigned long) current.unexpected_register_headers,
                 (unsigned long) STAGED_ENGINE_WINDOW_BASELINE.unexpected_register_headers,
                 (unsigned long) current.dropped_results, (unsigned long) STAGED_ENGINE_WINDOW_BASELINE.dropped_results,
                 (unsigned long) current.rejected_result_frames,
                 (unsigned long) STAGED_ENGINE_WINDOW_BASELINE.rejected_result_frames,
                 (unsigned long) current.unmatched_register_frames,
                 (unsigned long) STAGED_ENGINE_WINDOW_BASELINE.unmatched_register_frames,
                 (unsigned long) current.telemetry_decode_failures,
                 (unsigned long) STAGED_ENGINE_WINDOW_BASELINE.telemetry_decode_failures,
                 (unsigned) current.queued_results, (unsigned) current.buffered_bytes);
        return false;
    }
    STAGED_ENGINE_WINDOW_ACTIVE = false;
    bool resumed = staged_ramp_set_tdm(context, true);
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    return resumed;
}

static bool staged_ramp_write_register(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset,
                                       const void * data, size_t data_len)
{
    return staged_operation_check() &&
           bzm_serial_write_register_to(context, asic_id, engine_id, offset, data, data_len);
}

static bool staged_ramp_read_register(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset, void * data,
                                      size_t data_len)
{
    if (!staged_operation_check()) {
        return false;
    }
    int64_t started_us = esp_timer_get_time();
    bool result = bzm_serial_read_register(context, asic_id, engine_id, offset, data, data_len);
    int64_t elapsed_us = esp_timer_get_time() - started_us;
    if (elapsed_us > 20000) {
        ESP_LOGW(TAG, "Stage 6 slow register read: asic=0x%02x engine=0x%03x reg=0x%02x elapsed=%lld us result=%d",
                 asic_id, engine_id, offset, elapsed_us, result);
    }
    return result;
}

static void staged_ramp_delay_ms(void * context, uint32_t delay_ms)
{
    (void) context;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static bool staged_ramp_telemetry_sample(void * context, uint8_t asic_id, bzm_telemetry_sample_t * sample)
{
    if (!staged_operation_check() || sample == NULL) {
        return false;
    }
    if (STAGED_BATCH_TELEMETRY_ACTIVE) {
        const bzm_telemetry_sample_t * cached = bzm_telemetry_store_get(&STAGED_BATCH_TELEMETRY, asic_id);
        if (cached == NULL) {
            return false;
        }
        *sample = *cached;
        return true;
    }
    return bzm_serial_get_telemetry(context, asic_id, sample);
}

static bool staged_ramp_parser_stats(void * context, bzm_serial_parser_stats_t * stats)
{
    return staged_operation_check() && bzm_serial_get_parser_stats(context, stats);
}

static bool staged_ramp_final_barrier(void * context)
{
    bzm_serial_transport_t * transport = context;
    if (!staged_lease_check() || SERIAL_wait_tx_done(1000) != ESP_OK) {
        return false;
    }
    (void) bzm_serial_poll(transport, 30);
    return staged_lease_check();
}

static const bzm_balanced_ramp_ops_t STAGED_BALANCED_RAMP_OPS = {
    .begin_engine = staged_ramp_begin_engine,
    .write_register = staged_ramp_write_register,
    .read_register = staged_ramp_read_register,
    .delay_ms = staged_ramp_delay_ms,
    .telemetry_sample = staged_ramp_telemetry_sample,
    .parser_stats = staged_ramp_parser_stats,
    .final_barrier = staged_ramp_final_barrier,
};

static bool staged_balanced_pair_commit(void * context, uint8_t asic_id, const bzm_engine_pair_t * pair)
{
    int64_t started_us = esp_timer_get_time();
    bool committed = bzm_balanced_ramp_commit_pair(&STAGED_BALANCED_RAMP, &STAGED_BALANCED_RAMP_OPS, context, asic_id, pair);
    int64_t elapsed_us = esp_timer_get_time() - started_us;
    if (pair != NULL && pair->pair_index < 2) {
        ESP_LOGI(TAG, "Stage 6 pair timing: pair=%u asic=0x%02x elapsed=%lld us committed=%d",
                 pair->pair_index, asic_id, elapsed_us, committed);
    }
    if (!committed) {
        ESP_LOGE(TAG,
                 "Stage 6 pair failed: cause=%s asic=0x%02x engine=0x%03x reg=0x%02x expected=%lu actual=%lu "
                 "pairs=%u engines=%u",
                 bzm_balanced_ramp_failure_name(STAGED_BALANCED_RAMP.failure), STAGED_BALANCED_RAMP.failure_asic_id,
                 STAGED_BALANCED_RAMP.failure_engine_id, STAGED_BALANCED_RAMP.failure_register_offset,
                 (unsigned long) STAGED_BALANCED_RAMP.failure_expected, (unsigned long) STAGED_BALANCED_RAMP.failure_actual,
                 STAGED_BALANCED_RAMP.completed_pairs, STAGED_BALANCED_RAMP.completed_engines);
    }
    return committed;
}

static bool staged_activation_barrier(void * context, size_t asic_count, size_t pairs_per_asic)
{
    bzm_serial_parser_stats_t transition_stats = {0};
    if (bzm_serial_get_parser_stats(context, &transition_stats)) {
        (void) bzm_balanced_ramp_accept_transition_discards(&STAGED_BALANCED_RAMP, &transition_stats);
    }
    bool passed = bzm_balanced_ramp_barrier(&STAGED_BALANCED_RAMP, &STAGED_BALANCED_RAMP_OPS, context, asic_count,
                                            pairs_per_asic);
    if (!passed) {
        bzm_serial_parser_stats_t current = {0};
        (void) bzm_serial_get_parser_stats(context, &current);
        const bzm_serial_parser_stats_t * baseline = &STAGED_BALANCED_RAMP.parser_baseline;
        ESP_LOGE(TAG,
                 "Stage 6 barrier failed: cause=%s expected=%lu actual=%lu "
                 "discarded=%lu/%lu unexpected_register=%lu/%lu dropped=%lu/%lu rejected=%lu/%lu "
                 "unmatched=%lu/%lu telemetry_decode=%lu/%lu queued=%u buffered=%u",
                 bzm_balanced_ramp_failure_name(STAGED_BALANCED_RAMP.failure),
                 (unsigned long) STAGED_BALANCED_RAMP.failure_expected,
                 (unsigned long) STAGED_BALANCED_RAMP.failure_actual, (unsigned long) current.discarded_bytes,
                 (unsigned long) baseline->discarded_bytes, (unsigned long) current.unexpected_register_headers,
                 (unsigned long) baseline->unexpected_register_headers, (unsigned long) current.dropped_results,
                 (unsigned long) baseline->dropped_results, (unsigned long) current.rejected_result_frames,
                 (unsigned long) baseline->rejected_result_frames, (unsigned long) current.unmatched_register_frames,
                 (unsigned long) baseline->unmatched_register_frames, (unsigned long) current.telemetry_decode_failures,
                 (unsigned long) baseline->telemetry_decode_failures, (unsigned) current.queued_results,
                 (unsigned) current.buffered_bytes);
    }
    return passed;
}
#endif

static void staged_log_telemetry_diagnostics(void)
{
    bzm_telemetry_store_t snapshot;
    if (bzm_serial_get_telemetry_snapshot(&TRANSPORT, &snapshot)) {
        uint64_t now_us = (uint64_t) esp_timer_get_time();
        for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
            uint8_t asic_id = bzm_asic_wire_ids[index];
            const bzm_telemetry_sample_t * sample = bzm_telemetry_store_get(&snapshot, asic_id);
            if (sample == NULL || !sample->received) {
                ESP_LOGW(TAG, "stage telemetry ASIC 0x%02x missing", asic_id);
                continue;
            }
            uint64_t age_us = now_us >= sample->timestamp_us ? now_us - sample->timestamp_us : UINT64_MAX;
            ESP_LOGI(TAG,
                     "stage telemetry ASIC 0x%02x age=%llu us temp=%.2f C/code=0x%03x "
                     "ch0=%.2f mV/0x%04x ch1=%.2f mV/0x%04x ch2=%.2f mV/0x%04x "
                     "thermal(en=%u valid=%u fault=%u trip=%u) voltage(en=%u fault=%u trip=%u) "
                     "pll_pair_lock=%u",
                     asic_id, (unsigned long long) age_us, sample->temperature_c, sample->temperature_code, sample->ch0_mv,
                     sample->ch0_code, sample->ch1_mv, sample->ch1_code, sample->ch2_mv, sample->ch2_code, sample->thermal_enabled,
                     sample->thermal_validity, sample->thermal_fault, sample->thermal_trip, sample->voltage_enabled,
                     sample->voltage_fault, sample->voltage_trip, sample->pll_locked);
        }
    }

    bzm_serial_parser_stats_t stats;
    if (bzm_serial_get_parser_stats(&TRANSPORT, &stats)) {
        ESP_LOGI(TAG, "stage parser frames=%lu discarded=%lu unexpected_register=%lu telemetry_decode_failures=%lu buffered=%u",
                 (unsigned long) stats.emitted_frames, (unsigned long) stats.discarded_bytes,
                 (unsigned long) stats.unexpected_register_headers, (unsigned long) stats.telemetry_decode_failures,
                 (unsigned) stats.buffered_bytes);
    }
}

static const bzm_bringup_ops_t STAGED_OPS = {
    .probe_noop = staged_probe_noop,
    .write_u32 = staged_write_u32,
    .read_u32 = staged_read_u32,
    .delay_ms = staged_delay_ms,
    .now_us = staged_now_us,
    .telemetry_snapshot = staged_telemetry_snapshot,

#ifdef CONFIG_BZM_1002_STAGE6_BALANCED_RAMP
    .balanced_batch_begin = staged_balanced_batch_begin,
    .balanced_pair_commit = staged_balanced_pair_commit,
    .balanced_batch_end = staged_balanced_batch_end,
    .activation_barrier = staged_activation_barrier,
#else
    .balanced_batch_begin = NULL,
    .balanced_pair_commit = NULL,
    .balanced_batch_end = NULL,
    .activation_barrier = NULL,
#endif
};

static const bzm_bringup_ops_t STAGED_RAMP_OPS = {
    .probe_noop = staged_probe_noop,
    .write_u32 = staged_write_u32,
    .read_u32 = staged_read_u32,
    .delay_ms = staged_delay_ms,
    .now_us = staged_now_us,
    .telemetry_snapshot = staged_ramp_telemetry_snapshot,
#ifdef CONFIG_BZM_1002_STAGE6_BALANCED_RAMP
    .balanced_batch_begin = staged_balanced_batch_begin,
    .balanced_pair_commit = staged_balanced_pair_commit,
    .balanced_batch_end = staged_balanced_batch_end,
    .activation_barrier = staged_activation_barrier,
#else
    .balanced_batch_begin = NULL,
    .balanced_pair_commit = NULL,
    .balanced_batch_end = NULL,
    .activation_barrier = NULL,
#endif
};

static bool staged_mining_write_work(void * context, const bzm_work_t * work)
{
    /* Called once per engine from the reactor while REACTOR_LOCK is held.
     * Re-checking the external lease/interlock callback here stops a long
     * multi-engine dispatch as soon as authorization is withdrawn. */
    if (!STAGED_TRANSPORT_READY || !STAGED_BRINGUP.running_verified || !bzm_dispatch_gate_is_authorized(&STAGED_DISPATCH_GATE)) {
        return false;
    }
    return bzm_serial_write_work(context, work);
}

static bool staged_mining_dispatch_checkpoint(void * context)
{
    bzm_serial_transport_t * transport = context;
    if (transport == NULL || !STAGED_TRANSPORT_READY ||
        !STAGED_BRINGUP.running_verified ||
        !bzm_dispatch_gate_is_authorized(&STAGED_DISPATCH_GATE) ||
        !staged_mining_lease_service_due() ||
        SERIAL_wait_tx_done(100) != ESP_OK) {
        return false;
    }

    /* A full 236-engine dispatch otherwise keeps the 5 Mbaud ESP-to-bridge
     * link continuously busy for about one second. The bridge is also
     * forwarding addressed TDM telemetry/results in the other direction.
     * Give that receive path a bounded idle interval, then drain the ESP RX
     * ring before programming the next logical engine. Parser integrity is
     * still enforced without tolerance by the runtime baseline. */
#if CONFIG_BZM_1002_STAGE7_DISPATCH_GAP_US > 0
    esp_rom_delay_us(CONFIG_BZM_1002_STAGE7_DISPATCH_GAP_US);
#endif
    (void) bzm_serial_poll(transport, 1);
    return bzm_dispatch_gate_is_authorized(&STAGED_DISPATCH_GATE);
}

static const bzm_transport_ops_t STAGED_MINING_OPS = {
    .write_work = staged_mining_write_work,
    .dispatch_checkpoint = staged_mining_dispatch_checkpoint,
    .flush = bzm_serial_flush,
};

static bool staged_fail_closed_locked(void)
{
    INITIALIZED = false;
    STAGED_TRANSPORT_READY = false;
    STAGED_LEASE_IO_OK = false;
    STAGED_LEASE_SCHEDULE = (bzm_lease_guard_schedule_t){0};
    TRANSPORT.asic_count = 0;
    bzm_bringup_init(&STAGED_BRINGUP);
    bzm_balanced_ramp_init(&STAGED_BALANCED_RAMP);
    STAGED_ENGINE_WINDOW_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_ENGINE_WINDOW_ACTIVE = false;
    STAGED_SENSOR_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_SENSOR_PARSER_BASELINE_VALID = false;
    STAGED_RUNNING_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_RUNNING_PARSER_BASELINE_VALID = false;
    STAGED_RAMP_TELEMETRY_POLICY_READY = false;
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    bzm_dispatch_gate_init(&STAGED_DISPATCH_GATE);
    return BZM_bridge_set_asic_reset(false) == ESP_OK;
}

bzm_bringup_outcome_t BZM_staged_initialize(GlobalState * state, bzm_bringup_report_t * report)
{
    if (state == NULL || state->DEVICE_CONFIG.family.asic.core_count != BZM_ENGINES_PER_ASIC ||
        state->DEVICE_CONFIG.family.asic_count != BZM_BRINGUP_ASIC_COUNT) {
        pthread_mutex_lock(&REACTOR_LOCK);
        (void) staged_fail_closed_locked();
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_INVALID_ARGUMENT);
        return BZM_BRINGUP_BAD;
    }

    pthread_mutex_lock(&REACTOR_LOCK);
    INITIALIZED = false;
    STAGED_TRANSPORT_READY = false;
    STAGED_LEASE_IO_OK = true;
    STAGED_LEASE_SCHEDULE = (bzm_lease_guard_schedule_t){0};
    bzm_bringup_init(&STAGED_BRINGUP);
    bzm_balanced_ramp_init(&STAGED_BALANCED_RAMP);
    STAGED_ENGINE_WINDOW_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_ENGINE_WINDOW_ACTIVE = false;
    STAGED_SENSOR_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_SENSOR_PARSER_BASELINE_VALID = false;
    STAGED_RUNNING_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_RUNNING_PARSER_BASELINE_VALID = false;
    STAGED_RAMP_TELEMETRY_POLICY_READY = false;
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    bzm_dispatch_gate_init(&STAGED_DISPATCH_GATE);

    bool lease_ready = staged_lease_check();
    bool reset_held = lease_ready && BZM_bridge_set_asic_reset(false) == ESP_OK;
    bool serial_ready = SERIAL_prepare_session(BZM_BAUD_RATE) == ESP_OK;
    bzm_serial_transport_deinit(&TRANSPORT);
    TRANSPORT = (bzm_serial_transport_t){
        .engine_count = BZM_ENGINES_PER_ASIC,
        .enhanced_mode = true,
    };
    bool transport_ready = bzm_serial_transport_init(&TRANSPORT);
    STAGED_TRANSPORT_READY = lease_ready && reset_held && serial_ready && transport_ready;
    if (!STAGED_TRANSPORT_READY) {
        (void) staged_fail_closed_locked();
    }
    pthread_mutex_unlock(&REACTOR_LOCK);

    if (!STAGED_TRANSPORT_READY) {
        staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO);
        return BZM_BRINGUP_BAD;
    }
    staged_report(report, BZM_BRINGUP_GOOD, BZM_BRINGUP_REASON_NONE);
    return BZM_BRINGUP_GOOD;
}

bzm_bringup_outcome_t BZM_staged_chain4(bzm_bringup_report_t * report)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    if (!STAGED_TRANSPORT_READY) {
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE);
        return BZM_BRINGUP_BLOCKED;
    }

    /* Renew as the immediately preceding bridge command. The pulse itself
     * contains only a bounded 200 ms reset-low interval. */
    if (!staged_lease_check() || BZM_bridge_pulse_asic_reset() != ESP_OK) {
        (void) staged_fail_closed_locked();
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO);
        return BZM_BRINGUP_BAD;
    }
    staged_delay_ms(&TRANSPORT, 1000);

    bzm_bringup_outcome_t outcome = bzm_bringup_stage_chain4(&STAGED_BRINGUP, &STAGED_OPS, &TRANSPORT, report);
    if (outcome == BZM_BRINGUP_GOOD) {
        TRANSPORT.asic_count = BZM_BRINGUP_ASIC_COUNT;
        for (uint8_t index = 0; index < BZM_BRINGUP_ASIC_COUNT; ++index) {
            TRANSPORT.asic_ids[index] = bzm_asic_wire_ids[index];
        }
    } else {
        (void) staged_fail_closed_locked();
    }
    pthread_mutex_unlock(&REACTOR_LOCK);
    return outcome;
}

bzm_bringup_outcome_t BZM_staged_sensors(const bzm_bringup_sensor_profile_t * profile,
                                         const bzm_bringup_telemetry_policy_t * telemetry_policy, bzm_bringup_report_t * report)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    if (!STAGED_TRANSPORT_READY) {
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE);
        return BZM_BRINGUP_BLOCKED;
    }
    STAGED_SENSOR_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_SENSOR_PARSER_BASELINE_VALID = false;
    bzm_serial_parser_stats_t pre_sensor_baseline = {0};
    bool baseline_ready = bzm_serial_get_parser_stats(&TRANSPORT, &pre_sensor_baseline) &&
                          pre_sensor_baseline.queued_results == 0 && pre_sensor_baseline.buffered_bytes == 0;
    bzm_bringup_outcome_t outcome = baseline_ready
        ? bzm_bringup_stage_sensors(&STAGED_BRINGUP, &STAGED_OPS, &TRANSPORT, profile, telemetry_policy, report)
        : BZM_BRINGUP_BAD;
    if (!baseline_ready) {
        staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_ACTIVATION_BARRIER);
    } else if (outcome == BZM_BRINGUP_GOOD &&
               !staged_settle_parser(&TRANSPORT, "Stage 4", &pre_sensor_baseline,
                                     &STAGED_SENSOR_PARSER_BASELINE)) {
        STAGED_BRINGUP.sensors_verified = false;
        staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_ACTIVATION_BARRIER);
        outcome = BZM_BRINGUP_BAD;
    } else if (outcome == BZM_BRINGUP_GOOD) {
        STAGED_SENSOR_PARSER_BASELINE_VALID = true;
    }
    staged_log_telemetry_diagnostics();
    if (outcome != BZM_BRINGUP_GOOD)
        (void) staged_fail_closed_locked();
    pthread_mutex_unlock(&REACTOR_LOCK);
    return outcome;
}

bzm_bringup_outcome_t BZM_staged_clocks(const bzm_bringup_pll_profile_t * profile,
                                        const bzm_bringup_telemetry_policy_t * telemetry_policy, bzm_bringup_report_t * report)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    if (!STAGED_TRANSPORT_READY) {
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE);
        return BZM_BRINGUP_BLOCKED;
    }
    bzm_bringup_outcome_t outcome =
        bzm_bringup_stage_clocks(&STAGED_BRINGUP, &STAGED_OPS, &TRANSPORT, profile, telemetry_policy, report);
    if (outcome != BZM_BRINGUP_GOOD)
        (void) staged_fail_closed_locked();
    pthread_mutex_unlock(&REACTOR_LOCK);
    return outcome;
}

bzm_bringup_outcome_t BZM_staged_balanced_ramp(const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                               bzm_bringup_report_t * report)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    if (!STAGED_TRANSPORT_READY) {
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE);
        return BZM_BRINGUP_BLOCKED;
    }
    STAGED_RAMP_TELEMETRY_POLICY_READY = telemetry_policy != NULL;
    if (telemetry_policy != NULL) {
        STAGED_RAMP_TELEMETRY_POLICY = *telemetry_policy;
    }
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    bzm_bringup_outcome_t outcome =
        bzm_bringup_stage_balanced_ramp(&STAGED_BRINGUP, &STAGED_RAMP_OPS, &TRANSPORT, telemetry_policy, report);
    STAGED_RAMP_TELEMETRY_POLICY_READY = false;
    STAGED_BATCH_TELEMETRY_ACTIVE = false;
    if (outcome != BZM_BRINGUP_GOOD && report != NULL && STAGED_BALANCED_RAMP.failure != BZM_BALANCED_RAMP_FAILURE_NONE) {
        report->asic_id = STAGED_BALANCED_RAMP.failure_asic_id;
        report->register_offset = STAGED_BALANCED_RAMP.failure_register_offset;
        report->expected = STAGED_BALANCED_RAMP.failure_expected;
        report->actual = STAGED_BALANCED_RAMP.failure_actual;
    }
    if (outcome != BZM_BRINGUP_GOOD)
        (void) staged_fail_closed_locked();
    pthread_mutex_unlock(&REACTOR_LOCK);
    return outcome;
}

static bool staged_poll_parser_settle_window(bzm_serial_transport_t * transport, uint32_t window_ms,
                                             bzm_serial_parser_stats_t * stats)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t) window_ms * 1000;
    while (esp_timer_get_time() < deadline_us) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        uint16_t timeout_ms = (uint16_t) ((remaining_us + 999) / 1000);
        if (timeout_ms > 10) {
            timeout_ms = 10;
        }
        if (timeout_ms == 0) {
            timeout_ms = 1;
        }
        (void) bzm_serial_poll(transport, timeout_ms);
    }

    /* End at a parser frame boundary. Valid frames may arrive in every poll
     * with four spaced TDM senders, so requiring a frame-free millisecond can
     * falsely time out even though no partial frame remains. */
    for (uint8_t attempt = 0; attempt < 64; ++attempt) {
        (void) bzm_serial_poll(transport, 1);
        if (!bzm_serial_get_parser_stats(transport, stats)) {
            return false;
        }
        if (bzm_parser_settling_snapshot_at_frame_boundary(stats)) {
            return true;
        }
    }
    return false;
}

static bool staged_settle_parser(bzm_serial_transport_t * transport, const char * stage_name,
                                 const bzm_serial_parser_stats_t * initial_baseline,
                                 bzm_serial_parser_stats_t * accepted_baseline)
{
    if (transport == NULL || stage_name == NULL || initial_baseline == NULL || accepted_baseline == NULL) {
        return false;
    }

    bzm_parser_settling_t settling;
    bzm_parser_settling_init(&settling, initial_baseline);
    bzm_serial_parser_stats_t current = *initial_baseline;
    bzm_parser_settling_result_t result = BZM_PARSER_SETTLING_PENDING;
    for (uint8_t window = 0; result == BZM_PARSER_SETTLING_PENDING && window < BZM_PARSER_SETTLE_MAX_WINDOWS; ++window) {
        if (!staged_lease_check() ||
            !staged_poll_parser_settle_window(transport, BZM_PARSER_SETTLE_WINDOW_MS, &current)) {
            result = BZM_PARSER_SETTLING_BAD;
            break;
        }
        uint32_t discarded_before = settling.baseline.discarded_bytes;
        result = bzm_parser_settling_observe(&settling, &current, BZM_PARSER_SETTLE_REQUIRED_CLEAN_WINDOWS);
        if (result != BZM_PARSER_SETTLING_BAD && current.discarded_bytes != discarded_before) {
            ESP_LOGW(TAG,
                     "%s settling accepted transition discard burst: baseline=%lu current=%lu; next stage remains closed",
                     stage_name, (unsigned long) discarded_before, (unsigned long) current.discarded_bytes);
        }
    }

    if (result != BZM_PARSER_SETTLING_COMPLETE) {
        ESP_LOGE(TAG,
                 "%s parser did not settle: cleanWindows=%u/%u discarded=%lu unexpected=%lu dropped=%lu "
                 "rejected=%lu unmatched=%lu telemetryDecode=%lu queued=%u buffered=%u",
                 stage_name, settling.clean_windows, BZM_PARSER_SETTLE_REQUIRED_CLEAN_WINDOWS,
                 (unsigned long) current.discarded_bytes, (unsigned long) current.unexpected_register_headers,
                 (unsigned long) current.dropped_results, (unsigned long) current.rejected_result_frames,
                 (unsigned long) current.unmatched_register_frames, (unsigned long) current.telemetry_decode_failures,
                 (unsigned) current.queued_results, (unsigned) current.buffered_bytes);
        if (current.recent_discarded_length != 0) {
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, current.recent_discarded_bytes,
                                     current.recent_discarded_length, ESP_LOG_ERROR);
        }
        return false;
    }

    *accepted_baseline = settling.baseline;
    return true;
}

bzm_bringup_outcome_t BZM_staged_running(GlobalState * state, const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                         bzm_bringup_report_t * report)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    if (!STAGED_TRANSPORT_READY) {
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BLOCKED, BZM_BRINGUP_REASON_PREREQUISITE);
        return BZM_BRINGUP_BLOCKED;
    }
    if (state == NULL) {
        (void) staged_fail_closed_locked();
        pthread_mutex_unlock(&REACTOR_LOCK);
        staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_INVALID_ARGUMENT);
        return BZM_BRINGUP_BAD;
    }
    STAGED_RUNNING_PARSER_BASELINE = (bzm_serial_parser_stats_t){0};
    STAGED_RUNNING_PARSER_BASELINE_VALID = false;
    bzm_bringup_outcome_t outcome = bzm_bringup_check_running(&STAGED_BRINGUP, &STAGED_OPS, &TRANSPORT, telemetry_policy, report);
    if (outcome == BZM_BRINGUP_GOOD) {
        bzm_reactor_config_t config = {
            .engine_count = BZM_ENGINES_PER_ASIC,
            .timestamp_count = 16,
            .lead_zeros = CONFIG_BZM_1002_STAGE7_LEAD_ZEROS,
            .nonce_offset = BZM_NONCE_GAP_1002,
            .enhanced_mode = true,
        };
        INITIALIZED = bzm_reactor_init(&REACTOR, &state->asic_job_store, &config, &STAGED_MINING_OPS, &TRANSPORT);
        if (!INITIALIZED) {
            STAGED_BRINGUP.running_verified = false;
            staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_IO);
            outcome = BZM_BRINGUP_BAD;
        }
        if (outcome == BZM_BRINGUP_GOOD) {
            bzm_serial_parser_stats_t pre_running_baseline = {0};
            bool baseline_ready = bzm_serial_get_parser_stats(&TRANSPORT, &pre_running_baseline) &&
                                  pre_running_baseline.queued_results == 0 &&
                                  pre_running_baseline.buffered_bytes == 0;
            if (!baseline_ready ||
                !staged_settle_parser(&TRANSPORT, "Stage 7", &pre_running_baseline,
                                      &STAGED_RUNNING_PARSER_BASELINE)) {
                STAGED_BRINGUP.running_verified = false;
                staged_report(report, BZM_BRINGUP_BAD, BZM_BRINGUP_REASON_ACTIVATION_BARRIER);
                outcome = BZM_BRINGUP_BAD;
            } else {
                STAGED_RUNNING_PARSER_BASELINE_VALID = true;
            }
        }
    }
    if (outcome != BZM_BRINGUP_GOOD)
        (void) staged_fail_closed_locked();
    pthread_mutex_unlock(&REACTOR_LOCK);
    return outcome;
}

bool BZM_staged_get_state(bzm_bringup_state_t * state)
{
    if (state == NULL)
        return false;
    pthread_mutex_lock(&REACTOR_LOCK);
    *state = STAGED_BRINGUP;
    pthread_mutex_unlock(&REACTOR_LOCK);
    return true;
}

bool BZM_staged_hold_reset(void)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    bool held = staged_fail_closed_locked();
    pthread_mutex_unlock(&REACTOR_LOCK);
    return held;
}

bool BZM_staged_dispatch_enabled(void)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    bool running = INITIALIZED && STAGED_TRANSPORT_READY && STAGED_BRINGUP.running_verified;
    bzm_dispatch_gate_t gate = STAGED_DISPATCH_GATE;
    pthread_mutex_unlock(&REACTOR_LOCK);
    return running && bzm_dispatch_gate_is_authorized(&gate);
}

void BZM_staged_set_dispatch_authorizer(bzm_dispatch_authorizer_t authorize, void * context)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    bzm_dispatch_gate_set(&STAGED_DISPATCH_GATE, authorize, context);
    pthread_mutex_unlock(&REACTOR_LOCK);
}

void BZM_staged_set_operation_authorizer(bzm_dispatch_authorizer_t authorize, void * context)
{
    pthread_mutex_lock(&REACTOR_LOCK);
    STAGED_OPERATION_AUTHORIZE = authorize;
    STAGED_OPERATION_AUTHORIZE_CONTEXT = context;
    pthread_mutex_unlock(&REACTOR_LOCK);
}

size_t BZM_staged_poll(uint16_t timeout_ms)
{
    if (timeout_ms == 0)
        return 0;
    pthread_mutex_lock(&REACTOR_LOCK);
    size_t frames = STAGED_TRANSPORT_READY ? bzm_serial_poll(&TRANSPORT, timeout_ms) : 0;
    pthread_mutex_unlock(&REACTOR_LOCK);
    return frames;
}
