#include <stdlib.h>
#include <string.h>

#include "asic_capabilities.h"
#include "asic_job_store.h"
#include "bm_job_builder.h"
#include "bzm.h"
#include "bzm_bridge.h"
#include "bzm_reactor.h"
#include "bzm_transport.h"
#include "device_config.h"
#include "mining.h"
#include "unity.h"
#include "utils.h"

#define BZM_TEST_MAX_WRITES (ASIC_JOB_STORE_CAPACITY + 4U)

typedef struct {
    size_t write_count;
    size_t checkpoint_count;
    size_t flush_count;
    size_t fail_after;
    bool fail_immediately;
    bool fail_checkpoint;
    bool fail_flush;
    bzm_work_t work[BZM_TEST_MAX_WRITES];
} simulated_transport_t;

typedef struct {
    uint16_t engine_id;
    uint8_t offset;
    size_t data_len;
    uint8_t data[32];
} register_write_t;

typedef struct {
    size_t count;
    register_write_t writes[64];
} register_capture_t;

typedef struct {
    size_t available;
    size_t programmed;
    size_t read_count;
    size_t broadcast_noop_count;
    size_t addressed_noop_count;
    uint8_t programmed_ids[BZM_MAX_ASIC_COUNT];
    uint8_t chain_enabled[BZM_MAX_ASIC_COUNT];
} simulated_chain_t;

static bool simulated_noop(void *context, uint8_t asic_id)
{
    simulated_chain_t *chain = context;
    if (asic_id == BZM_BROADCAST_ASIC) {
        chain->broadcast_noop_count++;
        return chain->programmed < chain->available;
    }
    chain->addressed_noop_count++;
    size_t index = 0;
    return bzm_topology_asic_index(asic_id, &index) && index < chain->programmed;
}

static bool simulated_chain_write(void *context, uint8_t asic_id,
                                  uint16_t engine_id, uint8_t offset,
                                  const void *data, size_t data_len)
{
    simulated_chain_t *chain = context;
    const uint8_t *bytes = data;
    if (asic_id != BZM_BROADCAST_ASIC ||
        engine_id != BZM_CONTROL_ENGINE_ID || offset != 0x0b ||
        data_len != 4 || chain->programmed >= chain->available) {
        return false;
    }
    chain->programmed_ids[chain->programmed] = bytes[0];
    chain->chain_enabled[chain->programmed] = bytes[1];
    chain->programmed++;
    return true;
}

static bool simulated_chain_read(void *context, uint8_t asic_id,
                                 uint16_t engine_id, uint8_t offset,
                                 void *data, size_t data_len)
{
    simulated_chain_t *chain = context;
    uint8_t *bytes = data;
    size_t index = 0;
    if (engine_id != BZM_CONTROL_ENGINE_ID || offset != 0x0b ||
        data_len != 4 || !bzm_topology_asic_index(asic_id, &index) || index >= chain->programmed) {
        return false;
    }
    memset(bytes, 0, data_len);
    bytes[0] = chain->programmed_ids[index];
    bytes[1] = chain->chain_enabled[index];
    chain->read_count++;
    return true;
}

static void simulated_chain_delay(void *context, uint32_t delay_ms)
{
    (void)context;
    TEST_ASSERT_EQUAL_UINT32(200, delay_ms);
}

static const bzm_chain_ops_t SIMULATED_CHAIN_OPS = {
    .noop = simulated_noop,
    .write_register = simulated_chain_write,
    .read_register = simulated_chain_read,
    .delay_ms = simulated_chain_delay,
};

static bool capture_register(void *context, uint16_t engine_id,
                             uint8_t offset, const void *data,
                             size_t data_len)
{
    register_capture_t *capture = context;
    if (capture->count >= 64 || data_len > 32) return false;
    register_write_t *write = &capture->writes[capture->count++];
    write->engine_id = engine_id;
    write->offset = offset;
    write->data_len = data_len;
    memcpy(write->data, data, data_len);
    return true;
}

static bool simulated_write(void *context, const bzm_work_t *work)
{
    simulated_transport_t *transport = context;
    if (transport->fail_immediately ||
        transport->write_count >= BZM_TEST_MAX_WRITES ||
        (transport->fail_after != 0 &&
         transport->write_count >= transport->fail_after)) {
        return false;
    }
    transport->work[transport->write_count++] = *work;
    return true;
}

static bool simulated_flush(void *context)
{
    simulated_transport_t *transport = context;
    transport->flush_count++;
    return !transport->fail_flush;
}

static bool simulated_checkpoint(void *context)
{
    simulated_transport_t *transport = context;
    transport->checkpoint_count++;
    return !transport->fail_checkpoint;
}

static const bzm_transport_ops_t SIMULATED_OPS = {
    .write_work = simulated_write,
    .dispatch_checkpoint = simulated_checkpoint,
    .flush = simulated_flush,
};

static simulated_transport_t *new_transport(void)
{
    simulated_transport_t *transport = calloc(1, sizeof(*transport));
    TEST_ASSERT_NOT_NULL(transport);
    return transport;
}

static asic_job_store_t *new_store(void)
{
    asic_job_store_t *store = calloc(1, sizeof(*store));
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_TRUE(asic_job_store_init(store));
    return store;
}

static void delete_store(asic_job_store_t *store)
{
    asic_job_store_destroy(store);
    free(store);
}

static mining_template_t bzm_template(const char *job_id, bool clean_jobs)
{
    mining_template_t template = {
        .version = 0x20000004,
        .version_mask = 0x00006000,
        .ntime = 0x65010203,
        .target = 0x1705dd01,
        .starting_nonce = 0x10203040,
        .clean_jobs = clean_jobs,
        .share = {
            .protocol = MINING_PROTOCOL_SV2_STANDARD,
            .numeric_job_id = 42,
            .pool_difficulty = 1,
        },
    };
    for (size_t i = 0; i < 32; ++i) {
        template.prev_block_hash[i] = i;
        template.merkle_root[i] = 0x20 + i;
    }
    template.share.job_id = strdup(job_id);
    template.share.extranonce2 = strdup("");
    TEST_ASSERT_NOT_NULL(template.share.job_id);
    TEST_ASSERT_NOT_NULL(template.share.extranonce2);
    return template;
}

