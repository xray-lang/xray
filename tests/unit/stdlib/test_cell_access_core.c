/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cell_access_core.c - shared capture-cell slot owner KAT
 */

#include "../test_framework.h"
#include "xray_value_abi.h"
#include "shared/xr_cell_access_core.h"

static XrValue test_value(uint8_t tag, int64_t payload) {
    XrValue value = {0};
    value.tag = tag;
    value.i = payload;
    return value;
}

TEST(cell_access_load_preserves_full_tagged_value) {
    XrValue slot = test_value(37, INT64_C(0x123456789abcdef));
    XrValue loaded = xr_cell_access_load_core(&slot);
    ASSERT_EQ_INT(loaded.tag, 37);
    ASSERT_EQ_INT(loaded.i, INT64_C(0x123456789abcdef));
}

TEST(cell_access_replace_returns_old_and_publishes_new) {
    XrValue slot = test_value(3, -11);
    XrValue replacement = test_value(4, 29);
    XrValue old = xr_cell_access_replace_core(&slot, replacement);
    ASSERT_EQ_INT(old.tag, 3);
    ASSERT_EQ_INT(old.i, -11);
    ASSERT_EQ_INT(slot.tag, 4);
    ASSERT_EQ_INT(slot.i, 29);
}

TEST(cell_access_owner_guard_accepts_declared_consumers) {
    XrValue slot = test_value(3, 7);
    XrValue loaded = XR_CELL_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_HI, XR_SEM_OWNER_ID_SHARED_CELL_ACCESS_LO,
        XR_SEM_CONSUMER_VM, xr_cell_access_load_core(&slot));
    ASSERT_EQ_INT(loaded.i, 7);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Cell Access Core");
RUN_TEST(cell_access_load_preserves_full_tagged_value);
RUN_TEST(cell_access_replace_returns_old_and_publishes_new);
RUN_TEST(cell_access_owner_guard_accepts_declared_consumers);
TEST_MAIN_END()
