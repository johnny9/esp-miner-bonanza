#include "asic_capabilities.h"

#include <stddef.h>

#define BM_SUPPORTED_VERSION_MASK 0x1fffe000U

static const asic_capabilities_t NO_CAPABILITIES = {
    .version_rolling = ASIC_VERSION_ROLLING_NONE,
    .max_version_variants = 1,
    .work_refresh_policy = ASIC_WORK_REFRESH_PERIODIC,
};

static const asic_capabilities_t BM1397_CAPABILITIES = {
    .version_rolling = ASIC_VERSION_ROLLING_MIDSTATE,
    .supported_version_mask = BM_SUPPORTED_VERSION_MASK,
    .max_version_variants = 4,
    .work_refresh_policy = ASIC_WORK_REFRESH_PERIODIC,
};

static const asic_capabilities_t BM13XX_CAPABILITIES = {
    .version_rolling = ASIC_VERSION_ROLLING_HARDWARE,
    .supported_version_mask = BM_SUPPORTED_VERSION_MASK,
    .max_version_variants = 65536,
    .work_refresh_policy = ASIC_WORK_REFRESH_ON_NEW_JOB,
};

asic_capabilities_t ASIC_capabilities_for_chip_id(uint16_t chip_id)
{
    switch (chip_id) {
        case 1397:
            return BM1397_CAPABILITIES;
        case 1366:
        case 1368:
        case 1370:
            return BM13XX_CAPABILITIES;
        default:
            return NO_CAPABILITIES;
    }
}

bool ASIC_capabilities_support_version_rolling(const asic_capabilities_t *capabilities)
{
    return capabilities != NULL &&
           capabilities->version_rolling != ASIC_VERSION_ROLLING_NONE &&
           capabilities->supported_version_mask != 0 &&
           capabilities->max_version_variants > 1;
}

bool ASIC_capabilities_support_static_work(const asic_capabilities_t *capabilities)
{
    return capabilities != NULL &&
           capabilities->work_refresh_policy != ASIC_WORK_REFRESH_PERIODIC;
}