TEST_CASE("Bitaxe 1002 selects the Bonanza board profile",
          "[asic][bzm][board][1002]")
{
    const DeviceConfig *board = NULL;
    for (size_t i = 0;
         i < sizeof(default_configs) / sizeof(default_configs[0]); ++i) {
        if (strcmp(default_configs[i].board_version, "1002") == 0) {
            board = &default_configs[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(board);
    TEST_ASSERT_EQUAL(BONANZA, board->family.id);
    TEST_ASSERT_EQUAL_STRING("Bonanza", board->family.name);
    TEST_ASSERT_EQUAL(BZM, board->family.asic.id);
    TEST_ASSERT_EQUAL_STRING("BZM", board->family.asic.name);
    TEST_ASSERT_EQUAL_UINT8(4, board->family.asic_count);
    TEST_ASSERT_EQUAL_UINT16(BZM_ENGINES_PER_ASIC,
                             board->family.asic.core_count);
    TEST_ASSERT_EQUAL_UINT16(BZM_ENGINES_PER_ASIC,
                             board->family.asic.small_core_count);
    TEST_ASSERT_EQUAL_UINT16(50,
                             board->family.asic.reference_clock_mhz);
    TEST_ASSERT_EQUAL_UINT16(800,
                             board->family.asic.default_frequency_mhz);
    TEST_ASSERT_EQUAL_UINT16(800,
                             board->family.asic.frequency_options[0]);
    TEST_ASSERT_EQUAL_UINT16(0,
                             board->family.asic.frequency_options[1]);
    TEST_ASSERT_FALSE(board->family.asic.frequency_tunable);
    TEST_ASSERT_TRUE(device_config_accepts_frequency(board, 800.0f));
    TEST_ASSERT_FALSE(device_config_accepts_frequency(board, 50.0f));
    TEST_ASSERT_FALSE(device_config_accepts_frequency(board, 799.0f));
    TEST_ASSERT_FALSE(device_config_accepts_frequency(board, 801.0f));
    TEST_ASSERT_EQUAL_UINT16(2800,
                             board->family.asic.default_voltage_mv);
    TEST_ASSERT_EQUAL_UINT16(2800,
                             board->family.asic.voltage_options[0]);
    TEST_ASSERT_EQUAL_UINT16(0,
                             board->family.asic.voltage_options[1]);
    TEST_ASSERT_FALSE(board->family.voltage_tunable);
    TEST_ASSERT_TRUE(device_config_accepts_voltage(board, 2800));
    TEST_ASSERT_FALSE(device_config_accepts_voltage(board, 2700));
    TEST_ASSERT_FALSE(device_config_accepts_voltage(board, 2900));
    TEST_ASSERT_EQUAL_UINT16(140, board->family.max_power);
    TEST_ASSERT_EQUAL_UINT16(0, board->family.power_offset);
    TEST_ASSERT_EQUAL_UINT16(12, board->family.nominal_voltage);
    TEST_ASSERT_EQUAL_UINT16(1, board->family.voltage_domains);
    TEST_ASSERT_EQUAL_STRING("yellow", board->family.swarm_color);
    TEST_ASSERT_TRUE(board->TPS546);
    TEST_ASSERT_TRUE(board->bonanza_bridge);
    TEST_ASSERT_FALSE(board->asic_enable);
    TEST_ASSERT_FALSE(board->plug_sense);
    TEST_ASSERT_FALSE(board->EMC2101);
    TEST_ASSERT_FALSE(board->EMC2103);
    TEST_ASSERT_FALSE(board->EMC2302);
    TEST_ASSERT_FALSE(board->TMP1075);
    TEST_ASSERT_FALSE(board->DS4432U);
    TEST_ASSERT_FALSE(board->INA260);
    TEST_ASSERT_EQUAL_UINT16(0, board->power_consumption_target);
    TEST_ASSERT_EQUAL_UINT32(115200, BZM_BRIDGE_CONTROL_BAUD_RATE);
    TEST_ASSERT_EQUAL_UINT8(43, BZM_BRIDGE_CONTROL_TX_GPIO);
    TEST_ASSERT_EQUAL_UINT8(44, BZM_BRIDGE_CONTROL_RX_GPIO);
}

static bzm_reactor_t *new_reactor(asic_job_store_t *store,
                                  simulated_transport_t *transport,
                                  uint16_t engine_count)
{
    bzm_reactor_t *reactor = calloc(1, sizeof(*reactor));
    TEST_ASSERT_NOT_NULL(reactor);
    bzm_reactor_config_t config = {
        .engine_count = engine_count,
        .timestamp_count = 16,
        .lead_zeros = 36,
        .nonce_offset = BZM_NONCE_GAP_1002,
        .enhanced_mode = true,
    };
    TEST_ASSERT_TRUE(bzm_reactor_init(reactor, store, &config,
                                      &SIMULATED_OPS, transport));
    return reactor;
}

TEST_CASE("BZM work builder derives four family-private midstates",
          "[asic][bzm][work]")
{
    mining_template_t template = bzm_template("work", true);
    template.version_mask = 0x1fffe000;
    asic_work_t source = {
        .handle = 0x1234,
        .template = &template,
    };
    bzm_work_t work;
    TEST_ASSERT_TRUE(bzm_work_build(&source, 7, 5, 16, 36, true,
                                    &work));
    TEST_ASSERT_EQUAL_UINT16(7, work.engine_id);
    TEST_ASSERT_EQUAL_UINT8(5, work.logical_sequence);
    TEST_ASSERT_EQUAL_UINT8(4, work.midstate_count);
    TEST_ASSERT_EQUAL_HEX32(0x20000004, work.versions[0]);
    TEST_ASSERT_EQUAL_HEX32(0x2000e004, work.versions[1]);
    TEST_ASSERT_EQUAL_HEX32(0x3fff0004, work.versions[2]);
    TEST_ASSERT_EQUAL_HEX32(0x3fffe004, work.versions[3]);
    TEST_ASSERT_EQUAL_HEX32(template.ntime, work.start_ntime);
    TEST_ASSERT_EQUAL_HEX32(template.target, work.target);
    TEST_ASSERT_EQUAL_HEX32(template.starting_nonce, work.starting_nonce);
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, work.end_nonce);

    bm_job bm;
    TEST_ASSERT_TRUE(bm_job_build(&template, &bm));
    for (size_t word = 0; word < 8; ++word) {
        const uint8_t *big_endian_word = bm.midstate + (7 - word) * 4;
        const uint8_t expected_birds_word[4] = {
            big_endian_word[3], big_endian_word[2],
            big_endian_word[1], big_endian_word[0],
        };
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_birds_word,
                                      work.midstates[0] + word * 4, 4);
    }
    mining_template_free(&template);
}

TEST_CASE("BZM result frame decoder follows the mixed-endian wire layout",
          "[asic][bzm][result]")
{
    uint8_t frame[BZM_RESULT_FRAME_SIZE] = {
        0x83, 0x45, 0x78, 0x56, 0x34, 0x12, 0x17, 0x0d,
    };
    bzm_raw_result_t result;
    TEST_ASSERT_TRUE(bzm_result_decode(frame, 999, &result));
    TEST_ASSERT_EQUAL_UINT16(0x345, result.engine_id);
    TEST_ASSERT_EQUAL_UINT8(8, result.status);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, result.nonce);
    TEST_ASSERT_EQUAL_UINT8(0x17, result.sequence_id);
    TEST_ASSERT_EQUAL_UINT8(0x0d, result.time);
    TEST_ASSERT_EQUAL_UINT32(999, (uint32_t)result.timestamp_us);
}

