#include "bzm_local_arm.h"

#include <limits.h>
#include <string.h>

void bzm_local_arm_init(bzm_local_arm_t * arm)
{
    if (arm == NULL)
        return;
    *arm = (bzm_local_arm_t){0};
}

bzm_local_arm_result_t bzm_local_arm_issue(bzm_local_arm_t * arm, const char * provided_confirmation,
                                           const char * expected_confirmation, uint64_t now_ms, uint32_t window_ms)
{
    if (arm == NULL || provided_confirmation == NULL || expected_confirmation == NULL || expected_confirmation[0] == '\0' ||
        window_ms == 0 || UINT64_MAX - now_ms < window_ms) {
        return BZM_LOCAL_ARM_INVALID_ARGUMENT;
    }
    if (strcmp(provided_confirmation, expected_confirmation) != 0) {
        return BZM_LOCAL_ARM_CONFIRMATION_MISMATCH;
    }

    arm->deadline_ms = now_ms + window_ms;
    arm->armed = true;
    return BZM_LOCAL_ARM_ACCEPTED;
}

uint32_t bzm_local_arm_remaining_ms(const bzm_local_arm_t * arm, uint64_t now_ms)
{
    if (arm == NULL || !arm->armed || now_ms >= arm->deadline_ms)
        return 0;
    uint64_t remaining = arm->deadline_ms - now_ms;
    return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t) remaining;
}

bool bzm_local_arm_consume(bzm_local_arm_t * arm, uint64_t now_ms)
{
    if (arm == NULL)
        return false;
    bool accepted = bzm_local_arm_remaining_ms(arm, now_ms) != 0;
    arm->armed = false;
    arm->deadline_ms = 0;
    return accepted;
}

const char * bzm_local_arm_result_name(bzm_local_arm_result_t result)
{
    switch (result) {
    case BZM_LOCAL_ARM_ACCEPTED:
        return "ACCEPTED";
    case BZM_LOCAL_ARM_INVALID_ARGUMENT:
        return "INVALID_ARGUMENT";
    case BZM_LOCAL_ARM_CONFIRMATION_MISMATCH:
        return "CONFIRMATION_MISMATCH";
    default:
        return "INVALID_RESULT";
    }
}
