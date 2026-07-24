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

static uint32_t test_manifest_crc32(const uint8_t * bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void seal_manifest(uint8_t * manifest)
{
    uint32_t crc = test_manifest_crc32(
        manifest, BZM_BRIDGE_IMAGE_MANIFEST_SIZE - sizeof(crc));
    memcpy(
        manifest + BZM_BRIDGE_IMAGE_MANIFEST_SIZE - sizeof(crc),
        &crc, sizeof(crc));
}

static void add_valid_manifest(
    uint8_t * image, size_t image_size, size_t offset)
{
    static const uint8_t magic[16] = {
        'B', 'Z', 'M', '-', 'B', 'R', 'I', 'D',
        'G', 'E', '-', 'F', 'W', 0, 0, 0,
    };
    TEST_ASSERT_GREATER_OR_EQUAL(
        offset + BZM_BRIDGE_IMAGE_MANIFEST_SIZE, image_size);
    uint8_t * manifest = image + offset;
    memset(manifest, 0, BZM_BRIDGE_IMAGE_MANIFEST_SIZE);
    memcpy(manifest, magic, sizeof(magic));
    manifest[16] = 1;
    manifest[17] = BZM_BRIDGE_IMAGE_MANIFEST_SIZE;
    const uint16_t board_version =
        BZM_BRIDGE_IMAGE_TARGET_BOARD_VERSION;
    memcpy(manifest + 18, &board_version, sizeof(board_version));
    manifest[20] = 1;
    manifest[21] = BZM_BRIDGE_PROTOCOL_MAJOR;
    manifest[22] = BZM_BRIDGE_PROTOCOL_MINOR;
    manifest[23] = 5;
    memcpy(manifest + 24, "1.2.3", 5);
    seal_manifest(manifest);
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

TEST_CASE("bridge updater is enabled only for BZM bridge boards", "[asic][bzm][bridge-update][gate]")
{
    DeviceConfig config = {0};
    config.family.asic.id = BZM;
    config.bonanza_bridge = true;
    TEST_ASSERT_TRUE(bzm_bridge_update_enabled());
    TEST_ASSERT_TRUE(bzm_bridge_update_board_supported(&config));
    TEST_ASSERT_TRUE(bzm_bridge_update_supported(&config));
    TEST_ASSERT_TRUE(bzm_bridge_update_boot_recovery_allowed(
        &config, ESP_ERR_TIMEOUT));
    TEST_ASSERT_FALSE(bzm_bridge_update_boot_recovery_allowed(
        &config, ESP_OK));

    config.family.asic.id = BM1370;
    TEST_ASSERT_FALSE(bzm_bridge_update_board_supported(&config));
    TEST_ASSERT_FALSE(bzm_bridge_update_supported(&config));
    TEST_ASSERT_FALSE(bzm_bridge_update_boot_recovery_allowed(
        &config, ESP_ERR_TIMEOUT));
    config.family.asic.id = BZM;
    config.bonanza_bridge = false;
    TEST_ASSERT_FALSE(bzm_bridge_update_board_supported(&config));
    TEST_ASSERT_FALSE(bzm_bridge_update_supported(&config));
    TEST_ASSERT_FALSE(bzm_bridge_update_board_supported(NULL));
    TEST_ASSERT_FALSE(bzm_bridge_update_supported(NULL));
    TEST_ASSERT_FALSE(bzm_bridge_update_boot_recovery_allowed(
        NULL, ESP_ERR_TIMEOUT));
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

TEST_CASE("bridge updater identifies a valid embedded firmware manifest", "[asic][bzm][bridge-update][manifest]")
{
    uint8_t image[0x108 + BZM_BRIDGE_IMAGE_MANIFEST_SIZE];
    make_valid_image(image, sizeof(image));
    add_valid_manifest(image, sizeof(image), 0x108);

    bzm_bridge_image_manifest_t manifest;
    TEST_ASSERT_EQUAL(
        ESP_OK,
        bzm_bridge_update_validate_manifest(
            image, sizeof(image), &manifest));
    TEST_ASSERT_EQUAL_size_t(0x108, manifest.offset);
    TEST_ASSERT_EQUAL_UINT16(
        BZM_BRIDGE_IMAGE_TARGET_BOARD_VERSION,
        manifest.target_board_version);
    TEST_ASSERT_EQUAL_UINT8(
        BZM_BRIDGE_PROTOCOL_MAJOR, manifest.protocol_major);
    TEST_ASSERT_EQUAL_UINT8(
        BZM_BRIDGE_PROTOCOL_MINOR, manifest.protocol_minor);
    TEST_ASSERT_EQUAL_STRING("1.2.3", manifest.version);
}

TEST_CASE("bridge updater rejects missing duplicate and corrupt manifests", "[asic][bzm][bridge-update][manifest]")
{
    uint8_t image[
        0x108 + (2 * BZM_BRIDGE_IMAGE_MANIFEST_SIZE)];
    make_valid_image(image, sizeof(image));
    bzm_bridge_image_manifest_t manifest;

    TEST_ASSERT_EQUAL(
        ESP_ERR_NOT_FOUND,
        bzm_bridge_update_validate_manifest(
            image, sizeof(image), &manifest));

    add_valid_manifest(image, sizeof(image), 0x108);
    image[0x108 + BZM_BRIDGE_IMAGE_MANIFEST_SIZE - 1] ^= 0x01;
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_CRC,
        bzm_bridge_update_validate_manifest(
            image, sizeof(image), &manifest));

    add_valid_manifest(image, sizeof(image), 0x108);
    add_valid_manifest(
        image, sizeof(image),
        0x108 + BZM_BRIDGE_IMAGE_MANIFEST_SIZE);
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_RESPONSE,
        bzm_bridge_update_validate_manifest(
            image, sizeof(image), &manifest));
}

TEST_CASE("forced bridge uploads bypass only manifest identity", "[asic][bzm][bridge-update][manifest]")
{
    uint8_t image[0x108 + BZM_BRIDGE_IMAGE_MANIFEST_SIZE];
    make_valid_image(image, sizeof(image));
    bzm_bridge_image_manifest_t manifest;
    bool manifest_validated = true;

    TEST_ASSERT_EQUAL(
        ESP_ERR_NOT_FOUND,
        bzm_bridge_update_validate_upload(
            image, sizeof(image), false, &manifest,
            &manifest_validated));
    TEST_ASSERT_FALSE(manifest_validated);

    TEST_ASSERT_EQUAL(
        ESP_OK,
        bzm_bridge_update_validate_upload(
            image, sizeof(image), true, &manifest,
            &manifest_validated));
    TEST_ASSERT_FALSE(manifest_validated);

    image[0x104] &= 0xfe;
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_RESPONSE,
        bzm_bridge_update_validate_upload(
            image, sizeof(image), true, &manifest,
            &manifest_validated));
    TEST_ASSERT_FALSE(manifest_validated);
}

TEST_CASE("post-flash bridge identity must match the image manifest", "[asic][bzm][bridge-update][manifest]")
{
    bzm_bridge_image_manifest_t manifest = {
        .protocol_major = 1,
        .protocol_minor = 0,
    };
    strlcpy(manifest.version, "1.2.3", sizeof(manifest.version));
    bzm_bridge_info_t info = {
        .protocol_major = 1,
        .protocol_minor = 0,
    };
    strlcpy(info.version, "1.2.3", sizeof(info.version));

    TEST_ASSERT_TRUE(
        bzm_bridge_update_manifest_matches_info(
            &manifest, &info));
    info.protocol_major = 2;
    TEST_ASSERT_FALSE(
        bzm_bridge_update_manifest_matches_info(
            &manifest, &info));
    info.protocol_major = 1;
    strlcpy(info.version, "1.2.4", sizeof(info.version));
    TEST_ASSERT_FALSE(
        bzm_bridge_update_manifest_matches_info(
            &manifest, &info));
    TEST_ASSERT_FALSE(
        bzm_bridge_update_manifest_matches_info(NULL, &info));
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