TEST_CASE("BZM TDM result decoder preserves the ASIC address",
          "[asic][bzm][result]")
{
    uint8_t frame[BZM_TDM_RESULT_FRAME_SIZE] = {
        0x1e, 0x01,
        0x83, 0x45, 0x78, 0x56, 0x34, 0x12, 0x17, 0x0d,
    };
    bzm_raw_result_t result;
    TEST_ASSERT_TRUE(bzm_tdm_result_decode(frame, 999, &result));
    TEST_ASSERT_EQUAL_HEX8(0x1e, result.asic_id);
    TEST_ASSERT_EQUAL_UINT16(0x345, result.engine_id);
    frame[1] = 0x03;
    TEST_ASSERT_FALSE(bzm_tdm_result_decode(frame, 999, &result));
}

TEST_CASE("BZM temperature conversion follows the Intel 12-bit formula",
          "[asic][bzm][temperature]")
{
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 22.02f,
                             bzm_temperature_from_code(0x800));
}

TEST_CASE("BZM compact engine IDs skip every disabled 1002 coordinate",
          "[asic][bzm][engine-map]")
{
    TEST_ASSERT_EQUAL_UINT16(236, BZM_ENGINES_PER_ASIC);
    TEST_ASSERT_EQUAL_UINT16(240, BZM_ENGINE_GRID_COUNT);

    for (uint16_t logical = 0; logical < BZM_ENGINES_PER_ASIC; ++logical) {
        uint16_t physical;
        uint16_t round_trip;
        bzm_engine_location_t expected;
        TEST_ASSERT_TRUE(bzm_topology_engine_at(logical, &expected));
        TEST_ASSERT_TRUE(bzm_engine_physical_id(logical, &physical));
        TEST_ASSERT_EQUAL_UINT16(expected.physical_id, physical);
        TEST_ASSERT_TRUE(bzm_engine_logical_id(physical, &round_trip));
        TEST_ASSERT_EQUAL_UINT16(logical, round_trip);
    }

    static const struct {
        uint16_t logical;
        uint16_t physical;
    } boundaries[] = {
        {0, 0},
        {19, 19},
        {20, 64},
        {79, 211},
        {80, 257},
        {98, 275},
        {99, 321},
        {116, 338},
        {117, 384},
        {235, 722},
    };
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        uint16_t physical;
        TEST_ASSERT_TRUE(bzm_engine_physical_id(boundaries[i].logical,
                                                &physical));
        TEST_ASSERT_EQUAL_UINT16(boundaries[i].physical, physical);
    }

    uint16_t value;
    TEST_ASSERT_FALSE(bzm_engine_physical_id(BZM_ENGINES_PER_ASIC, &value));
    TEST_ASSERT_FALSE(bzm_engine_physical_id(0, NULL));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(0, NULL));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(20, &value));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(256, &value));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(320, &value));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(339, &value));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(723, &value));
}

TEST_CASE("BZM transport encoder emits byte-paired 9-bit write words",
          "[asic][bzm][transport]")
{
    TEST_ASSERT_EQUAL_HEX8(0xfa, BZM_BROADCAST_ASIC);
    TEST_ASSERT_EQUAL_HEX8(0xff, BZM_ALL_ASICS);
    uint8_t encoded[32];
    uint8_t data[] = {0xaa, 0xbb};
    size_t length = bzm_transport_encode_write(
        0xfa, 0x345, 0x40, data, sizeof(data), encoded,
        sizeof(encoded));
    const uint8_t expected[] = {
        0xfa, 0x01, // address word 0x1fa
        0x23, 0x00, // write opcode plus engine high nibble
        0x45, 0x00, // engine low byte
        0x40, 0x00, // register
        0x01, 0x00, // byte count minus one
        0xaa, 0x00,
        0xbb, 0x00,
        0x00, 0x00,
    };
    TEST_ASSERT_EQUAL(sizeof(expected), length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, encoded, sizeof(expected));
    TEST_ASSERT_EQUAL(0, bzm_transport_encode_write(
        0, BZM_MAX_ENGINE_COUNT, 0, data, sizeof(data), encoded,
        sizeof(encoded)));
}

