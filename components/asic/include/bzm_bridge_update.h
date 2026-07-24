#ifndef BZM_BRIDGE_UPDATE_H
#define BZM_BRIDGE_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bzm_bridge.h"
#include "bzm_bridge_swd.h"
#include "device_config.h"
#include "esp_err.h"

typedef struct GlobalState GlobalState;

#define BZM_BRIDGE_IMAGE_MANIFEST_SIZE 96u
#define BZM_BRIDGE_IMAGE_TARGET_BOARD_VERSION 1002u

typedef bool (*bzm_bridge_update_maintenance_fn)(void *context);

typedef enum {
    BZM_BRIDGE_UPDATE_IDLE = 0,
    BZM_BRIDGE_UPDATE_PREPARING,
    BZM_BRIDGE_UPDATE_ERASING,
    BZM_BRIDGE_UPDATE_PROGRAMMING,
    BZM_BRIDGE_UPDATE_VERIFYING,
    BZM_BRIDGE_UPDATE_RESETTING,
    BZM_BRIDGE_UPDATE_QUERYING,
    BZM_BRIDGE_UPDATE_COMPLETE,
    BZM_BRIDGE_UPDATE_FAILED,
} bzm_bridge_update_state_t;

typedef struct {
    size_t offset;
    uint16_t target_board_version;
    uint8_t protocol_major;
    uint8_t protocol_minor;
    char version[BZM_BRIDGE_VERSION_MAX_LENGTH + 1];
} bzm_bridge_image_manifest_t;

typedef struct {
    bzm_bridge_update_state_t state;
    uint8_t progress_percent;
    size_t image_size;
    bool running;
    bool manifest_validated;
    bool force_requested;
    bool version_query_supported;
    uint16_t target_board_version;
    uint8_t image_protocol_major;
    uint8_t image_protocol_minor;
    char image_version[BZM_BRIDGE_VERSION_MAX_LENGTH + 1];
    char current_version[BZM_BRIDGE_VERSION_MAX_LENGTH + 1];
    char error[96];
} bzm_bridge_update_status_t;

typedef void (*bzm_bridge_update_report_fn)(
    void *context, bzm_bridge_update_state_t state,
    uint8_t progress_percent);

typedef struct {
    esp_err_t (*prepare)(void *context);
    esp_err_t (*flash)(void *context, const uint8_t *image,
                       size_t image_size,
                       bzm_bridge_flash_progress_fn progress,
                       void *progress_context);
    esp_err_t (*restore)(void *context);
    esp_err_t (*query_info)(void *context, bzm_bridge_info_t *info);
} bzm_bridge_update_ops_t;

bool bzm_bridge_update_enabled(void);
bool bzm_bridge_update_board_supported(const DeviceConfig *config);
bool bzm_bridge_update_supported(const DeviceConfig *config);
bool bzm_bridge_update_boot_recovery_allowed(
    const DeviceConfig *config, esp_err_t bridge_error);
esp_err_t bzm_bridge_update_validate_image(const uint8_t *image,
                                            size_t image_size);
esp_err_t bzm_bridge_update_validate_manifest(
    const uint8_t *image, size_t image_size,
    bzm_bridge_image_manifest_t *manifest);
esp_err_t bzm_bridge_update_validate_upload(
    const uint8_t *image, size_t image_size, bool force,
    bzm_bridge_image_manifest_t *manifest,
    bool *manifest_validated);
bool bzm_bridge_update_manifest_matches_info(
    const bzm_bridge_image_manifest_t *manifest,
    const bzm_bridge_info_t *info);
const char *bzm_bridge_update_state_name(bzm_bridge_update_state_t state);

/*
 * Production integration must install exclusive safe-off supervisor hooks.
 * With no hooks installed, the production updater fails closed before SWD.
 */
bool BZM_bridge_update_set_maintenance_hooks(
    bzm_bridge_update_maintenance_fn acquire,
    bzm_bridge_update_maintenance_fn release, void *context);

esp_err_t bzm_bridge_update_run(
    const uint8_t *image, size_t image_size,
    const bzm_bridge_update_ops_t *ops, void *ops_context,
    bzm_bridge_update_report_fn report, void *report_context,
    bzm_bridge_info_t *installed_info, bool *version_query_supported);

bool BZM_bridge_update_is_running(void);
esp_err_t BZM_bridge_update_start(GlobalState *global_state,
                                  uint8_t *image, size_t image_size,
                                  bool force);
void BZM_bridge_update_get_status(bzm_bridge_update_status_t *status);

#endif /* BZM_BRIDGE_UPDATE_H */
