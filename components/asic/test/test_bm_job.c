#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asic_driver.h"
#include "asic_job_store.h"
#include "asic_result_handler.h"
#include "bm_job_builder.h"
#include "bm_result.h"
#include "device_config.h"
#include "mining_template.h"
#include "sv2_mining_template.h"
#include "unity.h"
#include "utils.h"

static void fill_sequence(uint8_t *destination, size_t length, uint8_t first)
{
    for (size_t i = 0; i < length; ++i) destination[i] = first + i;
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

static void assert_hex(const char *expected, const uint8_t *actual,
                       size_t length)
{
    uint8_t bytes[32];
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(bytes), length);
    TEST_ASSERT_EQUAL(length, hex2bin(expected, bytes, length));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(bytes, actual, length);
}

static mining_notify sv1_fixture(uint8_t branches[32])
{
    fill_sequence(branches, 32, 0);
    return (mining_notify) {
        .job_id = "sv1-job",
        .prev_block_hash =
            "000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f",
        .coinbase_1 = "010203",
        .coinbase_2 = "ccdd",
        .merkle_branches = branches,
        .n_merkle_branches = 1,
        .version = 0x20000004,
        .target = 0x1705dd01,
        .ntime = 0x64658bd8,
        .clean_jobs = true,
    };
}

static sv2_job_t standard_fixture(void)
{
    sv2_job_t source = {
        .job_id = 4294967295U,
        .version = 0x20000004,
        .ntime = 0x65010203,
        .nbits = 0x1705dd01,
        .clean_jobs = true,
    };
    fill_sequence(source.prev_hash, 32, 0);
    fill_sequence(source.merkle_root, 32, 0x20);
    return source;
}

static void extended_fixture(sv2_ext_job_t *source, sv2_conn_t *connection)
{
    static uint8_t prefix[] = {0x01, 0x02};
    static uint8_t suffix[] = {0xfe, 0xff};
    memset(source, 0, sizeof(*source));
    memset(connection, 0, sizeof(*connection));
    source->job_id = 123456789;
    source->version = 0x20000004;
    source->ntime = 0x65010203;
    source->nbits = 0x1705dd01;
    source->clean_jobs = true;
    source->coinbase_prefix = prefix;
    source->coinbase_prefix_len = sizeof(prefix);
    source->coinbase_suffix = suffix;
    source->coinbase_suffix_len = sizeof(suffix);
    fill_sequence(source->prev_hash, 32, 0);
    source->merkle_path_count = 2;
    fill_sequence(source->merkle_path[0], 32, 0x40);
    fill_sequence(source->merkle_path[1], 32, 0x60);
    connection->extranonce_prefix_len = 3;
    connection->extranonce_prefix[0] = 0xaa;
    connection->extranonce_prefix[1] = 0xbb;
    connection->extranonce_prefix[2] = 0xcc;
    connection->extranonce_size = 4;
}

TEST_CASE("SV1 adapter owns neutral metadata before Bitmain packet building",
          "[asic][template][sv1]")
{
    uint8_t branches[32];
    mining_notify source = sv1_fixture(branches);
    mining_template_t template;
    bm_job job;

    TEST_ASSERT_TRUE(mining_template_build_sv1(
        &source, "aabb", 4, 0x11223344, 0x00006000, 1234.5,
        &template));
    TEST_ASSERT_EQUAL(MINING_PROTOCOL_SV1, template.share.protocol);
    TEST_ASSERT_EQUAL_STRING("sv1-job", template.share.job_id);
    TEST_ASSERT_EQUAL_STRING("44332211", template.share.extranonce2);
    TEST_ASSERT_EQUAL_DOUBLE(1234.5, template.share.pool_difficulty);
    TEST_ASSERT_TRUE(bm_job_build(&template, &job));
    TEST_ASSERT_EQUAL_UINT8(4, job.num_midstates);
    TEST_ASSERT_EQUAL_HEX32(template.version, job.version);
    TEST_ASSERT_EQUAL_HEX32(template.ntime, job.ntime);
    TEST_ASSERT_EQUAL_HEX32(template.target, job.target);
    assert_hex("1f1e1d1c1b1a19181716151413121110"
               "0f0e0d0c0b0a09080706050403020100",
               job.prev_block_hash, 32);
    assert_hex("bfcf764af84aaaf42a6574d6301ac259"
               "daa4e94d2c1569a4c9ed38b1a9c8d31c",
               job.merkle_root, 32);
    mining_template_free(&template);
}

