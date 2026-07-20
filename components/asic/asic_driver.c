#include "asic_driver.h"

#include "bm1366.h"
#include "bm1368.h"
#include "bm1370.h"
#include "bm1397.h"
#include "bm_job_builder.h"
#include "bm_result.h"
#include "bzm.h"
#include "bzm_driver.h"
#include "device_config.h"

typedef task_result *(*bm_process_fn)(void *state);

static asic_event_t *adapt_bm_result(GlobalState *state, bm_process_fn process)
{
    task_result *result = process(state);
    if (result == NULL) return NULL;

    static asic_event_t event;
    return bm_result_to_event(result, &event) ? &event : NULL;
}

static asic_event_t *process_bm1397(GlobalState *state)
{
    return adapt_bm_result(state, BM1397_process_work);
}

static asic_event_t *process_bm1366(GlobalState *state)
{
    return adapt_bm_result(state, BM1366_process_work);
}

static asic_event_t *process_bm1368(GlobalState *state)
{
    return adapt_bm_result(state, BM1368_process_work);
}

static asic_event_t *process_bm1370(GlobalState *state)
{
    return adapt_bm_result(state, BM1370_process_work);
}

#define DEFINE_BM_SEND_WRAPPER(suffix)                                      \
    static bool send_bm##suffix(GlobalState *state,                         \
                                const mining_template_t *template)          \
    {                                                                       \
        bm_job job;                                                         \
        if (!bm_job_build(template, &job)) return false;                    \
        return BM##suffix##_send_work(state, &job, template);               \
    }

DEFINE_BM_SEND_WRAPPER(1397)
DEFINE_BM_SEND_WRAPPER(1366)
DEFINE_BM_SEND_WRAPPER(1368)
DEFINE_BM_SEND_WRAPPER(1370)

static const asic_driver_t DRIVERS[] = {
    {
        .id = BM1397,
        .chip_id = 1397,
        .name = "BM1397",
        .ops = {
            .init = (uint8_t (*)(GlobalState *))BM1397_init,
            .process_work = process_bm1397,
            .set_max_baud = BM1397_set_max_baud,
            .send_work = send_bm1397,
            .set_version_mask = BM1397_set_version_mask,
            .set_hash_frequency = BM1397_send_hash_frequency,
            .read_registers = BM1397_read_registers,
        },
    },
    {
        .id = BM1366,
        .chip_id = 1366,
        .name = "BM1366",
        .ops = {
            .init = (uint8_t (*)(GlobalState *))BM1366_init,
            .process_work = process_bm1366,
            .set_max_baud = BM1366_set_max_baud,
            .send_work = send_bm1366,
            .set_version_mask = BM1366_set_version_mask,
            .set_hash_frequency = BM1366_send_hash_frequency,
            .set_nonce_space = BM1366_set_nonce_space,
            .read_registers = BM1366_read_registers,
        },
    },
    {
        .id = BM1368,
        .chip_id = 1368,
        .name = "BM1368",
        .ops = {
            .init = (uint8_t (*)(GlobalState *))BM1368_init,
            .process_work = process_bm1368,
            .set_max_baud = BM1368_set_max_baud,
            .send_work = send_bm1368,
            .set_version_mask = BM1368_set_version_mask,
            .set_hash_frequency = BM1368_send_hash_frequency,
            .set_nonce_space = BM1368_set_nonce_space,
            .read_registers = BM1368_read_registers,
        },
    },
    {
        .id = BM1370,
        .chip_id = 1370,
        .name = "BM1370",
        .ops = {
            .init = (uint8_t (*)(GlobalState *))BM1370_init,
            .process_work = process_bm1370,
            .set_max_baud = BM1370_set_max_baud,
            .send_work = send_bm1370,
            .set_version_mask = BM1370_set_version_mask,
            .set_hash_frequency = BM1370_send_hash_frequency,
            .set_nonce_space = BM1370_set_nonce_space,
            .read_registers = BM1370_read_registers,
        },
    },
    {
        .id = BZM,
        .chip_id = BZM_CHIP_ID,
        .name = "BZM",
        .ops = {
            .init = BZM_init,
            .process_work = BZM_process_work,
            .set_max_baud = BZM_set_max_baud,
            .send_work = BZM_send_work,
            .clear_work = BZM_clear_work,
            .job_frequency_ms = BZM_job_frequency_ms,
            .hashrate_counter_snapshot = BZM_hashrate_counter_snapshot,
            .read_temperature = BZM_read_temperature,
            .record_local_result = BZM_record_local_result,
            .health_snapshot = BZM_driver_health_snapshot,
        },
    },
};

const asic_driver_t *asic_driver_for_id(int id)
{
    for (size_t i = 0; i < sizeof(DRIVERS) / sizeof(DRIVERS[0]); ++i) {
        if (DRIVERS[i].id == id) return &DRIVERS[i];
    }
    return NULL;
}

size_t asic_driver_count(void)
{
    return sizeof(DRIVERS) / sizeof(DRIVERS[0]);
}

const asic_driver_t *asic_driver_at(size_t index)
{
    return index < asic_driver_count() ? &DRIVERS[index] : NULL;
}

const char *asic_driver_lifecycle_name(asic_driver_lifecycle_t lifecycle)
{
    switch (lifecycle) {
    case ASIC_DRIVER_SAFE_OFF:
        return "SAFE_OFF";
    case ASIC_DRIVER_STARTING:
        return "STARTING";
    case ASIC_DRIVER_MINING:
        return "MINING";
    case ASIC_DRIVER_FAULT:
        return "FAULT";
    case ASIC_DRIVER_MAINTENANCE:
        return "MAINTENANCE";
    default:
        return "UNKNOWN";
    }
}
