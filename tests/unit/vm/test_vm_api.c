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
 *   entry path directly rather than going through xr_isolate_dostring.
 */

#include "../test_framework.h"
#include "xray_vm.h"
#include "runtime/xisolate_api.h"
#include "module/xmodule_identity.h"
#include "runtime/class/xinstance.h"
#include "runtime/object/xarray.h"
#include "runtime/object/xstring.h"
#include "base/xglobal_indices.h"
#include "vm/xvm.h"
#include "../test_helper.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static const XrModuleIdentityAuthority k_vm_api_memory_authority = {
    .kind = XR_MODULE_IDENTITY_MEMORY,
    .namespace_id = "vm-api-fixture-v1",
};

/* ========== xr_vm_current_ctx contract ========== */

TEST(vm_current_ctx_returns_elided_root_ctx) {
    char *script_argv[] = {"alpha", "beta"};
    XrVMConfig params = {0};
    params.script_file = "script_identity.xr";
    params.script_argc = 2;
    params.script_argv = script_argv;
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    /* A fresh isolate has a stable logical root context without a physical
     * coroutine. Scheduler-backed execution may materialize one later. */
    XrVMContext *ctx = xr_vm_current_ctx(iso);
    ASSERT_NOT_NULL(ctx);
    ASSERT_NOT_NULL(ctx->stack);
    ASSERT_NOT_NULL(ctx->frames);
    ASSERT_GT(ctx->stack_capacity, 0);
    ASSERT_GT(ctx->frame_capacity, 0);
    ASSERT_GE(ctx->stack_top, ctx->stack);
    ASSERT_LE(ctx->stack_top, ctx->stack + ctx->stack_capacity);
    ASSERT_GE(ctx->frame_count, 0);

    ASSERT_NULL(ctx->current_coro);
    ASSERT_NULL(iso->main_coro);
    ASSERT_STR_EQ(xr_isolate_get_script_file(iso), "script_identity.xr");
    ASSERT_EQ_INT(xr_isolate_get_script_argc(iso), 2);
    ASSERT_EQ_PTR(xr_isolate_get_script_argv(iso), script_argv);

    XrValue file = iso->vm.builtins[XR_GLOBAL_VAR_FILE];
    ASSERT_TRUE(XR_IS_STRING(file));
    const char *materialized_file = XR_STRING_CHARS(XR_TO_STRING(file));
    const char *file_tail = "script_identity.xr";
    ASSERT_GE(strlen(materialized_file), strlen(file_tail));
    ASSERT_STR_EQ(materialized_file + strlen(materialized_file) - strlen(file_tail), file_tail);

    XrValue process_value = iso->vm.builtins[XR_GLOBAL_VAR_PROCESS];
    ASSERT_TRUE(XR_IS_INSTANCE(process_value));
    XrInstance *process = xr_value_to_instance(process_value);
    ASSERT_NOT_NULL(process);
    XrValue process_file = xr_instance_get_field_fast(process, PROCESS_FIELD_FILE);
    ASSERT_TRUE(XR_IS_STRING(process_file));
    ASSERT_EQ_PTR(XR_TO_STRING(process_file), XR_TO_STRING(file));

    XrValue process_args = xr_instance_get_field_fast(process, PROCESS_FIELD_ARGS);
    ASSERT_TRUE(XR_IS_ARRAY(process_args));
    XrArray *args = XR_TO_ARRAY(process_args);
    ASSERT_EQ_INT(args->length, 2);
    XrValue *values = XR_ARRAY_DATA_AS(args, XrValue);
    ASSERT_TRUE(XR_IS_STRING(values[0]));
    ASSERT_TRUE(XR_IS_STRING(values[1]));
    ASSERT_STR_EQ(XR_STRING_CHARS(XR_TO_STRING(values[0])), "alpha");
    ASSERT_STR_EQ(XR_STRING_CHARS(XR_TO_STRING(values[1])), "beta");
    xray_vm_delete(iso);

    ASSERT_NULL(xray_vm_new_full(NULL));
    params.script_argc = 1;
    params.script_argv = NULL;
    ASSERT_NULL(xray_vm_new_full(&params));
}

