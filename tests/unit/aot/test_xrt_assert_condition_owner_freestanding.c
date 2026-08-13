/* Freestanding assertion-condition semantic-owner KAT. */

#include "../test_framework.h"
#include "aot/xrt_core_freestanding.h"

_Noreturn void xr_hook_panic(const char *message, size_t len) {
    (void) fwrite(message, 1, len, stderr);
    (void) fputc('\n', stderr);
    abort();
}

TEST(freestanding_assert_condition_adapter_covers_both_expectations) {
    ASSERT_FALSE(xrt_assert_condition_failed(true, true));
    ASSERT_TRUE(xrt_assert_condition_failed(false, true));
    ASSERT_FALSE(xrt_assert_condition_failed(false, false));
    ASSERT_TRUE(xrt_assert_condition_failed(true, false));
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Freestanding Assertion Condition Owner");
RUN_TEST(freestanding_assert_condition_adapter_covers_both_expectations);
TEST_MAIN_END()