TEST_CASE("Bitmain builder preserves literal version-rolled midstates",
          "[asic][bitmain][not-on-qemu]")
{
    sv2_job_t source = standard_fixture();
    mining_template_t template;
    bm_job job;
    TEST_ASSERT_TRUE(mining_template_build_sv2_standard(
        &source, 0x00006000, 99.25, &template));
    TEST_ASSERT_TRUE(bm_job_build(&template, &job));
    assert_hex("5575b33221dca8b17f0dbad2bae7a27a"
               "db80bad10d806a114f427a126ee1317f",
               job.midstate, 32);
    assert_hex("75283d79539e6198c2c0241123911835"
               "d5b0998c206b045957390b2e17826f14",
               job.midstate1, 32);
    assert_hex("984317c5d90375d7f37d1d1045e18587"
               "17ea220962451f40f8f30921e447accc",
               job.midstate2, 32);
    assert_hex("9d94523f7a43b1cd9149fd80d68de132"
               "5c0c6da6026c767102244a53211a7d99",
               job.midstate3, 32);
    mining_template_free(&template);
}

TEST_CASE("SV2 adapters preserve standard and extended submission metadata",
          "[asic][template][sv2]")
{
    sv2_job_t standard = standard_fixture();
    mining_template_t template;
    TEST_ASSERT_TRUE(mining_template_build_sv2_standard(
        &standard, 0, 99.25, &template));
    TEST_ASSERT_EQUAL(MINING_PROTOCOL_SV2_STANDARD,
                      template.share.protocol);
    TEST_ASSERT_EQUAL_STRING("4294967295", template.share.job_id);
    TEST_ASSERT_EQUAL_STRING("", template.share.extranonce2);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX,
                             template.share.numeric_job_id);
    mining_template_free(&template);

    sv2_ext_job_t extended;
    sv2_conn_t connection;
    extended_fixture(&extended, &connection);
    TEST_ASSERT_TRUE(mining_template_build_sv2_extended(
        &extended, &connection, 0x1122334455ULL, 0x00006000,
        456.75, &template));
    TEST_ASSERT_EQUAL(MINING_PROTOCOL_SV2_EXTENDED,
                      template.share.protocol);
    TEST_ASSERT_EQUAL_STRING("123456789", template.share.job_id);
    TEST_ASSERT_EQUAL_STRING("22334455", template.share.extranonce2);
    TEST_ASSERT_EQUAL_UINT8(4, template.share.extranonce2_len);
    assert_hex("22334455", template.share.extranonce2_bin, 4);
    assert_hex("09185a699f02d14d71232eafae6f0d45"
               "e5f48afcd2c0560724c26608f07f507f",
               template.merkle_root, 32);
    mining_template_free(&template);

    extended.merkle_path_count = SV2_MAX_MERKLE_BRANCHES + 1;
    TEST_ASSERT_FALSE(mining_template_build_sv2_extended(
        &extended, &connection, 0, 0, 1, &template));
}

static mining_template_t owned_template(mining_protocol_t protocol,
                                        const char *job_id,
                                        const char *extranonce2,
                                        double difficulty)
{
    mining_template_t template = {
        .version = 0x20000004,
        .version_mask = 0x00006000,
        .ntime = 0x65010203,
        .target = 0x1705dd01,
        .share = {
            .protocol = protocol,
            .numeric_job_id = 42,
            .extranonce2_len = 4,
            .pool_difficulty = difficulty,
        },
    };
    fill_sequence(template.prev_block_hash, 32, 0);
    fill_sequence(template.merkle_root, 32, 0x20);
    memcpy(template.share.extranonce2_bin,
           (uint8_t[]){0x11, 0x22, 0x33, 0x44}, 4);
    template.share.job_id = strdup(job_id);
    template.share.extranonce2 = strdup(extranonce2);
    TEST_ASSERT_NOT_NULL(template.share.job_id);
    TEST_ASSERT_NOT_NULL(template.share.extranonce2);
    return template;
}

