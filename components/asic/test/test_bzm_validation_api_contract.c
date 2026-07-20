#include <string.h>

#include "bzm_runtime_control.h"
#include "bzm_validation_api_contract.h"
#include "cJSON.h"
#include "unity.h"

static cJSON * parse_json(const char * text)
{
    cJSON * root = cJSON_Parse(text);
    TEST_ASSERT_NOT_NULL(root);
    return root;
}

static void assert_invalid_request(const char * text, uint32_t maximum_lease_seconds)
{
    cJSON * root = parse_json(text);
    bzm_validation_api_request_t request;
    memset(&request, 0xff, sizeof(request));
    TEST_ASSERT_FALSE(bzm_validation_api_parse_request(root, maximum_lease_seconds, &request));
    TEST_ASSERT_EQUAL_UINT32(0, request.lease_seconds);
    TEST_ASSERT_EQUAL_UINT8(0, request.confirmation[0]);
    cJSON_Delete(root);
}

TEST_CASE("BZM validation API contract accepts exact bounded JSON objects", "[asic][bzm][validation-api]")
{
    cJSON * root = parse_json("{\"targetStage\":7,\"hold\":true,\"leaseSeconds\":30,"
                              "\"confirm\":\"ENERGIZE_BZM_1002\"}");
    bzm_validation_api_request_t request;
    TEST_ASSERT_TRUE(bzm_validation_api_parse_request(root, 30, &request));
    TEST_ASSERT_EQUAL(BZM_STAGE_RUNNING, request.target_stage);
    TEST_ASSERT_TRUE(request.hold_after_success);
    TEST_ASSERT_EQUAL_UINT32(30, request.lease_seconds);
    TEST_ASSERT_EQUAL_STRING(BZM_RUNTIME_POWER_CONFIRMATION, request.confirmation);
    cJSON_Delete(root);

    root = parse_json("{\"leaseSeconds\":1}");
    bzm_validation_api_heartbeat_t heartbeat;
    TEST_ASSERT_TRUE(bzm_validation_api_parse_heartbeat(root, 30, &heartbeat));
    TEST_ASSERT_EQUAL_UINT32(1, heartbeat.lease_seconds);
    cJSON_Delete(root);

    root = parse_json("{}");
    bzm_validation_api_stop_t stop;
    TEST_ASSERT_TRUE(bzm_validation_api_parse_stop(root, &stop));
    TEST_ASSERT_EQUAL_STRING("operator stop", stop.reason);
    TEST_ASSERT_TRUE(bzm_validation_api_parse_empty_object(root));
    cJSON_Delete(root);

    root = parse_json("{\"reason\":\"manual validation stop\"}");
    TEST_ASSERT_TRUE(bzm_validation_api_parse_stop(root, &stop));
    TEST_ASSERT_EQUAL_STRING("manual validation stop", stop.reason);
    cJSON_Delete(root);
}

TEST_CASE("BZM validation API contract rejects ambiguous request schemas", "[asic][bzm][validation-api]")
{
    static const char * const invalid[] = {
        "{}",
        "{\"targetStage\":2,\"hold\":true,\"leaseSeconds\":5}",
        "{\"targetStage\":2,\"hold\":true,\"leaseSeconds\":5,"
        "\"confirm\":\"ENERGIZE_BZM_1002\",\"extra\":0}",
        "{\"targetStage\":2,\"hold\":true,\"hold\":false,"
        "\"leaseSeconds\":5,\"confirm\":\"ENERGIZE_BZM_1002\"}",
        "{\"targetStage\":-1,\"hold\":true,\"leaseSeconds\":5,"
        "\"confirm\":\"\"}",
        "{\"targetStage\":8,\"hold\":true,\"leaseSeconds\":5,"
        "\"confirm\":\"\"}",
        "{\"targetStage\":1.5,\"hold\":true,\"leaseSeconds\":5,"
        "\"confirm\":\"\"}",
        "{\"targetStage\":2,\"hold\":1,\"leaseSeconds\":5,"
        "\"confirm\":\"\"}",
        "{\"targetStage\":2,\"hold\":true,\"leaseSeconds\":31,"
        "\"confirm\":\"\"}",
        "{\"targetStage\":2,\"hold\":true,\"leaseSeconds\":1.5,"
        "\"confirm\":\"\"}",
        "{\"targetstage\":2,\"hold\":true,\"leaseSeconds\":5,"
        "\"confirm\":\"\"}",
        "{\"targetStage\":2,\"hold\":true,\"leaseSeconds\":5,"
        "\"confirm\":1}",
        "{\"targetStage\":2,\"hold\":true,\"leaseSeconds\":5,"
        "\"confirm\":\"123456789012345678901234567890123\"}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert_invalid_request(invalid[i], 30);
    }
}

TEST_CASE("BZM validation API action contracts reject loose or oversized input", "[asic][bzm][validation-api]")
{
    static const char * const invalid_heartbeats[] = {
        "{}",
        "{\"leaseSeconds\":0}",
        "{\"leaseSeconds\":31}",
        "{\"leaseSeconds\":1.5}",
        "{\"leaseSeconds\":1,\"extra\":true}",
        "{\"leaseSeconds\":1,\"leaseSeconds\":2}",
    };
    for (size_t i = 0; i < sizeof(invalid_heartbeats) / sizeof(invalid_heartbeats[0]); ++i) {
        cJSON * root = parse_json(invalid_heartbeats[i]);
        bzm_validation_api_heartbeat_t heartbeat;
        TEST_ASSERT_FALSE(bzm_validation_api_parse_heartbeat(root, 30, &heartbeat));
        cJSON_Delete(root);
    }

    cJSON * root = parse_json("{\"reason\":7}");
    bzm_validation_api_stop_t stop;
    TEST_ASSERT_FALSE(bzm_validation_api_parse_stop(root, &stop));
    cJSON_Delete(root);

    root = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(root);
    char long_reason[BZM_VALIDATION_API_MAX_STOP_REASON_LENGTH + 2];
    memset(long_reason, 'x', sizeof(long_reason) - 1);
    long_reason[sizeof(long_reason) - 1] = '\0';
    TEST_ASSERT_NOT_NULL(cJSON_AddStringToObject(root, "reason", long_reason));
    TEST_ASSERT_FALSE(bzm_validation_api_parse_stop(root, &stop));
    cJSON_Delete(root);

    root = parse_json("{\"unexpected\":true}");
    TEST_ASSERT_FALSE(bzm_validation_api_parse_stop(root, &stop));
    TEST_ASSERT_FALSE(bzm_validation_api_parse_empty_object(root));
    cJSON_Delete(root);

    root = parse_json("[]");
    TEST_ASSERT_FALSE(bzm_validation_api_parse_empty_object(root));
    cJSON_Delete(root);
}
