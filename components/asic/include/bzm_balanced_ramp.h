#ifndef BZM_BALANCED_RAMP_H
#define BZM_BALANCED_RAMP_H

#include <stdbool.h>
#include <stdint.h>

#include "bzm_bringup.h"
#include "bzm_transport.h"

#define BZM_BALANCED_RAMP_ENGINE_CONFIG 0x04U
#define BZM_BALANCED_RAMP_ENGINE_BUSY_MASK 0x01U
/* Bits 0, 1, 2, 5, and 6 are the reference firmware's writable TCE/mode
 * controls. Bit 4 is hardware-owned and asserts in the active readback. */
#define BZM_BALANCED_RAMP_ENGINE_CONFIG_WRITABLE_MASK 0x67U
#define BZM_BALANCED_RAMP_ENGINE_CONFIG_ACTIVE_MASK 0x10U
#define BZM_BALANCED_RAMP_ENGINE_CONFIG_ALLOWED_MASK \
    (BZM_BALANCED_RAMP_ENGINE_CONFIG_WRITABLE_MASK | BZM_BALANCED_RAMP_ENGINE_CONFIG_ACTIVE_MASK)

typedef enum
{
    BZM_BALANCED_RAMP_FAILURE_NONE = 0,
    BZM_BALANCED_RAMP_FAILURE_ARGUMENT,
    BZM_BALANCED_RAMP_FAILURE_PARSER_BASELINE,
    BZM_BALANCED_RAMP_FAILURE_TELEMETRY,
    BZM_BALANCED_RAMP_FAILURE_LEASE,
    BZM_BALANCED_RAMP_FAILURE_ENGINE_RESET,
    BZM_BALANCED_RAMP_FAILURE_CONFIG_WRITE,
    BZM_BALANCED_RAMP_FAILURE_SENTINEL_WRITE,
    BZM_BALANCED_RAMP_FAILURE_STATUS_READ,
    BZM_BALANCED_RAMP_FAILURE_NOT_BUSY,
    BZM_BALANCED_RAMP_FAILURE_CONFIG_READBACK,
    BZM_BALANCED_RAMP_FAILURE_INCOMPLETE,
    BZM_BALANCED_RAMP_FAILURE_FINAL_BARRIER,
    BZM_BALANCED_RAMP_FAILURE_PARSER_FINAL,
} bzm_balanced_ramp_failure_t;

typedef struct
{
    uint16_t next_pair[BZM_BRINGUP_ASIC_COUNT];
    bool prepared_asic[BZM_BRINGUP_ASIC_COUNT];
    uint16_t completed_pairs;
    uint16_t completed_engines;
    bool baseline_captured;
    bool failed;
    bzm_balanced_ramp_failure_t failure;
    uint8_t failure_asic_id;
    uint16_t failure_engine_id;
    uint8_t failure_register_offset;
    uint32_t failure_expected;
    uint32_t failure_actual;
    bzm_serial_parser_stats_t parser_baseline;
} bzm_balanced_ramp_t;

typedef struct
{
    /* Recheck bounded execution authorization immediately before each engine
     * is activated. The enclosing balanced-batch hooks own the external
     * safety-lease heartbeat. */
    bool (*begin_engine)(void * context, uint8_t asic_id, uint16_t engine_id);
    bool (*write_register)(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset, const void * data,
                           size_t data_len);
    bool (*read_register)(void * context, uint8_t asic_id, uint16_t engine_id, uint8_t offset, void * data,
                          size_t data_len);
    void (*delay_ms)(void * context, uint32_t delay_ms);
    bool (*telemetry_sample)(void * context, uint8_t asic_id, bzm_telemetry_sample_t * sample);
    bool (*parser_stats)(void * context, bzm_serial_parser_stats_t * stats);
    bool (*final_barrier)(void * context);
} bzm_balanced_ramp_ops_t;

void bzm_balanced_ramp_init(bzm_balanced_ramp_t * ramp);
const char * bzm_balanced_ramp_failure_name(bzm_balanced_ramp_failure_t failure);

/* Engine programming is allowed only across a byte-for-byte clean parser
 * window. TDM pause/resume can leave resynchronization bytes outside that
 * window; the adapter may acknowledge only those discard bytes after proving
 * every other parser counter and the sentinel-result queue stayed clean. */
bool bzm_balanced_ramp_parser_window_is_clean(const bzm_serial_parser_stats_t * baseline,
                                              const bzm_serial_parser_stats_t * current);
bool bzm_balanced_ramp_accept_transition_discards(bzm_balanced_ramp_t * ramp,
                                                  const bzm_serial_parser_stats_t * current);
/* Export the parser-error baseline after Stage 6 has explicitly accepted its
 * controlled TDM transition discards. Runtime monitoring must begin from this
 * exact boundary rather than the pre-ramp transport baseline. */
bool bzm_balanced_ramp_get_parser_baseline(const bzm_balanced_ramp_t * ramp,
                                           bzm_serial_parser_stats_t * baseline);

/*
 * Activate one bottom/top pair using the BIRDS reference ordering rule: the
 * higher-voltage stack is activated first, followed immediately by the other
 * stack. Each engine receives deterministic no-result sentinel work and must
 * acknowledge busy + enhanced-mode config before the next engine starts.
 * The only permitted transient imbalance is the first member of this pair.
 */
bool bzm_balanced_ramp_commit_pair(bzm_balanced_ramp_t * ramp, const bzm_balanced_ramp_ops_t * ops, void * ops_context,
                                   uint8_t asic_id, const bzm_engine_pair_t * pair);

/* Prove all 472 ASIC/pair commits completed and no sentinel result or parser
 * fault escaped before opening a later mining-dispatch stage. */
bool bzm_balanced_ramp_barrier(bzm_balanced_ramp_t * ramp, const bzm_balanced_ramp_ops_t * ops, void * ops_context,
                               size_t asic_count, size_t pairs_per_asic);

#endif // BZM_BALANCED_RAMP_H
