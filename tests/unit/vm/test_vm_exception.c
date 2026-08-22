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
#include "xray.h"
#include "../test_helper.h"

#include "runtime/xisolate_internal.h"
#include "runtime/xisolate_api.h"
#include "module/xmodule_identity.h"
#include "runtime/object/xpanic_info.h"
#include "runtime/object/xarray.h"
#include "vm/xvm_internal.h"

#include <string.h>

static const XrModuleIdentityAuthority k_vm_exception_memory_authority = {
    .kind = XR_MODULE_IDENTITY_MEMORY,
    .namespace_id = "vm-exception-fixture-v1",
};

/* Helper: spin up a full-feature isolate with stderr suppression
 * (uncaught throws still update ctx->current_exception). */
static XrVMRuntime *make_quiet_isolate(void) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    if (!iso)
        return NULL;
    xr_isolate_set_suppress_exception_print(iso, true);
    return iso;
}

/* ========== User throw propagates through call chain via error channel ========== */

TEST(uncaught_enum_error_returns_nonzero) {
    XrVMRuntime *iso = make_quiet_isolate();
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

    int rc = xr_isolate_dostring(iso, src, &k_vm_exception_memory_authority);
    /* Uncaught error — dostring returns non-zero. */
    ASSERT(rc != 0);

    xray_vm_delete(iso);
}

/* ========== Runtime error (e.g. division by zero) carries a trace ========== */

TEST(runtime_error_records_trace) {
    XrVMRuntime *iso = make_quiet_isolate();
    ASSERT_NOT_NULL(iso);

    /* Force the interpreter's VM_RUNTIME_ERROR path via a divide
     * by zero. Division by zero is a PANIC (unrecoverable runtime
     * fault), not a value-return error: it uses the unwind channel
     * and is reported via current_exception, never pending_error. */
    const char *src = "fn divider(a: int, b: int) -> int { return a / b }\n"
                      "var r = divider(10, 0)\n";

    int rc = xr_isolate_dostring(iso, src, &k_vm_exception_memory_authority);
    /* Uncaught panic fail-fasts — dostring returns non-zero. */
    ASSERT(rc != 0);

    /* The panic value lives in the panic channel (current_exception),
     * and the value-return error channel stays clean. */
    XrVMContext *ctx = xr_vm_current_ctx(iso);
    ASSERT(XR_IS_NULL(ctx->pending_error));
    ASSERT(xr_value_is_panic_info(iso, ctx->current_exception));

    xray_vm_delete(iso);
}

TEST(map_getk_missing_key_throws_e0431) {
    XrVMRuntime *iso = make_quiet_isolate();
    ASSERT_NOT_NULL(iso);
    ASSERT_NOT_NULL(xr_test_init_coro(iso));

    XrString *present = xr_string_intern(iso, "present", 7, xr_string_hash("present", 7));
    XrString *missing = xr_string_intern(iso, "missing", 7, xr_string_hash("missing", 7));
    ASSERT_NOT_NULL(present);
    ASSERT_NOT_NULL(missing);

    XrMap *map = xr_map_new(xr_test_get_coro(iso));
    ASSERT_NOT_NULL(map);
    xr_map_set(map, xr_string_value(present), xr_int(7));

    XrProto *proto = xr_instruction_unit_new();
    ASSERT_NOT_NULL(proto);
    proto->source_file = "<test-op-map-getk-missing-key>";
    proto->maxstacksize = 4;

    int map_k = xr_instruction_unit_add_constant(proto, XR_FROM_PTR(map));
    int missing_k = xr_instruction_unit_add_constant(proto, xr_string_value(missing));
    ASSERT_EQ_INT(map_k, 0);
    ASSERT_EQ_INT(missing_k, 1);

    xr_instruction_unit_write(proto, CREATE_ABx(OP_LOADK, 0, (uint32_t) map_k), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(OP_MAP_GETK, 1, 0, (uint32_t) missing_k), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(OP_RETURN1, 1, 0, 0), 1);

    XrVMResult result = xr_vm_interpret_proto(iso, proto);
    ASSERT_EQ_INT(result, XR_VM_RUNTIME_ERROR);

    XrVMContext *ctx = xr_vm_current_ctx(iso);
    ASSERT(xr_value_is_panic_info(iso, ctx->current_exception));
    ASSERT_EQ_INT(xr_panic_info_get_code(iso, ctx->current_exception), XR_ERR_KEY_NOT_FOUND);
    ASSERT_STR_EQ(xr_panic_info_get_message(iso, ctx->current_exception), "Map key not found");

    xray_vm_delete(iso);
}

/* ========== Catch clears the pending error channel ========== */

TEST(catch_clears_pending_error_state) {
    XrVMRuntime *iso = make_quiet_isolate();
    ASSERT_NOT_NULL(iso);

    /* In the new model, catch reads and clears pending_error.
     * Verify that after a caught error, the program continues
     * normally without stale error state. */
    const char *src = "enum VmErr { Test }\n"
                      "var caught = false\n"
                      "try {\n"
                      "    throw VmErr.Test\n"
                      "} catch (e) {\n"
                      "    caught = true\n"
                      "}\n"
                      "assert(caught)\n"
                      "var x = 42\n"
                      "assert_eq(x, 42)\n"
                      "assert_ne(x, 41)\n";

    int rc = xr_isolate_dostring(iso, src, &k_vm_exception_memory_authority);
    ASSERT_EQ_INT(rc, 0);
    /* No error left dangling. */
    XrVMContext *ctx = xr_vm_current_ctx(iso);
    ASSERT(XR_IS_NULL(ctx->pending_error));

    xray_vm_delete(iso);
}

/* ========== Rethrow reaches the top level and is consumed ========== */

/*
 * Value-return errors do not use the panic trace channel. A caught enum error
 * that is re-thrown propagates to the top level, where the uncaught-error
 * report consumes it: dostring returns non-zero and the error channel is left
 * clean — matching the scheduler-backed roots (run_finalize) and the sibling
 * catch_clears_pending_error_state, so the isolate stays re-entrant.
 */
TEST(caught_error_rethrow_reaches_top_level) {
    XrVMRuntime *iso = make_quiet_isolate();
    ASSERT_NOT_NULL(iso);

    const char *src = "enum VmErr { Deep(string) }\n"
                      "fn deep() { throw VmErr.Deep(\"deep\") }\n"
                      "fn level2() { deep() }\n"
                      "fn level1() { level2() }\n"
                      "try { level1() } catch (e) { throw e }\n";

    int rc = xr_isolate_dostring(iso, src, &k_vm_exception_memory_authority);
    /* Rethrow propagated uncaught to the top level. */
    ASSERT(rc != 0);

    /* The top-level report consumed the error; no dangling channel state. */
    XrVMContext *ctx = xr_vm_current_ctx(iso);
    ASSERT(XR_IS_NULL(ctx->pending_error));

    xray_vm_delete(iso);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("Unified throw/unwind contract");
RUN_TEST(uncaught_enum_error_returns_nonzero);
RUN_TEST(runtime_error_records_trace);
RUN_TEST(map_getk_missing_key_throws_e0431);

RUN_TEST_SUITE("Catch state cleanup");
RUN_TEST(catch_clears_pending_error_state);

RUN_TEST_SUITE("Error rethrow surface");
RUN_TEST(caught_error_rethrow_reaches_top_level);
TEST_MAIN_END()
