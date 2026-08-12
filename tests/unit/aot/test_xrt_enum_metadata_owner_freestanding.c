/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xrt_enum_metadata_owner_freestanding.c - freestanding enum owner KAT
 */

#include "../test_framework.h"
#include "aot/xrt_core_freestanding.h"

_Noreturn void xr_hook_panic(const char *message, size_t len) {
    (void) fwrite(message, 1, len, stderr);
    (void) fputc('\n', stderr);
    abort();
}

TEST(freestanding_enum_metadata_adapters_preserve_compact_views) {
    uint64_t payload_view = (UINT64_C(2) << 32) | UINT64_C(9);
    ASSERT_EQ_INT(xrt_enum_metadata_access_variant_at(4, 3), 3);
    ASSERT_EQ_UINT((uint64_t) xrt_enum_metadata_access_payload_at(payload_view, 1),
                   UINT64_C(0x0000000900000001));
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Freestanding Enum Metadata Owner");
RUN_TEST(freestanding_enum_metadata_adapters_preserve_compact_views);
TEST_MAIN_END()
