#ifndef ASIC_RESULT_H
#define ASIC_RESULT_H

#include <stdint.h>

typedef uint64_t asic_work_handle_t;

#define ASIC_WORK_HANDLE_INVALID UINT64_MAX

typedef enum
{
    REGISTER_INVALID = 0,
    REGISTER_HASHRATE,       // hashrate register (BM1397)
    REGISTER_TOTAL_COUNT,    // total counter (BM1366,BM1368,BM1370)
    REGISTER_DOMAIN_0_COUNT, // domain counters (BM1366,BM1368,BM1370)
    REGISTER_DOMAIN_1_COUNT,
    REGISTER_DOMAIN_2_COUNT,
    REGISTER_DOMAIN_3_COUNT,
    REGISTER_ERROR_COUNT,    // error count register (all current BM chips)
    REGISTER_PLL_PARAM,      // PLL/clock config readback (BM1370)
} register_type_t;

typedef struct {
    // Opaque outside the active ASIC-family adapter and its work store.
    asic_work_handle_t work_handle;
    uint32_t nonce;
    // Exact values mined by the ASIC after all rolling has been resolved.
    uint32_t final_ntime;
    uint32_t final_version;
    // Protocol submission delta, already resolved by the ASIC-family adapter.
    uint32_t version_bits;
    uint64_t timestamp_us;
    // Diagnostics only; these fields are not part of result identity.
    uint8_t asic_index;
    uint8_t core_id;
    uint8_t small_core_id;
} asic_result_t;

typedef struct {
    register_type_t register_type;
    uint8_t asic_index;
    uint32_t value;
    uint64_t timestamp_us;
} asic_register_result_t;

typedef enum {
    ASIC_EVENT_NONE = 0,
    ASIC_EVENT_SHARE_RESULT,
    ASIC_EVENT_REGISTER_RESULT,
} asic_event_type_t;

typedef struct {
    asic_event_type_t type;
    union {
        asic_result_t share;
        asic_register_result_t register_result;
    } data;
} asic_event_t;

#endif // ASIC_RESULT_H
