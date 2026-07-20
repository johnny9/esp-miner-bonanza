#include <string.h>

#include "bzm_bridge_swd.h"
#include "bzm_bridge_update.h"
#include "unity.h"

enum
{
    CALL_PREPARE = 1,
    CALL_FLASH,
    CALL_RESTORE,
    CALL_QUERY,
};

typedef struct
{
    int calls[8];
    size_t call_count;
    esp_err_t flash_result;
    esp_err_t query_result;
    bzm_bridge_update_state_t reported_state;
    uint8_t reported_progress;
} simulated_update_t;

static void make_valid_image(uint8_t * image, size_t size)
{
    memset(image, 0, size);
    image[0] = 0x42;
    const uint32_t stack_pointer = 0x20042000u;
    const uint32_t reset_vector = 0x10000101u;
    memcpy(image + 0x100, &stack_pointer, sizeof(stack_pointer));
    memcpy(image + 0x104, &reset_vector, sizeof(reset_vector));
}

static esp_err_t simulated_prepare(void * context)
{
    simulated_update_t * sim = context;
    sim->calls[sim->call_count++] = CALL_PREPARE;
    return ESP_OK;
}

static esp_err_t simulated_flash(void * context, const uint8_t * image, size_t image_size, bzm_bridge_flash_progress_fn progress,
                                 void * progress_context)
{
    simulated_update_t * sim = context;
    sim->calls[sim->call_count++] = CALL_FLASH;
    TEST_ASSERT_NOT_NULL(image);
    progress(progress_context, BZM_BRIDGE_FLASH_PROGRAMMING, image_size, image_size);
    return sim->flash_result;
}

static esp_err_t simulated_restore(void * context)
{
    simulated_update_t * sim = context;
    sim->calls[sim->call_count++] = CALL_RESTORE;
    return ESP_OK;
}

static esp_err_t simulated_query(void * context, bzm_bridge_info_t * info)
{
    simulated_update_t * sim = context;
    sim->calls[sim->call_count++] = CALL_QUERY;
    if (sim->query_result == ESP_OK) {
        info->schema_version = 1;
        info->protocol_major = 1;
        strlcpy(info->version, "1.2.3+gabcdef", sizeof(info->version));
    }
    return sim->query_result;
}

static void simulated_report(void * context, bzm_bridge_update_state_t state, uint8_t progress)
{
    simulated_update_t * sim = context;
    sim->reported_state = state;
    sim->reported_progress = progress;
}

static const bzm_bridge_update_ops_t SIMULATED_OPS = {
    .prepare = simulated_prepare,
    .flash = simulated_flash,
    .restore = simulated_restore,
    .query_info = simulated_query,
};

static bool simulated_maintenance(void * context)
{
    return context != NULL;
}

TEST_CASE("bridge updater requires a complete maintenance hook pair", "[asic][bzm][bridge-update][maintenance]")
{
    int context = 1;
    TEST_ASSERT_FALSE(BZM_bridge_update_set_maintenance_hooks(NULL, simulated_maintenance, &context));
    TEST_ASSERT_FALSE(BZM_bridge_update_set_maintenance_hooks(simulated_maintenance, NULL, &context));
    TEST_ASSERT_TRUE(BZM_bridge_update_set_maintenance_hooks(simulated_maintenance, simulated_maintenance, &context));
}

TEST_CASE("bridge updater is build-gated and limited to BZM bridge boards", "[asic][bzm][bridge-update][gate]")
{
    DeviceConfig config = {0};
    config.family.asic.id = BZM;
    config.bonanza_bridge = true;
    TEST_ASSERT_TRUE(bzm_bridge_update_board_supported(&config));
    TEST_ASSERT_EQUAL(bzm_bridge_update_enabled(), bzm_bridge_update_supported(&config));

    config.family.asic.id = BM1370;
    TEST_ASSERT_FALSE(bzm_bridge_update_board_supported(&config));
    TEST_ASSERT_FALSE(bzm_bridge_update_supported(&config));
    config.family.asic.id = BZM;
    config.bonanza_bridge = false;
    TEST_ASSERT_FALSE(bzm_bridge_update_board_supported(&config));
    TEST_ASSERT_FALSE(bzm_bridge_update_supported(&config));
    TEST_ASSERT_FALSE(bzm_bridge_update_board_supported(NULL));
    TEST_ASSERT_FALSE(bzm_bridge_update_supported(NULL));
}

TEST_CASE("bridge updater validates RP2040 raw images before maintenance", "[asic][bzm][bridge-update][validation]")
{
    uint8_t image[0x108];
    make_valid_image(image, sizeof(image));
    TEST_ASSERT_EQUAL(ESP_OK, bzm_bridge_update_validate_image(image, sizeof(image)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, bzm_bridge_update_validate_image(image, 0x107));

    image[0x104] &= 0xfe;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, bzm_bridge_update_validate_image(image, sizeof(image)));
    memset(image, 0xff, 0x100);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, bzm_bridge_update_validate_image(image, sizeof(image)));
}

