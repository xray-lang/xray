/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_run_closure_blocking.c - Coverage for xr_vm_run_closure_blocking, the
 * synchronous C entry that drives a suspending stdlib coroutine to completion.
 *
 * http.get("") throws HttpError.InvalidUrl before any socket I/O, so these
 * tests exercise the full driver path — import the exported closure, build a
 * string argument from C in the root context, COPY-bind it into the coroutine,
 * run it, and read back either the return value or the thrown error — without
 * depending on the network.
 */

#include "../test_framework.h"
#include "xray_vm.h"
#include "module/xmodule.h"
#include "vm/xvm_coro_api.h"
#include "runtime/value/xvalue.h"
#include "runtime/xisolate_api.h"
#include "runtime/core/xr_runtime_core.h"
#include "runtime/core/xr_exec_context.h"
#include "runtime/object/xstring.h"
#include "runtime/object/xarray.h"
#include <string.h>

static XrValue root_string(XrVMRuntime *iso, const char *s) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(iso);
    XrExecutionContext *prev = xr_exec_context_enter(xr_runtime_core_root_exec(core));
    XrString *str = xr_string_new(iso, s, s ? strlen(s) : 0);
    xr_exec_context_restore(prev);
    return str ? xr_string_value(str) : xr_null();
}

/* A thrown error propagates through out_error and the entry returns null. */
TEST(run_closure_blocking_propagates_thrown_error) {
    XrVMConfig params;
    xray_vm_config_init(&params);
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    XrValue get = xr_module_import_member(iso, "http", "get");
    ASSERT_TRUE(xr_value_is_closure(get));

    XrValue url = root_string(iso, "");
    ASSERT_FALSE(XR_IS_NULL(url));

    XrValue err = xr_null();
    XrValue result = xr_vm_run_closure_blocking(iso, xr_value_to_closure(get), &url, 1, &err);
    ASSERT_TRUE(XR_IS_NULL(result));
    ASSERT_FALSE(XR_IS_NULL(err));

    xray_vm_multicore_destroy(iso);
    xray_vm_delete(iso);
}

/* Reusing one isolate for several drives must stay stable (the entry reuses a
 * single main coroutine across calls, as a package command drives several
 * requests in sequence). */
TEST(run_closure_blocking_reuses_isolate_across_calls) {
    XrVMConfig params;
    xray_vm_config_init(&params);
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    XrValue get = xr_module_import_member(iso, "http", "get");
    ASSERT_TRUE(xr_value_is_closure(get));

    for (int i = 0; i < 3; i++) {
        XrValue url = root_string(iso, "");
        XrValue err = xr_null();
        XrValue result = xr_vm_run_closure_blocking(iso, xr_value_to_closure(get), &url, 1, &err);
        ASSERT_TRUE(XR_IS_NULL(result));
        ASSERT_FALSE(XR_IS_NULL(err));
    }

    xray_vm_multicore_destroy(iso);
    xray_vm_delete(iso);
}

/* An Array<byte> argument built from C is deep-copied into the coroutine and
 * survives the drive. http.post to an invalid URL throws before any I/O, so
 * this covers the byte-array argument path without the network. */
TEST(run_closure_blocking_accepts_byte_array_argument) {
    XrVMConfig params;
    xray_vm_config_init(&params);
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    XrValue post = xr_module_import_member(iso, "http", "post");
    ASSERT_TRUE(xr_value_is_closure(post));

    XrRuntimeCore *core = xr_isolate_get_runtime_core(iso);
    XrExecutionContext *prev = xr_exec_context_enter(xr_runtime_core_root_exec(core));
    XrValue url = xr_string_value(xr_string_new(iso, "", 0));
    XrValue ctype = xr_string_value(xr_string_new(iso, "application/octet-stream", 24));
    XrValue auth = xr_string_value(xr_string_new(iso, "Bearer t", 8));
    static const unsigned char payload[] = {1, 2, 3, 4, 5};
    XrArray *body = xr_array_with_capacity_in(&core->root_alloc, (int) sizeof(payload), XR_ELEM_U8);
    ASSERT_NOT_NULL(body);
    xr_byte_array_append_from_span(body, payload, (int64_t) sizeof(payload),
                                   payload + sizeof(payload));
    xr_exec_context_restore(prev);

    XrValue args[4] = {url, ctype, xr_value_from_array(body), auth};
    XrValue err = xr_null();
    XrValue result = xr_vm_run_closure_blocking(iso, xr_value_to_closure(post), args, 4, &err);
    ASSERT_TRUE(XR_IS_NULL(result));
    ASSERT_FALSE(XR_IS_NULL(err));

    xray_vm_multicore_destroy(iso);
    xray_vm_delete(iso);
}

/* NULL closure short-circuits to null without crashing. */
TEST(run_closure_blocking_null_closure_is_safe) {
    XrVMConfig params;
    xray_vm_config_init(&params);
    XrVMRuntime *iso = xray_vm_new_full(&params);
    ASSERT_NOT_NULL(iso);

    XrValue err = xr_null();
    XrValue result = xr_vm_run_closure_blocking(iso, NULL, NULL, 0, &err);
    ASSERT_TRUE(XR_IS_NULL(result));
    ASSERT_TRUE(XR_IS_NULL(err));

    xray_vm_delete(iso);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("xr_vm_run_closure_blocking");
RUN_TEST(run_closure_blocking_propagates_thrown_error);
RUN_TEST(run_closure_blocking_reuses_isolate_across_calls);
RUN_TEST(run_closure_blocking_accepts_byte_array_argument);
RUN_TEST(run_closure_blocking_null_closure_is_safe);
TEST_MAIN_END()
