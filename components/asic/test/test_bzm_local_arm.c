#include "bzm_local_arm.h"

#include <stdint.h>

#include "unity.h"

static const char * const CONFIRMATION = "ENERGIZE_BZM_1002";

TEST_CASE("BZM local arm requires the exact confirmation", "[asic][bzm][local-arm]")
{
    bzm_local_arm_t arm;
    bzm_local_arm_init(&arm);

    TEST_ASSERT_EQUAL(BZM_LOCAL_ARM_CONFIRMATION_MISMATCH, bzm_local_arm_issue(&arm, "wrong", CONFIRMATION, 1000, 30000));
    TEST_ASSERT_EQUAL_UINT32(0, bzm_local_arm_remaining_ms(&arm, 1000));
    TEST_ASSERT_FALSE(bzm_local_arm_consume(&arm, 1000));

    TEST_ASSERT_EQUAL(BZM_LOCAL_ARM_INVALID_ARGUMENT, bzm_local_arm_issue(&arm, CONFIRMATION, CONFIRMATION, 1000, 0));
    TEST_ASSERT_EQUAL(BZM_LOCAL_ARM_INVALID_ARGUMENT, bzm_local_arm_issue(&arm, CONFIRMATION, CONFIRMATION, UINT64_MAX, 1));
}

TEST_CASE("BZM local arm is bounded and consumed exactly once", "[asic][bzm][local-arm]")
{
    bzm_local_arm_t arm;
    bzm_local_arm_init(&arm);

    TEST_ASSERT_EQUAL(BZM_LOCAL_ARM_ACCEPTED, bzm_local_arm_issue(&arm, CONFIRMATION, CONFIRMATION, 1000, 30000));
    TEST_ASSERT_EQUAL_UINT32(30000, bzm_local_arm_remaining_ms(&arm, 1000));
    TEST_ASSERT_EQUAL_UINT32(1, bzm_local_arm_remaining_ms(&arm, 30999));
    TEST_ASSERT_TRUE(bzm_local_arm_consume(&arm, 30999));
    TEST_ASSERT_FALSE(bzm_local_arm_consume(&arm, 30999));
    TEST_ASSERT_EQUAL_UINT32(0, bzm_local_arm_remaining_ms(&arm, 30999));
}

TEST_CASE("BZM local arm fails closed at expiry", "[asic][bzm][local-arm]")
{
    bzm_local_arm_t arm;
    bzm_local_arm_init(&arm);

    TEST_ASSERT_EQUAL(BZM_LOCAL_ARM_ACCEPTED, bzm_local_arm_issue(&arm, CONFIRMATION, CONFIRMATION, 500, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, bzm_local_arm_remaining_ms(&arm, 1500));
    TEST_ASSERT_FALSE(bzm_local_arm_consume(&arm, 1500));
    TEST_ASSERT_FALSE(arm.armed);
    TEST_ASSERT_TRUE(arm.deadline_ms == 0);
}
