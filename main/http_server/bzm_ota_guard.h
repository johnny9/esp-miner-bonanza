#ifndef BZM_OTA_GUARD_H
#define BZM_OTA_GUARD_H

#include <stdbool.h>

typedef bool (*bzm_ota_guard_operation_fn)(void *context);

typedef struct {
    bzm_ota_guard_operation_fn acquire;
    bzm_ota_guard_operation_fn release;
    void *context;
    bool active;
    bool retained_for_reboot;
} bzm_ota_guard_t;

bool bzm_ota_guard_init(bzm_ota_guard_t *guard,
                        bzm_ota_guard_operation_fn acquire,
                        bzm_ota_guard_operation_fn release,
                        void *context);
bool bzm_ota_guard_begin(bzm_ota_guard_t *guard);
bool bzm_ota_guard_release(bzm_ota_guard_t *guard);
bool bzm_ota_guard_retain_for_reboot(bzm_ota_guard_t *guard);

#endif /* BZM_OTA_GUARD_H */
