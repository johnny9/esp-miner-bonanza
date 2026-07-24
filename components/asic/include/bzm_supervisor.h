#ifndef BZM_SUPERVISOR_H
#define BZM_SUPERVISOR_H

#include <stdbool.h>
#include <stdint.h>

#include "bzm_validation.h"

#define BZM_SUPERVISOR_FAULT_DETAIL_LENGTH 160U

typedef enum
{
    BZM_SUPERVISOR_OWNER_NONE = 0,
    BZM_SUPERVISOR_OWNER_VALIDATION,
    BZM_SUPERVISOR_OWNER_MINING,
    BZM_SUPERVISOR_OWNER_BRIDGE_UPDATE,
    BZM_SUPERVISOR_OWNER_FACTORY_TEST,
    BZM_SUPERVISOR_OWNER_ESP_OTA,
    BZM_SUPERVISOR_OWNER_ESP_RESTART,
} bzm_supervisor_owner_t;

typedef struct
{
    bzm_validation_stage_t build_max_stage;
    bzm_validation_stage_t implemented_max_stage;
    bool powered_stages_compiled;
    bool production_mode;
    bool independent_kill_available;
    bool allow_esp_only_kill_in_lab;
    bool board_managed_safety;
    uint32_t maximum_lease_ms;
} bzm_supervisor_config_t;

typedef struct
{
    bzm_supervisor_config_t config;
    bzm_validation_ops_t validation_ops;
    void * ops_context;
    bzm_validation_report_t report;
    bzm_supervisor_owner_t owner;
    uint32_t next_run_id;
    uint64_t lease_deadline_ms;
    bool initialized;
    bool fault_latched;
    uint32_t fault_code;
    char fault_detail[BZM_SUPERVISOR_FAULT_DETAIL_LENGTH];
} bzm_supervisor_t;

bool bzm_supervisor_init(bzm_supervisor_t * supervisor, const bzm_supervisor_config_t * config,
                         const bzm_validation_ops_t * validation_ops, void * ops_context);

bool bzm_supervisor_request_validation(bzm_supervisor_t * supervisor, bzm_validation_stage_t target_stage, bool local_arm_present,
                                       bool hold_after_success, uint32_t lease_ms, uint64_t now_ms);

bool bzm_supervisor_stop(bzm_supervisor_t * supervisor, const char * reason);
bool bzm_supervisor_tick(bzm_supervisor_t * supervisor, uint64_t now_ms);
bool bzm_supervisor_heartbeat(bzm_supervisor_t * supervisor, uint32_t lease_ms, uint64_t now_ms);

bool bzm_supervisor_latch_fault(bzm_supervisor_t * supervisor, uint32_t fault_code, const char * detail);
bool bzm_supervisor_clear_fault(bzm_supervisor_t * supervisor);

bool bzm_supervisor_acquire_maintenance(bzm_supervisor_t * supervisor, bzm_supervisor_owner_t owner, uint64_t now_ms);
/*
 * Factory-blank bridge recovery is the one maintenance operation that cannot
 * obtain bridge-backed safe-off evidence. The caller must first prove the
 * independently controlled regulator is off and confirm the bridge control
 * link is unavailable.
 */
bool bzm_supervisor_acquire_bridge_recovery(
    bzm_supervisor_t *supervisor);
bool bzm_supervisor_release_maintenance(bzm_supervisor_t * supervisor, bzm_supervisor_owner_t owner);
bool bzm_supervisor_prepare_restart(bzm_supervisor_t * supervisor);

bool bzm_supervisor_dispatch_allowed(const bzm_supervisor_t * supervisor, uint64_t now_ms);
bool bzm_supervisor_safe_off_verified(const bzm_supervisor_t * supervisor);
bool bzm_supervisor_owner_is_maintenance(bzm_supervisor_owner_t owner);
const bzm_validation_report_t * bzm_supervisor_report(const bzm_supervisor_t * supervisor);

const char * bzm_supervisor_owner_name(bzm_supervisor_owner_t owner);

#endif // BZM_SUPERVISOR_H
