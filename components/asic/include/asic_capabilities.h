#ifndef ASIC_CAPABILITIES_H
#define ASIC_CAPABILITIES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ASIC_VERSION_ROLLING_NONE = 0,
    ASIC_VERSION_ROLLING_MIDSTATE,
    ASIC_VERSION_ROLLING_HARDWARE,
} asic_version_rolling_mode_t;

typedef enum {
    // The host must periodically derive fresh work from the current template.
    ASIC_WORK_REFRESH_PERIODIC = 0,
    // One work item can remain active until the upstream pool sends a new job.
    ASIC_WORK_REFRESH_ON_NEW_JOB,
    // The ASIC-family driver owns work exhaustion and refresh scheduling.
    ASIC_WORK_REFRESH_DRIVER_MANAGED,
} asic_work_refresh_policy_t;

typedef struct {
    asic_version_rolling_mode_t version_rolling;
    uint32_t supported_version_mask;
    uint32_t max_version_variants;
    bool supports_ntime_rolling;
    uint32_t max_ntime_roll;
    asic_work_refresh_policy_t work_refresh_policy;
    bool driver_owns_scheduling;
} asic_capabilities_t;

// Maps device configuration data to protocol-neutral mining capabilities.
asic_capabilities_t ASIC_capabilities_for_chip_id(uint16_t chip_id);
bool ASIC_capabilities_support_version_rolling(const asic_capabilities_t *capabilities);

// True when one upstream template can remain active until a new pool job.
bool ASIC_capabilities_support_static_work(const asic_capabilities_t *capabilities);

#endif // ASIC_CAPABILITIES_H
