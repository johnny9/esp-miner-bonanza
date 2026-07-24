#include "bzm_bridge_update.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"

#define RP2040_FLASH_VECTOR_OFFSET 0x100u
#define RP2040_SRAM_START 0x20000000u
#define RP2040_SRAM_END 0x20042000u
#define RP2040_XIP_START 0x10000100u
#define RP2040_XIP_END 0x10200000u
#define BZM_BRIDGE_UPDATE_TASK_STACK_BYTES 6144u
#define BZM_BRIDGE_IMAGE_MANIFEST_SCHEMA_VERSION 1u
#define BZM_BRIDGE_IMAGE_FIRMWARE_KIND 1u
#define BZM_BRIDGE_IMAGE_MANIFEST_CRC_OFFSET \
    (BZM_BRIDGE_IMAGE_MANIFEST_SIZE - 4u)
#define BZM_BRIDGE_IMAGE_MANIFEST_VERSION_OFFSET 24u
#define BZM_BRIDGE_IMAGE_MANIFEST_VERSION_CAPACITY 64u

static const char *TAG = "bzm_bridge_update";
static const uint8_t BZM_BRIDGE_IMAGE_MANIFEST_MAGIC[16] = {
    'B', 'Z', 'M', '-', 'B', 'R', 'I', 'D',
    'G', 'E', '-', 'F', 'W', 0, 0, 0,
};
static pthread_mutex_t UPDATE_LOCK = PTHREAD_MUTEX_INITIALIZER;
static bzm_bridge_update_status_t UPDATE_STATUS;

typedef struct {
    GlobalState *global_state;
    uint8_t *image;
    size_t image_size;
    bool maintenance_entered;
    bool supervisor_lease_entered;
    bool manifest_validated;
    bool force_requested;
    bzm_bridge_image_manifest_t manifest;
} production_update_t;

