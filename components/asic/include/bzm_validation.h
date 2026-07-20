#ifndef BZM_VALIDATION_H
#define BZM_VALIDATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BZM_VALIDATION_SCHEMA_VERSION 1U
#define BZM_VALIDATION_DETAIL_LENGTH 160U

typedef enum
{
    BZM_STAGE_OFF_SAFE = 0,
    BZM_STAGE_CONTROLS,
    BZM_STAGE_POWER_RAIL,
    BZM_STAGE_CHAIN_4,
    BZM_STAGE_SENSORS,
    BZM_STAGE_CLOCKS,
    BZM_STAGE_BALANCED_RAMP,
    BZM_STAGE_RUNNING,
    BZM_STAGE_COUNT,
} bzm_validation_stage_t;

typedef enum
{
    BZM_CHECK_NOT_RUN = 0,
    BZM_CHECK_RUNNING,
    BZM_CHECK_GOOD,
    BZM_CHECK_BAD,
    BZM_CHECK_BLOCKED,
    BZM_CHECK_SKIPPED,
} bzm_check_status_t;

typedef enum
{
    BZM_VALIDATION_CODE_NONE = 0,
    BZM_VALIDATION_CODE_STAGE_OK,
    BZM_VALIDATION_CODE_STAGE_FAILED,
    BZM_VALIDATION_CODE_INVALID_CONFIGURATION,
    BZM_VALIDATION_CODE_BUILD_CEILING,
    BZM_VALIDATION_CODE_NOT_IMPLEMENTED,
    BZM_VALIDATION_CODE_POWER_NOT_COMPILED,
    BZM_VALIDATION_CODE_LOCAL_ARM_REQUIRED,
    BZM_VALIDATION_CODE_INDEPENDENT_KILL_REQUIRED,
    BZM_VALIDATION_CODE_POWER_LEASE_REQUIRED,
    BZM_VALIDATION_CODE_PREREQUISITE_FAILED,
    BZM_VALIDATION_CODE_SAFE_OFF_FAILED,
} bzm_validation_code_t;

typedef enum
{
    BZM_VALIDATION_IDLE = 0,
    BZM_VALIDATION_EXECUTING,
    BZM_VALIDATION_OFF_SAFE,
    BZM_VALIDATION_HOLDING,
    BZM_VALIDATION_FAULT_LATCHED,
    BZM_VALIDATION_SHUTDOWN_UNVERIFIED,
} bzm_validation_state_t;

typedef struct
{
    bzm_check_status_t status;
    bzm_validation_code_t code;
    char detail[BZM_VALIDATION_DETAIL_LENGTH];
} bzm_stage_result_t;

typedef struct
{
    uint32_t run_id;
    bzm_validation_stage_t requested_stage;
    bzm_validation_stage_t build_max_stage;
    bzm_validation_stage_t implemented_max_stage;
    bool powered_stages_compiled;
    bool local_arm_present;
    bool production_mode;
    bool independent_kill_available;
    bool allow_esp_only_kill_in_lab;
    bool board_managed_safety;
    bool hold_after_success;
    uint32_t lease_ms;
} bzm_validation_policy_t;

typedef struct
{
    bzm_stage_result_t (*run_stage)(void * context, bzm_validation_stage_t stage);
    bzm_stage_result_t (*force_safe_off)(void * context);
} bzm_validation_ops_t;

typedef struct
{
    uint8_t schema_version;
    uint32_t run_id;
    bzm_validation_stage_t requested_stage;
    bzm_validation_stage_t build_max_stage;
    bzm_validation_stage_t implemented_max_stage;
    bzm_validation_stage_t reached_stage;
    bzm_check_status_t overall;
    bzm_validation_state_t state;
    bool energized;
    uint32_t lease_ms;
    bzm_stage_result_t stages[BZM_STAGE_COUNT];
    bzm_stage_result_t final_safe_off;
} bzm_validation_report_t;

const char * bzm_validation_stage_name(bzm_validation_stage_t stage);
const char * bzm_validation_status_name(bzm_check_status_t status);
const char * bzm_validation_code_name(bzm_validation_code_t code);

bzm_stage_result_t bzm_validation_result(bzm_check_status_t status, bzm_validation_code_t code, const char * detail);

bool bzm_validation_execute(const bzm_validation_policy_t * policy, const bzm_validation_ops_t * ops, void * ops_context,
                            bzm_validation_report_t * report);

#endif // BZM_VALIDATION_H
