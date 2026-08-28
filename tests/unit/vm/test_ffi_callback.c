/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_ffi_callback.c - VM libffi CFn callback bridge tests.
 */

#include "../test_framework.h"

#include "base/xmalloc.h"
#include "vm/xvm_ffi.h"
#include "runtime/closure/xclosure.h"
#include "runtime/value/xchunk.h"
#include "runtime/value/xffi_sig.h"
#include "runtime/core/xr_exec_context.h"
#include "runtime/core/xr_runtime_core.h"
#include "runtime/xisolate_internal.h"
#include "vm/xvm.h"
#include "xray_vm.h"

#include <stdint.h>
#include <stdlib.h>

static XrVMRuntime *new_test_isolate(void) {
    XrVMConfig params = {0};
    return xray_vm_new_full(&params);
}

static XrProto *make_zero_comparator_proto(void) {
    XrProto *proto = xr_instruction_unit_new();
    if (!proto)
        return NULL;
    proto->source_file = xr_strdup("<ffi-callback-test>");
    proto->numparams = 2;
    proto->min_params = 2;
    proto->maxstacksize = 3;
    xr_instruction_unit_write(proto, CREATE_AsBx(OP_LOADI, 2, 0), 1);
    xr_instruction_unit_write(proto, CREATE_ABC(OP_RETURN1, 2, 0, 0), 1);
    return proto;
}

static XrProto *make_bsearch_proto(void) {
    XrProto *proto = xr_instruction_unit_new();
    if (!proto)
        return NULL;
    proto->source_file = xr_strdup("<ffi-bsearch>");
    proto->is_extern = true;

    XrFFISig *sig = xr_ffi_sig_new("bsearch", NULL, 5);
    if (!sig) {
        xr_instruction_unit_free(proto);
        return NULL;
    }
    sig->params[0] = XR_FFI_T_PTR;
    sig->params[1] = XR_FFI_T_PTR;
    sig->params[2] = XR_FFI_T_U64;
    sig->params[3] = XR_FFI_T_U64;
    sig->params[4] = XR_FFI_T_PTR;
    sig->ret = XR_FFI_T_PTR;

    uint8_t cb_params[2] = {XR_FFI_T_PTR, XR_FFI_T_PTR};
    if (!xr_ffi_sig_set_param_callback_codes(sig, 4, cb_params, 2, XR_FFI_T_I32)) {
        xr_ffi_sig_free(sig);
        xr_instruction_unit_free(proto);
        return NULL;
    }

    proto->ffi_sig = sig;
    return proto;
}

static XrProto *make_extern_dispatch_root(OpCode call_opcode, uint8_t result_count) {
    XrProto *root = xr_instruction_unit_new();
    XrProto *foreign = xr_instruction_unit_new();
    if (!root || !foreign) {
        xr_instruction_unit_free(root);
        xr_instruction_unit_free(foreign);
        return NULL;
    }
    root->source_file = xr_strdup("<ffi-dispatch-root>");
    root->maxstacksize = 2;
    foreign->source_file = xr_strdup("<ffi-dispatch-foreign>");
    foreign->maxstacksize = 1;
    foreign->is_extern = true;
    if (!root->source_file || !foreign->source_file) {
        xr_instruction_unit_free(root);
        xr_instruction_unit_free(foreign);
        return NULL;
    }
    if (xr_instruction_unit_add_child(root, foreign) != 0) {
        xr_instruction_unit_free(root);
        xr_instruction_unit_free(foreign);
        return NULL;
    }
    xr_instruction_unit_write(root, CREATE_ABx(OP_CLOSURE, 0, 0), 1);
    xr_instruction_unit_write(root, CREATE_ABC(call_opcode, 0, 0, result_count), 1);
    xr_instruction_unit_write(root, CREATE_ABC(OP_RETURN0, 0, 0, 0), 1);
    return root;
}

