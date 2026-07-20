#include "bzm_validation_api.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bzm_validation_api_contract.h"
#include "bzm_validation_runtime.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "http_server.h"

#define BZM_VALIDATION_API_MAX_BODY 384U
#define BZM_VALIDATION_API_RECV_RETRIES 3U

static int response_prebuffer_length = 4096;

static bool prepare_request(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        (void) httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return false;
    }
    if (set_cors_headers(req) != ESP_OK) {
        (void) httpd_resp_send_500(req);
        return false;
    }
    return true;
}

static esp_err_t send_json(httpd_req_t * req, const char * status, cJSON * root)
{
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t result = HTTP_send_json(req, root, &response_prebuffer_length);
    cJSON_Delete(root);
    return result;
}

static esp_err_t send_api_error(httpd_req_t * req, const char * status, const char * code, const char * message)
{
    cJSON * root = cJSON_CreateObject();
    if (root == NULL || cJSON_AddBoolToObject(root, "ok", false) == NULL || cJSON_AddStringToObject(root, "error", code) == NULL ||
        cJSON_AddStringToObject(root, "message", message) == NULL) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, status);
        return httpd_resp_sendstr(req, message);
    }
    return send_json(req, status, root);
}

static bool json_content_type(httpd_req_t * req)
{
    static const char expected[] = "application/json";
    char content_type[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, expected, sizeof(expected) - 1) != 0) {
        return false;
    }
    const char * suffix = content_type + sizeof(expected) - 1;
    while (*suffix == ' ' || *suffix == '\t')
        suffix++;
    return *suffix == '\0' || *suffix == ';';
}

static cJSON * receive_json_object(httpd_req_t * req, bool allow_empty)
{
    if (req->content_len == 0) {
        if (allow_empty)
            return cJSON_CreateObject();
        (void) send_api_error(req, "400 Bad Request", "BODY_REQUIRED", "A JSON request body is required");
        return NULL;
    }
    if (req->content_len > BZM_VALIDATION_API_MAX_BODY) {
        (void) send_api_error(req, "413 Payload Too Large", "BODY_TOO_LARGE", "The JSON request body exceeds 384 bytes");
        return NULL;
    }
    if (!json_content_type(req)) {
        (void) send_api_error(req, "415 Unsupported Media Type", "CONTENT_TYPE", "Content-Type must be application/json");
        return NULL;
    }

    char body[BZM_VALIDATION_API_MAX_BODY + 1];
    size_t received = 0;
    unsigned int retries = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && retries++ < BZM_VALIDATION_API_RECV_RETRIES) {
            continue;
        }
        if (count <= 0) {
            (void) send_api_error(req, "408 Request Timeout", "BODY_READ", "The complete JSON body was not received");
            return NULL;
        }
        retries = 0;
        received += (size_t) count;
    }
    if (memchr(body, '\0', received) != NULL) {
        (void) send_api_error(req, "400 Bad Request", "INVALID_JSON", "Embedded NUL bytes are not valid JSON");
        return NULL;
    }
    body[received] = '\0';

    const char * parse_end = NULL;
    cJSON * root = cJSON_ParseWithLengthOpts(body, received + 1, &parse_end, true);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        (void) send_api_error(req, "400 Bad Request", "INVALID_JSON", "The request body must be one JSON object");
        return NULL;
    }
    return root;
}

static bool add_number(cJSON * object, const char * name, double value)
{
    return cJSON_AddNumberToObject(object, name, value) != NULL;
}

static bool add_bool(cJSON * object, const char * name, bool value)
{
    return cJSON_AddBoolToObject(object, name, value) != NULL;
}

static bool add_string(cJSON * object, const char * name, const char * value)
{
    return cJSON_AddStringToObject(object, name, value != NULL ? value : "") != NULL;
}

static bool add_owned_object(cJSON * parent, const char * name, cJSON * child)
{
    if (child == NULL)
        return false;
    if (!cJSON_AddItemToObject(parent, name, child)) {
        cJSON_Delete(child);
        return false;
    }
    return true;
}

