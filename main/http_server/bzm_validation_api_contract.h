#ifndef BZM_VALIDATION_API_CONTRACT_H
#define BZM_VALIDATION_API_CONTRACT_H

#include <stdbool.h>
#include <stdint.h>

#include "bzm_validation.h"
#include "cJSON.h"

#define BZM_VALIDATION_API_MAX_CONFIRM_LENGTH 32U
#define BZM_VALIDATION_API_MAX_STOP_REASON_LENGTH 96U

typedef struct
{
    bzm_validation_stage_t target_stage;
    bool hold_after_success;
    uint32_t lease_seconds;
    char confirmation[BZM_VALIDATION_API_MAX_CONFIRM_LENGTH + 1];
} bzm_validation_api_request_t;

typedef struct
{
    uint32_t lease_seconds;
} bzm_validation_api_heartbeat_t;

typedef struct
{
    char reason[BZM_VALIDATION_API_MAX_STOP_REASON_LENGTH + 1];
} bzm_validation_api_stop_t;

bool bzm_validation_api_parse_request(const cJSON * object, uint32_t maximum_lease_seconds, bzm_validation_api_request_t * request);
bool bzm_validation_api_parse_heartbeat(const cJSON * object, uint32_t maximum_lease_seconds,
                                        bzm_validation_api_heartbeat_t * heartbeat);
bool bzm_validation_api_parse_stop(const cJSON * object, bzm_validation_api_stop_t * stop);
bool bzm_validation_api_parse_empty_object(const cJSON * object);

#endif /* BZM_VALIDATION_API_CONTRACT_H */
