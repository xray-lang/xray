/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_swiss_index.c - Unit tests for shared Swiss probing primitives.
 */

#include "../test_framework.h"
#include "shared/xr_swiss_index.h"

typedef struct SwissMatchCtx {
    int64_t *values;
    int64_t needle;
} SwissMatchCtx;

static int swiss_match_value(void *ctx_ptr, int64_t slot) {
    SwissMatchCtx *ctx = (SwissMatchCtx *) ctx_ptr;
    return ctx->values[slot] == ctx->needle;
}

static void swiss_clear(uint8_t *ctrl, int64_t slots) {
    memset(ctrl, (int) XR_SWISS_CTRL_EMPTY, (size_t) slots + XR_SWISS_GROUP);
}

TEST(swiss_slots_and_budget) {
    ASSERT_EQ_INT(xr_swiss_slots_for_i64(-10), 8);
    ASSERT_EQ_INT(xr_swiss_slots_for_i64(0), 8);
    ASSERT_EQ_INT(xr_swiss_slots_for_i64(7), 8);
    ASSERT_EQ_INT(xr_swiss_slots_for_i64(8), 16);
    ASSERT_EQ_INT(xr_swiss_slots_for_i64(14), 16);
    ASSERT_EQ_INT(xr_swiss_slots_for_i64(15), 32);
    ASSERT_EQ_INT(xr_swiss_capacity_budget_i64(8), 7);
    ASSERT_EQ_INT(xr_swiss_capacity_budget_i64(16), 14);
}

TEST(swiss_ctrl_set_mirrors_group_tail) {
    uint8_t ctrl[16 + XR_SWISS_GROUP];
    swiss_clear(ctrl, 16);

    xr_swiss_ctrl_set_i64(ctrl, 16, 0, 0x12u);
    xr_swiss_ctrl_set_i64(ctrl, 16, 7, 0x34u);
    xr_swiss_ctrl_set_i64(ctrl, 16, 8, 0x56u);

    ASSERT_EQ_INT(ctrl[0], 0x12u);
    ASSERT_EQ_INT(ctrl[16], 0x12u);
    ASSERT_EQ_INT(ctrl[7], 0x34u);
    ASSERT_EQ_INT(ctrl[23], 0x34u);
    ASSERT_EQ_INT(ctrl[8], 0x56u);
}

TEST(swiss_find_free_and_empty_split_tombstone_policy) {
    uint8_t ctrl[16 + XR_SWISS_GROUP];
    swiss_clear(ctrl, 16);

    xr_swiss_ctrl_set_i64(ctrl, 16, 0, XR_SWISS_CTRL_DELETED);

    ASSERT_EQ_INT(xr_swiss_find_free_i64(ctrl, 16, 0), 0);
    ASSERT_EQ_INT(xr_swiss_find_empty_i64(ctrl, 16, 0), 1);
}

TEST(swiss_find_match_uses_comparator) {
    uint8_t ctrl[16 + XR_SWISS_GROUP];
    int64_t values[16];
    swiss_clear(ctrl, 16);
    for (int i = 0; i < 16; i++)
        values[i] = -1;

    uint64_t h1 = 0x101u;
    uint64_t h2 = 0x181u;
    int64_t s1 = xr_swiss_find_empty_i64(ctrl, 16, h1);
    xr_swiss_ctrl_set_i64(ctrl, 16, s1, xr_swiss_h2(h1));
    values[s1] = 11;

    int64_t s2 = xr_swiss_find_empty_i64(ctrl, 16, h2);
    xr_swiss_ctrl_set_i64(ctrl, 16, s2, xr_swiss_h2(h2));
    values[s2] = 22;

    SwissMatchCtx ctx;
    ctx.values = values;
    ctx.needle = 11;
    ASSERT_EQ_INT(xr_swiss_find_match_i64(ctrl, 16, h1, swiss_match_value, &ctx), s1);

    ctx.needle = 99;
    ASSERT_EQ_INT(xr_swiss_find_match_i64(ctrl, 16, h1, swiss_match_value, &ctx), -1);

    ctx.needle = 22;
    ASSERT_EQ_INT(xr_swiss_find_match_i64(ctrl, 16, h2, swiss_match_value, &ctx), s2);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Swiss Index");
RUN_TEST(swiss_slots_and_budget);
RUN_TEST(swiss_ctrl_set_mirrors_group_tail);
RUN_TEST(swiss_find_free_and_empty_split_tombstone_policy);
RUN_TEST(swiss_find_match_uses_comparator);

TEST_MAIN_END()
