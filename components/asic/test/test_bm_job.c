#include <float.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>

#include "bm_job_builder.h"
#include "bm_job_slot.h"
#include "bm_result.h"
#include "esp_heap_caps.h"
#include "unity.h"
#include "utils.h"

static void assert_hex(const char *expected, const uint8_t *actual, size_t length)
{
    uint8_t bytes[32];
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(bytes), length);
    TEST_ASSERT_EQUAL(length, hex2bin(expected, bytes, length));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(bytes, actual, length);
}

static void fill_sequence(uint8_t *dest, size_t length, uint8_t first)
{
    for (size_t i = 0; i < length; ++i) dest[i] = first + i;
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

TEST_CASE("SV1 Bitmain work conversion preserves every field", "[bitmain][job][sv1]")
{
    uint8_t branches[32];
    mining_notify source = sv1_fixture(branches);
    bm_job *job = calloc(1, sizeof(*job));
    TEST_ASSERT_NOT_NULL(job);
    TEST_ASSERT_TRUE(bm_job_build_sv1(&source, "aabb", 4, 0x11223344,
                                      0x00006000, 1234.5, job));

    TEST_ASSERT_EQUAL_STRING("44332211", job->extranonce2);
    TEST_ASSERT_EQUAL_STRING("sv1-job", job->jobid);
    TEST_ASSERT_EQUAL_UINT32(0x20000004, job->version);
    TEST_ASSERT_EQUAL_UINT32(0x00006000, job->version_mask);
    TEST_ASSERT_EQUAL_UINT32(0x1705dd01, job->target);
    TEST_ASSERT_EQUAL_UINT32(0x64658bd8, job->ntime);
    TEST_ASSERT_EQUAL_UINT32(0, job->starting_nonce);
    TEST_ASSERT_EQUAL_DOUBLE(1234.5, job->pool_diff);
    TEST_ASSERT_EQUAL_UINT8(4, job->num_midstates);
    assert_hex("1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a09080706050403020100",
               job->prev_block_hash, 32);
    assert_hex("bfcf764af84aaaf42a6574d6301ac259daa4e94d2c1569a4c9ed38b1a9c8d31c",
               job->merkle_root, 32);
    uint8_t coinbase_hash[32];
    calculate_coinbase_tx_hash("010203", "ccdd", "aabb", "44332211",
                               coinbase_hash);
    assert_hex("a7e33e0652b1cb48288a8f5f75316b582e1b341cd6a4bb8b77e95f8f346c5486",
               coinbase_hash, 32);
    free_bm_job(job);
}

TEST_CASE("SV1 Bitmain work conversion generates literal midstates",
          "[bitmain][job][sv1][not-on-qemu]")
{
    uint8_t branches[32];
    mining_notify source = sv1_fixture(branches);
    bm_job *job = calloc(1, sizeof(*job));
    TEST_ASSERT_TRUE(bm_job_build_sv1(&source, "aabb", 4, 0x11223344,
                                      0x00006000, 1234.5, job));
    assert_hex("f4ef5bebcb4a9323cf48493c2cef224b15bffddfdf9b898bd1d2e45a8243d194",
               job->midstate, 32);
    assert_hex("0bdacf357cf88554cab3a42512747f59def8b3f0e4f93b1abffc382673092267",
               job->midstate1, 32);
    assert_hex("d0711ab6391378eb655b0c5547ff91d9bf464ead39829626432506195fca0187",
               job->midstate2, 32);
    assert_hex("197bc50112343ef5ae637b115f7d2dbf831bbb8dc3689ff633f868465ff8cc1e",
               job->midstate3, 32);
    free_bm_job(job);
}

TEST_CASE("SV1 zero mask creates one midstate and oversized extranonce is rejected",
          "[bitmain][job][sv1]")
{
    uint8_t branches[32];
    mining_notify source = sv1_fixture(branches);
    bm_job *job = calloc(1, sizeof(*job));
    TEST_ASSERT_TRUE(bm_job_build_sv1(&source, "aabb", 4, 0, 0, 1.0, job));
    TEST_ASSERT_EQUAL_UINT8(1, job->num_midstates);
    TEST_ASSERT_EQUAL_STRING("00000000", job->extranonce2);
    free_bm_job(job);

    job = calloc(1, sizeof(*job));
    TEST_ASSERT_FALSE(bm_job_build_sv1(&source, "aabb", 33, 0, 0, 1.0, job));
    TEST_ASSERT_NULL(job->jobid);
    TEST_ASSERT_NULL(job->extranonce2);
    free_bm_job(job);
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

TEST_CASE("SV2 standard conversion preserves byte order fields and rolled midstates",
          "[bitmain][job][sv2]")
{
    sv2_job_t source = standard_fixture();
    bm_job *job = calloc(1, sizeof(*job));
    TEST_ASSERT_TRUE(bm_job_build_sv2_standard(&source, 0x00006000, 99.25, job));
    TEST_ASSERT_EQUAL_STRING("4294967295", job->jobid);
    TEST_ASSERT_EQUAL_STRING("", job->extranonce2);
    TEST_ASSERT_EQUAL_UINT32(source.version, job->version);
    TEST_ASSERT_EQUAL_UINT32(source.ntime, job->ntime);
    TEST_ASSERT_EQUAL_UINT32(source.nbits, job->target);
    TEST_ASSERT_EQUAL_DOUBLE(99.25, job->pool_diff);
    TEST_ASSERT_EQUAL_UINT8(4, job->num_midstates);
    assert_hex("1c1d1e1f18191a1b14151617101112130c0d0e0f08090a0b0405060700010203",
               job->prev_block_hash, 32);
    assert_hex("3c3d3e3f38393a3b34353637303132332c2d2e2f28292a2b2425262720212223",
               job->merkle_root, 32);
    free_bm_job(job);

    job = calloc(1, sizeof(*job));
    TEST_ASSERT_TRUE(bm_job_build_sv2_standard(&source, 0, 1.0, job));
    TEST_ASSERT_EQUAL_UINT8(1, job->num_midstates);
    free_bm_job(job);
}

TEST_CASE("SV2 standard conversion generates literal rolled midstates",
          "[bitmain][job][sv2][not-on-qemu]")
{
    sv2_job_t source = standard_fixture();
    bm_job *job = calloc(1, sizeof(*job));
    TEST_ASSERT_TRUE(bm_job_build_sv2_standard(&source, 0x00006000, 99.25, job));
    assert_hex("5575b33221dca8b17f0dbad2bae7a27adb80bad10d806a114f427a126ee1317f",
               job->midstate, 32);
    assert_hex("75283d79539e6198c2c0241123911835d5b0998c206b045957390b2e17826f14",
               job->midstate1, 32);
    assert_hex("984317c5d90375d7f37d1d1045e1858717ea220962451f40f8f30921e447accc",
               job->midstate2, 32);
    assert_hex("9d94523f7a43b1cd9149fd80d68de1325c0c6da6026c767102244a53211a7d99",
               job->midstate3, 32);
    free_bm_job(job);
}

TEST_CASE("SV2 standard timeout never resends current work", "[bitmain][job][sv2]")
{
    TEST_ASSERT_TRUE(bm_job_should_generate_sv2_standard(true));
    TEST_ASSERT_FALSE(bm_job_should_generate_sv2_standard(false));
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

TEST_CASE("SV2 extended conversion composes extranonce merkle path and midstates",
          "[bitmain][job][sv2][extended]")
{
    sv2_ext_job_t source;
    sv2_conn_t connection;
    extended_fixture(&source, &connection);
    bm_job *job = calloc(1, sizeof(*job));
    TEST_ASSERT_TRUE(bm_job_build_sv2_extended(
        &source, &connection, 0x1122334455ULL, 0x00006000, 456.75, job));
    TEST_ASSERT_EQUAL_STRING("123456789", job->jobid);
    TEST_ASSERT_EQUAL_STRING("22334455", job->extranonce2);
    TEST_ASSERT_EQUAL_UINT32(source.ntime, job->ntime);
    TEST_ASSERT_EQUAL_UINT8(4, job->num_midstates);
    assert_hex("09185a699f02d14d71232eafae6f0d45e5f48afcd2c0560724c26608f07f507f",
               job->merkle_root, 32);
    free_bm_job(job);
}

TEST_CASE("SV2 extended conversion generates literal rolled midstates",
          "[bitmain][job][sv2][extended][not-on-qemu]")
{
    sv2_ext_job_t source;
    sv2_conn_t connection;
    extended_fixture(&source, &connection);
    bm_job *job = calloc(1, sizeof(*job));
    TEST_ASSERT_TRUE(bm_job_build_sv2_extended(
        &source, &connection, 0x1122334455ULL, 0x00006000, 456.75, job));
    assert_hex("8536dd805522d3821bdb8559db8daef8cbd114abd42774b8401f82d4d8e33142",
               job->midstate, 32);
    assert_hex("a8769fe0b2b29a052add9669d03c3736592bf744db1ccc44c50201f39efe4d29",
               job->midstate1, 32);
    assert_hex("c8ba5b6240a68d90f4d3d4befeae63797d33054b4cf8fd636b04d2d6064cebe8",
               job->midstate2, 32);
    assert_hex("859ecc6928934c9e44d4fa7b3de2342769c0adc8a9061c7c1d3afd16dd1d863f",
               job->midstate3, 32);
    free_bm_job(job);
}

TEST_CASE("SV2 extended handles path bounds counter width and missing metadata",
          "[bitmain][job][sv2][extended]")
{
    sv2_ext_job_t source;
    sv2_conn_t connection;
    extended_fixture(&source, &connection);

    source.merkle_path_count = 0;
    bm_job *job = calloc(1, sizeof(*job));
    TEST_ASSERT_TRUE(bm_job_build_sv2_extended(
        &source, &connection, 0x1122334455ULL, 0, 1.0, job));
    TEST_ASSERT_EQUAL_STRING("22334455", job->extranonce2);
    free_bm_job(job);

    source.merkle_path_count = SV2_MAX_MERKLE_BRANCHES;
    for (int i = 0; i < SV2_MAX_MERKLE_BRANCHES; ++i) {
        fill_sequence(source.merkle_path[i], 32, i);
    }
    job = calloc(1, sizeof(*job));
    TEST_ASSERT_TRUE(bm_job_build_sv2_extended(
        &source, &connection, 1, 0, 1.0, job));
    free_bm_job(job);

    job = calloc(1, sizeof(*job));
    TEST_ASSERT_FALSE(bm_job_build_sv2_extended(
        &source, NULL, 1, 0, 1.0, job));
    free_bm_job(job);
}

typedef struct {
    int register_count;
    int self_test_count;
    int ready_count;
    int sv1_count;
    int standard_count;
    int extended_count;
    int account_count;
    bool transport_ready;
    double self_test_diff;
    bm_share_submission share;
    char jobid[32];
    char extranonce2[65];
} callback_capture;

static void capture_share(callback_capture *capture,
                          const bm_share_submission *share)
{
    capture->share = *share;
    snprintf(capture->jobid, sizeof(capture->jobid), "%s",
             share->jobid ? share->jobid : "");
    snprintf(capture->extranonce2, sizeof(capture->extranonce2), "%s",
             share->extranonce2 ? share->extranonce2 : "");
}

static void cb_register(void *opaque, const task_result *result)
{
    callback_capture *capture = opaque;
    capture->register_count++;
    TEST_ASSERT_EQUAL(REGISTER_TOTAL_COUNT, result->register_type);
}

static void cb_self_test(void *opaque, double difficulty)
{
    callback_capture *capture = opaque;
    capture->self_test_count++;
    capture->self_test_diff = difficulty;
}

static bool cb_ready(void *opaque, const bm_share_submission *share)
{
    callback_capture *capture = opaque;
    capture->ready_count++;
    capture_share(capture, share);
    return capture->transport_ready;
}

static int cb_sv1(void *opaque, const bm_share_submission *share)
{
    callback_capture *capture = opaque;
    capture->sv1_count++;
    capture_share(capture, share);
    return 0;
}

static int cb_standard(void *opaque, const bm_share_submission *share)
{
    callback_capture *capture = opaque;
    capture->standard_count++;
    capture_share(capture, share);
    return 0;
}

static int cb_extended(void *opaque, const bm_share_submission *share)
{
    callback_capture *capture = opaque;
    capture->extended_count++;
    capture_share(capture, share);
    return 0;
}

static void cb_account(void *opaque, const bm_share_submission *share)
{
    callback_capture *capture = opaque;
    capture->account_count++;
    capture_share(capture, share);
}

static const bm_result_callbacks callbacks = {
    .monitor_register = cb_register,
    .record_self_test = cb_self_test,
    .sv1_transport_ready = cb_ready,
    .submit_sv1 = cb_sv1,
    .submit_sv2_standard = cb_standard,
    .submit_sv2_extended = cb_extended,
    .account_share = cb_account,
};

typedef struct {
    pthread_mutex_t lock;
    uint8_t valid[BM_JOB_SLOT_COUNT];
    bm_job *jobs[BM_JOB_SLOT_COUNT];
    callback_capture capture;
    bm_result_context context;
} result_fixture;

static bm_job *routing_job(double pool_diff, const char *jobid,
                           const char *extranonce2)
{
    sv2_job_t source = standard_fixture();
    bm_job *job = calloc(1, sizeof(*job));
    TEST_ASSERT_TRUE(bm_job_build_sv2_standard(&source, 0, pool_diff, job));
    free(job->jobid);
    free(job->extranonce2);
    job->jobid = strdup(jobid);
    job->extranonce2 = strdup(extranonce2);
    return job;
}

static void result_fixture_init(result_fixture *fixture, double pool_diff)
{
    memset(fixture, 0, sizeof(*fixture));
    TEST_ASSERT_EQUAL(0, pthread_mutex_init(&fixture->lock, NULL));
    fixture->jobs[8] = routing_job(pool_diff, "42", "11223344");
    fixture->valid[8] = 1;
    fixture->capture.transport_ready = true;
    fixture->context = (bm_result_context) {
        .valid_jobs_lock = &fixture->lock,
        .valid_jobs = fixture->valid,
        .active_jobs = fixture->jobs,
        .protocol = BM_RESULT_PROTOCOL_SV1,
        .username = "worker.name",
        .sv2_extranonce2_len = 4,
        .callback_context = &fixture->capture,
    };
}

static void result_fixture_destroy(result_fixture *fixture)
{
    free_bm_job(fixture->jobs[8]);
    pthread_mutex_destroy(&fixture->lock);
}

static task_result nonce_result(void)
{
    return (task_result) {
        .job_id = 8,
        .nonce = 0x12345678,
        .rolled_version = 0x20006004,
        .register_type = REGISTER_INVALID,
        .asic_nr = 3,
        .core_id = 4,
        .small_core_id = 5,
        .timestamp_us = 1000,
    };
}

TEST_CASE("Register events bypass all job and share handling", "[bitmain][result]")
{
    callback_capture capture = {0};
    bm_result_context context = {.callback_context = &capture};
    task_result result = {.register_type = REGISTER_TOTAL_COUNT};
    TEST_ASSERT_EQUAL(BM_RESULT_HANDLED_REGISTER,
                      bm_result_handle(&result, &context, &callbacks));
    TEST_ASSERT_EQUAL(1, capture.register_count);
    TEST_ASSERT_EQUAL(0, capture.account_count);
    TEST_ASSERT_EQUAL(0, capture.sv1_count);
}

TEST_CASE("Invalid and null Bitmain slots never submit", "[bitmain][result][slot]")
{
    result_fixture fixture;
    result_fixture_init(&fixture, 0);
    task_result result = nonce_result();
    fixture.valid[8] = 0;
    TEST_ASSERT_EQUAL(BM_RESULT_REJECTED_JOB,
                      bm_result_handle(&result, &fixture.context, &callbacks));
    fixture.valid[8] = 1;
    free_bm_job(fixture.jobs[8]);
    fixture.jobs[8] = NULL;
    TEST_ASSERT_EQUAL(BM_RESULT_REJECTED_JOB,
                      bm_result_handle(&result, &fixture.context, &callbacks));
    TEST_ASSERT_EQUAL(0, fixture.capture.sv1_count);
    pthread_mutex_destroy(&fixture.lock);
}

TEST_CASE("Below difficulty is accounted but not submitted", "[bitmain][result]")
{
    result_fixture fixture;
    result_fixture_init(&fixture, DBL_MAX);
    task_result result = nonce_result();
    TEST_ASSERT_EQUAL(BM_RESULT_ACCOUNTED,
                      bm_result_handle(&result, &fixture.context, &callbacks));
    TEST_ASSERT_EQUAL(0, fixture.capture.ready_count);
    TEST_ASSERT_EQUAL(0, fixture.capture.sv1_count);
    TEST_ASSERT_EQUAL(1, fixture.capture.account_count);
    result_fixture_destroy(&fixture);
}

TEST_CASE("SV1 submission fields preserve username metadata and XOR version bits",
          "[bitmain][result][sv1]")
{
    result_fixture fixture;
    result_fixture_init(&fixture, 0);
    task_result result = nonce_result();
    bm_result_handle(&result, &fixture.context, &callbacks);
    TEST_ASSERT_EQUAL(1, fixture.capture.sv1_count);
    TEST_ASSERT_EQUAL_STRING("worker.name", fixture.capture.share.username);
    TEST_ASSERT_EQUAL_STRING("42", fixture.capture.jobid);
    TEST_ASSERT_EQUAL_STRING("11223344", fixture.capture.extranonce2);
    TEST_ASSERT_EQUAL_UINT32(0x65010203, fixture.capture.share.ntime);
    TEST_ASSERT_EQUAL_UINT32(result.nonce, fixture.capture.share.nonce);
    TEST_ASSERT_EQUAL_UINT32(0x00006000, fixture.capture.share.version_bits);
    TEST_ASSERT_DOUBLE_WITHIN(1e-20, 4.771996165422653e-10,
                              fixture.capture.share.nonce_diff);
    TEST_ASSERT_EQUAL(1, fixture.capture.account_count);
    result_fixture_destroy(&fixture);
}

TEST_CASE("SV2 standard and extended route exact share fields", "[bitmain][result][sv2]")
{
    result_fixture fixture;
    result_fixture_init(&fixture, 0);
    task_result result = nonce_result();

    fixture.context.protocol = BM_RESULT_PROTOCOL_SV2_STANDARD;
    bm_result_handle(&result, &fixture.context, &callbacks);
    TEST_ASSERT_EQUAL(1, fixture.capture.standard_count);
    TEST_ASSERT_EQUAL_UINT32(42, fixture.capture.share.numeric_job_id);
    TEST_ASSERT_EQUAL_UINT32(result.nonce, fixture.capture.share.nonce);
    TEST_ASSERT_EQUAL_UINT32(0x65010203, fixture.capture.share.ntime);
    TEST_ASSERT_EQUAL_UINT32(result.rolled_version,
                             fixture.capture.share.rolled_version);

    fixture.context.protocol = BM_RESULT_PROTOCOL_SV2_EXTENDED;
    bm_result_handle(&result, &fixture.context, &callbacks);
    TEST_ASSERT_EQUAL(1, fixture.capture.extended_count);
    TEST_ASSERT_EQUAL_UINT8(4, fixture.capture.share.extranonce2_len);
    assert_hex("11223344", fixture.capture.share.extranonce2_bin, 4);
    result_fixture_destroy(&fixture);
}

TEST_CASE("Self test records difficulty and bypasses accounting and submission",
          "[bitmain][result][self-test]")
{
    result_fixture fixture;
    result_fixture_init(&fixture, 0);
    fixture.context.self_test = true;
    task_result result = nonce_result();
    TEST_ASSERT_EQUAL(BM_RESULT_RECORDED_SELF_TEST,
                      bm_result_handle(&result, &fixture.context, &callbacks));
    TEST_ASSERT_EQUAL(1, fixture.capture.self_test_count);
    TEST_ASSERT_GREATER_THAN_DOUBLE(0, fixture.capture.self_test_diff);
    TEST_ASSERT_EQUAL(0, fixture.capture.sv1_count);
    TEST_ASSERT_EQUAL(0, fixture.capture.account_count);
    result_fixture_destroy(&fixture);
}

TEST_CASE("Null SV1 transport drops pool share but keeps normal accounting",
          "[bitmain][result][sv1]")
{
    result_fixture fixture;
    result_fixture_init(&fixture, 0);
    fixture.capture.transport_ready = false;
    task_result result = nonce_result();
    bm_result_handle(&result, &fixture.context, &callbacks);
    TEST_ASSERT_EQUAL(1, fixture.capture.ready_count);
    TEST_ASSERT_EQUAL(0, fixture.capture.sv1_count);
    TEST_ASSERT_EQUAL(1, fixture.capture.account_count);
    result_fixture_destroy(&fixture);
}

TEST_CASE("Bitmain job snapshots own metadata across slot reuse", "[bitmain][slot]")
{
    pthread_mutex_t lock;
    uint8_t valid[BM_JOB_SLOT_COUNT] = {0};
    bm_job *jobs[BM_JOB_SLOT_COUNT] = {0};
    pthread_mutex_init(&lock, NULL);
    jobs[12] = routing_job(1, "old-job", "aabb");
    valid[12] = 1;

    bm_job *snapshot = bm_job_slot_snapshot(&lock, valid, jobs, 12);
    TEST_ASSERT_NOT_NULL(snapshot);
    TEST_ASSERT_NOT_EQUAL(jobs[12]->jobid, snapshot->jobid);
    TEST_ASSERT_NOT_EQUAL(jobs[12]->extranonce2, snapshot->extranonce2);
    free_bm_job(jobs[12]);
    jobs[12] = routing_job(1, "new-job", "ccdd");
    TEST_ASSERT_EQUAL_STRING("old-job", snapshot->jobid);
    TEST_ASSERT_EQUAL_STRING("aabb", snapshot->extranonce2);
    free_bm_job(snapshot);
    free_bm_job(jobs[12]);
    pthread_mutex_destroy(&lock);
}

TEST_CASE("Bitmain snapshots preserve null metadata and tolerate repeated release",
          "[bitmain][slot]")
{
    pthread_mutex_t lock;
    uint8_t valid[BM_JOB_SLOT_COUNT] = {0};
    bm_job *jobs[BM_JOB_SLOT_COUNT] = {0};
    pthread_mutex_init(&lock, NULL);
    jobs[1] = calloc(1, sizeof(*jobs[1]));
    valid[1] = 1;
    bm_job *snapshot = bm_job_slot_snapshot(&lock, valid, jobs, 1);
    TEST_ASSERT_NOT_NULL(snapshot);
    TEST_ASSERT_NULL(snapshot->jobid);
    TEST_ASSERT_NULL(snapshot->extranonce2);
    free_bm_job(snapshot);

    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    for (int i = 0; i < 100; ++i) {
        snapshot = bm_job_slot_snapshot(&lock, valid, jobs, 1);
        TEST_ASSERT_NOT_NULL(snapshot);
        free_bm_job(snapshot);

        bm_job *replacement = calloc(1, sizeof(*replacement));
        TEST_ASSERT_NOT_NULL(replacement);
        pthread_mutex_lock(&lock);
        free_bm_job(jobs[1]);
        jobs[1] = replacement;
        valid[1] = 1;
        pthread_mutex_unlock(&lock);
    }
    TEST_ASSERT_EQUAL_UINT32(free_before,
                             heap_caps_get_free_size(MALLOC_CAP_8BIT));
    TEST_ASSERT_TRUE(heap_caps_check_integrity_all(true));
    free_bm_job(jobs[1]);
    pthread_mutex_destroy(&lock);
}

typedef struct {
    pthread_mutex_t lock;
    sem_t snapshot_locked;
    sem_t producer_started;
    uint8_t valid[BM_JOB_SLOT_COUNT];
    bm_job *jobs[BM_JOB_SLOT_COUNT];
    bool snapshot_copy_complete;
    bool producer_saw_complete_copy;
    bm_job *snapshot;
    bm_job *replacement;
} concurrent_slot_fixture;

static void *snapshot_thread(void *opaque)
{
    concurrent_slot_fixture *fixture = opaque;
    pthread_mutex_lock(&fixture->lock);
    sem_post(&fixture->snapshot_locked);
    sem_wait(&fixture->producer_started);
    fixture->snapshot =
        bm_job_slot_snapshot_locked(fixture->valid, fixture->jobs, 4);
    fixture->snapshot_copy_complete = true;
    pthread_mutex_unlock(&fixture->lock);
    return NULL;
}

static void *replace_thread(void *opaque)
{
    concurrent_slot_fixture *fixture = opaque;
    sem_post(&fixture->producer_started);
    pthread_mutex_lock(&fixture->lock);
    fixture->producer_saw_complete_copy = fixture->snapshot_copy_complete;
    free_bm_job(fixture->jobs[4]);
    fixture->jobs[4] = fixture->replacement;
    fixture->replacement = NULL;
    fixture->valid[4] = 1;
    pthread_mutex_unlock(&fixture->lock);
    return NULL;
}

TEST_CASE("Slot replacement cannot free a job while result snapshot copies it",
          "[bitmain][slot][concurrency]")
{
    concurrent_slot_fixture fixture = {0};
    pthread_t snapshot_task;
    pthread_t producer_task;
    pthread_mutex_init(&fixture.lock, NULL);
    sem_init(&fixture.snapshot_locked, 0, 0);
    sem_init(&fixture.producer_started, 0, 0);
    fixture.jobs[4] = routing_job(1, "source", "aabb");
    fixture.replacement = routing_job(1, "replacement", "ccdd");
    fixture.valid[4] = 1;

    TEST_ASSERT_EQUAL(0, pthread_create(&snapshot_task, NULL, snapshot_thread,
                                        &fixture));
    sem_wait(&fixture.snapshot_locked);
    TEST_ASSERT_EQUAL(0, pthread_create(&producer_task, NULL, replace_thread,
                                        &fixture));

    pthread_join(snapshot_task, NULL);
    pthread_join(producer_task, NULL);
    TEST_ASSERT_TRUE(fixture.producer_saw_complete_copy);
    TEST_ASSERT_EQUAL_STRING("source", fixture.snapshot->jobid);
    TEST_ASSERT_EQUAL_STRING("replacement", fixture.jobs[4]->jobid);
    free_bm_job(fixture.snapshot);
    free_bm_job(fixture.jobs[4]);
    sem_destroy(&fixture.snapshot_locked);
    sem_destroy(&fixture.producer_started);
    pthread_mutex_destroy(&fixture.lock);
}
