/* Runtime-neutral null-test semantic-owner KAT. */

#include "../test_framework.h"
#include "shared/xr_null_test_core.h"

TEST(null_test_owner_distinguishes_zero_tag_and_pointer) {
    ASSERT_TRUE(XR_NULL_TEST_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_NULL_TEST_HI, XR_SEM_OWNER_ID_SHARED_NULL_TEST_LO,
        XR_SEM_CONSUMER_VM, xr_null_test_tagged_core(UINT8_C(0))));
    ASSERT_FALSE(XR_NULL_TEST_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_NULL_TEST_HI, XR_SEM_OWNER_ID_SHARED_NULL_TEST_LO,
        XR_SEM_CONSUMER_AOT_HOSTED, xr_null_test_tagged_core(UINT8_C(1))));
    ASSERT_TRUE(xr_null_test_pointer_is_null_core(NULL));
    ASSERT_FALSE(xr_null_test_pointer_is_null_core((const void *) (uintptr_t) 1));
}

TEST(null_test_owner_rejects_all_nonzero_tag_values) {
    for (uint16_t tag = 1; tag <= UINT8_MAX; tag++)
        ASSERT_FALSE(xr_null_test_tagged_core((uint8_t) tag));
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Null Test Owner");
RUN_TEST(null_test_owner_distinguishes_zero_tag_and_pointer);
RUN_TEST(null_test_owner_rejects_all_nonzero_tag_values);
TEST_MAIN_END()
