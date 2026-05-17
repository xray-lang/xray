/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_vm_api.c - Lock down the unified src/vm entry contract:
 *     - xr_vm_current_ctx() is the single authoritative ctx resolver
 *     - xr_vm_prepare_entry() guarantees stack/frame capacity for entries
 *     - xr_vm_call_closure() / xr_vm_interpret_proto() never crash on
 *       large maxstacksize, deep recursion, vararg or NULL inputs.
 *
 *   These tests are intentionally low-level; they exercise the C-side
 *   entry path directly rather than going through xray_isolate_dostring.
 */

#include "../test_framework.h"
#include "xray_isolate.h"
#include "../test_helper.h"

#include <stddef.h>

/* ========== xr_vm_current_ctx contract ========== */

TEST(vm_current_ctx_returns_main_coro_ctx) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    ASSERT_NOT_NULL(iso);

    /* On a fresh isolate the main coroutine owns the canonical vm_ctx. */
    XrVMContext *ctx = xr_vm_current_ctx(iso);
    ASSERT_NOT_NULL(ctx);
    ASSERT_NOT_NULL(ctx->stack);
    ASSERT_NOT_NULL(ctx->frames);
    ASSERT_GT(ctx->stack_capacity, 0);
    ASSERT_GT(ctx->frame_capacity, 0);
    ASSERT_GE(ctx->stack_top, ctx->stack);
    ASSERT_LE(ctx->stack_top, ctx->stack + ctx->stack_capacity);
    ASSERT_GE(ctx->frame_count, 0);

    /* current_coro is non-NULL on the main path; this guarantees
     * prepare_entry can grow the backing storage on demand. */
    ASSERT_NOT_NULL(ctx->current_coro);

    xray_isolate_delete(iso);
}

/* ========== xr_vm_prepare_entry contract ========== */

TEST(vm_prepare_entry_within_capacity_is_noop) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    ASSERT_NOT_NULL(iso);

    XrVMContext *ctx = xr_vm_current_ctx(iso);
    int prev_cap = ctx->stack_capacity;
    int prev_fcap = ctx->frame_capacity;

    /* Asking for a tiny window that already fits must succeed without
     * touching backing storage. */
    bool ok = xr_vm_prepare_entry(ctx, 8);
    ASSERT_TRUE(ok);
    ASSERT_EQ_INT(ctx->stack_capacity, prev_cap);
    ASSERT_EQ_INT(ctx->frame_capacity, prev_fcap);

    xray_isolate_delete(iso);
}

TEST(vm_prepare_entry_grows_for_large_window) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    ASSERT_NOT_NULL(iso);

    XrVMContext *ctx = xr_vm_current_ctx(iso);
    int prev_cap = ctx->stack_capacity;

    /* Request a window strictly larger than current capacity to force
     * a grow path. */
    int huge = prev_cap + 4096;
    bool ok = xr_vm_prepare_entry(ctx, huge);
    ASSERT_TRUE(ok);
    /* Capacity must have grown — exact amount is xr_coro_grow_stack policy
     * (stack_capacity + extra_slots), so we only assert strict growth. */
    ASSERT_GT(ctx->stack_capacity, prev_cap);

    /* Pointers must remain consistent post-grow. */
    ASSERT_NOT_NULL(ctx->stack);
    ASSERT_GE(ctx->stack_top, ctx->stack);
    ASSERT_LE(ctx->stack_top, ctx->stack + ctx->stack_capacity);

    xray_isolate_delete(iso);
}

TEST(vm_prepare_entry_zero_extra_succeeds) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    ASSERT_NOT_NULL(iso);

    XrVMContext *ctx = xr_vm_current_ctx(iso);
    /* extra_stack=0 is a valid no-op when frames also have headroom. */
    bool ok = xr_vm_prepare_entry(ctx, 0);
    ASSERT_TRUE(ok);

    xray_isolate_delete(iso);
}

/* ========== xr_vm_call_closure NULL safety ========== */

TEST(vm_call_closure_null_closure_returns_null) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    ASSERT_NOT_NULL(iso);

    /* NULL closure must short-circuit to xr_null without crashing. */
    XrValue r = xr_vm_call_closure(iso, NULL, NULL, 0);
    ASSERT_TRUE(XR_IS_NULL(r));

    xray_isolate_delete(iso);
}

/* ========== xr_vm_interpret_proto NULL safety (Debug-aware) ========== */

