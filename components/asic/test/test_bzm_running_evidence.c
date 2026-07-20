#include "bzm_running_evidence.h"
#include "unity.h"

#include <math.h>
#include <string.h>

static const bzm_running_evidence_config_t CONFIG = {
    .required_chip_engine_writes = 944,
    .minimum_valid_results = 1,
    .allow_mapping_recovery = false,
    .maximum_mapping_rejections = 2,
    .maximum_local_rejections = 1,
    .proof_timeout_ms = 15000,
    .recovery_timeout_ms = 15000,
};

TEST_CASE("BZM Stage-7 proof floor classifies valid and rejected difficulty",
          "[asic][bzm][running][evidence]")
{
    TEST_ASSERT_TRUE(bzm_running_result_meets_proof(1.0, 1.0));
    TEST_ASSERT_TRUE(bzm_running_result_meets_proof(58.6, 1.0));
    TEST_ASSERT_FALSE(bzm_running_result_meets_proof(0.999, 1.0));
    TEST_ASSERT_FALSE(bzm_running_result_meets_proof(0.0, 1.0));
    TEST_ASSERT_FALSE(bzm_running_result_meets_proof(NAN, 1.0));
    TEST_ASSERT_FALSE(bzm_running_result_meets_proof(INFINITY, 1.0));
    TEST_ASSERT_FALSE(bzm_running_result_meets_proof(1.0, 0.0));

    bzm_running_stats_t baseline = {0};
    bzm_running_stats_t current = {
        .dispatch_batches = 1,
        .dispatched_logical_engines = 236,
        .dispatched_chip_engines = 944,
        .mapped_results = 2,
    };
    bzm_running_evidence_result_t result =
        bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 10);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_PENDING, result.status);
    TEST_ASSERT_EQUAL_UINT32(
        0, (uint32_t) result.observed.locally_rejected_results);

    current.locally_valid_results = 1;
    result = bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 20);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
}

TEST_CASE("BZM Stage-7 evidence stays pending until dispatch and a valid result",
          "[asic][bzm][running][evidence]")
{
    bzm_running_stats_t baseline = {.dispatch_batches = 9, .mapped_results = 4};
    bzm_running_stats_t current = baseline;
    bzm_running_evidence_result_t result =
        bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 100);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_PENDING, result.status);

    current.dispatch_batches++;
    current.dispatched_logical_engines += 236;
    current.dispatched_chip_engines += 944;
    result = bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 200);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_PENDING, result.status);

    current.mapped_results++;
    current.locally_valid_results++;
    result = bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 300);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    TEST_ASSERT_EQUAL_UINT32(944,
        (uint32_t) result.observed.dispatched_chip_engines);
    TEST_ASSERT_EQUAL_UINT32(1,
        (uint32_t) result.observed.locally_valid_results);
}

TEST_CASE("BZM Stage-7 evidence rejects asymmetric or unattributed work",
          "[asic][bzm][running][evidence]")
{
    bzm_running_stats_t baseline = {0};
    bzm_running_stats_t current = {.dispatch_failures = 1};
    bzm_running_evidence_result_t result =
        bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 10);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_DISPATCH, result.fault);

    current = (bzm_running_stats_t){.mapping_rejections = 1};
    result = bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 10);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_MAPPING, result.fault);

    current = (bzm_running_stats_t){
        .locally_rejected_results = 1,
        .local_rejection_streak = 1,
        .local_recovery_pending = true,
    };
    result = bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 10);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_PENDING, result.status);

    current.locally_rejected_results = 2;
    current.local_rejection_streak = 2;
    result = bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 10);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_PENDING, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_NONE, result.fault);
}

TEST_CASE("BZM Stage-7 evidence accepts one transient rejection with valid proof",
          "[asic][bzm][running][evidence]")
{
    bzm_running_stats_t baseline = {0};
    bzm_running_stats_t current = {
        .dispatch_batches = 1,
        .dispatched_logical_engines = 236,
        .dispatched_chip_engines = 944,
        .mapped_results = 2,
        .locally_valid_results = 1,
        .locally_rejected_results = 1,
        .local_rejection_streak = 0,
        .local_recovery_pending = false,
    };
    bzm_running_evidence_result_t result =
        bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 10);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_NONE, result.fault);
}

