#ifndef BZM_VALIDATION_RUNTIME_H
#define BZM_VALIDATION_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "bzm_bridge.h"
#include "bzm_local_arm.h"
#include "bzm_runtime_control.h"
#include "bzm_runtime_health.h"
#include "bzm_running_evidence.h"
#include "bzm_supervisor.h"
#include "esp_err.h"
#include "global_state.h"

typedef struct
{
    bool active;
    bool initialized;
    bool monitor_running;
    bzm_runtime_gate_result_t last_gate_result;
    bzm_supervisor_config_t config;
    bzm_validation_report_t report;
    bzm_supervisor_owner_t owner;
    uint64_t lease_deadline_ms;
    uint32_t fault_code;
    char fault_detail[BZM_SUPERVISOR_FAULT_DETAIL_LENGTH];
    bool bridge_info_valid;
    bzm_bridge_info_t bridge_info;
    bool bridge_status_valid;
    bzm_bridge_safety_status_t bridge_status;
    uint16_t fan_rpm;
    bool health_valid;
    uint64_t health_sampled_at_ms;
    bzm_runtime_health_result_t health;
    bool running_evidence_requested;
    bool running_evidence_monitoring;
    uint64_t running_evidence_started_at_ms;
    bzm_running_evidence_config_t running_evidence_config;
    bzm_running_evidence_result_t running_evidence;
    bool usb_serial_arm_enabled;
    uint32_t local_arm_window_ms;
    uint32_t local_arm_remaining_ms;
} bzm_validation_runtime_snapshot_t;

typedef enum
{
    BZM_RUNTIME_STOPPED = 0,
    BZM_RUNTIME_STOP_CONFLICT,
    BZM_RUNTIME_STOP_SAFE_OFF_FAILED,
    BZM_RUNTIME_STOP_UNAVAILABLE,
} bzm_runtime_stop_result_t;

/*
 * Initializes the Bonanza supervisor and immediately proves OFF_SAFE. This is
 * a no-op on non-Bonanza products. No requested target is persisted.
 */
esp_err_t bzm_validation_runtime_init(GlobalState * global_state);
bool bzm_validation_runtime_active(void);

/*
 * Called once after the network-facing mining queue exists. The production
 * controller performs the complete fixed-profile startup and starts the
 * normal mining tasks without an operator request.
 */
bool bzm_validation_runtime_mining_stack_ready(void);

/*
 * Powered requests require the exact BZM_RUNTIME_POWER_CONFIRMATION string
 * and a local arm that cannot be supplied by an HTTP client. Lab images may
 * use a one-shot USB Serial/JTAG command; other images sample BOOT. The
 * returned gate result describes admission. A request can be admitted while
 * its hardware report subsequently becomes BAD or BLOCKED, so callers must
 * also inspect the snapshot report.
 */
bzm_runtime_gate_result_t bzm_validation_runtime_request(bzm_validation_stage_t target_stage, bool hold_after_success,
                                                         uint32_t lease_ms, const char * confirmation);
bool bzm_validation_runtime_heartbeat(uint32_t lease_ms);
bzm_runtime_stop_result_t bzm_validation_runtime_stop(const char * reason);
bool bzm_validation_runtime_clear_fault(void);

/* Issues the configured RAM-only one-shot arm from the local USB console. */
bzm_local_arm_result_t bzm_validation_runtime_arm_local(const char * confirmation, uint32_t * remaining_ms);

bool bzm_validation_runtime_snapshot(bzm_validation_runtime_snapshot_t * snapshot);
bool bzm_validation_runtime_dispatch_allowed(void);

/* Exclusive, safe-off maintenance ownership for the bridge updater. */
bool bzm_validation_runtime_acquire_maintenance(bzm_supervisor_owner_t owner);
bool bzm_validation_runtime_release_maintenance(bzm_supervisor_owner_t owner);

/*
 * Non-BZM products return true without changing state. On BZM this closes
 * dispatch, proves OFF_SAFE, and holds exclusive ownership until reset.
 */
bool bzm_validation_runtime_prepare_restart(void);

#endif /* BZM_VALIDATION_RUNTIME_H */
