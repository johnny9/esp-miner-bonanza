#include <stddef.h>

#include "bzm_ota_guard.h"
#include "unity.h"

typedef struct {
    bool acquire_ok;
    bool release_ok;
    size_t acquire_calls;
    size_t release_calls;
} simulated_ota_owner_t;

static bool acquire_owner(void *context)
{
    simulated_ota_owner_t *owner = context;
    owner->acquire_calls++;
    return owner->acquire_ok;
}

static bool release_owner(void *context)
{
    simulated_ota_owner_t *owner = context;
    owner->release_calls++;
    return owner->release_ok;
}

static bzm_ota_guard_t guard_for(simulated_ota_owner_t *owner)
{
    bzm_ota_guard_t guard;
    TEST_ASSERT_TRUE(bzm_ota_guard_init(
        &guard, acquire_owner, release_owner, owner));
    return guard;
}

TEST_CASE("BZM OTA acquisition failure performs no release",
          "[asic][bzm][ota]")
{
    simulated_ota_owner_t owner = {.release_ok = true};
    bzm_ota_guard_t guard = guard_for(&owner);
    TEST_ASSERT_FALSE(bzm_ota_guard_begin(&guard));
    TEST_ASSERT_EQUAL_UINT32(1, owner.acquire_calls);
    TEST_ASSERT_EQUAL_UINT32(0, owner.release_calls);
    TEST_ASSERT_FALSE(guard.active);
    TEST_ASSERT_FALSE(bzm_ota_guard_release(&guard));
}

TEST_CASE("BZM failed or WWW OTA releases ownership exactly once",
          "[asic][bzm][ota]")
{
    simulated_ota_owner_t owner = {
        .acquire_ok = true,
        .release_ok = true,
    };
    bzm_ota_guard_t guard = guard_for(&owner);
    TEST_ASSERT_TRUE(bzm_ota_guard_begin(&guard));
    TEST_ASSERT_TRUE(bzm_ota_guard_release(&guard));
    TEST_ASSERT_EQUAL_UINT32(1, owner.acquire_calls);
    TEST_ASSERT_EQUAL_UINT32(1, owner.release_calls);
    TEST_ASSERT_FALSE(guard.active);
    TEST_ASSERT_FALSE(bzm_ota_guard_release(&guard));
    TEST_ASSERT_EQUAL_UINT32(1, owner.release_calls);
}

TEST_CASE("BZM OTA reports a failed safe-off release without retrying",
          "[asic][bzm][ota]")
{
    simulated_ota_owner_t owner = {.acquire_ok = true};
    bzm_ota_guard_t guard = guard_for(&owner);
    TEST_ASSERT_TRUE(bzm_ota_guard_begin(&guard));
    TEST_ASSERT_FALSE(bzm_ota_guard_release(&guard));
    TEST_ASSERT_EQUAL_UINT32(1, owner.release_calls);
    TEST_ASSERT_FALSE(guard.active);
    TEST_ASSERT_FALSE(bzm_ota_guard_release(&guard));
    TEST_ASSERT_EQUAL_UINT32(1, owner.release_calls);
}

TEST_CASE("BZM firmware OTA retains exclusive ownership through reboot",
          "[asic][bzm][ota]")
{
    simulated_ota_owner_t owner = {
        .acquire_ok = true,
        .release_ok = true,
    };
    bzm_ota_guard_t guard = guard_for(&owner);
    TEST_ASSERT_TRUE(bzm_ota_guard_begin(&guard));
    TEST_ASSERT_TRUE(bzm_ota_guard_retain_for_reboot(&guard));
    TEST_ASSERT_TRUE(guard.active);
    TEST_ASSERT_TRUE(guard.retained_for_reboot);
    TEST_ASSERT_FALSE(bzm_ota_guard_release(&guard));
    TEST_ASSERT_EQUAL_UINT32(0, owner.release_calls);
}