TEST(vm_elided_root_allocates_without_task_identity) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);
    ASSERT_NULL(xr_current_coro(iso));

    const char *src = "import text\n"
                      "var xs = [1, 2, 3]\n"
                      "var ys = xs.map(fn(x) { return x * 2 })\n"
                      "var upper = text.upper(\"root\")\n";
    ASSERT_EQ_INT(xr_isolate_dostring(iso, src, &k_vm_api_memory_authority), 0);
    ASSERT_NULL(iso->main_coro);
    ASSERT_NULL(xr_current_coro(iso));

    xray_vm_delete(iso);
}

/* ========== xr_vm_prepare_entry contract ========== */

TEST(vm_prepare_entry_within_capacity_is_noop) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
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

    xray_vm_delete(iso);
}

TEST(vm_prepare_entry_rejects_elided_root_overflow) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    XrVMContext *ctx = xr_vm_current_ctx(iso);
    int prev_cap = ctx->stack_capacity;

    /* Request a window strictly larger than current capacity to force
     * a grow path. */
    int huge = prev_cap + 4096;
    bool ok = xr_vm_prepare_entry(ctx, huge);
    ASSERT_FALSE(ok);
    ASSERT_EQ_INT(ctx->stack_capacity, prev_cap);
    ASSERT_NOT_NULL(ctx->stack);
    ASSERT_GE(ctx->stack_top, ctx->stack);
    ASSERT_LE(ctx->stack_top, ctx->stack + ctx->stack_capacity);

    xray_vm_delete(iso);
}

TEST(vm_prepare_entry_zero_extra_succeeds) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    XrVMContext *ctx = xr_vm_current_ctx(iso);
    /* extra_stack=0 is a valid no-op when frames also have headroom. */
    bool ok = xr_vm_prepare_entry(ctx, 0);
    ASSERT_TRUE(ok);

    xray_vm_delete(iso);
}

/* ========== xr_vm_call_closure NULL safety ========== */

TEST(vm_call_closure_null_closure_returns_null) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    /* NULL closure must short-circuit to xr_null without crashing. */
    XrValue r = xr_vm_call_closure(iso, NULL, NULL, 0);
    ASSERT_TRUE(XR_IS_NULL(r));

    xray_vm_delete(iso);
}

/* ========== xr_vm_interpret_proto NULL safety (Debug-aware) ========== */

#ifdef NDEBUG
TEST(vm_interpret_proto_null_proto_returns_error) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    /* In Release the DCHECK is a no-op; entry must still surface an error
     * rather than dereferencing the NULL proto. */
    XrVMResult r = xr_vm_interpret_proto(iso, NULL);
    ASSERT_NE(r, XR_VM_OK);

    xray_vm_delete(iso);
}
#endif

TEST(vm_bind_proto_shared_slots_is_vm_owned) {
    XrVMConfig params = {0};
    XrVMRuntime *iso1 = xray_vm_new_full(&params);
    XrVMRuntime *iso2 = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso1);
    ASSERT_NOT_NULL(iso2);

    XrProto *proto = xr_instruction_unit_new();
    ASSERT_NOT_NULL(proto);
    proto->shared_count = 2;

    ASSERT_TRUE(xr_vm_bind_proto_shared_slots(iso1, proto));
    ASSERT_TRUE(proto->shared_slots_bound);
    ASSERT_EQ_PTR(proto->shared_slots_owner, iso1);
    ASSERT_EQ_INT(proto->shared_offset, 0);
    ASSERT_TRUE(xr_vm_bind_proto_shared_slots(iso1, proto));
    ASSERT_FALSE(xr_vm_bind_proto_shared_slots(iso2, proto));

    xr_instruction_unit_free(proto);
    xray_vm_delete(iso2);
    xray_vm_delete(iso1);
}

/* ========== End-to-end: deep recursion exercises grow path ========== */

TEST(vm_deep_recursion_via_dostring) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    /* 200 levels of recursion: well below stack overflow threshold but
     * deep enough to exercise xr_coro_grow_stack in run(). prepare_entry
     * must keep entry frame and grow frame array consistently. */
    const char *src = "enum VmApiErr { CheckFailed }\n"
                      "fn dive(n: i64) -> i64 {\n"
                      "  if (n <= 0) { return 0; }\n"
                      "  return dive(n - 1) + 1;\n"
                      "}\n"
                      "var r = dive(200);\n"
                      "if (r != 200) { throw VmApiErr.CheckFailed; }\n";

    int rc = xr_isolate_dostring(iso, src, &k_vm_api_memory_authority);
    ASSERT_EQ_INT(rc, 0);

    xray_vm_delete(iso);
}

