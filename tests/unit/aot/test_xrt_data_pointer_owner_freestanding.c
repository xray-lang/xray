/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xrt_data_pointer_owner_freestanding.c - freestanding pointer owner KAT
 */

#include "../test_framework.h"
/* The minimal freestanding test prelude does not import hosted xrt_value.h. */
#define XR_TO_BOOL(value) ((int) (value).i)
#include "aot/xrt_core_freestanding.h"

TEST(freestanding_data_pointer_adapter_preserves_borrow) {
    uint8_t storage[2] = {8, 9};
    XrDataPointerProjection result =
        xrt_data_pointer_project(storage, XR_DATA_POINTER_OWNER_BORROW);
    ASSERT_EQ(result.address, storage);
    ASSERT_EQ(result.lifetime, XR_DATA_POINTER_OWNER_BORROW);
}

TEST(freestanding_data_pointer_adapter_preserves_static_storage) {
    static const uint8_t storage[2] = {1, 2};
    XrDataPointerProjection result = xrt_data_pointer_project(storage, XR_DATA_POINTER_STATIC);
    ASSERT_EQ(result.address, storage);
    ASSERT_EQ(result.lifetime, XR_DATA_POINTER_STATIC);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Freestanding Data Pointer Owner");
RUN_TEST(freestanding_data_pointer_adapter_preserves_borrow);
RUN_TEST(freestanding_data_pointer_adapter_preserves_static_storage);
TEST_MAIN_END()