TEST_CASE("BZM transport encodes read and noop commands",
          "[asic][bzm][transport]")
{
    uint8_t encoded[16];
    const uint8_t expected_read[] = {
        0x42, 0x01,
        0x3f, 0x00,
        0xff, 0x00,
        0x0b, 0x00,
        0x03, 0x00,
        0x00, 0x00,
    };
    TEST_ASSERT_EQUAL(sizeof(expected_read), bzm_transport_encode_read(
        0x42, BZM_CONTROL_ENGINE_ID, 0x0b, 4, encoded,
        sizeof(encoded)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_read, encoded,
                                  sizeof(expected_read));

    const uint8_t expected_noop[] = {
        0xfa, 0x01,
        0xf0, 0x00,
    };
    TEST_ASSERT_EQUAL(sizeof(expected_noop), bzm_transport_encode_noop(
        BZM_BROADCAST_ASIC, encoded, sizeof(encoded)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_noop, encoded,
                                  sizeof(expected_noop));
}

TEST_CASE("BZM chain discovery reports every chain length from zero to four",
          "[asic][bzm][discovery]")
{
    for (size_t available = 0; available <= BZM_MAX_ASIC_COUNT;
         ++available) {
        uint8_t ids[BZM_MAX_ASIC_COUNT] = {0};
        simulated_chain_t chain = {.available = available};
        TEST_ASSERT_EQUAL_UINT32(available, bzm_discover_chain(
            BZM_MAX_ASIC_COUNT, ids, sizeof(ids),
            &SIMULATED_CHAIN_OPS, &chain));
        TEST_ASSERT_EQUAL_UINT32(available, chain.read_count);
        TEST_ASSERT_EQUAL_UINT32(0, chain.addressed_noop_count);
        for (size_t i = 0; i < available; ++i) {
            TEST_ASSERT_EQUAL_UINT8(bzm_asic_wire_ids[i], ids[i]);
            TEST_ASSERT_EQUAL_UINT8(bzm_asic_wire_ids[i], chain.programmed_ids[i]);
            TEST_ASSERT_EQUAL_UINT8(i == 0 ? 0 : 1,
                                    chain.chain_enabled[i]);
        }
    }
}

TEST_CASE("BZM transport partitions an engine nonce range across ASICs",
          "[asic][bzm][transport][nonce]")
{
    for (size_t i = 0; i < 4; ++i) {
        uint32_t start;
        uint32_t end;
        TEST_ASSERT_TRUE(bzm_partition_nonce_range(
            0x10000000, 0x1fffffff, i, 4, &start, &end));
        TEST_ASSERT_EQUAL_HEX32(0x10000000 + i * 0x04000000,
                                start);
        TEST_ASSERT_EQUAL_HEX32(0x13ffffff + i * 0x04000000,
                                end);
    }
    uint32_t start;
    uint32_t end;
    TEST_ASSERT_FALSE(bzm_partition_nonce_range(
        4, 3, 0, 1, &start, &end));
}

TEST_CASE("BZM transport programs ordered enhanced work and flush jobs",
          "[asic][bzm][transport][program]")
{
    mining_template_t template = bzm_template("transport", false);
    asic_work_t source = {
        .handle = 0x1234,
        .template = &template,
    };
    bzm_work_t work;
    TEST_ASSERT_TRUE(bzm_work_build(&source, 7, 5, 16, 36, true,
                                    &work));

    register_capture_t capture = {0};
    TEST_ASSERT_TRUE(bzm_transport_program_work(
        &work, capture_register, &capture));
    TEST_ASSERT_EQUAL_UINT32(16, capture.count);
    TEST_ASSERT_EQUAL_HEX8(0x90, capture.writes[1].data[0]);
    static const uint8_t expected_merkle_residue[] = {0x23, 0x22, 0x21, 0x20};
    static const uint8_t expected_ntime[] = {0x65, 0x01, 0x02, 0x03};
    static const uint8_t expected_target[] = {0x17, 0x05, 0xdd, 0x01};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_merkle_residue, capture.writes[4].data, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_ntime, capture.writes[5].data, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_target, capture.writes[6].data, 4);
    for (size_t i = 7; i < 11; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x10, capture.writes[i].offset);
    }
    for (size_t i = 11; i < 15; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x38, capture.writes[i].offset);
        TEST_ASSERT_EQUAL_UINT8(20 + (i - 11),
                                capture.writes[i].data[0]);
    }
    TEST_ASSERT_EQUAL_HEX8(0x39, capture.writes[15].offset);
    TEST_ASSERT_EQUAL_UINT8(3, capture.writes[15].data[0]);

    TEST_ASSERT_TRUE(bzm_result_queue_capacity_covers(
        BZM_PENDING_RESULT_COUNT,
        BZM_RESULT_DESIGN_RATE_PER_SECOND,
        BZM_RESULT_MAX_DISPATCH_BLACKOUT_MS));
    TEST_ASSERT_FALSE(bzm_result_queue_capacity_covers(
        32U,
        BZM_RESULT_DESIGN_RATE_PER_SECOND,
        BZM_RESULT_MAX_DISPATCH_BLACKOUT_MS));

    memset(&capture, 0, sizeof(capture));
    TEST_ASSERT_TRUE(bzm_transport_program_flush(
        1, true, capture_register, &capture));
    TEST_ASSERT_EQUAL_UINT32(12, capture.count);
    TEST_ASSERT_EQUAL_HEX8(0xff, capture.writes[0].data[0]);
    for (size_t i = 1; i < 5; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0xfb + i, capture.writes[i].data[0]);
    }
    TEST_ASSERT_EQUAL_UINT8(3, capture.writes[5].data[0]);
    TEST_ASSERT_EQUAL_UINT8(1, capture.writes[11].data[0]);

    memset(&capture, 0, sizeof(capture));
    TEST_ASSERT_TRUE(bzm_transport_program_stage6_sentinel(
        10, capture_register, &capture));
    TEST_ASSERT_EQUAL_UINT32(22, capture.count);
    TEST_ASSERT_EQUAL_UINT16(10, capture.writes[0].engine_id);
    TEST_ASSERT_EQUAL_HEX8(0x49, capture.writes[0].offset);
    TEST_ASSERT_EQUAL_UINT8(32, capture.writes[0].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xff, capture.writes[1].data[0]);
    for (size_t i = 11; i < 15; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x38, capture.writes[i].offset);
        TEST_ASSERT_EQUAL_UINT8(0xfc + (i - 11), capture.writes[i].data[0]);
    }
    TEST_ASSERT_EQUAL_HEX8(0x39, capture.writes[15].offset);
    TEST_ASSERT_EQUAL_UINT8(3, capture.writes[15].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xff, capture.writes[16].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x39, capture.writes[21].offset);
    TEST_ASSERT_EQUAL_UINT8(1, capture.writes[21].data[0]);

    memset(&capture, 0, sizeof(capture));
    TEST_ASSERT_TRUE(bzm_transport_program_flush(
        2, true, capture_register, &capture));
    TEST_ASSERT_EQUAL_UINT32(24, capture.count);
    TEST_ASSERT_EQUAL_UINT16(0, capture.writes[0].engine_id);
    TEST_ASSERT_EQUAL_UINT16(10, capture.writes[12].engine_id);

    mining_template_free(&template);
}