static bool add_owned_array_item(cJSON * array, cJSON * item)
{
    if (item == NULL)
        return false;
    if (!cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

static const char * validation_state_name(bzm_validation_state_t state)
{
    switch (state) {
    case BZM_VALIDATION_IDLE:
        return "IDLE";
    case BZM_VALIDATION_EXECUTING:
        return "EXECUTING";
    case BZM_VALIDATION_OFF_SAFE:
        return "OFF_SAFE";
    case BZM_VALIDATION_HOLDING:
        return "HOLDING";
    case BZM_VALIDATION_FAULT_LATCHED:
        return "FAULT_LATCHED";
    case BZM_VALIDATION_SHUTDOWN_UNVERIFIED:
        return "SHUTDOWN_UNVERIFIED";
    default:
        return "INVALID_STATE";
    }
}

static const char * bridge_stage_name(bzm_bridge_safety_stage_t stage)
{
    switch (stage) {
    case BZM_BRIDGE_SAFETY_STAGE_BOOT_SAFE:
        return "BOOT_SAFE";
    case BZM_BRIDGE_SAFETY_STAGE_LEASE:
        return "LEASE";
    case BZM_BRIDGE_SAFETY_STAGE_TRIP_LATCH:
        return "TRIP_LATCH";
    default:
        return "INVALID_STAGE";
    }
}

static const char * bridge_state_name(bzm_bridge_safety_state_t state)
{
    switch (state) {
    case BZM_BRIDGE_SAFETY_STATE_SAFE_OFF:
        return "SAFE_OFF";
    case BZM_BRIDGE_SAFETY_STATE_CONTROLLED:
        return "CONTROLLED";
    case BZM_BRIDGE_SAFETY_STATE_FAULT_LATCHED:
        return "FAULT_LATCHED";
    default:
        return "INVALID_STATE";
    }
}

static const char * bridge_fault_name(bzm_bridge_safety_fault_t fault)
{
    switch (fault) {
    case BZM_BRIDGE_SAFETY_FAULT_NONE:
        return "NONE";
    case BZM_BRIDGE_SAFETY_FAULT_LEASE_EXPIRED:
        return "LEASE_EXPIRED";
    case BZM_BRIDGE_SAFETY_FAULT_ASIC_TRIP:
        return "ASIC_TRIP";
    case BZM_BRIDGE_SAFETY_FAULT_STATUS_INVALID:
        return "STATUS_INVALID";
    default:
        return "INVALID_FAULT";
    }
}

static const char * bridge_runtime_verdict_name(bzm_bridge_safety_runtime_verdict_t verdict)
{
    switch (verdict) {
    case BZM_BRIDGE_SAFETY_RUNTIME_GOOD_SAFE_OFF:
        return "GOOD_SAFE_OFF";
    case BZM_BRIDGE_SAFETY_RUNTIME_GOOD_CONTROLLED:
        return "GOOD_CONTROLLED";
    case BZM_BRIDGE_SAFETY_RUNTIME_BAD_FAULT:
        return "BAD_FAULT";
    case BZM_BRIDGE_SAFETY_RUNTIME_BAD_LEASE:
        return "BAD_LEASE";
    case BZM_BRIDGE_SAFETY_RUNTIME_BAD_TRIP_INPUT:
        return "BAD_TRIP_INPUT";
    case BZM_BRIDGE_SAFETY_RUNTIME_BAD_UNSAFE_OUTPUTS:
        return "BAD_UNSAFE_OUTPUTS";
    default:
        return "INVALID_RUNTIME_VERDICT";
    }
}

static const char * bridge_production_verdict_name(bzm_bridge_safety_production_verdict_t verdict)
{
    switch (verdict) {
    case BZM_BRIDGE_SAFETY_PRODUCTION_GOOD:
        return "GOOD";
    case BZM_BRIDGE_SAFETY_PRODUCTION_BAD_STAGE_DISABLED:
        return "BAD_STAGE_DISABLED";
    case BZM_BRIDGE_SAFETY_PRODUCTION_BAD_CAPABILITY_GAP:
        return "BAD_CAPABILITY_GAP";
    case BZM_BRIDGE_SAFETY_PRODUCTION_BAD_RUNTIME:
        return "BAD_RUNTIME";
    default:
        return "INVALID_PRODUCTION_VERDICT";
    }
}

static cJSON * stage_result_json(const bzm_stage_result_t * result)
{
    cJSON * object = cJSON_CreateObject();
    if (object == NULL || !add_number(object, "status", result->status) ||
        !add_string(object, "statusName", bzm_validation_status_name(result->status)) ||
        !add_number(object, "code", result->code) || !add_string(object, "codeName", bzm_validation_code_name(result->code)) ||
        !add_string(object, "detail", result->detail)) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static cJSON * stage_definition_json(const bzm_stage_definition_t * definition)
{
    cJSON * object = cJSON_CreateObject();
    if (definition == NULL || object == NULL || !add_number(object, "stage", definition->stage) ||
        !add_string(object, "name", definition->name) || !add_string(object, "configuration", definition->configuration) ||
        !add_string(object, "expectedGood", definition->good_criteria) ||
        !add_string(object, "expectedBad", definition->bad_criteria) ||
        !add_bool(object, "energizesAsicRail", definition->energizes_asic_rail) ||
        !add_bool(object, "requiresLocalArm", definition->requires_local_arm)) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static cJSON * stage_definitions_json(void)
{
    cJSON * array = cJSON_CreateArray();
    if (array == NULL)
        return NULL;
    for (int stage = BZM_STAGE_OFF_SAFE; stage < BZM_STAGE_COUNT; ++stage) {
        if (!add_owned_array_item(array, stage_definition_json(bzm_validation_stage_definition((bzm_validation_stage_t) stage)))) {
            cJSON_Delete(array);
            return NULL;
        }
    }
    return array;
}

static cJSON * configuration_json(const bzm_validation_runtime_snapshot_t * snapshot)
{
    const bzm_supervisor_config_t * config = &snapshot->config;
#ifdef CONFIG_BZM_1002_STAGE7_MINING
    const bool stage7_mining_enabled = true;
    const uint32_t stage7_dispatch_gap_us = CONFIG_BZM_1002_STAGE7_DISPATCH_GAP_US;
#else
    const bool stage7_mining_enabled = false;
    const uint32_t stage7_dispatch_gap_us = 0;
#endif
#ifdef CONFIG_BZM_1002_STAGE7_ALLOW_PARSER_REALIGN
    const bool stage7_parser_realign_enabled = true;
#else
    const bool stage7_parser_realign_enabled = false;
#endif
#ifdef CONFIG_BZM_1002_STAGE7_ALLOW_MAPPING_RECOVERY
    const bool stage7_mapping_recovery_enabled = true;
#else
    const bool stage7_mapping_recovery_enabled = false;
#endif
    cJSON * object = cJSON_CreateObject();
    if (object == NULL || !add_number(object, "buildMaxStage", config->build_max_stage) ||
        !add_string(object, "buildMaxStageName", bzm_validation_stage_name(config->build_max_stage)) ||
        !add_number(object, "implementedMaxStage", config->implemented_max_stage) ||
        !add_string(object, "implementedMaxStageName", bzm_validation_stage_name(config->implemented_max_stage)) ||
        !add_bool(object, "poweredStagesCompiled", config->powered_stages_compiled) ||
        !add_bool(object, "productionMode", config->production_mode) ||
        !add_bool(object, "independentKillAvailable", config->independent_kill_available) ||
        !add_bool(object, "allowEspOnlyKillInLab", config->allow_esp_only_kill_in_lab) ||
        !add_bool(object, "boardManagedSafety", config->board_managed_safety) ||
        !add_number(object, "maximumLeaseMs", config->maximum_lease_ms) ||
        !add_number(object, "maximumLeaseSeconds", config->maximum_lease_ms / 1000U) ||
        !add_number(object, "firstPoweredStage", BZM_STAGE_POWER_RAIL) ||
        !add_bool(object, "stage7MiningEnabled", stage7_mining_enabled) ||
        !add_number(object, "stage7ProofTimeoutMs", snapshot->running_evidence_config.proof_timeout_ms) ||
        !add_number(object, "stage7MinimumValidResults", snapshot->running_evidence_config.minimum_valid_results) ||
        !add_bool(object, "stage7MappingRecoveryEnabled", stage7_mapping_recovery_enabled) ||
        !add_number(object, "stage7MaximumMappingRejections", snapshot->running_evidence_config.maximum_mapping_rejections) ||
        !add_number(object, "stage7MaximumLocalRejections", snapshot->running_evidence_config.maximum_local_rejections) ||
        !add_number(object, "stage7MinimumNonceDifficulty", CONFIG_BZM_1002_STAGE7_MIN_NONCE_DIFFICULTY) ||
        !add_number(object, "stage7LeadZeros", CONFIG_BZM_1002_STAGE7_LEAD_ZEROS) ||
        !add_number(object, "stage7DispatchGapUs", stage7_dispatch_gap_us) ||
        !add_bool(object, "stage7ParserRealignEnabled", stage7_parser_realign_enabled) ||
        !add_number(object, "stage7ParserRealignMaxDiscards", CONFIG_BZM_1002_STAGE7_PARSER_REALIGN_MAX_DISCARDS) ||
        !add_number(object, "stage7ParserRealignCleanWindows", CONFIG_BZM_1002_STAGE7_PARSER_REALIGN_CLEAN_WINDOWS) ||
        !add_number(object, "stage7ParserRealignMaxWindows", CONFIG_BZM_1002_STAGE7_PARSER_REALIGN_MAX_WINDOWS) ||
        !add_number(object, "stage7ParserRealignMaxEvents", CONFIG_BZM_1002_STAGE7_PARSER_REALIGN_MAX_EVENTS) ||
        !add_number(object, "sensorTdmGapCount", CONFIG_BZM_1002_SENSOR_TDM_GAP_COUNT) ||
        !add_number(object, "pllLockConfirmSamples", CONFIG_BZM_1002_PLL_LOCK_CONFIRM_SAMPLES) ||
        !add_string(object, "powerConfirmation", BZM_RUNTIME_POWER_CONFIRMATION) ||
        !add_string(object, "armMethod", snapshot->usb_serial_arm_enabled ? "USB_SERIAL_JTAG" : "BOOT_BUTTON") ||
        !add_bool(object, "bootButtonRequired", !snapshot->usb_serial_arm_enabled) ||
        !add_number(object, "localArmWindowMs", snapshot->local_arm_window_ms)) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static cJSON * local_arm_json(const bzm_validation_runtime_snapshot_t * snapshot)
{
    cJSON * object = cJSON_CreateObject();
    if (object == NULL || !add_bool(object, "enabled", snapshot->usb_serial_arm_enabled) ||
        !add_string(object, "method", snapshot->usb_serial_arm_enabled ? "USB_SERIAL_JTAG" : "BOOT_BUTTON") ||
        !add_bool(object, "active", snapshot->local_arm_remaining_ms != 0) ||
        !add_number(object, "remainingMs", snapshot->local_arm_remaining_ms) ||
        !add_number(object, "windowMs", snapshot->local_arm_window_ms) ||
        !add_bool(object, "singleUse", snapshot->usb_serial_arm_enabled) ||
        !add_string(object, "command", snapshot->usb_serial_arm_enabled ? "bzm-arm ENERGIZE_BZM_1002" : "")) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static cJSON * report_json(const bzm_validation_report_t * report)
{
    cJSON * object = cJSON_CreateObject();
    cJSON * stages = NULL;
    if (object == NULL || !add_number(object, "schemaVersion", report->schema_version) ||
        !add_number(object, "runId", report->run_id) || !add_number(object, "requestedStage", report->requested_stage) ||
        !add_string(object, "requestedStageName", bzm_validation_stage_name(report->requested_stage)) ||
        !add_number(object, "buildMaxStage", report->build_max_stage) ||
        !add_string(object, "buildMaxStageName", bzm_validation_stage_name(report->build_max_stage)) ||
        !add_number(object, "implementedMaxStage", report->implemented_max_stage) ||
        !add_string(object, "implementedMaxStageName", bzm_validation_stage_name(report->implemented_max_stage)) ||
        !add_number(object, "reachedStage", report->reached_stage) ||
        !add_string(object, "reachedStageName", bzm_validation_stage_name(report->reached_stage)) ||
        !add_number(object, "overall", report->overall) ||
        !add_string(object, "overallName", bzm_validation_status_name(report->overall)) ||
        !add_number(object, "state", report->state) || !add_string(object, "stateName", validation_state_name(report->state)) ||
        !add_bool(object, "energized", report->energized) || !add_number(object, "leaseMs", report->lease_ms)) {
        cJSON_Delete(object);
        return NULL;
    }

    stages = cJSON_CreateArray();
    if (stages == NULL) {
        cJSON_Delete(object);
        return NULL;
    }
    for (int stage = BZM_STAGE_OFF_SAFE; stage < BZM_STAGE_COUNT; ++stage) {
        cJSON * entry = stage_result_json(&report->stages[stage]);
        if (entry == NULL || !add_number(entry, "stage", stage) ||
            !add_string(entry, "stageName", bzm_validation_stage_name((bzm_validation_stage_t) stage))) {
            cJSON_Delete(entry);
            cJSON_Delete(stages);
            cJSON_Delete(object);
            return NULL;
        }
        if (!add_owned_array_item(stages, entry)) {
            cJSON_Delete(stages);
            cJSON_Delete(object);
            return NULL;
        }
    }
    if (!add_owned_object(object, "stages", stages)) {
        cJSON_Delete(object);
        return NULL;
    }
    if (!add_owned_object(object, "finalSafeOff", stage_result_json(&report->final_safe_off))) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static cJSON * bridge_json(const bzm_validation_runtime_snapshot_t * snapshot)
{
    const bzm_bridge_info_t * info = &snapshot->bridge_info;
    const bzm_bridge_safety_status_t * status = &snapshot->bridge_status;
    cJSON * object = cJSON_CreateObject();
    cJSON * info_object = cJSON_CreateObject();
    cJSON * status_object = cJSON_CreateObject();
    if (object == NULL || info_object == NULL || status_object == NULL ||
        !add_bool(info_object, "valid", snapshot->bridge_info_valid) ||
        !add_number(info_object, "schemaVersion", info->schema_version) ||
        !add_number(info_object, "protocolMajor", info->protocol_major) ||
        !add_number(info_object, "protocolMinor", info->protocol_minor) || !add_string(info_object, "version", info->version) ||
        !add_bool(status_object, "valid", snapshot->bridge_status_valid && status->valid) ||
        !add_bool(status_object, "snapshotValid", snapshot->bridge_status_valid) ||
        !add_bool(status_object, "payloadValid", status->valid) ||
        !add_number(status_object, "schemaVersion", status->schema_version) || !add_number(status_object, "stage", status->stage) ||
        !add_string(status_object, "stageName", bridge_stage_name(status->stage)) ||
        !add_number(status_object, "state", status->state) ||
        !add_string(status_object, "stateName", bridge_state_name(status->state)) ||
        !add_number(status_object, "fault", status->fault) ||
        !add_string(status_object, "faultName", bridge_fault_name(status->fault)) ||
        !add_number(status_object, "runtimeVerdict", status->runtime_verdict) ||
        !add_string(status_object, "runtimeVerdictName", bridge_runtime_verdict_name(status->runtime_verdict)) ||
        !add_number(status_object, "productionVerdict", status->production_verdict) ||
        !add_string(status_object, "productionVerdictName", bridge_production_verdict_name(status->production_verdict)) ||
        !add_number(status_object, "capabilities", status->capabilities) ||
        !add_number(status_object, "evidence", status->evidence) ||
        !add_number(status_object, "leaseRemainingMs", status->lease_remaining_ms) ||
        !add_bool(status_object, "fiveVoltEnabled", status->five_volt_enabled) ||
        !add_bool(status_object, "asicResetAsserted", status->asic_reset_asserted) ||
        !add_bool(status_object, "fanFull", status->fan_full) || !add_number(status_object, "fanPercent", status->fan_percent) ||
        !add_bool(status_object, "tripInputAsserted", status->trip_input_asserted)) {
        cJSON_Delete(info_object);
        cJSON_Delete(status_object);
        cJSON_Delete(object);
        return NULL;
    }
    if (!add_number(object, "fanRpm", snapshot->fan_rpm)) {
        cJSON_Delete(info_object);
        cJSON_Delete(status_object);
        cJSON_Delete(object);
        return NULL;
    }
    if (!add_owned_object(object, "info", info_object)) {
        cJSON_Delete(status_object);
        cJSON_Delete(object);
        return NULL;
    }
    if (!add_owned_object(object, "safetyStatus", status_object)) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static cJSON * lease_json(const bzm_validation_runtime_snapshot_t * snapshot, uint64_t current_ms)
{
    uint64_t remaining_ms = snapshot->lease_deadline_ms > current_ms ? snapshot->lease_deadline_ms - current_ms : 0;
    bool has_deadline = snapshot->lease_deadline_ms != 0;
    cJSON * object = cJSON_CreateObject();
    if (object == NULL || !add_bool(object, "active", has_deadline && remaining_ms != 0) ||
        !add_bool(object, "expired", has_deadline && remaining_ms == 0) ||
        !add_number(object, "deadlineMs", (double) snapshot->lease_deadline_ms) ||
        !add_number(object, "remainingMs", (double) remaining_ms) ||
        !add_number(object, "requestedMs", snapshot->report.lease_ms) ||
        !add_number(object, "maximumMs", snapshot->config.maximum_lease_ms)) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static cJSON * fault_json(const bzm_validation_runtime_snapshot_t * snapshot)
{
    cJSON * object = cJSON_CreateObject();
    if (object == NULL || !add_bool(object, "latched", snapshot->fault_code != 0) ||
        !add_number(object, "code", snapshot->fault_code) || !add_string(object, "detail", snapshot->fault_detail)) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static cJSON * health_json(const bzm_validation_runtime_snapshot_t * snapshot)
{
    const bzm_runtime_health_result_t * health = &snapshot->health;
    cJSON * object = cJSON_CreateObject();
    if (object == NULL || !add_bool(object, "valid", snapshot->health_valid) || !add_number(object, "status", health->status) ||
        !add_string(object, "statusName", bzm_runtime_health_status_name(health->status)) ||
        !add_number(object, "fault", health->fault) ||
        !add_string(object, "faultName", bzm_runtime_health_fault_name(health->fault)) ||
        !add_string(object, "detail", health->detail) ||
        !add_number(object, "sampledAtMs", (double) snapshot->health_sampled_at_ms)) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static cJSON * running_evidence_json(const bzm_validation_runtime_snapshot_t * snapshot)
{
    const bzm_running_evidence_result_t * evidence = &snapshot->running_evidence;
    const bzm_running_stats_t * observed = &evidence->observed;
    cJSON * object = cJSON_CreateObject();
    cJSON * counters = cJSON_CreateObject();
    if (object == NULL || counters == NULL || !add_bool(object, "requested", snapshot->running_evidence_requested) ||
        !add_bool(object, "monitoring", snapshot->running_evidence_monitoring) ||
        !add_bool(object, "complete", evidence->status == BZM_RUNNING_EVIDENCE_GOOD) ||
        !add_number(object, "startedAtMs", (double) snapshot->running_evidence_started_at_ms) ||
        !add_number(object, "elapsedMs", (double) evidence->elapsed_ms) || !add_number(object, "status", evidence->status) ||
        !add_string(object, "statusName", bzm_running_evidence_status_name(evidence->status)) ||
        !add_number(object, "fault", evidence->fault) ||
        !add_string(object, "faultName", bzm_running_evidence_fault_name(evidence->fault)) ||
        !add_string(object, "detail", evidence->detail) ||
        !add_number(object, "requiredChipEngineWrites", snapshot->running_evidence_config.required_chip_engine_writes) ||
        !add_number(object, "minimumValidResults", snapshot->running_evidence_config.minimum_valid_results) ||
        !add_bool(object, "mappingRecoveryEnabled", snapshot->running_evidence_config.allow_mapping_recovery) ||
        !add_number(object, "maximumMappingRejections", snapshot->running_evidence_config.maximum_mapping_rejections) ||
        !add_number(object, "maximumLocalRejections", snapshot->running_evidence_config.maximum_local_rejections) ||
        !add_number(object, "proofTimeoutMs", snapshot->running_evidence_config.proof_timeout_ms) ||
        !add_number(counters, "dispatchBatches", (double) observed->dispatch_batches) ||
        !add_number(counters, "dispatchedLogicalEngines", (double) observed->dispatched_logical_engines) ||
        !add_number(counters, "dispatchedChipEngines", (double) observed->dispatched_chip_engines) ||
        !add_number(counters, "dispatchFailures", (double) observed->dispatch_failures) ||
        !add_number(counters, "mappedResults", (double) observed->mapped_results) ||
        !add_number(counters, "mappingRejections", (double) observed->mapping_rejections) ||
        !add_number(counters, "mappingRejectionStreak", observed->mapping_rejection_streak) ||
        !add_bool(counters, "mappingRecoveryPending", observed->mapping_recovery_pending) ||
        !add_number(counters, "locallyValidResults", (double) observed->locally_valid_results) ||
        !add_number(counters, "locallyRejectedResults", (double) observed->locally_rejected_results) ||
        !add_number(counters, "localRejectionStreak", observed->local_rejection_streak) ||
        !add_bool(counters, "localRecoveryPending", observed->local_recovery_pending)) {
        cJSON_Delete(counters);
        cJSON_Delete(object);
        return NULL;
    }
    if (!add_owned_object(object, "counters", counters)) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static cJSON * runtime_json(const bzm_validation_runtime_snapshot_t * snapshot, uint64_t current_ms)
{
    bool dispatch_allowed = snapshot->fault_code == 0 && snapshot->owner == BZM_SUPERVISOR_OWNER_MINING &&
                            snapshot->report.state == BZM_VALIDATION_HOLDING && snapshot->report.overall == BZM_CHECK_GOOD &&
                            snapshot->report.reached_stage == BZM_STAGE_RUNNING && snapshot->lease_deadline_ms > current_ms;
    cJSON * object = cJSON_CreateObject();
    if (object == NULL || !add_bool(object, "active", snapshot->active) ||
        !add_bool(object, "initialized", snapshot->initialized) || !add_bool(object, "monitorRunning", snapshot->monitor_running) ||
        !add_bool(object, "dispatchAllowed", dispatch_allowed) ||
        !add_bool(object, "stage7Complete", snapshot->running_evidence.status == BZM_RUNNING_EVIDENCE_GOOD) ||
        !add_number(object, "lastGateResult", snapshot->last_gate_result) ||
        !add_string(object, "lastGateResultName", bzm_runtime_gate_result_name(snapshot->last_gate_result)) ||
        !add_number(object, "owner", snapshot->owner) ||
        !add_string(object, "ownerName", bzm_supervisor_owner_name(snapshot->owner))) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static cJSON * snapshot_json(const bzm_validation_runtime_snapshot_t * snapshot)
{
    uint64_t current_ms = (uint64_t) (esp_timer_get_time() / 1000);
    cJSON * root = cJSON_CreateObject();
    if (root == NULL || !add_bool(root, "available", snapshot->active && snapshot->initialized) ||
        !add_owned_object(root, "runtime", runtime_json(snapshot, current_ms)) ||
        !add_owned_object(root, "configuration", configuration_json(snapshot)) ||
        !add_owned_object(root, "localArm", local_arm_json(snapshot)) ||
        !add_owned_object(root, "lease", lease_json(snapshot, current_ms)) ||
        !add_owned_object(root, "fault", fault_json(snapshot)) || !add_owned_object(root, "health", health_json(snapshot)) ||
        !add_owned_object(root, "runningEvidence", running_evidence_json(snapshot)) ||
        !add_owned_object(root, "bridge", bridge_json(snapshot)) ||
        !add_owned_object(root, "report", report_json(&snapshot->report)) ||
        !add_owned_object(root, "stageDefinitions", stage_definitions_json())) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool snapshot_active(bzm_validation_runtime_snapshot_t * snapshot)
{
    return bzm_validation_runtime_snapshot(snapshot) && snapshot->active && snapshot->initialized;
}

static bzm_validation_runtime_snapshot_t * allocate_snapshot(void)
{
    /*
     * A snapshot contains the complete eight-stage report and is too large
     * for the HTTP task's nested POST call chain. Keep it off the task stack
     * so a diagnostic request cannot trip the stack-overflow watchdog.
     */
    return calloc(1, sizeof(bzm_validation_runtime_snapshot_t));
}

static esp_err_t send_snapshot_unavailable(httpd_req_t * req, const bzm_validation_runtime_snapshot_t * snapshot)
{
    cJSON * root = snapshot_json(snapshot);
    if (root != NULL) {
        (void) add_string(root, "error", "RUNTIME_UNAVAILABLE");
        (void) add_string(root, "message", "Bonanza validation runtime is unavailable");
    }
    return send_json(req, "503 Service Unavailable", root);
}

static esp_err_t get_validation(httpd_req_t * req)
{
    if (!prepare_request(req))
        return ESP_OK;
    bzm_validation_runtime_snapshot_t * snapshot = allocate_snapshot();
    if (snapshot == NULL)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    bool active = snapshot_active(snapshot);
    esp_err_t result = active ? send_json(req, "200 OK", snapshot_json(snapshot)) : send_snapshot_unavailable(req, snapshot);
    free(snapshot);
    return result;
}

static const char * gate_http_status(bzm_runtime_gate_result_t result)
{
    switch (result) {
    case BZM_RUNTIME_GATE_ACCEPTED:
        return "200 OK";
    case BZM_RUNTIME_GATE_BUSY:
    case BZM_RUNTIME_GATE_FAULT_LATCHED:
        return "409 Conflict";
    case BZM_RUNTIME_GATE_BUILD_CEILING:
    case BZM_RUNTIME_GATE_POWER_NOT_COMPILED:
    case BZM_RUNTIME_GATE_CONFIRMATION_REQUIRED:
    case BZM_RUNTIME_GATE_LOCAL_ARM_REQUIRED:
    case BZM_RUNTIME_GATE_LEASE_REQUIRED:
    case BZM_RUNTIME_GATE_LEASE_TOO_LONG:
        return "412 Precondition Failed";
    case BZM_RUNTIME_GATE_INVALID_STAGE:
    default:
        return "400 Bad Request";
    }
}

static esp_err_t post_validation(httpd_req_t * req)
{
    if (!prepare_request(req))
        return ESP_OK;

    bzm_validation_runtime_snapshot_t * snapshot = allocate_snapshot();
    if (snapshot == NULL)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    if (!snapshot_active(snapshot)) {
        esp_err_t result = send_snapshot_unavailable(req, snapshot);
        free(snapshot);
        return result;
    }
    cJSON * body = receive_json_object(req, false);
    if (body == NULL) {
        free(snapshot);
        return ESP_OK;
    }

    bzm_validation_api_request_t request;
    uint32_t maximum_seconds = snapshot->config.maximum_lease_ms / 1000U;
    if (!bzm_validation_api_parse_request(body, maximum_seconds, &request)) {
        cJSON_Delete(body);
        free(snapshot);
        return send_api_error(req, "400 Bad Request", "INVALID_REQUEST",
                              "Expected exactly targetStage (integer 0..7), hold (boolean), "
                              "leaseSeconds (bounded integer), and confirm (bounded string)");
    }

    bzm_runtime_gate_result_t gate = bzm_validation_runtime_request(request.target_stage, request.hold_after_success,
                                                                    request.lease_seconds * 1000U, request.confirmation);
    cJSON_Delete(body);

    (void) bzm_validation_runtime_snapshot(snapshot);
    cJSON * root = snapshot_json(snapshot);
    free(snapshot);
    cJSON * operation = cJSON_CreateObject();
    if (root == NULL || operation == NULL || !add_string(operation, "type", "validation") ||
        !add_bool(operation, "accepted", gate == BZM_RUNTIME_GATE_ACCEPTED) || !add_number(operation, "gateResult", gate) ||
        !add_string(operation, "gateResultName", bzm_runtime_gate_result_name(gate)) ||
        !add_number(operation, "targetStage", request.target_stage) || !add_bool(operation, "hold", request.hold_after_success) ||
        !add_number(operation, "leaseSeconds", request.lease_seconds)) {
        cJSON_Delete(operation);
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    }
    if (!add_owned_object(root, "operation", operation)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    }
    return send_json(req, gate_http_status(gate), root);
}

static esp_err_t post_heartbeat(httpd_req_t * req)
{
    if (!prepare_request(req))
        return ESP_OK;

    bzm_validation_runtime_snapshot_t * snapshot = allocate_snapshot();
    if (snapshot == NULL)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    if (!snapshot_active(snapshot)) {
        esp_err_t result = send_snapshot_unavailable(req, snapshot);
        free(snapshot);
        return result;
    }
    cJSON * body = receive_json_object(req, false);
    if (body == NULL) {
        free(snapshot);
        return ESP_OK;
    }
    bzm_validation_api_heartbeat_t heartbeat;
    uint32_t maximum_seconds = snapshot->config.maximum_lease_ms / 1000U;
    if (!bzm_validation_api_parse_heartbeat(body, maximum_seconds, &heartbeat)) {
        cJSON_Delete(body);
        free(snapshot);
        return send_api_error(req, "400 Bad Request", "INVALID_REQUEST",
                              "Expected exactly leaseSeconds as an integer from 1 through "
                              "the compiled maximum");
    }
    cJSON_Delete(body);

    bool accepted = bzm_validation_runtime_heartbeat(heartbeat.lease_seconds * 1000U);
    (void) bzm_validation_runtime_snapshot(snapshot);
    cJSON * root = snapshot_json(snapshot);
    free(snapshot);
    cJSON * operation = cJSON_CreateObject();
    if (root == NULL || operation == NULL || !add_string(operation, "type", "heartbeat") ||
        !add_bool(operation, "accepted", accepted) || !add_number(operation, "leaseSeconds", heartbeat.lease_seconds)) {
        cJSON_Delete(operation);
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    }
    if (!add_owned_object(root, "operation", operation)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    }
    return send_json(req, accepted ? "200 OK" : "409 Conflict", root);
}

static esp_err_t post_stop(httpd_req_t * req)
{
    if (!prepare_request(req))
        return ESP_OK;

    bzm_validation_runtime_snapshot_t * snapshot = allocate_snapshot();
    if (snapshot == NULL)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    if (!snapshot_active(snapshot)) {
        esp_err_t result = send_snapshot_unavailable(req, snapshot);
        free(snapshot);
        return result;
    }
    cJSON * body = receive_json_object(req, true);
    if (body == NULL) {
        free(snapshot);
        return ESP_OK;
    }
    bzm_validation_api_stop_t stop;
    if (!bzm_validation_api_parse_stop(body, &stop)) {
        cJSON_Delete(body);
        free(snapshot);
        return send_api_error(req, "400 Bad Request", "INVALID_REQUEST",
                              "The optional reason must be a string of at most 96 bytes");
    }
    bzm_runtime_stop_result_t stop_result = bzm_validation_runtime_stop(stop.reason);
    bool stopped = stop_result == BZM_RUNTIME_STOPPED;
    cJSON_Delete(body);

    (void) bzm_validation_runtime_snapshot(snapshot);
    cJSON * root = snapshot_json(snapshot);
    free(snapshot);
    cJSON * operation = cJSON_CreateObject();
    if (root == NULL || operation == NULL || !add_string(operation, "type", "stop") || !add_bool(operation, "accepted", stopped)) {
        cJSON_Delete(operation);
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    }
    if (!add_owned_object(root, "operation", operation)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    }
    const char * status = "500 Internal Server Error";
    if (stopped) {
        status = "200 OK";
    } else if (stop_result == BZM_RUNTIME_STOP_CONFLICT) {
        status = "409 Conflict";
    } else if (stop_result == BZM_RUNTIME_STOP_UNAVAILABLE) {
        status = "503 Service Unavailable";
    }
    return send_json(req, status, root);
}

static esp_err_t post_clear_fault(httpd_req_t * req)
{
    if (!prepare_request(req))
        return ESP_OK;

    bzm_validation_runtime_snapshot_t * snapshot = allocate_snapshot();
    if (snapshot == NULL)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    if (!snapshot_active(snapshot)) {
        esp_err_t result = send_snapshot_unavailable(req, snapshot);
        free(snapshot);
        return result;
    }
    cJSON * body = receive_json_object(req, true);
    if (body == NULL) {
        free(snapshot);
        return ESP_OK;
    }
    if (!bzm_validation_api_parse_empty_object(body)) {
        cJSON_Delete(body);
        free(snapshot);
        return send_api_error(req, "400 Bad Request", "INVALID_REQUEST", "Fault clear accepts only an empty JSON object");
    }
    cJSON_Delete(body);

    bool cleared = bzm_validation_runtime_clear_fault();
    (void) bzm_validation_runtime_snapshot(snapshot);
    cJSON * root = snapshot_json(snapshot);
    free(snapshot);
    cJSON * operation = cJSON_CreateObject();
    if (root == NULL || operation == NULL || !add_string(operation, "type", "faultClear") ||
        !add_bool(operation, "accepted", cleared)) {
        cJSON_Delete(operation);
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    }
    if (!add_owned_object(root, "operation", operation)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
    }
    return send_json(req, cleared ? "200 OK" : "409 Conflict", root);
}

esp_err_t bzm_validation_api_register(httpd_handle_t server, void * user_context)
{
    if (server == NULL)
        return ESP_ERR_INVALID_ARG;
    const httpd_uri_t routes[] = {
        {
            .uri = "/api/system/bzm/validation",
            .method = HTTP_GET,
            .handler = get_validation,
            .user_ctx = user_context,
        },
        {
            .uri = "/api/system/bzm/validation",
            .method = HTTP_POST,
            .handler = post_validation,
            .user_ctx = user_context,
        },
        {
            .uri = "/api/system/bzm/validation/heartbeat",
            .method = HTTP_POST,
            .handler = post_heartbeat,
            .user_ctx = user_context,
        },
        {
            .uri = "/api/system/bzm/validation/stop",
            .method = HTTP_POST,
            .handler = post_stop,
            .user_ctx = user_context,
        },
        {
            .uri = "/api/system/bzm/fault/clear",
            .method = HTTP_POST,
            .handler = post_clear_fault,
            .user_ctx = user_context,
        },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        esp_err_t result = httpd_register_uri_handler(server, &routes[i]);
        if (result != ESP_OK)
            return result;
    }
    return ESP_OK;
}