TEST_CASE("BZM Stage-7 mapping recovery requires valid proof after the last rejection",
          "[asic][bzm][running][evidence]")
{
    bzm_running_evidence_config_t recovery = CONFIG;
    recovery.allow_mapping_recovery = true;
    recovery.maximum_mapping_rejections = 8;
    recovery.recovery_timeout_ms = 5000;
    bzm_running_stats_t baseline = {0};
    bzm_running_stats_t current = {
        .dispatch_batches = 1,
        .dispatched_logical_engines = 236,
        .dispatched_chip_engines = 944,
        .mapped_results = 3,
        .mapping_rejections = 12,
        .mapping_rejection_streak = 7,
        .mapping_recovery_pending = true,
        .locally_valid_results = 3,
    };

    bzm_running_evidence_result_t result =
        bzm_running_evidence_evaluate(&baseline, &current, &recovery, 100);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_PENDING, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_NONE, result.fault);
    TEST_ASSERT_TRUE(result.observed.mapping_recovery_pending);

    current.mapped_results++;
    current.locally_valid_results++;
    current.mapping_rejection_streak = 0;
    current.mapping_recovery_pending = false;
    result = bzm_running_evidence_evaluate(&baseline, &current, &recovery, 200);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    TEST_ASSERT_EQUAL_UINT32(12,
        (uint32_t) result.observed.mapping_rejections);
    TEST_ASSERT_FALSE(result.observed.mapping_recovery_pending);
}

TEST_CASE("BZM Stage-7 mapping recovery bounds corruption by proof time rather than raw streak",
          "[asic][bzm][running][evidence]")
{
    bzm_running_evidence_config_t recovery = CONFIG;
    recovery.allow_mapping_recovery = true;
    recovery.maximum_mapping_rejections = 8;
    bzm_running_stats_t baseline = {0};
    bzm_running_stats_t current = {
        .mapping_rejections = 12,
        .mapping_rejection_streak = 9,
        .mapping_recovery_pending = true,
    };

    bzm_running_evidence_result_t result =
        bzm_running_evidence_evaluate(&baseline, &current, &recovery, 100);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_PENDING, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_NONE, result.fault);

    current.mapping_rejection_streak = 7;
    result = bzm_running_evidence_evaluate(
        &baseline, &current, &recovery, recovery.proof_timeout_ms);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_MAPPING, result.fault);
}

TEST_CASE("BZM Stage-7 local rejection streak recovers only after valid proof",
          "[asic][bzm][running][evidence]")
{
    bzm_running_stats_t baseline = {0};
    bzm_running_stats_t current = {
        .dispatch_batches = 2,
        .dispatched_logical_engines = 472,
        .dispatched_chip_engines = 1888,
        .mapped_results = 12,
        .locally_valid_results = 4,
        .locally_rejected_results = 8,
        .local_rejection_streak = 1,
        .local_recovery_pending = true,
    };

    bzm_running_evidence_result_t result =
        bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 100);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_PENDING, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_NONE, result.fault);

    current.mapped_results++;
    current.locally_valid_results++;
    current.local_rejection_streak = 0;
    current.local_recovery_pending = false;
    result = bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 200);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);

    current.locally_rejected_results += 2;
    current.local_rejection_streak = 2;
    current.local_recovery_pending = true;
    result = bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 300);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_PENDING, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_NONE, result.fault);
    result = bzm_running_evidence_evaluate(
        &baseline, &current, &CONFIG, CONFIG.proof_timeout_ms);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_LOCAL_VALIDATION, result.fault);
}

TEST_CASE("BZM Stage-7 evidence times out without adequate mining progress",
          "[asic][bzm][running][evidence]")
{
    bzm_running_stats_t baseline = {0};
    bzm_running_stats_t current = {
        .dispatch_batches = 1,
        .dispatched_logical_engines = 236,
        .dispatched_chip_engines = 944,
    };
    bzm_running_evidence_result_t result =
        bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 14999);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_PENDING, result.status);
    result = bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 15000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_TIMEOUT, result.fault);
}

