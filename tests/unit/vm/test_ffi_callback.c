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
#include "xray_vm.h"

#include <stdint.h>
#include <stdlib.h>

static XrVMRuntime *new_test_isolate(void) {
    XrVMConfig params;
    xray_vm_config_init(&params);
    return xray_vm_new_full(&params);
}

static XrProto *make_zero_comparator_proto(void) {
    XrProto *proto = xr_vm_proto_new();
    if (!proto)
        return NULL;
    proto->source_file = xr_strdup("<ffi-callback-test>");
    proto->numparams = 2;
    proto->min_params = 2;
    proto->maxstacksize = 3;
    xr_vm_proto_write(proto, CREATE_AsBx(OP_LOADI, 2, 0), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN1, 2, 0, 0), 1);
    return proto;
}

static XrProto *make_bsearch_proto(void) {
    XrProto *proto = xr_vm_proto_new();
    if (!proto)
        return NULL;
    proto->source_file = xr_strdup("<ffi-bsearch>");
    proto->is_extern = true;

    XrFFISig *sig = xr_ffi_sig_new("bsearch", NULL, 5);
    if (!sig) {
        xr_vm_proto_free(proto);
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
        xr_vm_proto_free(proto);
        return NULL;
    }

    proto->ffi_sig = sig;
    return proto;
}

TEST(vm_ffi_bsearch_invokes_xray_cfn_callback) {
#ifndef XRAY_HAVE_LIBFFI
    return;
#else
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

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

    XrValue result = xr_ffi_call_proto(iso, bsearch_proto, args, 5);
    ASSERT_TRUE(XR_IS_INT(result));

    uintptr_t found = (uintptr_t) XR_TO_INT(result);
    uintptr_t start = (uintptr_t) values;
    uintptr_t end = (uintptr_t) (values + 3);
    ASSERT_GE(found, start);
    ASSERT_LT(found, end);

    xr_vm_proto_free(bsearch_proto);
    xr_vm_proto_free(callback_proto);
    xray_vm_delete(iso);
#endif
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("VM FFI callback bridge");
RUN_TEST(vm_ffi_bsearch_invokes_xray_cfn_callback);
TEST_MAIN_END()