#ifdef NDEBUG
TEST(vm_interpret_proto_null_proto_returns_error) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    ASSERT_NOT_NULL(iso);

    /* In Release the DCHECK is a no-op; entry must still surface an error
     * rather than dereferencing the NULL proto. */
    XrVMResult r = xr_vm_interpret_proto(iso, NULL);
    ASSERT_NE(r, XR_VM_OK);

    xray_isolate_delete(iso);
}
#endif

/* ========== End-to-end: deep recursion exercises grow path ========== */

TEST(vm_deep_recursion_via_dostring) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    ASSERT_NOT_NULL(iso);

    /* 200 levels of recursion: well below stack overflow threshold but
     * deep enough to exercise xr_coro_grow_stack in run(). prepare_entry
     * must keep entry frame and grow frame array consistently. */
    const char *src = "fn dive(n: int): int {\n"
                      "  if (n <= 0) { return 0; }\n"
                      "  return dive(n - 1) + 1;\n"
                      "}\n"
                      "let r = dive(200);\n"
                      "if (r != 200) { throw \"wrong recursion result\"; }\n";

    int rc = xray_isolate_dostring(iso, src);
    ASSERT_EQ_INT(rc, 0);

    xray_isolate_delete(iso);
}

/* ========== End-to-end: large maxstacksize entry ========== */

TEST(vm_large_maxstacksize_entry) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    ASSERT_NOT_NULL(iso);

    /* Many local variables push proto->maxstacksize past the 128-slot
     * default coroutine stack, forcing prepare_entry to grow before
     * the first instruction runs. */
    const char *src = "fn wide(): int {\n"
                      "  let a01 = 1; let a02 = 2; let a03 = 3; let a04 = 4;\n"
                      "  let a05 = 5; let a06 = 6; let a07 = 7; let a08 = 8;\n"
                      "  let a09 = 9; let a10 = 10; let a11 = 11; let a12 = 12;\n"
                      "  let a13 = 13; let a14 = 14; let a15 = 15; let a16 = 16;\n"
                      "  let a17 = 17; let a18 = 18; let a19 = 19; let a20 = 20;\n"
                      "  let a21 = 21; let a22 = 22; let a23 = 23; let a24 = 24;\n"
                      "  let a25 = 25; let a26 = 26; let a27 = 27; let a28 = 28;\n"
                      "  let a29 = 29; let a30 = 30; let a31 = 31; let a32 = 32;\n"
                      "  return a01 + a32;\n"
                      "}\n"
                      "let r = wide();\n"
                      "if (r != 33) { throw \"wide failed\"; }\n";

    int rc = xray_isolate_dostring(iso, src);
    ASSERT_EQ_INT(rc, 0);

    xray_isolate_delete(iso);
}

/* ========== End-to-end: vararg entry ========== */

TEST(vm_vararg_entry) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    ASSERT_NOT_NULL(iso);

    /* Xray rest-param syntax: ...nums (no type annotation on rest param).
     * Exercises the vararg branch of xr_vm_call_closure. */
    const char *src = "fn sumAll(...nums): int {\n"
                      "  let total = 0\n"
                      "  for (let i = 0; i < nums.length; i = i + 1) {\n"
                      "    total = total + nums[i]\n"
                      "  }\n"
                      "  return total\n"
                      "}\n"
                      "let r = sumAll(1, 2, 3, 4, 5)\n"
                      "if (r != 15) { throw \"vararg failed\" }\n";

    int rc = xray_isolate_dostring(iso, src);
    ASSERT_EQ_INT(rc, 0);

    xray_isolate_delete(iso);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("xr_vm_current_ctx contract");
RUN_TEST(vm_current_ctx_returns_main_coro_ctx);

RUN_TEST_SUITE("xr_vm_prepare_entry contract");
RUN_TEST(vm_prepare_entry_within_capacity_is_noop);
RUN_TEST(vm_prepare_entry_grows_for_large_window);
RUN_TEST(vm_prepare_entry_zero_extra_succeeds);

RUN_TEST_SUITE("Public entry NULL safety");
RUN_TEST(vm_call_closure_null_closure_returns_null);
#ifdef NDEBUG
RUN_TEST(vm_interpret_proto_null_proto_returns_error);
#endif

RUN_TEST_SUITE("End-to-end entry path");
RUN_TEST(vm_deep_recursion_via_dostring);
RUN_TEST(vm_large_maxstacksize_entry);
RUN_TEST(vm_vararg_entry);
TEST_MAIN_END()
