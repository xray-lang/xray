/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_data_pointer_core.c - data-pointer projection owner KAT
 */

#include "../test_framework.h"
#include "shared/xr_data_pointer_core.h"

TEST(data_pointer_projection_preserves_owner_borrow) {
    uint8_t storage[3] = {4, 5, 6};
    XrDataPointerProjection result =
        xr_data_pointer_project_core(storage, XR_DATA_POINTER_OWNER_BORROW);
    ASSERT_EQ(result.address, storage);
    ASSERT_EQ(result.lifetime, XR_DATA_POINTER_OWNER_BORROW);
}

TEST(data_pointer_projection_preserves_static_lifetime) {
    static const uint8_t bytes[3] = {'o', 'k', 0};
    XrDataPointerProjection result =
        xr_data_pointer_project_core(bytes, XR_DATA_POINTER_STATIC);
    ASSERT_EQ(result.address, bytes);
    ASSERT_EQ(result.lifetime, XR_DATA_POINTER_STATIC);
}

TEST(data_pointer_projection_preserves_empty_storage_address) {
    XrDataPointerProjection result =
        xr_data_pointer_project_core(NULL, XR_DATA_POINTER_OWNER_BORROW);
    ASSERT_EQ(result.address, NULL);
    ASSERT_EQ(result.lifetime, XR_DATA_POINTER_OWNER_BORROW);
}

TEST(data_pointer_owner_guard_accepts_declared_vm_consumer) {
    uint8_t storage = 7;
    XrDataPointerProjection result = XR_DATA_POINTER_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_DATA_POINTER_HI, XR_SEM_OWNER_ID_SHARED_DATA_POINTER_LO,
        XR_SEM_CONSUMER_VM, &storage, XR_DATA_POINTER_OWNER_BORROW);
    ASSERT_EQ(result.address, &storage);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Data Pointer Core");
RUN_TEST(data_pointer_projection_preserves_owner_borrow);
RUN_TEST(data_pointer_projection_preserves_static_lifetime);
RUN_TEST(data_pointer_projection_preserves_empty_storage_address);
RUN_TEST(data_pointer_owner_guard_accepts_declared_vm_consumer);
TEST_MAIN_END()
