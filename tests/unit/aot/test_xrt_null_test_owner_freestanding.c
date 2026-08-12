/* Freestanding null-test semantic-owner KAT. */

#include "../test_framework.h"
#include "aot/xrt_core_freestanding.h"

_Noreturn void xr_hook_panic(const char *message, size_t len) {
    (void) fwrite(message, 1, len, stderr);
    (void) fputc('\n', stderr);
    abort();
}

TEST(freestanding_null_test_adapters_cover_tagged_and_pointer_storage) {
    ASSERT_TRUE(xrt_null_test_tagged(XR_TAG_NULL));
    ASSERT_FALSE(xrt_null_test_tagged(XR_TAG_I64));
    ASSERT_TRUE(xrt_null_test_pointer(NULL));
    ASSERT_FALSE(xrt_null_test_pointer((const void *) (uintptr_t) 1));
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Freestanding Null Test Owner");
RUN_TEST(freestanding_null_test_adapters_cover_tagged_and_pointer_storage);
TEST_MAIN_END()
