/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_backend.c - Unit tests for lightweight native coroutine backend
 */

#include "../test_framework.h"
#include "coro/xcoro_pool.h"
#include "coro/xcoroutine.h"
#include "runtime/xisolate_internal.h"
#include <string.h>

static void native_increment(void *arg) {
    int *value = (int *) arg;
    if (value)
        (*value)++;
}

TEST(native_coroutine_uses_native_backend_without_vm_state) {
    XrayIsolate isolate;
    memset(&isolate, 0, sizeof(isolate));

    int counter = 0;
    XrCoroutine *coro = xr_coro_create_native(&isolate, native_increment, &counter, "native");
    ASSERT_NOT_NULL(coro);
    ASSERT_NOT_NULL(coro->backend);
    ASSERT_EQ_INT(coro->backend->kind, XR_CORO_BACKEND_NATIVE);
    ASSERT_NULL(xr_coro_maybe_vm_state(coro));
    ASSERT_TRUE((coro->gc_flags & XR_CORO_GC_LIGHTWEIGHT) != 0);

    XrCoroEvent event = {
        .kind = XR_CORO_EVENT_START,
        .value = XR_NULL_VAL,
        .flags = 0,
    };
    XrCoroRunContext run_ctx = {
        .worker = NULL,
        .isolate = &isolate,
    };
    XrCoroRunResult result = coro->backend->resume(coro, &event, &run_ctx);

    ASSERT_EQ_INT(result.kind, XR_CORO_RUN_DONE);
    ASSERT_EQ_INT(counter, 1);
    ASSERT_NULL(xr_coro_maybe_vm_state(coro));

    xr_coro_destroy(coro);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Native Coroutine Backend");
RUN_TEST(native_coroutine_uses_native_backend_without_vm_state);

TEST_MAIN_END()
