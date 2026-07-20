#ifndef BZM_BRINGUP_H
#define BZM_BRINGUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bzm_telemetry.h"
#include "bzm_topology.h"

#define BZM_BRINGUP_ASIC_COUNT BZM_MAX_ASIC_COUNT
#define BZM_BRINGUP_FIRST_ASIC_ID BZM_FIRST_ASIC_ID
#define BZM_BRINGUP_LAST_ASIC_ID BZM_LAST_ASIC_ID
#define BZM_BRINGUP_CONTROL_ENGINE_ID 0x0fffU

typedef enum
{
    BZM_BRINGUP_GOOD = 0,
    BZM_BRINGUP_BAD,
    BZM_BRINGUP_BLOCKED,
} bzm_bringup_outcome_t;

typedef enum
{
    BZM_BRINGUP_REASON_NONE = 0,
    BZM_BRINGUP_REASON_INVALID_ARGUMENT,
    BZM_BRINGUP_REASON_PREREQUISITE,
    BZM_BRINGUP_REASON_CAPABILITY_UNAVAILABLE,
    BZM_BRINGUP_REASON_IO,
    BZM_BRINGUP_REASON_CHAIN_MISSING,
    BZM_BRINGUP_REASON_CHAIN_ID_MISMATCH,
    BZM_BRINGUP_REASON_CHAIN_EXTRA_ASIC,
    BZM_BRINGUP_REASON_REGISTER_READBACK,
    BZM_BRINGUP_REASON_TELEMETRY_MISSING,
    BZM_BRINGUP_REASON_TELEMETRY_PRECONFIG,
    BZM_BRINGUP_REASON_TELEMETRY_STALE,
    BZM_BRINGUP_REASON_TELEMETRY_UNSAFE,
    BZM_BRINGUP_REASON_PLL_UNLOCKED,
    BZM_BRINGUP_REASON_TOPOLOGY,
    BZM_BRINGUP_REASON_BALANCED_PAIR_UNAVAILABLE,
    BZM_BRINGUP_REASON_BALANCED_PAIR_COMMIT,
    BZM_BRINGUP_REASON_ACTIVATION_BARRIER,
    BZM_BRINGUP_REASON_BALANCED_BATCH,
} bzm_bringup_reason_t;

typedef enum
{
    BZM_BRINGUP_PROBE_RESPONSE = 0,
    BZM_BRINGUP_PROBE_NO_RESPONSE,
    BZM_BRINGUP_PROBE_IO_ERROR,
} bzm_bringup_probe_result_t;

typedef struct
{
    bzm_bringup_outcome_t outcome;
    bzm_bringup_reason_t reason;
    uint8_t asic_id;
    uint8_t pll_index;
    uint8_t register_offset;
    uint32_t expected;
    uint32_t actual;
    uint16_t completed_items;
} bzm_bringup_report_t;

typedef struct
{
    bool chain_verified;
    bool sensors_verified;
    bool clocks_verified;
    bool balanced_ramp_verified;
    bool running_verified;
    uint64_t sensors_configured_us;
    uint64_t clocks_configured_us;
} bzm_bringup_state_t;

typedef struct
{
    uint8_t slow_clock_divider;
    uint8_t tdm_slot_bit_count;
    uint8_t tdm_slot_count;
    uint8_t tdm_delay;
    uint8_t sensor_clock_divider;
    uint8_t tdm_gap_count;
    uint8_t thermal_threshold_count;
    uint8_t voltage_threshold_count;
    uint16_t thermal_trip_code;
    uint16_t voltage_ch0_shutdown_code;
    uint16_t voltage_ch1_shutdown_code;
    uint16_t settle_delay_ms;
} bzm_bringup_sensor_profile_t;

typedef struct
{
    uint16_t target_mhz;
    uint8_t reference_mhz;
    uint8_t reference_divider;
    uint8_t post1_divider;
    uint8_t post2_divider;
    uint16_t feedback_divider;
    uint32_t postdiv_register;
    uint8_t lock_attempts;
    uint16_t lock_poll_delay_ms;
} bzm_bringup_pll_profile_t;

typedef struct
{
    bzm_telemetry_bounds_t bounds;
    uint64_t max_age_us;
    /* CH2 excursions, including a voltage-fault bit in the same unchecksummed
     * frame, require this many consecutive fresh frames. A value of one
     * preserves immediate fail-closed behavior. */
    uint8_t ch2_confirm_samples;
} bzm_bringup_telemetry_policy_t;

typedef struct
{
    bzm_bringup_probe_result_t (*probe_noop)(void * context, uint8_t asic_id);
    bool (*write_u32)(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset, uint32_t value);
    bool (*read_u32)(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset, uint32_t * value);
    void (*delay_ms)(void * context, uint32_t delay_ms);
    uint64_t (*now_us)(void * context);
    bool (*telemetry_snapshot)(void * context, bzm_telemetry_store_t * store);

    /*
     * All four callbacks are mandatory for BALANCED_RAMP. The batch hooks
     * bracket one pair across all ASICs so the adapter can pause unsolicited
     * telemetry while collecting addressed acknowledgements. pair_commit
     * activates the higher-voltage stack first and must complete the other
     * stack before returning, limiting transient skew to one engine. The
     * barrier keeps ordinary mining dispatch closed until every sequential
     * pair is acknowledged.
     */
    bool (*balanced_batch_begin)(void * context, uint16_t pair_index);
    bool (*balanced_pair_commit)(void * context, uint8_t asic_id, const bzm_engine_pair_t * pair);
    bool (*balanced_batch_end)(void * context, uint16_t pair_index);
    bool (*activation_barrier)(void * context, size_t asic_count, size_t pairs_per_asic);
} bzm_bringup_ops_t;

void bzm_bringup_init(bzm_bringup_state_t * state);
void bzm_bringup_reference_sensor_profile(bzm_bringup_sensor_profile_t * profile);
void bzm_bringup_pll_800_profile(bzm_bringup_pll_profile_t * profile);
uint32_t bzm_bringup_reference_tdm_control(void);

const char * bzm_bringup_outcome_name(bzm_bringup_outcome_t outcome);
const char * bzm_bringup_reason_name(bzm_bringup_reason_t reason);

bzm_bringup_outcome_t bzm_bringup_stage_chain4(bzm_bringup_state_t * state, const bzm_bringup_ops_t * ops, void * ops_context,
                                               bzm_bringup_report_t * report);
bzm_bringup_outcome_t bzm_bringup_stage_sensors(bzm_bringup_state_t * state, const bzm_bringup_ops_t * ops, void * ops_context,
                                                const bzm_bringup_sensor_profile_t * profile,
                                                const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                                bzm_bringup_report_t * report);
bzm_bringup_outcome_t bzm_bringup_stage_clocks(bzm_bringup_state_t * state, const bzm_bringup_ops_t * ops, void * ops_context,
                                               const bzm_bringup_pll_profile_t * profile,
                                               const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                               bzm_bringup_report_t * report);
bzm_bringup_outcome_t bzm_bringup_stage_balanced_ramp(bzm_bringup_state_t * state, const bzm_bringup_ops_t * ops,
                                                      void * ops_context, const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                                      bzm_bringup_report_t * report);
bzm_bringup_outcome_t bzm_bringup_check_running(bzm_bringup_state_t * state, const bzm_bringup_ops_t * ops, void * ops_context,
                                                const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                                bzm_bringup_report_t * report);

#endif // BZM_BRINGUP_H
