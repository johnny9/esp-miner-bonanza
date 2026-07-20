#ifndef BZM_RUNTIME_HEALTH_H
#define BZM_RUNTIME_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#include "bzm_bridge.h"
#include "bzm_telemetry.h"
#include "bzm_transport.h"
#include "bzm_validation.h"

#define BZM_RUNTIME_HEALTH_DETAIL_LENGTH 160U

/* PMBus values used by the BZM TPS546 runtime interlock. */
#define BZM_RUNTIME_HEALTH_TPS_OPERATION_ON_MASK 0x80U
/* A held powered stage accepts no PMBus warning, fault, busy, or
 * none-of-the-above indication. The stage samples only after setup settles,
 * so every asserted STATUS_WORD bit is fail-closed. */
#define BZM_RUNTIME_HEALTH_TPS_STATUS_FAULT_MASK 0xffffU

typedef enum
{
    BZM_RUNTIME_HEALTH_GOOD = 0,
    BZM_RUNTIME_HEALTH_BAD = 1,
} bzm_runtime_health_status_t;

/*
 * Values are explicit because these codes are persisted by the supervisor
 * and exposed through the validation API.  New codes must not renumber an
 * existing value.
 */
typedef enum
{
    BZM_RUNTIME_HEALTH_FAULT_NONE = 0x0000,
    BZM_RUNTIME_HEALTH_FAULT_INVALID_INPUT = 0x0001,
    BZM_RUNTIME_HEALTH_FAULT_INVALID_STAGE = 0x0002,

    BZM_RUNTIME_HEALTH_FAULT_BRIDGE_UNAVAILABLE = 0x0100,
    BZM_RUNTIME_HEALTH_FAULT_BRIDGE_STATUS_INVALID = 0x0101,
    BZM_RUNTIME_HEALTH_FAULT_BRIDGE_CAPABILITY_MISSING = 0x0102,
    BZM_RUNTIME_HEALTH_FAULT_BRIDGE_STATE = 0x0103,
    BZM_RUNTIME_HEALTH_FAULT_BRIDGE_FAULT = 0x0104,
    BZM_RUNTIME_HEALTH_FAULT_BRIDGE_LEASE = 0x0105,
    BZM_RUNTIME_HEALTH_FAULT_BRIDGE_OUTPUT = 0x0106,

    BZM_RUNTIME_HEALTH_FAULT_FAN_NOT_FULL = 0x0200,
    BZM_RUNTIME_HEALTH_FAULT_FAN_TACH_UNAVAILABLE = 0x0201,
    BZM_RUNTIME_HEALTH_FAULT_FAN_TACH_LOW = 0x0202,

    BZM_RUNTIME_HEALTH_FAULT_TPS_UNAVAILABLE = 0x0300,
    BZM_RUNTIME_HEALTH_FAULT_TPS_PGOOD_LOW = 0x0301,
    BZM_RUNTIME_HEALTH_FAULT_TPS_OPERATION_OFF = 0x0302,
    BZM_RUNTIME_HEALTH_FAULT_TPS_STATUS = 0x0303,
    BZM_RUNTIME_HEALTH_FAULT_TPS_VIN_RANGE = 0x0304,
    BZM_RUNTIME_HEALTH_FAULT_TPS_VOUT_RANGE = 0x0305,
    BZM_RUNTIME_HEALTH_FAULT_TPS_TEMPERATURE_RANGE = 0x0306,
    BZM_RUNTIME_HEALTH_FAULT_TPS_IOUT_RANGE = 0x0307,
    BZM_RUNTIME_HEALTH_FAULT_TPS_VOUT_COMMAND = 0x0308,

    BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_UNAVAILABLE = 0x0400,
    BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_MISSING = 0x0401,
    BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_INVALID = 0x0402,
    BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_TRIP = 0x0403,
    BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_STALE = 0x0404,
    BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_BOUNDS = 0x0405,
    BZM_RUNTIME_HEALTH_FAULT_TELEMETRY_CLOCK_UNLOCKED = 0x0406,

    BZM_RUNTIME_HEALTH_FAULT_PARSER_UNAVAILABLE = 0x0500,
    BZM_RUNTIME_HEALTH_FAULT_PARSER_COUNTER_REGRESSED = 0x0501,
    BZM_RUNTIME_HEALTH_FAULT_PARSER_ERROR_DELTA = 0x0502,
} bzm_runtime_health_fault_t;

typedef struct
{
    bool available;
    bool pgood;
    uint8_t operation;
    uint16_t status_word;
    float vout_command_v;
    bool vout_command_matches_expected;
    float vin_v;
    float vout_v;
    float iout_a;
    float temperature_c;
} bzm_runtime_health_tps_sample_t;

typedef struct
{
    float vin_min_v;
    float vin_max_v;
    float vout_command_v;
    float vout_command_tolerance_v;
    float vout_min_v;
    float vout_max_v;
    float iout_min_a;
    float iout_max_a;
    float temperature_min_c;
    float temperature_max_c;
} bzm_runtime_health_tps_bounds_t;