TEST_CASE("BZM reactor dispatches one stored generation to every engine",
          "[asic][bzm][reactor]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 4);
    mining_template_t template = bzm_template("dispatch", false);

    size_t assigned = 0;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, &assigned));
    TEST_ASSERT_EQUAL_UINT32(4, assigned);
    TEST_ASSERT_EQUAL_UINT32(4, transport->write_count);
    TEST_ASSERT_EQUAL_UINT32(4, transport->checkpoint_count);
    asic_work_handle_t shared = transport->work[0].source.handle;
    int bottom_count = 0;
    int top_count = 0;
    for (size_t i = 0; i < assigned; ++i) {
        bzm_engine_location_t expected;
        TEST_ASSERT_TRUE(bzm_topology_activation_at(
            i, BZM_ENGINE_STACK_BOTTOM, &expected));
        TEST_ASSERT_EQUAL_UINT32(expected.physical_id,
                                 transport->work[i].engine_id);
        if (expected.stack == BZM_ENGINE_STACK_BOTTOM) {
            ++bottom_count;
        } else {
            ++top_count;
        }
        TEST_ASSERT_LESS_OR_EQUAL_INT(1, abs(bottom_count - top_count));
        TEST_ASSERT_EQUAL_HEX32((uint32_t)shared,
                                (uint32_t)transport->work[i].source.handle);
        TEST_ASSERT_EQUAL_HEX32((uint32_t)(shared >> 32),
                                (uint32_t)(transport->work[i].source.handle >> 32));
        TEST_ASSERT_EQUAL_HEX32(i * 0x40000000U,
                                transport->work[i].starting_nonce);
        TEST_ASSERT_EQUAL_HEX32(((i + 1) * 0x40000000ULL) - 1,
                                transport->work[i].end_nonce);
    }

    mining_template_t snapshot;
    TEST_ASSERT_TRUE(asic_job_store_snapshot(store, shared, &snapshot));
    TEST_ASSERT_EQUAL_STRING("dispatch", snapshot.share.job_id);
    mining_template_free(&snapshot);
    mining_template_free(&template);
    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM full dispatch covers 236 engines in balanced write order",
          "[asic][bzm][reactor][topology][balance]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(
        store, transport, BZM_ENGINES_PER_ASIC);
    mining_template_t template = bzm_template("full-topology", false);
    bool seen[BZM_ENGINE_GRID_COUNT] = {false};

    size_t assigned = 0;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, &assigned));
    TEST_ASSERT_EQUAL_UINT16(BZM_ENGINES_PER_ASIC, assigned);
    TEST_ASSERT_EQUAL_UINT16(BZM_ENGINES_PER_ASIC,
                             transport->write_count);
    TEST_ASSERT_EQUAL_UINT16(BZM_ENGINES_PER_ASIC,
                             transport->checkpoint_count);

    int bottom_count = 0;
    int top_count = 0;
    for (size_t schedule_index = 0; schedule_index < assigned;
         ++schedule_index) {
        bzm_engine_location_t expected;
        TEST_ASSERT_TRUE(bzm_topology_activation_at(
            schedule_index, BZM_ENGINE_STACK_BOTTOM, &expected));
        TEST_ASSERT_EQUAL_UINT16(expected.physical_id,
                                 transport->work[schedule_index].engine_id);
        TEST_ASSERT_FALSE(seen[expected.grid_id]);
        seen[expected.grid_id] = true;

        if (expected.stack == BZM_ENGINE_STACK_BOTTOM) {
            ++bottom_count;
        } else {
            ++top_count;
        }
        TEST_ASSERT_LESS_OR_EQUAL_INT(1, abs(bottom_count - top_count));
    }
    TEST_ASSERT_EQUAL_INT(BZM_TOPOLOGY_STACK_ENGINE_COUNT, bottom_count);
    TEST_ASSERT_EQUAL_INT(BZM_TOPOLOGY_STACK_ENGINE_COUNT, top_count);

    // Balanced ordering limits transient skew, but does not claim an atomic
    // pairwise commit. The separate partial-dispatch test verifies fail/flush.
    TEST_ASSERT_FALSE(seen[4 * BZM_ENGINE_ROWS]);
    TEST_ASSERT_FALSE(seen[5 * BZM_ENGINE_ROWS]);
    TEST_ASSERT_FALSE(seen[5 * BZM_ENGINE_ROWS + 19]);
    TEST_ASSERT_FALSE(seen[11 * BZM_ENGINE_ROWS + 19]);

    mining_template_free(&template);
    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM incremental assignments retain compact IDs in balanced order",
          "[asic][bzm][reactor][topology][result]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = calloc(1, sizeof(*reactor));
    TEST_ASSERT_NOT_NULL(reactor);
    bzm_reactor_config_t config = {
        .engine_count = 4,
        .timestamp_count = 16,
        .lead_zeros = 36,
        .nonce_offset = BZM_NONCE_GAP_1002,
        .enhanced_mode = false,
    };
    TEST_ASSERT_TRUE(bzm_reactor_init(reactor, store, &config,
                                      &SIMULATED_OPS, transport));
    mining_template_t template = bzm_template("incremental", false);

    for (uint16_t schedule_index = 0; schedule_index < 4;
         ++schedule_index) {
        bzm_work_t work;
        bzm_engine_location_t expected;
        TEST_ASSERT_TRUE(bzm_topology_activation_at(
            schedule_index, BZM_ENGINE_STACK_BOTTOM, &expected));
        TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                          bzm_reactor_assign(reactor, &template, &work));
        TEST_ASSERT_EQUAL_UINT16(expected.physical_id, work.engine_id);
        TEST_ASSERT_EQUAL_HEX32(0, work.starting_nonce);
        TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, work.end_nonce);
        TEST_ASSERT_EQUAL_UINT8(0, work.logical_sequence);
    }

    // Physical engine 10 is the second scheduled engine but compact engine
    // 10. Result routing must use the stable compact ID, not schedule index.
    bzm_raw_result_t raw = {
        .asic_id = BZM_FIRST_ASIC_ID,
        .engine_id = 10,
        .status = 8,
        .nonce = 0x100,
        .sequence_id = 0,
        .time = 16,
    };
    asic_event_t event;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT16(10, event.data.share.engine_id);

    mining_template_free(&template);
    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM assigns independent templates and retains one prior job per engine",
          "[asic][bzm][reactor][scheduler][result]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 2);

    mining_template_t engine0_first = bzm_template("engine0-first", false);
    mining_template_t engine1_first = bzm_template("engine1-first", false);
    mining_template_t engine0_next = bzm_template("engine0-next", false);
    engine0_first.ntime = 100;
    engine1_first.ntime = 200;
    engine0_next.ntime = 300;

    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_assign(reactor, &engine0_first, NULL));
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_assign(reactor, &engine1_first, NULL));
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_assign(reactor, &engine0_next, NULL));

    TEST_ASSERT_EQUAL_UINT32(3, transport->write_count);
    TEST_ASSERT_EQUAL_UINT32(3, transport->checkpoint_count);
    TEST_ASSERT_NOT_EQUAL(
        (uint32_t)transport->work[0].source.handle,
        (uint32_t)transport->work[1].source.handle);
    TEST_ASSERT_NOT_EQUAL(
        (uint32_t)transport->work[0].source.handle,
        (uint32_t)transport->work[2].source.handle);
    TEST_ASSERT_EQUAL_UINT8(0, transport->work[0].logical_sequence);
    TEST_ASSERT_EQUAL_UINT8(0, transport->work[1].logical_sequence);
    TEST_ASSERT_EQUAL_UINT8(1, transport->work[2].logical_sequence);
    TEST_ASSERT_EQUAL_HEX32(0, transport->work[2].starting_nonce);
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, transport->work[2].end_nonce);

    bzm_raw_result_t raw = {
        .asic_id = BZM_FIRST_ASIC_ID,
        .engine_id = transport->work[0].engine_id,
        .status = 8,
        .sequence_id = 0,
        .time = 16,
    };
    asic_event_t event;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)transport->work[0].source.handle,
        (uint32_t)event.data.share.work_handle);
    TEST_ASSERT_EQUAL_HEX32(100, event.data.share.final_ntime);

    raw.sequence_id = 4;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)transport->work[2].source.handle,
        (uint32_t)event.data.share.work_handle);
    TEST_ASSERT_EQUAL_HEX32(300, event.data.share.final_ntime);

    mining_template_free(&engine0_next);
    mining_template_free(&engine1_first);
    mining_template_free(&engine0_first);
    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM rejects delayed assignments after their bounded store slot is reused",
          "[asic][bzm][reactor][scheduler][result]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(
        store, transport, BZM_ENGINES_PER_ASIC);
    mining_template_t template = bzm_template("bounded-history", false);

    for (size_t index = 0; index < BZM_ENGINES_PER_ASIC; ++index) {
        TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                          bzm_reactor_assign(reactor, &template, NULL));
    }
    const bzm_work_t first = transport->work[0];

    /* Twenty spare slots retain five seconds of history at the production
     * 250 ms assignment cadence. The next assignment safely retires the
     * oldest handle instead of emitting an event whose template is gone. */
    for (size_t index = 0;
         index <= ASIC_JOB_STORE_CAPACITY - BZM_ENGINES_PER_ASIC;
         ++index) {
        TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                          bzm_reactor_assign(reactor, &template, NULL));
    }

    bzm_raw_result_t stale = {
        .asic_id = BZM_FIRST_ASIC_ID,
        .engine_id = first.engine_id,
        .status = 8,
        .nonce = 0x1234,
        .sequence_id = 0,
        .time = 16,
    };
    asic_event_t event;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &stale, &event));
    TEST_ASSERT_FALSE(asic_job_store_contains(store, first.source.handle));

    mining_template_free(&template);
    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM reactor resolves microstate version and timestamp rolling",
          "[asic][bzm][reactor][result]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 21);
    mining_template_t template = bzm_template("result", false);
    template.version_mask = 0x1fffe000;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));

    bzm_raw_result_t raw = {
        .asic_id = 0x1e,
        .engine_id = 64,
        .status = 8,
        .nonce = 0x12345678,
        .sequence_id = 2,
        .time = 13,
        .timestamp_us = 555,
    };
    asic_event_t event;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT8(2, event.data.share.asic_index);
    TEST_ASSERT_EQUAL(ASIC_EVENT_SHARE_RESULT, event.type);
    TEST_ASSERT_EQUAL_HEX32(0x2c563412, event.data.share.nonce);
    TEST_ASSERT_EQUAL_HEX32(0x3fff0004,
                            event.data.share.final_version);
    TEST_ASSERT_EQUAL_HEX32(0x1fff0000,
                            event.data.share.version_bits);
    TEST_ASSERT_EQUAL_HEX32(template.ntime + 3,
                            event.data.share.final_ntime);
    raw.asic_id = 0x1f;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT16(20, event.data.share.engine_id);
    TEST_ASSERT_EQUAL_UINT8(2, event.data.share.micro_job_id);
    TEST_ASSERT_EQUAL_UINT8(0, event.data.share.sequence_id);
    TEST_ASSERT_EQUAL_UINT8(2, event.data.share.asic_index);

    raw.asic_id = 0x1e;
    raw.nonce++;
    raw.sequence_id = 3;
    raw.time = 12;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_HEX32(0x3fffe004,
                            event.data.share.final_version);
    TEST_ASSERT_EQUAL_HEX32(template.ntime + 4,
                            event.data.share.final_ntime);

    raw.engine_id = 0;
    raw.sequence_id = 7;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &raw, &event));
    raw.sequence_id = 0;
    raw.status = 0;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &raw, &event));
    raw.status = 8;
    raw.time = 17;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &raw, &event));

    mining_template_free(&template);
    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM reactor retains both enhanced sequence generations",
          "[asic][bzm][reactor][result][pipeline]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 2);
    mining_template_t first = bzm_template("first", false);
    mining_template_t second = bzm_template("second", false);
    first.ntime = 100;
    second.ntime = 200;

    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &first, NULL));
    asic_work_handle_t first_handle = transport->work[0].source.handle;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &second, NULL));
    asic_work_handle_t second_handle = transport->work[2].source.handle;
    TEST_ASSERT_NOT_EQUAL((uint32_t) first_handle,
                          (uint32_t) second_handle);

    bzm_raw_result_t raw = {
        .asic_id = BZM_FIRST_ASIC_ID,
        .engine_id = transport->work[0].engine_id,
        .status = 8,
        .sequence_id = 0,
        .time = 16,
    };
    asic_event_t event;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT32((uint32_t) first_handle,
                             (uint32_t) event.data.share.work_handle);
    TEST_ASSERT_EQUAL_HEX32(100, event.data.share.final_ntime);

    raw.sequence_id = 4;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_UINT32((uint32_t) second_handle,
                             (uint32_t) event.data.share.work_handle);
    TEST_ASSERT_EQUAL_HEX32(200, event.data.share.final_ntime);

    TEST_ASSERT_EQUAL(BZM_ASSIGN_FLUSH_REQUIRED,
                      bzm_reactor_dispatch(reactor, &second, NULL));
    TEST_ASSERT_TRUE(bzm_reactor_is_flush_pending(reactor));

    mining_template_free(&second);
    mining_template_free(&first);
    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM 1002 nonce gap reproduces captured Stage 7 hardware proof",
          "[asic][bzm][reactor][result][hardware-vector]")
{
    mining_template_t template = {
        .version = 0x20000000,
        .version_mask = 0x1fffe000,
        .ntime = 0x6a5dcd19,
        .target = 0x1702369d,
    };
    TEST_ASSERT_EQUAL_UINT32(32, hex2bin(
        "0000000000000000208701005e2b873f7a0a2c50613cf7d5ae390721a3617ee3",
        template.prev_block_hash, sizeof(template.prev_block_hash)));
    TEST_ASSERT_EQUAL_UINT32(32, hex2bin(
        "6c96a113f1dba64f8d6f1b9767e5770801955e00f112cfeb26a07d3224f9e611",
        template.merkle_root, sizeof(template.merkle_root)));

    const uint32_t raw_nonce = 0x2d6dac84;
    uint32_t mapped_nonce = __builtin_bswap32(
        raw_nonce - BZM_NONCE_GAP_1002);
    double captured_difficulty = mining_test_nonce_value(
        &template, mapped_nonce, template.ntime + 4, 0x3fff0000);
    TEST_ASSERT_GREATER_THAN_DOUBLE(1.0, captured_difficulty);

    uint32_t former_assumption = __builtin_bswap32(raw_nonce - 0x28U);
    double former_difficulty = mining_test_nonce_value(
        &template, former_assumption, template.ntime + 4, 0x3fff0000);
    TEST_ASSERT_LESS_THAN_DOUBLE(1.0, former_difficulty);
}