/* ========== End-to-end: large maxstacksize entry ========== */

TEST(vm_large_maxstacksize_entry) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    /* Many local variables push proto->maxstacksize past the 128-slot
     * default coroutine stack, forcing prepare_entry to grow before
     * the first instruction runs. */
    const char *src = "enum VmApiErr { CheckFailed }\n"
                      "fn wide() -> i64 {\n"
                      "  var a01 = 1; var a02 = 2; var a03 = 3; var a04 = 4;\n"
                      "  var a05 = 5; var a06 = 6; var a07 = 7; var a08 = 8;\n"
                      "  var a09 = 9; var a10 = 10; var a11 = 11; var a12 = 12;\n"
                      "  var a13 = 13; var a14 = 14; var a15 = 15; var a16 = 16;\n"
                      "  var a17 = 17; var a18 = 18; var a19 = 19; var a20 = 20;\n"
                      "  var a21 = 21; var a22 = 22; var a23 = 23; var a24 = 24;\n"
                      "  var a25 = 25; var a26 = 26; var a27 = 27; var a28 = 28;\n"
                      "  var a29 = 29; var a30 = 30; var a31 = 31; var a32 = 32;\n"
                      "  return a01 + a32;\n"
                      "}\n"
                      "var r = wide();\n"
                      "if (r != 33) { throw VmApiErr.CheckFailed; }\n";

    int rc = xr_isolate_dostring(iso, src, &k_vm_api_memory_authority);
    ASSERT_EQ_INT(rc, 0);

    xray_vm_delete(iso);
}

/* ========== End-to-end: vararg entry ========== */

TEST(vm_vararg_entry) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    /* Xray rest-param syntax: ...nums (no type annotation on rest param).
     * Exercises the vararg branch of xr_vm_call_closure. */
    const char *src = "enum VmApiErr { CheckFailed }\n"
                      "fn sumAll(...nums) -> i64 {\n"
                      "  var total = 0\n"
                      "  for (var i = 0; i < len(nums); i = i + 1) {\n"
                      "    total = total + nums[i]\n"
                      "  }\n"
                      "  return total\n"
                      "}\n"
                      "var r = sumAll(1, 2, 3, 4, 5)\n"
                      "if (r != 15) { throw VmApiErr.CheckFailed }\n";

    int rc = xr_isolate_dostring(iso, src, &k_vm_api_memory_authority);
    ASSERT_EQ_INT(rc, 0);

    xray_vm_delete(iso);
}

TEST(vm_enum_ordinal_conversion_uses_packed_typed_mode) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    const char *src = "enum VmApiErr { CheckFailed }\n"
                      "enum VmOrdinal { Idle, Ready }\n"
                      "var ready: VmOrdinal = VmOrdinal.Ready\n"
                      "var compact: u8 = ready as u8\n"
                      "var values: Array<VmOrdinal> = [ready, ready]\n"
                      "var boxed: i64 = values[1] as i64\n"
                      "if (compact != 1 || boxed != 1) { throw VmApiErr.CheckFailed }\n";
    int rc = xr_isolate_dostring(iso, src, &k_vm_api_memory_authority);
    ASSERT_EQ_INT(rc, 0);

    xray_vm_delete(iso);
}

static void assert_malformed_enum_conversion_mode_fails_closed(OpCode opcode) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    XrProto *proto = xr_instruction_unit_new();
    ASSERT_NOT_NULL(proto);
    proto->source_file = xr_strdup("<enum-ordinal-malformed-mode>");
    ASSERT_NOT_NULL(proto->source_file);
    proto->maxstacksize = 2;
    xr_instruction_unit_write(proto, CREATE_AsBx(OP_LOADI, 1, 1), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(opcode, 0, 1, UINT16_C(0x0800)), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(OP_RETURN1, 0, 0, 0), 1);

    bool saved_suppression = xr_isolate_get_suppress_exception_print(iso);
    xr_isolate_set_suppress_exception_print(iso, true);
    XrExecutionContext *previous = xr_exec_context_enter(xr_runtime_core_root_exec(iso->core_rt));
    XrVMResult result = xr_vm_interpret_proto(iso, proto);
    xr_exec_context_restore(previous);
    xr_isolate_set_suppress_exception_print(iso, saved_suppression);

    ASSERT_EQ_INT(result, XR_VM_RUNTIME_ERROR);
    xr_instruction_unit_free(proto);
    xray_vm_delete(iso);
}