typedef struct
{
    bzm_validation_stage_t reached_stage;
    bool holding;

    bool bridge_status_available;
    bzm_bridge_safety_status_t bridge_status;
    bool require_independent_kill;
    bool fan_tach_available;
    uint16_t fan_rpm;
    uint16_t fan_min_rpm;

    bzm_runtime_health_tps_sample_t tps;
    bzm_runtime_health_tps_bounds_t tps_bounds;

    bool telemetry_available;
    bzm_telemetry_store_t telemetry;
    bzm_telemetry_bounds_t telemetry_bounds;
    uint64_t telemetry_now_us;
    uint64_t telemetry_max_age_us;
    /* The runtime supervisor may defer only the finite CH2 absolute-bound
     * decision to its consecutive-sample confirmation state, including a
     * voltage-fault bit carried in that same unchecksummed frame. */
    bool defer_ch2_bounds;
    /* The runtime supervisor may defer only the unchecksummed combined PLL
     * telemetry bit to its consecutive-sample confirmation state. Initial
     * Stage-5 PLL register and lock validation remains strict. */
    bool defer_clock_locks;

    bool parser_stats_available;
    bzm_serial_parser_stats_t parser_baseline;
    bzm_serial_parser_stats_t parser_current;
} bzm_runtime_health_input_t;

typedef struct
{
    bzm_runtime_health_status_t status;
    bzm_runtime_health_fault_t fault;
    char detail[BZM_RUNTIME_HEALTH_DETAIL_LENGTH];
} bzm_runtime_health_result_t;

typedef struct
{
    bzm_serial_parser_stats_t baseline;
    uint8_t clean_windows;
    bool initialized;
} bzm_parser_settling_t;

typedef enum
{
    BZM_PARSER_SETTLING_BAD = 0,
    BZM_PARSER_SETTLING_PENDING,
    BZM_PARSER_SETTLING_COMPLETE,
} bzm_parser_settling_result_t;

typedef struct
{
    bzm_serial_parser_stats_t accepted;
    bzm_serial_parser_stats_t last;
    uint32_t burst_discard_baseline;
    uint32_t burst_unexpected_register_baseline;
    uint8_t clean_windows;
    uint8_t observed_windows;
    uint8_t episode_bursts;
    bool initialized;
    bool recovering;
} bzm_parser_realign_t;

typedef enum
{
    BZM_PARSER_REALIGN_BAD = 0,
    BZM_PARSER_REALIGN_CLEAN,
    BZM_PARSER_REALIGN_PENDING,
    BZM_PARSER_REALIGN_RECOVERED,
} bzm_parser_realign_result_t;

/*
 * Pure fail-closed evaluation of one runtime snapshot.  OFF_SAFE and any
 * non-holding snapshot intentionally return GOOD without inspecting fields.
 * UART parser deltas become applicable at CHAIN_4, when that transport is
 * active. Sensor samples become applicable at SENSORS; CLOCKS and later also
 * require the documented combined PLL0/PLL1 lock indication. DLLs remain
 * disabled and are verified during staged clock setup.
 */
bzm_runtime_health_result_t bzm_runtime_health_evaluate(const bzm_runtime_health_input_t * input);

/* A pre-dispatch settling window may advance emitted frames, but it must not
 * add or reset any parser error counter and must leave no queued result. */
bool bzm_runtime_health_parser_window_is_clean(const bzm_serial_parser_stats_t * baseline,
                                               const bzm_serial_parser_stats_t * current);

/* Stage-entry settling may absorb only an increase in discarded bytes from
 * the TDM/result-report transition. Every other parser counter must remain
 * exact, and no partial frame or result may remain pending. */
bool bzm_runtime_health_parser_window_is_discard_transition(const bzm_serial_parser_stats_t * baseline,
                                                            const bzm_serial_parser_stats_t * current);

/* A settling snapshot may contain newly emitted valid TDM frames. It is at a
 * safe parser boundary when no partial frame bytes remain buffered. */
bool bzm_parser_settling_snapshot_at_frame_boundary(const bzm_serial_parser_stats_t * stats);

void bzm_parser_settling_init(bzm_parser_settling_t * settling, const bzm_serial_parser_stats_t * baseline);
bzm_parser_settling_result_t bzm_parser_settling_observe(bzm_parser_settling_t * settling,
                                                         const bzm_serial_parser_stats_t * current, uint8_t required_clean_windows);

/* RUNNING may optionally tolerate the same byte-at-a-time resynchronization
 * used by BIRDS. Discarded bytes may advance, and the parser's rejected
 * register-header classification may advance only in an observation where
 * discarded bytes also advance. A valid frame must be emitted after the most
 * recent discard, followed by clean observations. The byte and time-window
 * limits bound one recovery episode. The event limit bounds additional
 * discard growth inside that still-unresolved episode; a completed
 * realignment resets it so later independently recovered corruption does not
 * become a lifetime failure. */
void bzm_parser_realign_init(bzm_parser_realign_t * realign, const bzm_serial_parser_stats_t * baseline);
bzm_parser_realign_result_t bzm_parser_realign_observe(bzm_parser_realign_t * realign, const bzm_serial_parser_stats_t * current,
                                                       uint32_t maximum_discarded_bytes, uint8_t required_clean_windows,
                                                       uint8_t maximum_windows, uint8_t maximum_events);

const char * bzm_runtime_health_status_name(bzm_runtime_health_status_t status);
const char * bzm_runtime_health_fault_name(bzm_runtime_health_fault_t fault);

#endif /* BZM_RUNTIME_HEALTH_H */