TEST_CASE("BZM distinguishes nonce results from non-share status frames",
          "[asic][bzm][result]")
{
    bzm_raw_result_t result = {.status = 0x07};
    TEST_ASSERT_FALSE(bzm_raw_result_has_valid_nonce(NULL));
    TEST_ASSERT_FALSE(bzm_raw_result_has_valid_nonce(&result));
    result.status = 0x08;
    TEST_ASSERT_TRUE(bzm_raw_result_has_valid_nonce(&result));
    result.status = 0x0f;
    TEST_ASSERT_TRUE(bzm_raw_result_has_valid_nonce(&result));
}

TEST_CASE("BZM failed flush remains a barrier until transport recovers",
          "[asic][bzm][reactor][flush-error]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 1);
    mining_template_t template = bzm_template("flush-error", false);
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));

    transport->fail_flush = true;
    reactor->next_sequence = 2;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_TRANSPORT_ERROR,
                      bzm_reactor_dispatch(reactor, &template, NULL));
    TEST_ASSERT_TRUE(bzm_reactor_is_flush_pending(reactor));
    TEST_ASSERT_EQUAL_UINT32(1, transport->flush_count);

    bzm_reactor_finish_flush(reactor);
    TEST_ASSERT_TRUE(bzm_reactor_is_flush_pending(reactor));
    TEST_ASSERT_EQUAL(BZM_ASSIGN_FLUSH_REQUIRED,
                      bzm_reactor_dispatch(reactor, &template, NULL));

    transport->fail_flush = false;
    TEST_ASSERT_TRUE(bzm_reactor_begin_flush(reactor));
    TEST_ASSERT_EQUAL_UINT32(2, transport->flush_count);
    bzm_reactor_finish_flush(reactor);
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(reactor));
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));

    mining_template_free(&template);
    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM clean-job barrier rejects stale results and invalidates handles",
          "[asic][bzm][reactor][clean-job]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 1);
    mining_template_t template = bzm_template("old", false);
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));
    asic_work_handle_t old_handle = transport->work[0].source.handle;

    bzm_raw_result_t old_result = {
        .engine_id = 0,
        .status = 8,
        .sequence_id = 0,
        .time = 16,
    };
    asic_event_t event;
    TEST_ASSERT_TRUE(bzm_reactor_clear_work(reactor));
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(reactor));
    TEST_ASSERT_EQUAL_UINT32(1, transport->flush_count);
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &old_result, &event));
    mining_template_t snapshot;
    TEST_ASSERT_FALSE(asic_job_store_snapshot(store, old_handle, &snapshot));

    mining_template_free(&template);
    template = bzm_template("new", false);
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));
    asic_work_handle_t new_handle =
        transport->work[transport->write_count - 1].source.handle;
    TEST_ASSERT_NOT_EQUAL((uint32_t)old_handle, (uint32_t)new_handle);
    TEST_ASSERT_FALSE(asic_job_store_snapshot(store, old_handle, &snapshot));

    mining_template_free(&template);
    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM idle clean-job barrier does not disturb the hardware link",
          "[asic][bzm][reactor][clean-job][idle]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 1);
    mining_template_t template = bzm_template("queued-only", false);
    asic_work_handle_t handle = ASIC_WORK_HANDLE_INVALID;
    TEST_ASSERT_TRUE(asic_job_store_store_generated(store, &template,
                                                     &handle));

    TEST_ASSERT_TRUE(bzm_reactor_clear_work(reactor));
    TEST_ASSERT_EQUAL_UINT32(0, transport->flush_count);
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(reactor));
    mining_template_t snapshot;
    TEST_ASSERT_FALSE(asic_job_store_snapshot(store, handle, &snapshot));

    mining_template_free(&template);
    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM sequence wrap forces a flush before identity reuse",
          "[asic][bzm][reactor][wrap]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 1);

    for (size_t i = 0; i < 2; ++i) {
        mining_template_t template = bzm_template("wrap", false);
        TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                          bzm_reactor_dispatch(reactor, &template, NULL));
        mining_template_free(&template);
    }
    mining_template_t template = bzm_template("wrapped", false);
    TEST_ASSERT_EQUAL(BZM_ASSIGN_FLUSH_REQUIRED,
                      bzm_reactor_dispatch(reactor, &template, NULL));
    TEST_ASSERT_TRUE(bzm_reactor_is_flush_pending(reactor));
    TEST_ASSERT_EQUAL_UINT32(1, transport->flush_count);

    bzm_raw_result_t stale = {
        .engine_id = 0,
        .status = 8,
        .sequence_id = 0,
        .time = 16,
    };
    asic_event_t event;
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &stale, &event));
    bzm_reactor_finish_flush(reactor);
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));
    TEST_ASSERT_EQUAL_UINT8(0,
        transport->work[transport->write_count - 1].logical_sequence);

    mining_template_free(&template);
    free(transport);
    free(reactor);
    delete_store(store);
}