TEST_CASE("BZM Stage-7 evidence rejects rollback and invalid configuration",
          "[asic][bzm][running][evidence]")
{
    bzm_running_stats_t baseline = {.dispatch_batches = 1};
    bzm_running_stats_t current = {0};
    bzm_running_evidence_result_t result =
        bzm_running_evidence_evaluate(&baseline, &current, &CONFIG, 0);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_COUNTER_ROLLBACK, result.fault);

    bzm_running_evidence_config_t invalid = CONFIG;
    invalid.minimum_valid_results = 0;
    result = bzm_running_evidence_evaluate(&current, &current, &invalid, 0);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_INVALID_CONFIGURATION, result.fault);

    invalid = CONFIG;
    invalid.allow_mapping_recovery = true;
    invalid.maximum_mapping_rejections = 0;
    result = bzm_running_evidence_evaluate(&current, &current, &invalid, 0);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_INVALID_CONFIGURATION, result.fault);

    invalid = CONFIG;
    invalid.recovery_timeout_ms = 0;
    result = bzm_running_evidence_evaluate(&current, &current, &invalid, 0);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_INVALID_CONFIGURATION, result.fault);
}

TEST_CASE("BZM Stage-7 lifecycle retains proof only during bounded recoverable bursts",
          "[asic][bzm][running][evidence][lifecycle]")
{
    bzm_running_evidence_config_t recovery = CONFIG;
    recovery.allow_mapping_recovery = true;
    recovery.maximum_mapping_rejections = 8;
    recovery.recovery_timeout_ms = 5000;
    bzm_running_stats_t baseline = {0};
    bzm_running_stats_t current = {
        .dispatch_batches = 2,
        .dispatched_logical_engines = 472,
        .dispatched_chip_engines = 1888,
        .mapped_results = 6,
        .locally_valid_results = 2,
    };
    bzm_running_evidence_lifecycle_t lifecycle;
    bzm_running_evidence_lifecycle_init(&lifecycle);

    bzm_running_evidence_result_t result = bzm_running_evidence_track(
        &lifecycle, &baseline, &current, &recovery, 1000, 2000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    TEST_ASSERT_TRUE(lifecycle.completed_once);
    TEST_ASSERT_FALSE(lifecycle.recovery_active);

    current.mapping_rejections = 2;
    current.mapping_rejection_streak = 2;
    current.mapping_recovery_pending = true;
    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &recovery, 1000, 3000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    TEST_ASSERT_TRUE(lifecycle.recovery_active);
    TEST_ASSERT_NOT_NULL(strstr(result.detail, "proof retained"));

    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &recovery, 1000, 7999);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &recovery, 1000, 8000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_MAPPING, result.fault);

    current.mapping_rejection_streak = 0;
    current.mapping_recovery_pending = false;
    current.mapped_results++;
    current.locally_valid_results++;
    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &recovery, 1000, 8001);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    TEST_ASSERT_FALSE(lifecycle.recovery_active);

    current.mapping_rejections += 9;
    current.mapping_rejection_streak = 9;
    current.mapping_recovery_pending = true;
    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &recovery, 1000, 8002);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    TEST_ASSERT_TRUE(lifecycle.recovery_active);
    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &recovery, 1000, 13002);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_MAPPING, result.fault);
}

