#ifndef BZM_CONTROLLER_H
#define BZM_CONTROLLER_H

#include <stdbool.h>

#include "bzm_supervisor.h"
#include "esp_err.h"
#include "global_state.h"

/* Fixed-profile Bonanza production controller. These operations are invoked
 * by normal boot, OTA, and restart flows; there is no external staged control
 * surface in a production image. */
esp_err_t bzm_controller_init(GlobalState *global_state);
bool bzm_controller_active(void);
bool bzm_controller_mining_stack_ready(void);
bool bzm_controller_dispatch_allowed(void);

/* Exclusive verified-safe-off ownership for production maintenance. */
bool bzm_controller_acquire_maintenance(
    bzm_supervisor_owner_t owner);
bool bzm_controller_release_maintenance(
    bzm_supervisor_owner_t owner);

/* Non-BZM products return true. Bonanza closes dispatch and proves safe-off
 * before the caller proceeds with restart. */
bool bzm_controller_prepare_restart(void);

#endif /* BZM_CONTROLLER_H */
