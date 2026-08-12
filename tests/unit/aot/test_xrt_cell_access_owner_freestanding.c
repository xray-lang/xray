/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xrt_cell_access_owner_freestanding.c - freestanding cell adapter KAT
 */

#include "../test_framework.h"
#include "aot/xrt_core_freestanding.h"

_Noreturn void xr_hook_panic(const char *message, size_t len) {
    (void) fwrite(message, 1, len, stderr);
    abort();
}

void xr_hook_free(void *ptr) {
    (void) ptr;
}

TEST(freestanding_cell_adapters_load_and_replace_slot) {
    xrt_cell_t storage = {.value = XR_FROM_INT(11)};
    XrValue cell = xr_mkptr(&storage, XR_TAG_CELL);
    ASSERT_EQ_INT(XR_TO_INT(xrt_cell_access_get(cell)), 11);
    xrt_cell_access_set(cell, XR_FROM_INT(29));
    ASSERT_EQ_INT(XR_TO_INT(storage.value), 29);
}

TEST(freestanding_cell_adapters_preserve_invalid_carrier_behavior) {
    XrValue invalid = XR_FROM_INT(7);
    ASSERT_EQ_INT(XR_TO_INT(xrt_cell_access_get(invalid)), 7);
    xrt_cell_access_set(invalid, XR_FROM_INT(9));
    ASSERT_EQ_INT(XR_TO_INT(invalid), 7);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Freestanding Cell Access Owner");
RUN_TEST(freestanding_cell_adapters_load_and_replace_slot);
RUN_TEST(freestanding_cell_adapters_preserve_invalid_carrier_behavior);
TEST_MAIN_END()
