#include "bzm_ota_guard.h"

#include <string.h>

bool bzm_ota_guard_init(bzm_ota_guard_t *guard,
                        bzm_ota_guard_operation_fn acquire,
                        bzm_ota_guard_operation_fn release,
                        void *context)
{
    if (guard == NULL || acquire == NULL || release == NULL) return false;
    memset(guard, 0, sizeof(*guard));
    guard->acquire = acquire;
    guard->release = release;
    guard->context = context;
    return true;
}

bool bzm_ota_guard_begin(bzm_ota_guard_t *guard)
{
    if (guard == NULL || guard->acquire == NULL || guard->release == NULL ||
        guard->active || guard->retained_for_reboot ||
        !guard->acquire(guard->context)) {
        return false;
    }
    guard->active = true;
    return true;
}

bool bzm_ota_guard_release(bzm_ota_guard_t *guard)
{
    if (guard == NULL || !guard->active || guard->retained_for_reboot ||
        guard->release == NULL) {
        return false;
    }
    guard->active = false;
    return guard->release(guard->context);
}

bool bzm_ota_guard_retain_for_reboot(bzm_ota_guard_t *guard)
{
    if (guard == NULL || !guard->active || guard->retained_for_reboot) {
        return false;
    }
    guard->retained_for_reboot = true;
    return true;
}