static void assert_enum_mode_rejects_non_enum_value(OpCode opcode) {
    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    XrProto *proto = xr_instruction_unit_new();
    ASSERT_NOT_NULL(proto);
    proto->source_file = xr_strdup("<enum-ordinal-non-enum>");
    ASSERT_NOT_NULL(proto->source_file);
    proto->maxstacksize = 2;
    xr_instruction_unit_write(proto, CREATE_AsBx(OP_LOADI, 1, 1), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(opcode, 0, 1, XR_CONVERSION_BC_ENUM_ORDINAL), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(OP_RETURN1, 0, 0, 0), 1);

    bool saved_suppression = xr_isolate_get_suppress_exception_print(iso);
    xr_isolate_set_suppress_exception_print(iso, true);
    XrExecutionContext *previous = xr_exec_context_enter(xr_runtime_core_root_exec(iso->core_rt));
    XrVMResult result = xr_vm_interpret_proto(iso, proto);
    xr_exec_context_restore(previous);
    xr_isolate_set_suppress_exception_print(iso, saved_suppression);

    ASSERT_EQ_INT(result, XR_VM_RUNTIME_ERROR);
    xr_instruction_unit_free(proto);
    xray_vm_delete(iso);
}

TEST(vm_enum_ordinal_malformed_modes_fail_closed) {
    assert_malformed_enum_conversion_mode_fails_closed(OP_TOINT);
    assert_malformed_enum_conversion_mode_fails_closed(OP_TOFLOAT);
    assert_enum_mode_rejects_non_enum_value(OP_TOINT);
}

TEST(vm_dofile_debug_null_out_proto_releases_proto) {
    char path[] = "/tmp/xray_vm_debug_null_XXXXXX";
    int fd = xr_test_mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    FILE *f = xr_test_fdopen(fd, "w");
    ASSERT_NOT_NULL(f);
    fputs("enum DebugErr { Bad(s: string) }\nvar answer = 40 + 2\nif (answer != 42) { throw "
          "DebugErr.Bad(\"bad\") }\n",
          f);
    fclose(f);

    XrVMConfig params = {0};
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    XrModuleIdentityAuthority authority = {0};
    char *authority_root = NULL;
    ASSERT_TRUE(xr_module_identity_script_authority_from_source(path, &authority,
                                                                 &authority_root));
    int rc = xr_isolate_dofile_debug(iso, path, &authority, NULL);
    xr_free(authority_root);
    ASSERT_EQ_INT(rc, 0);

    xray_vm_delete(iso);
    xr_test_unlink(path);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("xr_vm_current_ctx contract");
RUN_TEST(vm_current_ctx_returns_elided_root_ctx);
RUN_TEST(vm_elided_root_allocates_without_task_identity);

RUN_TEST_SUITE("xr_vm_prepare_entry contract");
RUN_TEST(vm_prepare_entry_within_capacity_is_noop);
RUN_TEST(vm_prepare_entry_rejects_elided_root_overflow);
RUN_TEST(vm_prepare_entry_zero_extra_succeeds);

RUN_TEST_SUITE("Public entry NULL safety");
RUN_TEST(vm_call_closure_null_closure_returns_null);
#ifdef NDEBUG
RUN_TEST(vm_interpret_proto_null_proto_returns_error);
#endif
RUN_TEST(vm_bind_proto_shared_slots_is_vm_owned);

RUN_TEST_SUITE("End-to-end entry path");
RUN_TEST(vm_deep_recursion_via_dostring);
RUN_TEST(vm_large_maxstacksize_entry);
RUN_TEST(vm_vararg_entry);
RUN_TEST(vm_enum_ordinal_conversion_uses_packed_typed_mode);
RUN_TEST(vm_enum_ordinal_malformed_modes_fail_closed);
RUN_TEST(vm_dofile_debug_null_out_proto_releases_proto);
TEST_MAIN_END()