TEST_CASE("BZM Stage-7 lifecycle establishes real proof during bounded pending recovery",
          "[asic][bzm][running][evidence][lifecycle]")
{
    bzm_running_evidence_config_t recovery = CONFIG;
    recovery.allow_mapping_recovery = true;
    recovery.maximum_mapping_rejections = 8;
    recovery.maximum_local_rejections = 32;
    bzm_running_stats_t baseline = {0};
    bzm_running_stats_t current = {
        .dispatch_batches = 2,
        .dispatched_logical_engines = 472,
        .dispatched_chip_engines = 1888,
        .mapped_results = 10,
        .mapping_rejections = 3,
        .mapping_rejection_streak = 2,
        .mapping_recovery_pending = true,
        .locally_valid_results = 3,
        .locally_rejected_results = 7,
        .local_rejection_streak = 1,
        .local_recovery_pending = true,
    };
    bzm_running_evidence_lifecycle_t lifecycle;
    bzm_running_evidence_lifecycle_init(&lifecycle);

    bzm_running_evidence_result_t result = bzm_running_evidence_track(
        &lifecycle, &baseline, &current, &recovery, 1000, 2000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    TEST_ASSERT_TRUE(lifecycle.completed_once);
    TEST_ASSERT_TRUE(lifecycle.recovery_active);
    TEST_ASSERT_EQUAL_UINT32(2000, (uint32_t) lifecycle.recovery_started_at_ms);
    TEST_ASSERT_NOT_NULL(strstr(result.detail, "proof established"));

    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &recovery, 1000, 16999);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &recovery, 1000, 17000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_MAPPING, result.fault);
}

TEST_CASE("BZM Stage-7 lifecycle starts a fresh recovery after newly verified proof",
          "[asic][bzm][running][evidence][lifecycle]")
{
    bzm_running_evidence_config_t recovery = CONFIG;
    recovery.allow_mapping_recovery = true;
    recovery.maximum_mapping_rejections = 8;
    recovery.maximum_local_rejections = 32;
    bzm_running_stats_t baseline = {0};
    bzm_running_stats_t current = {
        .dispatch_batches = 1,
        .dispatched_logical_engines = 236,
        .dispatched_chip_engines = 944,
        .mapped_results = 2,
        .locally_valid_results = 1,
    };
    bzm_running_evidence_lifecycle_t lifecycle;
    bzm_running_evidence_lifecycle_init(&lifecycle);

    bzm_running_evidence_result_t result = bzm_running_evidence_track(
        &lifecycle, &baseline, &current, &recovery, 1000, 2000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);

    current.mapping_rejections = 2;
    current.mapping_rejection_streak = 2;
    current.mapping_recovery_pending = true;
    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &recovery, 1000, 3000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    TEST_ASSERT_EQUAL_UINT32(3000, (uint32_t) lifecycle.recovery_started_at_ms);

    /* A valid nonce cleared that episode between samples. The current pending
     * rejection is therefore a new bounded episode with its own deadline. */
    current.mapped_results++;
    current.locally_valid_results++;
    current.mapping_rejections++;
    current.mapping_rejection_streak = 1;
    current.mapping_recovery_pending = true;
    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &recovery, 1000, 17000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    TEST_ASSERT_EQUAL_UINT32(17000, (uint32_t) lifecycle.recovery_started_at_ms);

    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &recovery, 1000, 31999);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_GOOD, result.status);
    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &recovery, 1000, 32000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_MAPPING, result.fault);
}

TEST_CASE("BZM Stage-7 lifecycle keeps the initial proof deadline fail closed",
          "[asic][bzm][running][evidence][lifecycle]")
{
    bzm_running_stats_t baseline = {0};
    bzm_running_stats_t current = {0};
    bzm_running_evidence_lifecycle_t lifecycle;
    bzm_running_evidence_lifecycle_init(&lifecycle);

    bzm_running_evidence_result_t result = bzm_running_evidence_track(
        &lifecycle, &baseline, &current, &CONFIG, 1000, 15999);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_PENDING, result.status);
    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &CONFIG, 1000, 16000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_TIMEOUT, result.fault);

    current = (bzm_running_stats_t){
        .dispatch_batches = 1,
        .dispatched_logical_engines = 236,
        .dispatched_chip_engines = 944,
        .mapped_results = 1,
        .locally_valid_results = 1,
    };
    bzm_running_evidence_lifecycle_init(&lifecycle);
    result = bzm_running_evidence_track(&lifecycle, &baseline, &current,
                                        &CONFIG, 1000, 16000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_TIMEOUT, result.fault);

    result = bzm_running_evidence_track(NULL, &baseline, &current, &CONFIG,
                                        1000, 1000);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_BAD, result.status);
    TEST_ASSERT_EQUAL(BZM_RUNNING_EVIDENCE_FAULT_INVALID_CONFIGURATION,
                      result.fault);
}