static bzm_bridge_update_maintenance_fn MAINTENANCE_ACQUIRE;
static bzm_bridge_update_maintenance_fn MAINTENANCE_RELEASE;
static void *MAINTENANCE_CONTEXT;

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t manifest_crc32(const uint8_t *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^
                  (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

bool bzm_bridge_update_enabled(void)
{
    return true;
}

bool bzm_bridge_update_board_supported(const DeviceConfig *config)
{
    return config != NULL && config->bonanza_bridge &&
           config->family.asic.id == BZM;
}

bool bzm_bridge_update_supported(const DeviceConfig *config)
{
    return bzm_bridge_update_enabled() &&
           bzm_bridge_update_board_supported(config);
}

bool bzm_bridge_update_boot_recovery_allowed(
    const DeviceConfig *config, esp_err_t bridge_error)
{
    return bridge_error != ESP_OK &&
           bzm_bridge_update_supported(config);
}

esp_err_t bzm_bridge_update_validate_image(const uint8_t *image,
                                            size_t image_size)
{
    if (image == NULL) return ESP_ERR_INVALID_ARG;
    if (image_size < RP2040_FLASH_VECTOR_OFFSET + 8u ||
        image_size > BZM_BRIDGE_FLASH_CAPACITY) {
        return ESP_ERR_INVALID_SIZE;
    }

    bool boot2_has_data = false;
    for (size_t i = 0; i < RP2040_FLASH_VECTOR_OFFSET; ++i) {
        if (image[i] != 0x00 && image[i] != 0xff) {
            boot2_has_data = true;
            break;
        }
    }
    if (!boot2_has_data) return ESP_ERR_INVALID_RESPONSE;

    uint32_t stack_pointer =
        read_le32(image + RP2040_FLASH_VECTOR_OFFSET);
    uint32_t reset_vector =
        read_le32(image + RP2040_FLASH_VECTOR_OFFSET + 4u);
    uint32_t reset_address = reset_vector & ~1u;

    if ((stack_pointer & 0x3u) != 0 ||
        stack_pointer < RP2040_SRAM_START ||
        stack_pointer > RP2040_SRAM_END ||
        (reset_vector & 1u) == 0 ||
        reset_address < RP2040_XIP_START ||
        reset_address >= RP2040_XIP_END ||
        reset_address >= 0x10000000u + image_size) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t bzm_bridge_update_validate_manifest(
    const uint8_t *image, size_t image_size,
    bzm_bridge_image_manifest_t *manifest)
{
    if (image == NULL || manifest == NULL) return ESP_ERR_INVALID_ARG;
    memset(manifest, 0, sizeof(*manifest));
    if (image_size < BZM_BRIDGE_IMAGE_MANIFEST_SIZE) {
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t *encoded = NULL;
    size_t offset = 0;
    for (size_t index = 0;
         index + BZM_BRIDGE_IMAGE_MANIFEST_SIZE <= image_size;
         ++index) {
        if (memcmp(image + index, BZM_BRIDGE_IMAGE_MANIFEST_MAGIC,
                   sizeof(BZM_BRIDGE_IMAGE_MANIFEST_MAGIC)) != 0) {
            continue;
        }
        if (encoded != NULL) return ESP_ERR_INVALID_RESPONSE;
        encoded = image + index;
        offset = index;
    }
    if (encoded == NULL) return ESP_ERR_NOT_FOUND;

    uint8_t version_length =
        encoded[BZM_BRIDGE_IMAGE_MANIFEST_VERSION_OFFSET - 1u];
    if (encoded[16] != BZM_BRIDGE_IMAGE_MANIFEST_SCHEMA_VERSION ||
        encoded[17] != BZM_BRIDGE_IMAGE_MANIFEST_SIZE ||
        read_le16(encoded + 18) !=
            BZM_BRIDGE_IMAGE_TARGET_BOARD_VERSION ||
        encoded[20] != BZM_BRIDGE_IMAGE_FIRMWARE_KIND ||
        encoded[21] != BZM_BRIDGE_PROTOCOL_MAJOR ||
        version_length == 0 ||
        version_length > BZM_BRIDGE_VERSION_MAX_LENGTH) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t index = 0; index < version_length; ++index) {
        uint8_t byte =
            encoded[BZM_BRIDGE_IMAGE_MANIFEST_VERSION_OFFSET + index];
        if (byte < 0x20 || byte > 0x7e) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    for (size_t index =
             BZM_BRIDGE_IMAGE_MANIFEST_VERSION_OFFSET + version_length;
         index < BZM_BRIDGE_IMAGE_MANIFEST_CRC_OFFSET; ++index) {
        if (encoded[index] != 0) return ESP_ERR_INVALID_RESPONSE;
    }
    if (read_le32(encoded + BZM_BRIDGE_IMAGE_MANIFEST_CRC_OFFSET) !=
        manifest_crc32(encoded,
                       BZM_BRIDGE_IMAGE_MANIFEST_CRC_OFFSET)) {
        return ESP_ERR_INVALID_CRC;
    }

    manifest->offset = offset;
    manifest->target_board_version = read_le16(encoded + 18);
    manifest->protocol_major = encoded[21];
    manifest->protocol_minor = encoded[22];
    memcpy(manifest->version,
           encoded + BZM_BRIDGE_IMAGE_MANIFEST_VERSION_OFFSET,
           version_length);
    manifest->version[version_length] = '\0';
    return ESP_OK;
}

esp_err_t bzm_bridge_update_validate_upload(
    const uint8_t *image, size_t image_size, bool force,
    bzm_bridge_image_manifest_t *manifest,
    bool *manifest_validated)
{
    if (manifest == NULL || manifest_validated == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *manifest_validated = false;
    ESP_RETURN_ON_ERROR(
        bzm_bridge_update_validate_image(image, image_size),
        TAG, "invalid RP2040 image");
    esp_err_t err = bzm_bridge_update_validate_manifest(
        image, image_size, manifest);
    if (err == ESP_OK) {
        *manifest_validated = true;
        return ESP_OK;
    }
    return force ? ESP_OK : err;
}

bool bzm_bridge_update_manifest_matches_info(
    const bzm_bridge_image_manifest_t *manifest,
    const bzm_bridge_info_t *info)
{
    return manifest != NULL && info != NULL &&
           info->protocol_major == manifest->protocol_major &&
           info->protocol_minor == manifest->protocol_minor &&
           strcmp(info->version, manifest->version) == 0;
}

const char *bzm_bridge_update_state_name(bzm_bridge_update_state_t state)
{
    switch (state) {
    case BZM_BRIDGE_UPDATE_IDLE: return "idle";
    case BZM_BRIDGE_UPDATE_PREPARING: return "preparing";
    case BZM_BRIDGE_UPDATE_ERASING: return "erasing";
    case BZM_BRIDGE_UPDATE_PROGRAMMING: return "programming";
    case BZM_BRIDGE_UPDATE_VERIFYING: return "verifying";
    case BZM_BRIDGE_UPDATE_RESETTING: return "resetting";
    case BZM_BRIDGE_UPDATE_QUERYING: return "querying";
    case BZM_BRIDGE_UPDATE_COMPLETE: return "complete";
    case BZM_BRIDGE_UPDATE_FAILED: return "failed";
    default: return "unknown";
    }
}

bool BZM_bridge_update_set_maintenance_hooks(
    bzm_bridge_update_maintenance_fn acquire,
    bzm_bridge_update_maintenance_fn release, void *context)
{
    if (acquire == NULL || release == NULL) return false;
    pthread_mutex_lock(&UPDATE_LOCK);
    if (UPDATE_STATUS.running) {
        pthread_mutex_unlock(&UPDATE_LOCK);
        return false;
    }
    MAINTENANCE_ACQUIRE = acquire;
    MAINTENANCE_RELEASE = release;
    MAINTENANCE_CONTEXT = context;
    pthread_mutex_unlock(&UPDATE_LOCK);
    return true;
}

typedef struct {
    bzm_bridge_update_report_fn report;
    void *context;
} progress_adapter_t;

static void bridge_flash_progress(void *context,
                                  bzm_bridge_flash_phase_t phase,
                                  size_t completed, size_t total)
{
    progress_adapter_t *adapter = context;
    bzm_bridge_update_state_t state;
    switch (phase) {
    case BZM_BRIDGE_FLASH_PREPARING:
        state = BZM_BRIDGE_UPDATE_PREPARING;
        break;
    case BZM_BRIDGE_FLASH_ERASING:
        state = BZM_BRIDGE_UPDATE_ERASING;
        break;
    case BZM_BRIDGE_FLASH_PROGRAMMING:
        state = BZM_BRIDGE_UPDATE_PROGRAMMING;
        break;
    case BZM_BRIDGE_FLASH_VERIFYING:
        state = BZM_BRIDGE_UPDATE_VERIFYING;
        break;
    case BZM_BRIDGE_FLASH_RESETTING:
        state = BZM_BRIDGE_UPDATE_RESETTING;
        break;
    default:
        return;
    }
    uint8_t percent = total == 0 ? 0 :
        (uint8_t)(completed >= total ? 100 : completed * 100u / total);
    if (adapter->report != NULL) {
        adapter->report(adapter->context, state, percent);
    }
}

esp_err_t bzm_bridge_update_run(
    const uint8_t *image, size_t image_size,
    const bzm_bridge_update_ops_t *ops, void *ops_context,
    bzm_bridge_update_report_fn report, void *report_context,
    bzm_bridge_info_t *installed_info, bool *version_query_supported)
{
    if (image == NULL || ops == NULL || ops->prepare == NULL ||
        ops->flash == NULL || ops->restore == NULL ||
        ops->query_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(bzm_bridge_update_validate_image(image, image_size),
                        TAG, "invalid RP2040 image");

    if (installed_info != NULL) memset(installed_info, 0, sizeof(*installed_info));
    if (version_query_supported != NULL) *version_query_supported = false;
    if (report != NULL) {
        report(report_context, BZM_BRIDGE_UPDATE_PREPARING, 0);
    }

    esp_err_t err = ops->prepare(ops_context);
    if (err != ESP_OK) return err;

    progress_adapter_t adapter = {
        .report = report,
        .context = report_context,
    };
    err = ops->flash(ops_context, image, image_size,
                     bridge_flash_progress, &adapter);
    esp_err_t restore_err = ops->restore(ops_context);
    if (err == ESP_OK) err = restore_err;
    if (err != ESP_OK) return err;

    if (report != NULL) {
        report(report_context, BZM_BRIDGE_UPDATE_QUERYING, 100);
    }
    bzm_bridge_info_t info;
    esp_err_t query_err = ops->query_info(ops_context, &info);
    if (query_err == ESP_OK) {
        if (installed_info != NULL) *installed_info = info;
        if (version_query_supported != NULL) *version_query_supported = true;
    } else if (query_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "bridge version query failed: %s",
                 esp_err_to_name(query_err));
        return query_err;
    }
    return ESP_OK;
}

static void set_status(bzm_bridge_update_state_t state,
                       uint8_t progress_percent, const char *error)
{
    pthread_mutex_lock(&UPDATE_LOCK);
    UPDATE_STATUS.state = state;
    UPDATE_STATUS.progress_percent = progress_percent;
    if (error != NULL) {
        strlcpy(UPDATE_STATUS.error, error, sizeof(UPDATE_STATUS.error));
    }
    pthread_mutex_unlock(&UPDATE_LOCK);
}

static void production_report(void *context,
                              bzm_bridge_update_state_t state,
                              uint8_t progress_percent)
{
    (void)context;
    set_status(state, progress_percent, NULL);
}

static esp_err_t production_prepare(void *context)
{
    production_update_t *update = context;
    GlobalState *state = update->global_state;
    state->SYSTEM_MODULE.mining_paused = true;

    pthread_mutex_lock(&UPDATE_LOCK);
    bzm_bridge_update_maintenance_fn acquire = MAINTENANCE_ACQUIRE;
    bzm_bridge_update_maintenance_fn release = MAINTENANCE_RELEASE;
    void *maintenance_context = MAINTENANCE_CONTEXT;
    pthread_mutex_unlock(&UPDATE_LOCK);
    if (acquire == NULL || release == NULL ||
        !acquire(maintenance_context)) {
        return ESP_ERR_INVALID_STATE;
    }
    update->supervisor_lease_entered = true;

    esp_err_t err = BZM_bridge_begin_maintenance();
    if (err != ESP_OK) {
        (void)release(maintenance_context);
        update->supervisor_lease_entered = false;
        ESP_LOGE(TAG, "could not enter bridge maintenance mode: %s",
                 esp_err_to_name(err));
        return err;
    }
    update->maintenance_entered = true;
    return ESP_OK;
}

static esp_err_t production_flash(
    void *context, const uint8_t *image, size_t image_size,
    bzm_bridge_flash_progress_fn progress, void *progress_context)
{
    (void)context;
    return bzm_bridge_swd_flash(image, image_size,
                                progress, progress_context);
}

static esp_err_t production_restore(void *context)
{
    production_update_t *update = context;
    esp_err_t err = ESP_OK;
    if (update->maintenance_entered) {
        vTaskDelay(pdMS_TO_TICKS(250));
        err = BZM_bridge_end_maintenance();
        update->maintenance_entered = false;
        if (err == ESP_OK) {
            (void)BZM_bridge_set_fan_percent(1.0f);
        }
    }
    if (update->supervisor_lease_entered) {
        pthread_mutex_lock(&UPDATE_LOCK);
        bzm_bridge_update_maintenance_fn release = MAINTENANCE_RELEASE;
        void *maintenance_context = MAINTENANCE_CONTEXT;
        pthread_mutex_unlock(&UPDATE_LOCK);
        if (release == NULL || !release(maintenance_context)) {
            if (err == ESP_OK) err = ESP_FAIL;
        }
        update->supervisor_lease_entered = false;
    }
    return err;
}

static esp_err_t production_query(void *context, bzm_bridge_info_t *info)
{
    (void)context;
    return BZM_bridge_get_info(info);
}

static const bzm_bridge_update_ops_t PRODUCTION_OPS = {
    .prepare = production_prepare,
    .flash = production_flash,
    .restore = production_restore,
    .query_info = production_query,
};

static void update_task(void *parameter)
{
    production_update_t *update = parameter;
    bzm_bridge_info_t installed_info;
    bool version_query_supported = false;
    esp_err_t err = bzm_bridge_update_run(
        update->image, update->image_size,
        &PRODUCTION_OPS, update, production_report, NULL,
        &installed_info, &version_query_supported);
    if (err == ESP_OK && update->manifest_validated &&
        (!version_query_supported ||
         !bzm_bridge_update_manifest_matches_info(
             &update->manifest, &installed_info))) {
        ESP_LOGE(TAG,
                 "installed bridge identity does not match image manifest");
        err = ESP_ERR_INVALID_RESPONSE;
    }

    pthread_mutex_lock(&UPDATE_LOCK);
    UPDATE_STATUS.running = false;
    UPDATE_STATUS.version_query_supported = version_query_supported;
    if (version_query_supported) {
        strlcpy(UPDATE_STATUS.current_version, installed_info.version,
                sizeof(UPDATE_STATUS.current_version));
    }
    if (err == ESP_OK) {
        UPDATE_STATUS.state = BZM_BRIDGE_UPDATE_COMPLETE;
        UPDATE_STATUS.progress_percent = 100;
    } else {
        UPDATE_STATUS.state = BZM_BRIDGE_UPDATE_FAILED;
        snprintf(UPDATE_STATUS.error, sizeof(UPDATE_STATUS.error),
                 "%s", esp_err_to_name(err));
    }
    pthread_mutex_unlock(&UPDATE_LOCK);

    free(update->image);
    free(update);
    vTaskDelete(NULL);
}

bool BZM_bridge_update_is_running(void)
{
    pthread_mutex_lock(&UPDATE_LOCK);
    bool running = UPDATE_STATUS.running;
    pthread_mutex_unlock(&UPDATE_LOCK);
    return running;
}

esp_err_t BZM_bridge_update_start(GlobalState *global_state,
                                  uint8_t *image, size_t image_size,
                                  bool force)
{
    if (!bzm_bridge_update_enabled()) return ESP_ERR_NOT_SUPPORTED;
    if (global_state == NULL || image == NULL ||
        !bzm_bridge_update_supported(&global_state->DEVICE_CONFIG)) {
        return ESP_ERR_INVALID_ARG;
    }
    bzm_bridge_image_manifest_t manifest;
    bool manifest_validated = false;
    ESP_RETURN_ON_ERROR(
        bzm_bridge_update_validate_upload(
            image, image_size, force, &manifest,
            &manifest_validated),
        TAG, "bridge upload validation failed");

    production_update_t *update = calloc(1, sizeof(*update));
    if (update == NULL) return ESP_ERR_NO_MEM;
    update->global_state = global_state;
    update->image = image;
    update->image_size = image_size;
    update->manifest_validated = manifest_validated;
    update->force_requested = force;
    if (update->manifest_validated) update->manifest = manifest;
    if (force) {
        ESP_LOGW(TAG,
                 "forced bridge upload requested; manifest valid=%s",
                 update->manifest_validated ? "true" : "false");
    }

    pthread_mutex_lock(&UPDATE_LOCK);
    if (UPDATE_STATUS.running) {
        pthread_mutex_unlock(&UPDATE_LOCK);
        free(update);
        return ESP_ERR_INVALID_STATE;
    }
    UPDATE_STATUS = (bzm_bridge_update_status_t) {
        .state = BZM_BRIDGE_UPDATE_PREPARING,
        .progress_percent = 0,
        .image_size = image_size,
        .running = true,
        .manifest_validated = update->manifest_validated,
        .force_requested = force,
    };
    if (update->manifest_validated) {
        UPDATE_STATUS.target_board_version =
            update->manifest.target_board_version;
        UPDATE_STATUS.image_protocol_major =
            update->manifest.protocol_major;
        UPDATE_STATUS.image_protocol_minor =
            update->manifest.protocol_minor;
        strlcpy(UPDATE_STATUS.image_version,
                update->manifest.version,
                sizeof(UPDATE_STATUS.image_version));
    }
    pthread_mutex_unlock(&UPDATE_LOCK);

    /*
     * Board 1002 has a 7680-byte largest internal heap block after normal
     * startup. Keep this worker below that bound; its image buffer is staged
     * separately in PSRAM.
     */
    if (xTaskCreate(update_task, "bridge_update",
                    BZM_BRIDGE_UPDATE_TASK_STACK_BYTES,
                    update, 8, NULL) !=
        pdPASS) {
        pthread_mutex_lock(&UPDATE_LOCK);
        UPDATE_STATUS.running = false;
        UPDATE_STATUS.state = BZM_BRIDGE_UPDATE_FAILED;
        strlcpy(UPDATE_STATUS.error, "task creation failed",
                sizeof(UPDATE_STATUS.error));
        pthread_mutex_unlock(&UPDATE_LOCK);
        free(update);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void BZM_bridge_update_get_status(bzm_bridge_update_status_t *status)
{
    if (status == NULL) return;
    pthread_mutex_lock(&UPDATE_LOCK);
    *status = UPDATE_STATUS;
    pthread_mutex_unlock(&UPDATE_LOCK);
}
