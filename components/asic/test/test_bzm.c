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
#include "unity.h"

typedef struct {
    size_t write_count;
    size_t flush_count;
    size_t fail_after;
    bool fail_flush;
    bzm_work_t work[BZM_MAX_ACTIVE_WORK];
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
    uint8_t programmed_ids[BZM_MAX_ASIC_COUNT];
    uint8_t chain_enabled[BZM_MAX_ASIC_COUNT];
} simulated_chain_t;

static bool simulated_noop(void *context, uint8_t asic_id)
{
    simulated_chain_t *chain = context;
    if (asic_id == BZM_BROADCAST_ASIC) {
        return chain->programmed < chain->available;
    }
    return asic_id >= BZM_FIRST_ASIC_ID &&
           asic_id < BZM_FIRST_ASIC_ID + chain->programmed;
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
    size_t index = asic_id - BZM_FIRST_ASIC_ID;
    if (engine_id != BZM_CONTROL_ENGINE_ID || offset != 0x0b ||
        data_len != 4 || index >= chain->programmed) {
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
    if (transport->fail_after != 0 &&
        transport->write_count >= transport->fail_after) {
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

static const bzm_transport_ops_t SIMULATED_OPS = {
    .write_work = simulated_write,
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
    TEST_ASSERT_EQUAL_UINT16(240, board->family.asic.core_count);
    TEST_ASSERT_EQUAL_UINT16(50,
                             board->family.asic.default_frequency_mhz);
    TEST_ASSERT_EQUAL_UINT16(50,
                             board->family.asic.frequency_options[0]);
    TEST_ASSERT_EQUAL_UINT16(0,
                             board->family.asic.frequency_options[1]);
    TEST_ASSERT_FALSE(board->family.asic.frequency_tunable);
    TEST_ASSERT_TRUE(device_config_accepts_frequency(board, 50.0f));
    TEST_ASSERT_FALSE(device_config_accepts_frequency(board, 49.0f));
    TEST_ASSERT_FALSE(device_config_accepts_frequency(board, 51.0f));
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
        .nonce_offset = BZM_NONCE_GAP_ENHANCED,
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
    TEST_ASSERT_EQUAL_HEX32(0x20002004, work.versions[1]);
    TEST_ASSERT_EQUAL_HEX32(0x20004004, work.versions[2]);
    TEST_ASSERT_EQUAL_HEX32(0x20006004, work.versions[3]);
    TEST_ASSERT_EQUAL_HEX32(template.ntime, work.start_ntime);
    TEST_ASSERT_EQUAL_HEX32(template.target, work.target);
    TEST_ASSERT_EQUAL_HEX32(template.starting_nonce, work.starting_nonce);
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, work.end_nonce);

    bm_job bm;
    TEST_ASSERT_TRUE(bm_job_build(&template, &bm));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(bm.midstate, work.midstates[0], 32);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(bm.midstate1, work.midstates[1], 32);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(bm.midstate2, work.midstates[2], 32);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(bm.midstate3, work.midstates[3], 32);
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
        0x44, 0x01,
        0x83, 0x45, 0x78, 0x56, 0x34, 0x12, 0x17, 0x0d,
    };
    bzm_raw_result_t result;
    TEST_ASSERT_TRUE(bzm_tdm_result_decode(frame, 999, &result));
    TEST_ASSERT_EQUAL_HEX8(0x44, result.asic_id);
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

TEST_CASE("BZM maps contiguous logical engines onto the 20 by 12 grid",
          "[asic][bzm][engine-map]")
{
    uint16_t physical;
    uint16_t logical;
    TEST_ASSERT_TRUE(bzm_engine_physical_id(0, &physical));
    TEST_ASSERT_EQUAL_UINT16(0, physical);
    TEST_ASSERT_TRUE(bzm_engine_physical_id(19, &physical));
    TEST_ASSERT_EQUAL_UINT16(19, physical);
    TEST_ASSERT_TRUE(bzm_engine_physical_id(20, &physical));
    TEST_ASSERT_EQUAL_UINT16(64, physical);
    TEST_ASSERT_TRUE(bzm_engine_physical_id(239, &physical));
    TEST_ASSERT_EQUAL_UINT16(723, physical);
    TEST_ASSERT_TRUE(bzm_engine_logical_id(723, &logical));
    TEST_ASSERT_EQUAL_UINT16(239, logical);
    TEST_ASSERT_FALSE(bzm_engine_physical_id(240, &physical));
    TEST_ASSERT_FALSE(bzm_engine_logical_id(20, &logical));
}

TEST_CASE("BZM transport encoder emits byte-paired 9-bit write words",
          "[asic][bzm][transport]")
{
    TEST_ASSERT_EQUAL_HEX8(0xfa, BZM_BROADCAST_ASIC);
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
            BZM_MAX_ASIC_COUNT, BZM_FIRST_ASIC_ID, ids, sizeof(ids),
            &SIMULATED_CHAIN_OPS, &chain));
        TEST_ASSERT_EQUAL_UINT32(available, chain.read_count);
        for (size_t i = 0; i < available; ++i) {
            TEST_ASSERT_EQUAL_UINT8(BZM_FIRST_ASIC_ID + i, ids[i]);
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
    asic_work_handle_t shared = transport->work[0].source.handle;
    for (size_t i = 0; i < assigned; ++i) {
        TEST_ASSERT_EQUAL_UINT32(i, transport->work[i].engine_id);
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

TEST_CASE("BZM reactor resolves microstate version and timestamp rolling",
          "[asic][bzm][reactor][result]")
{
    asic_job_store_t *store = new_store();
    simulated_transport_t *transport = new_transport();
    bzm_reactor_t *reactor = new_reactor(store, transport, 21);
    mining_template_t template = bzm_template("result", false);
    TEST_ASSERT_EQUAL(BZM_ASSIGN_OK,
                      bzm_reactor_dispatch(reactor, &template, NULL));

    bzm_raw_result_t raw = {
        .asic_id = 0x44,
        .engine_id = 64,
        .status = 8,
        .nonce = 0x12345678,
        .sequence_id = 2,
        .time = 13,
        .timestamp_us = 555,
    };
    asic_event_t event;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL(ASIC_EVENT_SHARE_RESULT, event.type);
    TEST_ASSERT_EQUAL_HEX32(0x12345650, event.data.share.nonce);
    TEST_ASSERT_EQUAL_HEX32(0x20004004,
                            event.data.share.final_version);
    TEST_ASSERT_EQUAL_HEX32(0x00004000,
                            event.data.share.version_bits);
    TEST_ASSERT_EQUAL_HEX32(template.ntime + 3,
                            event.data.share.final_ntime);
    TEST_ASSERT_EQUAL_UINT16(20, event.data.share.engine_id);
    TEST_ASSERT_EQUAL_UINT8(2, event.data.share.micro_job_id);
    TEST_ASSERT_EQUAL_UINT8(0, event.data.share.sequence_id);
    TEST_ASSERT_EQUAL_UINT8(2, event.data.share.asic_index);

    raw.nonce++;
    raw.sequence_id = 3;
    raw.time = 12;
    TEST_ASSERT_TRUE(bzm_reactor_map_result(reactor, &raw, &event));
    TEST_ASSERT_EQUAL_HEX32(0x20006004,
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

TEST_CASE("BZM flush barrier rejects stale results and invalidates handles",
          "[asic][bzm][reactor][stale]")
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
    TEST_ASSERT_TRUE(bzm_reactor_begin_flush(reactor));
    TEST_ASSERT_TRUE(bzm_reactor_is_flush_pending(reactor));
    TEST_ASSERT_EQUAL_UINT32(1, transport->flush_count);
    TEST_ASSERT_FALSE(bzm_reactor_map_result(reactor, &old_result, &event));
    mining_template_t snapshot;
    TEST_ASSERT_FALSE(asic_job_store_snapshot(store, old_handle, &snapshot));

    bzm_reactor_finish_flush(reactor);
    TEST_ASSERT_FALSE(bzm_reactor_is_flush_pending(reactor));
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

    simulated_transport_t *transport2 = new_transport();
    bzm_reactor_t *invalid = calloc(1, sizeof(*invalid));
    TEST_ASSERT_NOT_NULL(invalid);
    bzm_reactor_config_t bad = {
        .engine_count = BZM_MAX_ACTIVE_WORK + 1,
        .timestamp_count = 16,
        .enhanced_mode = true,
    };
    TEST_ASSERT_FALSE(bzm_reactor_init(invalid, store, &bad,
                                       &SIMULATED_OPS, transport2));

    mining_template_free(&template);
    free(transport2);
    free(transport);
    free(invalid);
    free(reactor);
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