TEST_CASE("BZM partial dispatch is flushed and never publishes a handle",
          "[asic][bzm][reactor][error]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    transport->fail_after = 1;
    bzm_reactor_t *reactor = new_reactor(store, transport, 2);
    mining_template_t template = bzm_template("partial", false);
    size_t assigned;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_TRANSPORT_ERROR,
                      bzm_reactor_dispatch(reactor, &template, &assigned));
    TEST_ASSERT_EQUAL_UINT32(0, assigned);
    TEST_ASSERT_EQUAL_UINT32(1, transport->flush_count);
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(reactor));
    mining_template_t snapshot;
    TEST_ASSERT_FALSE(asic_job_store_snapshot(
        store, transport->work[0].source.handle, &snapshot));
    free(transport);
    free(reactor);

    simulated_transport_t *first_write_failure = new_transport();
    first_write_failure->fail_immediately = true;
    bzm_reactor_t *first_write_reactor = new_reactor(
        store, first_write_failure, 2);
    assigned = 99;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_TRANSPORT_ERROR,
                      bzm_reactor_dispatch(first_write_reactor, &template,
                                           &assigned));
    TEST_ASSERT_EQUAL_UINT32(0, assigned);
    TEST_ASSERT_EQUAL_UINT32(1, first_write_failure->flush_count);
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(first_write_reactor));
    free(first_write_failure);
    free(first_write_reactor);

    simulated_transport_t *checkpoint_failure = new_transport();
    checkpoint_failure->fail_checkpoint = true;
    bzm_reactor_t *checkpoint_reactor = new_reactor(
        store, checkpoint_failure, 2);
    assigned = 99;
    TEST_ASSERT_EQUAL(BZM_ASSIGN_TRANSPORT_ERROR,
                      bzm_reactor_dispatch(checkpoint_reactor, &template,
                                           &assigned));
    TEST_ASSERT_EQUAL_UINT32(0, assigned);
    TEST_ASSERT_EQUAL_UINT32(1, checkpoint_failure->write_count);
    TEST_ASSERT_EQUAL_UINT32(1, checkpoint_failure->checkpoint_count);
    TEST_ASSERT_EQUAL_UINT32(1, checkpoint_failure->flush_count);
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(checkpoint_reactor));
    free(checkpoint_failure);
    free(checkpoint_reactor);

    simulated_transport_t *transport2 = new_transport();
    bzm_reactor_t *invalid = calloc(1, sizeof(*invalid));
    TEST_ASSERT_NOT_NULL(invalid);
    bzm_reactor_config_t bad = {
        .engine_count = BZM_ENGINES_PER_ASIC + 1,
        .timestamp_count = 16,
        .enhanced_mode = true,
    };
    TEST_ASSERT_FALSE(bzm_reactor_init(invalid, store, &bad,
                                       &SIMULATED_OPS, transport2));

    mining_template_free(&template);
    free(transport2);
    free(invalid);
    delete_store(store);
}

TEST_CASE("BZM advertises rolling features without exposing chip identity",
          "[asic][bzm][capabilities]")
{
    asic_capabilities_t capabilities =
        ASIC_capabilities_for_chip_id(BZM_CHIP_ID);
    TEST_ASSERT_EQUAL(ASIC_VERSION_ROLLING_MIDSTATE,
                      capabilities.version_rolling);
    TEST_ASSERT_EQUAL_HEX32(0x1fffe000,
                            capabilities.supported_version_mask);
    TEST_ASSERT_EQUAL_UINT32(4, capabilities.max_version_variants);
    TEST_ASSERT_TRUE(capabilities.supports_ntime_rolling);
    TEST_ASSERT_EQUAL_UINT32(127, capabilities.max_ntime_roll);
    TEST_ASSERT_EQUAL(ASIC_WORK_REFRESH_DRIVER_MANAGED,
                      capabilities.work_refresh_policy);
    TEST_ASSERT_TRUE(capabilities.driver_owns_scheduling);
    TEST_ASSERT_FALSE(ASIC_capabilities_support_static_work(&capabilities));
}