TEST_CASE("bridge chunk plan never reads beyond image and pads only final page", "[asic][bzm][bridge-update][chunks]")
{
    bzm_bridge_flash_chunk_t chunk;
    size_t image_size = BZM_BRIDGE_FLASH_CHUNK_SIZE + 257;

    TEST_ASSERT_TRUE(bzm_bridge_flash_next_chunk(image_size, 0, &chunk));
    TEST_ASSERT_EQUAL(BZM_BRIDGE_FLASH_CHUNK_SIZE, chunk.source_length);
    TEST_ASSERT_EQUAL(chunk.source_length, chunk.program_length);

    TEST_ASSERT_TRUE(bzm_bridge_flash_next_chunk(image_size, BZM_BRIDGE_FLASH_CHUNK_SIZE, &chunk));
    TEST_ASSERT_EQUAL(BZM_BRIDGE_FLASH_PAGE_SIZE, chunk.source_length);
    TEST_ASSERT_EQUAL(BZM_BRIDGE_FLASH_PAGE_SIZE, chunk.program_length);

    TEST_ASSERT_TRUE(bzm_bridge_flash_next_chunk(image_size, BZM_BRIDGE_FLASH_CHUNK_SIZE + BZM_BRIDGE_FLASH_PAGE_SIZE, &chunk));
    TEST_ASSERT_EQUAL(1, chunk.source_length);
    TEST_ASSERT_EQUAL(BZM_BRIDGE_FLASH_PAGE_SIZE, chunk.program_length);
    TEST_ASSERT_FALSE(bzm_bridge_flash_next_chunk(image_size, image_size, &chunk));
}

TEST_CASE("bridge flash poll accepts a stub that completes before running is sampled", "[asic][bzm][bridge-update][swd-poll]")
{
    TEST_ASSERT_EQUAL(BZM_BRIDGE_STUB_POLL_COMPLETE,
                      bzm_bridge_flash_stub_poll_result(false, true, BZM_BRIDGE_STUB_STATUS_OK, BZM_BRIDGE_STUB_STAGE_DONE));
    TEST_ASSERT_EQUAL(BZM_BRIDGE_STUB_POLL_WAIT,
                      bzm_bridge_flash_stub_poll_result(false, true, BZM_BRIDGE_STUB_STATUS_IDLE, BZM_BRIDGE_STUB_STAGE_IDLE));
    TEST_ASSERT_EQUAL(BZM_BRIDGE_STUB_POLL_WAIT,
                      bzm_bridge_flash_stub_poll_result(true, false, BZM_BRIDGE_STUB_STATUS_BUSY, BZM_BRIDGE_STUB_STAGE_IDLE));
    TEST_ASSERT_EQUAL(BZM_BRIDGE_STUB_POLL_FAILED,
                      bzm_bridge_flash_stub_poll_result(true, true, BZM_BRIDGE_STUB_STATUS_BUSY, BZM_BRIDGE_STUB_STAGE_IDLE));
}

TEST_CASE("bridge update workflow restores bridge and confirms version", "[asic][bzm][bridge-update][workflow]")
{
    uint8_t image[0x108];
    make_valid_image(image, sizeof(image));
    simulated_update_t sim = {0};
    bzm_bridge_info_t info;
    bool query_supported = false;
    const int expected[] = {
        CALL_PREPARE,
        CALL_FLASH,
        CALL_RESTORE,
        CALL_QUERY,
    };

    TEST_ASSERT_EQUAL(
        ESP_OK, bzm_bridge_update_run(image, sizeof(image), &SIMULATED_OPS, &sim, simulated_report, &sim, &info, &query_supported));
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, sim.calls, sizeof(expected) / sizeof(expected[0]));
    TEST_ASSERT_TRUE(query_supported);
    TEST_ASSERT_EQUAL_STRING("1.2.3+gabcdef", info.version);
    TEST_ASSERT_EQUAL(BZM_BRIDGE_UPDATE_QUERYING, sim.reported_state);
    TEST_ASSERT_EQUAL_UINT8(100, sim.reported_progress);
}

TEST_CASE("bridge update workflow restores maintenance state after flash failure", "[asic][bzm][bridge-update][rollback]")
{
    uint8_t image[0x108];
    make_valid_image(image, sizeof(image));
    simulated_update_t sim = {.flash_result = ESP_FAIL};
    const int expected[] = {CALL_PREPARE, CALL_FLASH, CALL_RESTORE};

    TEST_ASSERT_EQUAL(ESP_FAIL,
                      bzm_bridge_update_run(image, sizeof(image), &SIMULATED_OPS, &sim, simulated_report, &sim, NULL, NULL));
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, sim.calls, sizeof(expected) / sizeof(expected[0]));
}

TEST_CASE("older bridge firmware may update without version command support", "[asic][bzm][bridge-update][compatibility]")
{
    uint8_t image[0x108];
    make_valid_image(image, sizeof(image));
    simulated_update_t sim = {.query_result = ESP_ERR_NOT_SUPPORTED};
    bool query_supported = true;

    TEST_ASSERT_EQUAL(ESP_OK,
                      bzm_bridge_update_run(image, sizeof(image), &SIMULATED_OPS, &sim, NULL, NULL, NULL, &query_supported));
    TEST_ASSERT_FALSE(query_supported);
}

TEST_CASE("bridge update fails when post-flash version query times out", "[asic][bzm][bridge-update][confirmation]")
{
    uint8_t image[0x108];
    make_valid_image(image, sizeof(image));
    simulated_update_t sim = {.query_result = ESP_ERR_TIMEOUT};
    const int expected[] = {
        CALL_PREPARE,
        CALL_FLASH,
        CALL_RESTORE,
        CALL_QUERY,
    };

    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, bzm_bridge_update_run(image, sizeof(image), &SIMULATED_OPS, &sim, NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, sim.calls, sizeof(expected) / sizeof(expected[0]));
}