TEST_CASE("Job store snapshots own metadata and generated handles reject reuse",
          "[asic][store]")
{
    asic_job_store_t *store = new_store();
    mining_template_t original = owned_template(
        MINING_PROTOCOL_SV1, "old-job", "aabb", 1);
    asic_work_handle_t old_handle;
    TEST_ASSERT_TRUE(asic_job_store_store_generated(
        store, &original, &old_handle));
    TEST_ASSERT_TRUE(asic_job_store_contains(store, old_handle));

    mining_template_t snapshot;
    TEST_ASSERT_TRUE(asic_job_store_snapshot(store, old_handle, &snapshot));
    TEST_ASSERT_NOT_EQUAL(original.share.job_id, snapshot.share.job_id);
    mining_template_free(&original);
    TEST_ASSERT_EQUAL_STRING("old-job", snapshot.share.job_id);
    mining_template_free(&snapshot);

    for (size_t i = 0; i < ASIC_JOB_STORE_CAPACITY; ++i) {
        mining_template_t replacement = owned_template(
            MINING_PROTOCOL_SV1, "new-job", "ccdd", 1);
        TEST_ASSERT_TRUE(asic_job_store_store_generated(
            store, &replacement, NULL));
        mining_template_free(&replacement);
    }
    TEST_ASSERT_FALSE(asic_job_store_snapshot(store, old_handle, &snapshot));
    TEST_ASSERT_FALSE(asic_job_store_contains(store, old_handle));
    asic_job_store_invalidate_all(store);
    TEST_ASSERT_FALSE(asic_job_store_snapshot(store, old_handle, &snapshot));
    delete_store(store);
}

TEST_CASE("Compatibility slots retain hardware slot handles",
          "[asic][store][bitmain]")
{
    asic_job_store_t *store = new_store();
    mining_template_t template = owned_template(
        MINING_PROTOCOL_SV1, "slot", "00", 1);
    asic_work_handle_t handle = ASIC_WORK_HANDLE_INVALID;
    TEST_ASSERT_TRUE(asic_job_store_store_slot(
        store, 12, &template, &handle));
    TEST_ASSERT_EQUAL_UINT32(12, (uint32_t)handle);
    mining_template_t snapshot;
    TEST_ASSERT_TRUE(asic_job_store_snapshot(store, 12, &snapshot));
    TEST_ASSERT_EQUAL_STRING("slot", snapshot.share.job_id);
    mining_template_free(&snapshot);
    mining_template_free(&template);
    delete_store(store);
}

typedef struct {
    int self_test_count;
    int ready_count;
    int sv1_count;
    int standard_count;
    int extended_count;
    int account_count;
    bool transport_ready;
    double self_test_diff;
    asic_share_submission_t share;
    char job_id[32];
    char extranonce2[65];
} callback_capture_t;

static void capture_share(callback_capture_t *capture,
                          const asic_share_submission_t *share)
{
    capture->share = *share;
    snprintf(capture->job_id, sizeof(capture->job_id), "%s",
             share->job_id ? share->job_id : "");
    snprintf(capture->extranonce2, sizeof(capture->extranonce2), "%s",
             share->extranonce2 ? share->extranonce2 : "");
}

static void record_self_test(void *context, double difficulty)
{
    callback_capture_t *capture = context;
    capture->self_test_count++;
    capture->self_test_diff = difficulty;
}

static bool transport_ready(void *context,
                            const asic_share_submission_t *share)
{
    callback_capture_t *capture = context;
    capture->ready_count++;
    capture_share(capture, share);
    return capture->transport_ready;
}

#define DEFINE_SUBMIT(name, field)                                         \
    static int name(void *context, const asic_share_submission_t *share)   \
    {                                                                      \
        callback_capture_t *capture = context;                             \
        capture->field++;                                                  \
        capture_share(capture, share);                                     \
        return 0;                                                          \
    }

DEFINE_SUBMIT(submit_sv1, sv1_count)
DEFINE_SUBMIT(submit_standard, standard_count)
DEFINE_SUBMIT(submit_extended, extended_count)

