#include "bzm_validation_api_contract.h"

#include <math.h>
#include <string.h>

static bool object_has_exact_keys(const cJSON * object, const char * const * allowed, size_t allowed_count, uint32_t required_mask)
{
    if (!cJSON_IsObject(object) || allowed_count > 32)
        return false;
    uint32_t seen = 0;
    const cJSON * item = NULL;
    cJSON_ArrayForEach(item, object)
    {
        if (item->string == NULL)
            return false;
        size_t index = 0;
        while (index < allowed_count && strcmp(item->string, allowed[index]) != 0) {
            index++;
        }
        if (index == allowed_count || (seen & (1U << index)) != 0) {
            return false;
        }
        seen |= 1U << index;
    }
    return (seen & required_mask) == required_mask;
}

static bool json_uint32_between(const cJSON * item, uint32_t minimum, uint32_t maximum, uint32_t * value)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < (double) minimum ||
        item->valuedouble > (double) maximum) {
        return false;
    }
    uint32_t integer = (uint32_t) item->valuedouble;
    if ((double) integer != item->valuedouble)
        return false;
    if (value != NULL)
        *value = integer;
    return true;
}

static bool copy_json_string(const cJSON * item, char * destination, size_t destination_size, size_t maximum_length)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL || destination == NULL || destination_size <= maximum_length) {
        return false;
    }
    size_t length = strnlen(item->valuestring, maximum_length + 1);
    if (length > maximum_length)
        return false;
    memcpy(destination, item->valuestring, length + 1);
    return true;
}

bool bzm_validation_api_parse_request(const cJSON * object, uint32_t maximum_lease_seconds, bzm_validation_api_request_t * request)
{
    static const char * const keys[] = {
        "targetStage",
        "hold",
        "leaseSeconds",
        "confirm",
    };
    if (request == NULL)
        return false;
    memset(request, 0, sizeof(*request));

    uint32_t target_stage;
    const cJSON * target = cJSON_GetObjectItemCaseSensitive(object, "targetStage");
    const cJSON * hold = cJSON_GetObjectItemCaseSensitive(object, "hold");
    const cJSON * lease = cJSON_GetObjectItemCaseSensitive(object, "leaseSeconds");
    const cJSON * confirm = cJSON_GetObjectItemCaseSensitive(object, "confirm");
    if (!object_has_exact_keys(object, keys, 4, 0x0fU) ||
        !json_uint32_between(target, BZM_STAGE_OFF_SAFE, BZM_STAGE_RUNNING, &target_stage) || !cJSON_IsBool(hold) ||
        !json_uint32_between(lease, 0, maximum_lease_seconds, &request->lease_seconds) ||
        !copy_json_string(confirm, request->confirmation, sizeof(request->confirmation), BZM_VALIDATION_API_MAX_CONFIRM_LENGTH)) {
        memset(request, 0, sizeof(*request));
        return false;
    }
    request->target_stage = (bzm_validation_stage_t) target_stage;
    request->hold_after_success = cJSON_IsTrue(hold);
    return true;
}

bool bzm_validation_api_parse_heartbeat(const cJSON * object, uint32_t maximum_lease_seconds,
                                        bzm_validation_api_heartbeat_t * heartbeat)
{
    static const char * const keys[] = {"leaseSeconds"};
    if (heartbeat == NULL)
        return false;
    memset(heartbeat, 0, sizeof(*heartbeat));
    const cJSON * lease = cJSON_GetObjectItemCaseSensitive(object, "leaseSeconds");
    return object_has_exact_keys(object, keys, 1, 0x01U) &&
           json_uint32_between(lease, 1, maximum_lease_seconds, &heartbeat->lease_seconds);
}

bool bzm_validation_api_parse_stop(const cJSON * object, bzm_validation_api_stop_t * stop)
{
    static const char * const keys[] = {"reason"};
    if (stop == NULL)
        return false;
    memset(stop, 0, sizeof(*stop));
    const cJSON * reason = cJSON_GetObjectItemCaseSensitive(object, "reason");
    if (!object_has_exact_keys(object, keys, 1, 0))
        return false;
    if (reason == NULL) {
        memcpy(stop->reason, "operator stop", sizeof("operator stop"));
        return true;
    }
    return copy_json_string(reason, stop->reason, sizeof(stop->reason), BZM_VALIDATION_API_MAX_STOP_REASON_LENGTH);
}

bool bzm_validation_api_parse_empty_object(const cJSON * object)
{
    return cJSON_IsObject(object) && object->child == NULL;
}
