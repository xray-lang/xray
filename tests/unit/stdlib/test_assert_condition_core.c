/* Runtime-neutral assertion-condition semantic-owner KAT. */

#include "../test_framework.h"
#include "shared/xr_assert_condition_core.h"

#define ASSERT_FAILED(consumer, truthy, expected)                                                 \
    XR_ASSERT_CONDITION_OWNER_APPLY(                                                             \
        XR_SEM_OWNER_ID_SHARED_ASSERT_CONDITION_HI,                                              \
        XR_SEM_OWNER_ID_SHARED_ASSERT_CONDITION_LO, (consumer), (truthy), (expected))

TEST(assert_condition_owner_covers_both_expectations) {
    ASSERT_FALSE(ASSERT_FAILED(XR_SEM_CONSUMER_VM, true, true));
    ASSERT_TRUE(ASSERT_FAILED(XR_SEM_CONSUMER_VM, false, true));
    ASSERT_FALSE(ASSERT_FAILED(XR_SEM_CONSUMER_AOT_HOSTED, false, false));
    ASSERT_TRUE(ASSERT_FAILED(XR_SEM_CONSUMER_AOT_FREESTANDING, true, false));
    ASSERT_FALSE(ASSERT_FAILED(XR_SEM_CONSUMER_CGEN, true, true));
}

TEST(assert_condition_owner_accepts_classified_equality) {
    bool equal = true;
    ASSERT_FALSE(ASSERT_FAILED(XR_SEM_CONSUMER_VM, equal, true));
    ASSERT_TRUE(ASSERT_FAILED(XR_SEM_CONSUMER_AOT_HOSTED, equal, false));
    equal = false;
    ASSERT_TRUE(ASSERT_FAILED(XR_SEM_CONSUMER_AOT_FREESTANDING, equal, true));
    ASSERT_FALSE(ASSERT_FAILED(XR_SEM_CONSUMER_CGEN, equal, false));
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Assertion Condition Owner");
RUN_TEST(assert_condition_owner_covers_both_expectations);
RUN_TEST(assert_condition_owner_accepts_classified_equality);
TEST_MAIN_END()