static void account_share(void *context,
                          const asic_share_submission_t *share)
{
    callback_capture_t *capture = context;
    capture->account_count++;
    capture_share(capture, share);
}

static const asic_result_callbacks_t RESULT_CALLBACKS = {
    .record_self_test = record_self_test,
    .sv1_transport_ready = transport_ready,
    .submit_sv1 = submit_sv1,
    .submit_sv2_standard = submit_standard,
    .submit_sv2_extended = submit_extended,
    .account_share = account_share,
};

static asic_result_t result_for(asic_work_handle_t handle)
{
    return (asic_result_t) {
        .work_handle = handle,
        .nonce = 0x12345678,
        .final_ntime = 0x6501020a,
        .final_version = 0x20006004,
        .version_bits = 0x00006000,
        .timestamp_us = 1000,
    };
}

static void exercise_protocol(mining_protocol_t protocol,
                              callback_capture_t *capture)
{
    asic_job_store_t *store = new_store();
    mining_template_t template = owned_template(
        protocol, "42", "11223344", 0);
    asic_work_handle_t handle;
    TEST_ASSERT_TRUE(asic_job_store_store_generated(
        store, &template, &handle));
    asic_result_context_t context = {
        .job_store = store,
        .username = "worker.name",
        .callback_context = capture,
    };
    capture->transport_ready = true;
    asic_result_t result = result_for(handle);
    TEST_ASSERT_EQUAL(ASIC_RESULT_ACCOUNTED,
                      asic_result_handle(&result, &context,
                                         &RESULT_CALLBACKS));
    TEST_ASSERT_EQUAL_STRING("42", capture->job_id);
    TEST_ASSERT_EQUAL_STRING("11223344", capture->extranonce2);
    TEST_ASSERT_EQUAL_STRING("worker.name", capture->share.username);
    TEST_ASSERT_EQUAL_UINT32(result.final_ntime, capture->share.ntime);
    TEST_ASSERT_EQUAL_UINT32(result.final_version,
                             capture->share.final_version);
    TEST_ASSERT_EQUAL_UINT32(result.version_bits,
                             capture->share.version_bits);
    TEST_ASSERT_EQUAL_UINT32(result.nonce, capture->share.nonce);
    mining_template_free(&template);
    delete_store(store);
}

TEST_CASE("Generic result routing uses stored protocol metadata only",
          "[asic][result][routing]")
{
    callback_capture_t capture = {0};
    exercise_protocol(MINING_PROTOCOL_SV1, &capture);
    TEST_ASSERT_EQUAL(1, capture.sv1_count);
    TEST_ASSERT_EQUAL(1, capture.account_count);

    memset(&capture, 0, sizeof(capture));
    exercise_protocol(MINING_PROTOCOL_SV2_STANDARD, &capture);
    TEST_ASSERT_EQUAL(1, capture.standard_count);
    TEST_ASSERT_EQUAL_UINT32(42, capture.share.numeric_job_id);

    memset(&capture, 0, sizeof(capture));
    exercise_protocol(MINING_PROTOCOL_SV2_EXTENDED, &capture);
    TEST_ASSERT_EQUAL(1, capture.extended_count);
    TEST_ASSERT_EQUAL_UINT8(4, capture.share.extranonce2_len);
    assert_hex("11223344", capture.share.extranonce2_bin, 4);
}

