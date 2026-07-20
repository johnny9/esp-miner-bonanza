#ifndef BZM_LOCAL_ARM_H
#define BZM_LOCAL_ARM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    BZM_LOCAL_ARM_ACCEPTED = 0,
    BZM_LOCAL_ARM_INVALID_ARGUMENT,
    BZM_LOCAL_ARM_CONFIRMATION_MISMATCH,
} bzm_local_arm_result_t;

typedef struct
{
    bool armed;
    uint64_t deadline_ms;
} bzm_local_arm_t;

void bzm_local_arm_init(bzm_local_arm_t * arm);
bzm_local_arm_result_t bzm_local_arm_issue(bzm_local_arm_t * arm, const char * provided_confirmation,
                                           const char * expected_confirmation, uint64_t now_ms, uint32_t window_ms);
bool bzm_local_arm_consume(bzm_local_arm_t * arm, uint64_t now_ms);
uint32_t bzm_local_arm_remaining_ms(const bzm_local_arm_t * arm, uint64_t now_ms);
const char * bzm_local_arm_result_name(bzm_local_arm_result_t result);

#endif /* BZM_LOCAL_ARM_H */
