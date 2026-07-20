#ifndef BZM_DRIVER_H
#define BZM_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "asic_result.h"
#include "asic_driver.h"
#include "bzm_bringup.h"
#include "bzm_dispatch_gate.h"
#include "bzm_running_evidence.h"
#include "bzm_transport.h"
#include "mining.h"

typedef struct GlobalState GlobalState;

uint8_t BZM_init(GlobalState * state);
int BZM_set_max_baud(void);
bool BZM_send_work(GlobalState * state, const mining_template_t * template);
bool BZM_clear_work(GlobalState * state);
asic_event_t * BZM_process_work(GlobalState * state);
float BZM_read_temperature(GlobalState * state);

/* Lock-free Stage-7 evidence counters. A baseline/current delta represents
 * exactly one RUNNING validation attempt. */
bool BZM_running_stats_snapshot(bzm_running_stats_t * stats);
void BZM_running_record_proof(void);
void BZM_running_record_rejection(void);
void BZM_record_local_result(GlobalState *state, bool valid,
                             double nonce_difficulty);

/* Thread-safe copies of the singleton transport's receive diagnostics. */
bool BZM_get_telemetry(uint8_t asic_id, bzm_telemetry_sample_t * sample);
bool BZM_get_telemetry_snapshot(bzm_telemetry_store_t * snapshot);
bool BZM_get_parser_stats(bzm_serial_parser_stats_t * stats);
/* Stage 6's proven parser boundary, including only explicitly accepted TDM
 * transition discards. Available after a successful balanced-ramp barrier. */
bool BZM_staged_get_parser_baseline(bzm_serial_parser_stats_t * stats);
/* Parser boundary proven by the clean Stage-4 TDM startup settling window. */
bool BZM_staged_get_sensor_parser_baseline(bzm_serial_parser_stats_t * stats);
/* Parser boundary proven by a quiet pre-dispatch Stage-7 settling window. */
bool BZM_staged_get_running_parser_baseline(bzm_serial_parser_stats_t * stats);

/*
 * Production staged entry points. These keep mining dispatch closed until
 * RUNNING is proven. BALANCED_RAMP is additionally compile-gated and uses a
 * reference-style sequential pair activation with no more than one engine of
 * transient bottom/top skew.
 */
bzm_bringup_outcome_t BZM_staged_initialize(GlobalState * state, bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_staged_chain4(bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_staged_sensors(const bzm_bringup_sensor_profile_t * profile,
                                         const bzm_bringup_telemetry_policy_t * telemetry_policy, bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_staged_clocks(const bzm_bringup_pll_profile_t * profile,
                                        const bzm_bringup_telemetry_policy_t * telemetry_policy, bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_staged_balanced_ramp(const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                               bzm_bringup_report_t * report);
bzm_bringup_outcome_t BZM_staged_running(GlobalState * state, const bzm_bringup_telemetry_policy_t * telemetry_policy,
                                         bzm_bringup_report_t * report);
bool BZM_staged_get_state(bzm_bringup_state_t * state);
bool BZM_staged_hold_reset(void);
bool BZM_staged_dispatch_enabled(void);
/* The callback is evaluated before dispatch and before every engine write.
 * It must be non-blocking and must not call back into this driver. NULL is
 * fail-closed. */
void BZM_staged_set_dispatch_authorizer(bzm_dispatch_authorizer_t authorize, void * context);
/* Evaluated before every staged bridge/UART operation. The runtime uses it
 * to enforce the request's absolute powered-execution deadline even while
 * the synchronous validation call owns the supervisor mutex. */
void BZM_staged_set_operation_authorizer(bzm_dispatch_authorizer_t authorize, void * context);
/* Pump the singleton parser for health monitoring; returns emitted frames. */
size_t BZM_staged_poll(uint16_t timeout_ms);

/* The production controller publishes a compact generic snapshot here; the
 * shared HTTP/API layer reads it only through ASIC_get_health(). */
void BZM_driver_health_publish(const asic_driver_health_t *health);
bool BZM_driver_health_snapshot(GlobalState *state,
                                asic_driver_health_t *health);

#endif // BZM_DRIVER_H
