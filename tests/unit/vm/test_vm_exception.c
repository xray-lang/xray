/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_vm_exception.c - Lock down the error / panic contract.
 *
 * KEY POINTS:
 *   - VM_RUNTIME_ERROR (interpreter), VM_COLD_THROW (cold path) and
 *     OP_THROW (panic channel) must all funnel through the same helper
 *     so user-visible stack traces stay consistent regardless of
 *     who started the unwind.
 *   - Catching a source-level enum error must clear pending_error.
 */

#include "../test_framework.h"
#include "xray_isolate.h"
#include "../test_helper.h"

#include "runtime/xisolate_internal.h"
#include "runtime/object/xpanic_info.h"
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

/* ========== User throw propagates through call chain via error channel ========== */

TEST(uncaught_enum_error_returns_nonzero) {
    XrayIsolate *iso = make_quiet_isolate();
    ASSERT_NOT_NULL(iso);

    /* In the new error model, throw writes to pending_error and returns.
     * Each caller sees the error and also returns (auto-propagation).
     * The error value ends up in pending_error at the top level. */
    const char *src = "enum VmErr { Boom(string) }\n"
                      "fn deep() {\n"
                      "    throw VmErr.Boom(\"boom\")\n"
                      "}\n"
                      "fn level3() { deep() }\n"
                      "fn level2() { level3() }\n"
                      "fn level1() { level2() }\n"
                      "level1()\n";

    int rc = xray_isolate_dostring(iso, src);
    /* Uncaught error — dostring returns non-zero. */
    ASSERT(rc != 0);

    xray_isolate_delete(iso);
}

/* ========== Runtime error (e.g. division by zero) carries a trace ========== */

TEST(runtime_error_records_trace) {
    XrayIsolate *iso = make_quiet_isolate();
    ASSERT_NOT_NULL(iso);

    /* Force the interpreter's VM_RUNTIME_ERROR path via a divide
     * by zero. Division by zero is a PANIC (unrecoverable runtime
     * fault), not a value-return error: it uses the unwind channel
     * and is reported via current_exception, never pending_error. */
    const char *src = "fn divider(a: int, b: int) -> int { return a / b }\n"
                      "let r = divider(10, 0)\n";

    int rc = xray_isolate_dostring(iso, src);
    /* Uncaught panic fail-fasts — dostring returns non-zero. */
    ASSERT(rc != 0);

    /* The panic value lives in the panic channel (current_exception),
     * and the value-return error channel stays clean. */
    XrVMContext *ctx = xr_vm_current_ctx(iso);
    ASSERT(XR_IS_NULL(ctx->pending_error));
    ASSERT(xr_value_is_panic_info(iso, ctx->current_exception));

    xray_isolate_delete(iso);
}

/* ========== Catch clears the pending error channel ========== */

TEST(catch_clears_pending_error_state) {
    XrayIsolate *iso = make_quiet_isolate();
    ASSERT_NOT_NULL(iso);

    /* In the new model, catch reads and clears pending_error.
     * Verify that after a caught error, the program continues
     * normally without stale error state. */
    const char *src = "enum VmErr { Test }\n"
                      "let caught = false\n"
                      "try {\n"
                      "    throw VmErr.Test\n"
                      "} catch (e) {\n"
                      "    caught = true\n"
                      "}\n"
                      "assert(caught)\n"
                      "let x = 42\n"
                      "assert_eq(x, 42)\n";

    int rc = xray_isolate_dostring(iso, src);
    ASSERT_EQ_INT(rc, 0);
    /* No error left dangling. */
    XrVMContext *ctx = xr_vm_current_ctx(iso);
    ASSERT(XR_IS_NULL(ctx->pending_error));

    xray_isolate_delete(iso);
}

/* ========== Caught enum error can be rethrown ========== */

/*
 * Value-return errors do not use the panic trace channel. Verify that
 * catch + rethrow preserves the enum error and leaves it pending at top
 * level.
 */
TEST(caught_error_rethrow_preserves_pending_error) {
    XrayIsolate *iso = make_quiet_isolate();
    ASSERT_NOT_NULL(iso);

    /* In the new model, throw + catch + re-throw all go through
     * the value-return error channel.  Verify the error value
     * survives catch and re-throw. */
    const char *src = "enum VmErr { Deep(string) }\n"
                      "fn deep() { throw VmErr.Deep(\"deep\") }\n"
                      "fn level2() { deep() }\n"
                      "fn level1() { level2() }\n"
                      "try { level1() } catch (e) { throw e }\n";

    int rc = xray_isolate_dostring(iso, src);
    ASSERT(rc != 0);

    /* Error propagated to top level via pending_error */
    XrVMContext *ctx = xr_vm_current_ctx(iso);
    ASSERT(!XR_IS_NULL(ctx->pending_error));

    xray_isolate_delete(iso);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Unified throw/unwind contract");
RUN_TEST(uncaught_enum_error_returns_nonzero);
RUN_TEST(runtime_error_records_trace);

RUN_TEST_SUITE("Catch state cleanup");
RUN_TEST(catch_clears_pending_error_state);

RUN_TEST_SUITE("Error rethrow surface");
RUN_TEST(caught_error_rethrow_preserves_pending_error);
TEST_MAIN_END()
