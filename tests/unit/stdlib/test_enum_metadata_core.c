/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_enum_metadata_core.c - enum metadata checked-view KAT
 */

#include "../test_framework.h"
#include "shared/xr_enum_metadata_core.h"

TEST(enum_variant_view_preserves_valid_index) {
    XrEnumMetadataResult first = xr_enum_metadata_variant_at_core(3, 0);
    XrEnumMetadataResult last = xr_enum_metadata_variant_at_core(3, 2);
    ASSERT_EQ(first.status, XR_ENUM_METADATA_OK);
    ASSERT_EQ_INT(first.value, 0);
    ASSERT_EQ(last.status, XR_ENUM_METADATA_OK);
    ASSERT_EQ_INT(last.value, 2);
}

TEST(enum_variant_view_rejects_signed_and_upper_bounds) {
    ASSERT_EQ(xr_enum_metadata_variant_at_core(3, -1).status,
              XR_ENUM_METADATA_INDEX_OUT_OF_BOUNDS);
    ASSERT_EQ(xr_enum_metadata_variant_at_core(3, 3).status,
              XR_ENUM_METADATA_INDEX_OUT_OF_BOUNDS);
    ASSERT_EQ(xr_enum_metadata_variant_at_core(-1, 0).status,
              XR_ENUM_METADATA_INDEX_OUT_OF_BOUNDS);
}

TEST(enum_payload_view_preserves_ordinal_and_field_index) {
    uint64_t view = (UINT64_C(3) << 32) | UINT64_C(0xfedcba98);
    XrEnumMetadataResult first = xr_enum_metadata_payload_at_core(view, 0);
    XrEnumMetadataResult last = xr_enum_metadata_payload_at_core(view, 2);
    ASSERT_EQ(first.status, XR_ENUM_METADATA_OK);
    ASSERT_EQ_UINT((uint64_t) first.value, UINT64_C(0xfedcba9800000000));
    ASSERT_EQ(last.status, XR_ENUM_METADATA_OK);
    ASSERT_EQ_UINT((uint64_t) last.value, UINT64_C(0xfedcba9800000002));
}

TEST(enum_payload_view_uses_unsigned_count_boundary) {
    uint64_t view = (UINT64_C(0xffffffff) << 32) | UINT64_C(7);
    ASSERT_EQ(xr_enum_metadata_payload_at_core(view, -1).status,
              XR_ENUM_METADATA_INDEX_OUT_OF_BOUNDS);
    ASSERT_EQ(xr_enum_metadata_payload_at_core(view, UINT32_MAX).status,
              XR_ENUM_METADATA_INDEX_OUT_OF_BOUNDS);
    ASSERT_EQ(xr_enum_metadata_payload_at_core(view, UINT32_MAX - 1).status,
              XR_ENUM_METADATA_OK);
}

TEST(enum_metadata_owner_guard_accepts_declared_vm_consumer) {
    XrEnumMetadataResult result = XR_ENUM_METADATA_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_HI,
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_LO, XR_SEM_CONSUMER_VM,
        xr_enum_metadata_variant_at_core(1, 0));
    ASSERT_EQ(result.status, XR_ENUM_METADATA_OK);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Enum Metadata Core");
RUN_TEST(enum_variant_view_preserves_valid_index);
RUN_TEST(enum_variant_view_rejects_signed_and_upper_bounds);
RUN_TEST(enum_payload_view_preserves_ordinal_and_field_index);
RUN_TEST(enum_payload_view_uses_unsigned_count_boundary);
RUN_TEST(enum_metadata_owner_guard_accepts_declared_vm_consumer);
TEST_MAIN_END()