TEST(vm_ffi_bsearch_invokes_xray_cfn_callback) {
#ifndef XRAY_HAVE_LIBFFI
    return;
#else
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);
    XrExecutionContext *previous = xr_exec_context_enter(xr_runtime_core_root_exec(iso->core_rt));

    XrProto *callback_proto = make_zero_comparator_proto();
    ASSERT_NOT_NULL(callback_proto);
    XrClosure *callback = xr_closure_new(iso, callback_proto, NULL);
    ASSERT_NOT_NULL(callback);

    XrProto *bsearch_proto = make_bsearch_proto();
    ASSERT_NOT_NULL(bsearch_proto);

    int key = 20;
    int values[3] = {10, 20, 30};
    XrValue args[5] = {
        xr_int((xr_Integer) (uintptr_t) &key),  xr_int((xr_Integer) (uintptr_t) values), xr_int(3),
        xr_int((xr_Integer) sizeof(values[0])), xr_value_from_closure(callback),
    };

    XrValue result = xr_null();
    ASSERT_EQ_INT(xr_ffi_call_proto(iso, bsearch_proto, args, 5, &result), XR_FFI_CALL_OK);
    ASSERT_TRUE(XR_IS_INT(result));

    uintptr_t found = (uintptr_t) XR_TO_INT(result);
    uintptr_t start = (uintptr_t) values;
    uintptr_t end = (uintptr_t) (values + 3);
    ASSERT_GE(found, start);
    ASSERT_LT(found, end);

    xr_instruction_unit_free(bsearch_proto);
    xr_instruction_unit_free(callback_proto);
    xr_exec_context_restore(previous);
    xray_vm_delete(iso);
#endif
}

TEST(vm_ffi_failure_never_commits_result_storage) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrValue result = xr_int(73);
    XrFfiCallStatus status = xr_ffi_call_proto(iso, NULL, NULL, 0, &result);
#ifndef XRAY_HAVE_LIBFFI
    ASSERT_EQ_INT(status, XR_FFI_CALL_PROVIDER_UNAVAILABLE);
#else
    ASSERT_EQ_INT(status, XR_FFI_CALL_FAILED);
#endif
    ASSERT_TRUE(XR_IS_INT(result));
    ASSERT_EQ_INT(XR_TO_INT(result), 73);
    ASSERT_EQ_INT(xr_ffi_call_proto(iso, NULL, NULL, 0, NULL), XR_FFI_CALL_FAILED);

    xray_vm_delete(iso);
}

static void assert_extern_dispatch_fails_closed(OpCode call_opcode, uint8_t result_count) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);
    XrProto *root = make_extern_dispatch_root(call_opcode, result_count);
    ASSERT_NOT_NULL(root);

    /* Interpreting a proto allocates its entry closure from the caller's
     * execution context before dispatch begins, so the logical root has to be
     * installed exactly as xr_execute does for an elided root. Without it the
     * allocation fails, the closure comes back NULL, and the interpreter
     * returns the error this case expects before reaching a single foreign
     * call - the assertion holds for the wrong reason. Restore before the
     * assertion: a failing assertion returns, and a leaked context would
     * follow into the next case. */
    XrExecutionContext *previous =
        xr_exec_context_enter(xr_runtime_core_root_exec(iso->core_rt));
    XrVMResult result = xr_vm_interpret_proto(iso, root);
    xr_exec_context_restore(previous);
    ASSERT_EQ_INT(result, XR_VM_RUNTIME_ERROR);

    xr_instruction_unit_free(root);
    xray_vm_delete(iso);
}

TEST(vm_ffi_normal_call_failure_stops_dispatch) {
    assert_extern_dispatch_fails_closed(OP_CALL, 1);
}

TEST(vm_ffi_tail_call_failure_stops_dispatch) {
    assert_extern_dispatch_fails_closed(OP_TAILCALL, 1);
}

TEST(vm_ffi_unused_void_call_failure_stops_dispatch) {
    assert_extern_dispatch_fails_closed(OP_CALL, 0);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("VM FFI callback bridge");
RUN_TEST(vm_ffi_bsearch_invokes_xray_cfn_callback);
RUN_TEST(vm_ffi_failure_never_commits_result_storage);
RUN_TEST(vm_ffi_normal_call_failure_stops_dispatch);
RUN_TEST(vm_ffi_tail_call_failure_stops_dispatch);
RUN_TEST(vm_ffi_unused_void_call_failure_stops_dispatch);
TEST_MAIN_END()
