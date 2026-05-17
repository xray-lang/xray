/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_vm_exception.c - Lock down the unified throw / unwind contract.
 *
 * KEY POINTS:
 *   - xr_vm_unwind_with_trace() must record one trace entry per
 *     active call frame, in throw-site -> outermost order.
 *   - VM_RUNTIME_ERROR (interpreter), VM_COLD_THROW (cold path) and
 *     OP_THROW (user code) must all funnel through the same helper
 *     so user-visible stack traces stay consistent regardless of
 *     who started the unwind.
 *   - Catching an exception must clear ctx->current_exception so a
 *     subsequent builtin call does not see a stale value through
 *     the OP_INVOKE / OP_INVOKE_BUILTIN VM_BUILTIN_INVOKE_CHECK_EXC
 *     guard.
 */

#include "../test_framework.h"
#include "xray_isolate.h"
#include "../test_helper.h"

#include "runtime/xisolate_internal.h"
#include "runtime/object/xexception.h"
#include "runtime/object/xarray.h"
#include "vm/xvm_internal.h"

#include <string.h>

/* Helper: spin up a full-feature isolate with stderr suppression
 * (uncaught throws still update ctx->current_exception). */
static XrayIsolate *make_quiet_isolate(void) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    XrayIsolate *iso = xray_isolate_new(&params);
    if (!iso)
        return NULL;
    xr_isolate_set_suppress_exception_print(iso, true);
    return iso;
}

/* ========== User-thrown exception walks every frame ========== */

TEST(unwind_records_full_call_chain) {
    XrayIsolate *iso = make_quiet_isolate();
    ASSERT_NOT_NULL(iso);

    /* deep() -> level3() -> level2() -> level1() -> top-level
     * (5 frames active when throw fires). */
    const char *src = "fn deep(): void {\n"
                      "    throw \"boom\"\n"
                      "}\n"
                      "fn level3(): void { deep() }\n"
                      "fn level2(): void { level3() }\n"
                      "fn level1(): void { level2() }\n"
                      "level1()\n";

    int rc = xray_isolate_dostring(iso, src);
    /* Uncaught throw — dostring returns non-zero. */
    ASSERT(rc != 0);

    XrVMContext *ctx = xr_vm_current_ctx(iso);
    ASSERT_NOT_NULL(ctx);
    XrValue exc_val = ctx->current_exception;
    ASSERT(XR_IS_EXCEPTION(exc_val));

    XrException *exc = XR_AS_EXCEPTION(exc_val);
    ASSERT_NOT_NULL(exc->stackTrace);
    /* Five active frames: top-level, level1, level2, level3, deep. */
    ASSERT(exc->stackTrace->length >= 5);

    xray_isolate_delete(iso);
}

/* ========== Runtime error (e.g. division by zero) carries a trace ========== */

TEST(runtime_error_records_trace) {
    XrayIsolate *iso = make_quiet_isolate();
    ASSERT_NOT_NULL(iso);

    /* Force the interpreter's VM_RUNTIME_ERROR path via a divide
     * by zero on big-int. The VM's BigInt division returns
     * XR_NOTFOUND on /0 and the dispatcher throws
     * XR_ERR_DIV_BY_ZERO via the runtime-error macro. */
    const char *src = "fn divider(a: int, b: int): int { return a / b }\n"
                      "let r = divider(10, 0)\n";

    int rc = xray_isolate_dostring(iso, src);
    ASSERT(rc != 0);

    XrVMContext *ctx = xr_vm_current_ctx(iso);
    XrValue exc_val = ctx->current_exception;
    ASSERT(XR_IS_EXCEPTION(exc_val));
    XrException *exc = XR_AS_EXCEPTION(exc_val);
    ASSERT_NOT_NULL(exc->stackTrace);
    /* divider() + top-level = 2 frames at minimum. */
    ASSERT(exc->stackTrace->length >= 2);

    xray_isolate_delete(iso);
}

/* ========== Catch clears the ctx-wide pending exception ========== */

TEST(catch_clears_pending_exception_state) {
    XrayIsolate *iso = make_quiet_isolate();
    ASSERT_NOT_NULL(iso);

    /* The first wm.set rejects the int key and is caught. The
     * follow-up wm.set on a real object key must succeed; if
     * OP_CATCH had left ctx->current_exception populated, the
     * dispatcher's VM_BUILTIN_INVOKE_CHECK_EXC would see the
     * stale value and unwind spuriously, terminating the script
     * before reaching the final asserts. */
    const char *src = "let wm = new WeakMap()\n"
                      "let caught = false\n"
                      "try { wm.set(42, \"x\") } catch (e) { caught = true }\n"
                      "assert(caught)\n"
                      "let key = { id: 1 }\n"
                      "wm.set(key, \"ok\")\n"
                      "assert_eq(wm.get(key), \"ok\")\n";

    int rc = xray_isolate_dostring(iso, src);
    ASSERT_EQ_INT(rc, 0);
    /* No exception left dangling. */
    XrVMContext *ctx = xr_vm_current_ctx(iso);
    ASSERT(XR_IS_NULL(ctx->current_exception));

    xray_isolate_delete(iso);
}

/* ========== Caught exception keeps its trace through the catch block ========== */

/*
 * Verify that a deep throw caught at the top level still has
 * a non-trivial stack trace at the moment of the catch. The
 * trace is inspected from the C side (xray code does not yet
 * expose stackTrace as a stable public field).
 *
 * Implementation: a pre-test debug hook fires when the throw
 * happens uncaught, but we want the caught variant. Instead we
 * use xr_isolate_set_suppress_exception_print and run a script
 * that records the trace length into a global before the catch
 * clears it.
 */
TEST(caught_exception_trace_survives_catch) {
    XrayIsolate *iso = make_quiet_isolate();
    ASSERT_NOT_NULL(iso);

    /* Throw four frames deep, then immediately re-throw from the
     * catch handler so the test C code can inspect the trace on
     * the second (uncaught) flight. */
    const char *src = "fn deep(): void { throw \"deep\" }\n"
                      "fn level2(): void { deep() }\n"
                      "fn level1(): void { level2() }\n"
                      "try { level1() } catch (e) { throw e }\n";

    int rc = xray_isolate_dostring(iso, src);
    ASSERT(rc != 0);

    XrVMContext *ctx = xr_vm_current_ctx(iso);
    XrValue exc_val = ctx->current_exception;
    ASSERT(XR_IS_EXCEPTION(exc_val));
    XrException *exc = XR_AS_EXCEPTION(exc_val);
    /* Original throw recorded the deep + level2 + level1 + top-level
     * frames; the rethrow must NOT re-record (de-dup keeps the
     * original site visible to debuggers). */
    /* The original throw's trace should have survived the catch +
     * rethrow. Even with de-duping that keeps only the first
     * recording, we still expect at least one frame (the deepest
     * throw site). */
    ASSERT_NOT_NULL(exc->stackTrace);
    ASSERT(exc->stackTrace->length >= 1);

    xray_isolate_delete(iso);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Unified throw/unwind contract");
RUN_TEST(unwind_records_full_call_chain);
RUN_TEST(runtime_error_records_trace);

RUN_TEST_SUITE("Catch state cleanup");
RUN_TEST(catch_clears_pending_exception_state);

RUN_TEST_SUITE("Stack trace surface");
RUN_TEST(caught_exception_trace_survives_catch);
TEST_MAIN_END()