TEST_CASE("Generic result rejects stale work and accounts low difficulty",
          "[asic][result]")
{
    asic_job_store_t *store = new_store();
    callback_capture_t capture = {.transport_ready = true};
    asic_result_context_t context = {
        .job_store = store,
        .callback_context = &capture,
    };
    asic_result_t result = result_for(0x1234);
    TEST_ASSERT_EQUAL(ASIC_RESULT_REJECTED_WORK,
                      asic_result_handle(&result, &context,
                                         &RESULT_CALLBACKS));

    mining_template_t template = owned_template(
        MINING_PROTOCOL_SV1, "job", "00", DBL_MAX);
    TEST_ASSERT_TRUE(asic_job_store_store_generated(
        store, &template, &result.work_handle));
    TEST_ASSERT_EQUAL(ASIC_RESULT_ACCOUNTED,
                      asic_result_handle(&result, &context,
                                         &RESULT_CALLBACKS));
    TEST_ASSERT_EQUAL(0, capture.sv1_count);
    TEST_ASSERT_EQUAL(1, capture.account_count);

    result.version_bits = 1;
    TEST_ASSERT_EQUAL(ASIC_RESULT_REJECTED_WORK,
                      asic_result_handle(&result, &context,
                                         &RESULT_CALLBACKS));
    TEST_ASSERT_EQUAL(1, capture.account_count);
    result.version_bits = 0x00006000;

    context.self_test = true;
    TEST_ASSERT_EQUAL(ASIC_RESULT_RECORDED_SELF_TEST,
                      asic_result_handle(&result, &context,
                                         &RESULT_CALLBACKS));
    TEST_ASSERT_EQUAL(1, capture.self_test_count);
    TEST_ASSERT_GREATER_THAN_DOUBLE(0, capture.self_test_diff);
    mining_template_free(&template);
    delete_store(store);
}

TEST_CASE("Bitmain raw adapter keeps share and register events distinct",
          "[asic][bitmain][result]")
{
    task_result raw = {
        .job_id = 8,
        .nonce = 0x12345678,
        .ntime = 0x65010203,
        .rolled_version = 0x20006004,
        .version_bits = 0x00006000,
        .asic_nr = 3,
        .core_id = 4,
        .small_core_id = 5,
        .timestamp_us = 123456,
    };
    asic_event_t event;
    TEST_ASSERT_TRUE(bm_result_to_event(&raw, &event));
    TEST_ASSERT_EQUAL(ASIC_EVENT_SHARE_RESULT, event.type);
    TEST_ASSERT_EQUAL_UINT32(8, (uint32_t)event.data.share.work_handle);
    TEST_ASSERT_EQUAL_HEX32(raw.rolled_version,
                            event.data.share.final_version);

    raw.register_type = REGISTER_TOTAL_COUNT;
    raw.value = 99;
    TEST_ASSERT_TRUE(bm_result_to_event(&raw, &event));
    TEST_ASSERT_EQUAL(ASIC_EVENT_REGISTER_RESULT, event.type);
    TEST_ASSERT_EQUAL_UINT32(99, event.data.register_result.value);
}

TEST_CASE("ASIC driver table exposes operations without switch dispatch",
          "[asic][driver]")
{
    TEST_ASSERT_EQUAL_UINT32(5, asic_driver_count());
    const char *names[] = {"BM1397", "BM1366", "BM1368", "BM1370", "BZM"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        const asic_driver_t *driver = asic_driver_at(i);
        TEST_ASSERT_NOT_NULL(driver);
        TEST_ASSERT_EQUAL_STRING(names[i], driver->name);
        TEST_ASSERT_EQUAL(driver, asic_driver_for_id(driver->id));
        TEST_ASSERT_NOT_NULL(driver->ops.init);
        TEST_ASSERT_NOT_NULL(driver->ops.process_work);
        TEST_ASSERT_NOT_NULL(driver->ops.send_work);
        if (driver->id == BZM) {
            TEST_ASSERT_NOT_NULL(driver->ops.clear_work);
            TEST_ASSERT_NOT_NULL(driver->ops.job_frequency_ms);
            TEST_ASSERT_NOT_NULL(driver->ops.hashrate_counter_snapshot);
            TEST_ASSERT_NOT_NULL(driver->ops.record_local_result);
            TEST_ASSERT_NOT_NULL(driver->ops.health_snapshot);
        } else {
            TEST_ASSERT_NULL(driver->ops.clear_work);
            TEST_ASSERT_NULL(driver->ops.job_frequency_ms);
            TEST_ASSERT_NULL(driver->ops.hashrate_counter_snapshot);
            TEST_ASSERT_NULL(driver->ops.record_local_result);
            TEST_ASSERT_NULL(driver->ops.health_snapshot);
        }
    }
    TEST_ASSERT_NULL(asic_driver_for_id(99));
    TEST_ASSERT_NULL(asic_driver_at(asic_driver_count()));
}
