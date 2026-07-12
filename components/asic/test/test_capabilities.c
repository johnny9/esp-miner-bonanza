#include "asic_capabilities.h"
#include "unity.h"

TEST_CASE("BM1397 advertises midstate rolling and periodic work refresh",
          "[asic][capabilities]")
{
    asic_capabilities_t capabilities = ASIC_capabilities_for_chip_id(1397);

    TEST_ASSERT_EQUAL(ASIC_VERSION_ROLLING_MIDSTATE,
                      capabilities.version_rolling);
    TEST_ASSERT_EQUAL_HEX32(0x1fffe000, capabilities.supported_version_mask);
    TEST_ASSERT_EQUAL_UINT32(4, capabilities.max_version_variants);
    TEST_ASSERT_FALSE(capabilities.supports_ntime_rolling);
    TEST_ASSERT_EQUAL_UINT32(0, capabilities.max_ntime_roll);
    TEST_ASSERT_EQUAL(ASIC_WORK_REFRESH_PERIODIC,
                      capabilities.work_refresh_policy);
    TEST_ASSERT_FALSE(capabilities.driver_owns_scheduling);
    TEST_ASSERT_TRUE(
        ASIC_capabilities_support_version_rolling(&capabilities));
    TEST_ASSERT_FALSE(ASIC_capabilities_support_static_work(&capabilities));
}

TEST_CASE("BM1366 BM1368 and BM1370 advertise hardware version rolling",
          "[asic][capabilities]")
{
    const uint16_t chip_ids[] = {1366, 1368, 1370};

    for (size_t i = 0; i < sizeof(chip_ids) / sizeof(chip_ids[0]); ++i) {
        asic_capabilities_t capabilities =
            ASIC_capabilities_for_chip_id(chip_ids[i]);
        TEST_ASSERT_EQUAL(ASIC_VERSION_ROLLING_HARDWARE,
                          capabilities.version_rolling);
        TEST_ASSERT_EQUAL_HEX32(0x1fffe000,
                                capabilities.supported_version_mask);
        TEST_ASSERT_EQUAL_UINT32(65536,
                                 capabilities.max_version_variants);
        TEST_ASSERT_FALSE(capabilities.supports_ntime_rolling);
        TEST_ASSERT_EQUAL(ASIC_WORK_REFRESH_ON_NEW_JOB,
                          capabilities.work_refresh_policy);
        TEST_ASSERT_FALSE(capabilities.driver_owns_scheduling);
        TEST_ASSERT_TRUE(
            ASIC_capabilities_support_version_rolling(&capabilities));
        TEST_ASSERT_TRUE(
            ASIC_capabilities_support_static_work(&capabilities));
    }
}

TEST_CASE("Missing and unknown ASIC capabilities use conservative defaults",
          "[asic][capabilities]")
{
    asic_capabilities_t missing = ASIC_capabilities_for_chip_id(0);
    asic_capabilities_t unknown = ASIC_capabilities_for_chip_id(99);

    TEST_ASSERT_EQUAL(ASIC_VERSION_ROLLING_NONE, missing.version_rolling);
    TEST_ASSERT_EQUAL_UINT32(1, missing.max_version_variants);
    TEST_ASSERT_EQUAL(ASIC_WORK_REFRESH_PERIODIC,
                      missing.work_refresh_policy);
    TEST_ASSERT_FALSE(ASIC_capabilities_support_version_rolling(&missing));
    TEST_ASSERT_FALSE(ASIC_capabilities_support_static_work(&missing));

    TEST_ASSERT_EQUAL(ASIC_VERSION_ROLLING_NONE, unknown.version_rolling);
    TEST_ASSERT_EQUAL_HEX32(0, unknown.supported_version_mask);
    TEST_ASSERT_EQUAL_UINT32(1, unknown.max_version_variants);
    TEST_ASSERT_FALSE(unknown.supports_ntime_rolling);
    TEST_ASSERT_EQUAL_UINT32(0, unknown.max_ntime_roll);
    TEST_ASSERT_EQUAL(ASIC_WORK_REFRESH_PERIODIC,
                      unknown.work_refresh_policy);
    TEST_ASSERT_FALSE(unknown.driver_owns_scheduling);
    TEST_ASSERT_FALSE(ASIC_capabilities_support_version_rolling(NULL));
    TEST_ASSERT_FALSE(ASIC_capabilities_support_static_work(NULL));
}
