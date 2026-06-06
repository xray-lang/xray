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
#include "coro/xaot_coro.h"
#include "coro/xcoro_pool.h"
#include "coro/xcoroutine.h"
#include "runtime/xisolate_internal.h"
#include <stdatomic.h>
#include <string.h>

static void native_increment(void *arg) {
    int *value = (int *) arg;
    if (value)
        (*value)++;
}

typedef enum AotTestMode {
    AOT_TEST_DONE,
    AOT_TEST_BLOCK,
    AOT_TEST_ERROR,
} AotTestMode;

typedef struct AotTestFrame {
    AotTestMode mode;
    int *release_count;
    int *trace_count;
    int resume_count;
    XrValue value;
    XrValue error;
} AotTestFrame;

static XrAotResult aot_test_resume(void *raw_frame, const XrAotContext *ctx) {
    AotTestFrame *frame = (AotTestFrame *) raw_frame;
    if (!frame || !ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    frame->resume_count++;
    switch (frame->mode) {
        case AOT_TEST_DONE:
            return xr_aot_done(frame->value);
        case AOT_TEST_BLOCK:
            return xr_aot_blocked();
        case AOT_TEST_ERROR:
            return xr_aot_error(frame->error, true);
        default:
            return xr_aot_error(XR_NULL_VAL, false);
    }
}

static void aot_test_trace(void *raw_frame, void *visitor) {
    AotTestFrame *frame = (AotTestFrame *) raw_frame;
    (void) visitor;
    if (frame && frame->trace_count)
        (*frame->trace_count)++;
}

static void aot_test_release(void *raw_frame, struct XrCoroGC *gc) {
    AotTestFrame *frame = (AotTestFrame *) raw_frame;
    (void) gc;
    if (frame && frame->release_count)
        (*frame->release_count)++;
    xr_aot_frame_free(frame);
}

static const XrAotCoroDesc aot_test_desc = {
    .name = "aot_test",
    .frame_size = sizeof(AotTestFrame),
    .root_count = 0,
    .release_count = 0,
    .resume = aot_test_resume,
    .trace_roots = aot_test_trace,
    .release_frame = aot_test_release,
};

static AotTestFrame *aot_test_frame_new(AotTestMode mode, XrValue value, XrValue error,
                                        int *release_count, int *trace_count) {
    AotTestFrame *frame = (AotTestFrame *) xr_aot_frame_alloc(sizeof(AotTestFrame));
    if (!frame)
        return NULL;
    frame->mode = mode;
    frame->release_count = release_count;
    frame->trace_count = trace_count;
    frame->value = value;
    frame->error = error;
    return frame;
}

TEST(native_coroutine_uses_native_backend_without_vm_state) {
    XrayIsolate isolate;
    memset(&isolate, 0, sizeof(isolate));

    int counter = 0;
    XrCoroutine *coro = xr_coro_create_native(&isolate, native_increment, &counter, "native");
    ASSERT_NOT_NULL(coro);
    ASSERT_NOT_NULL(coro->backend);
    ASSERT_EQ_INT(coro->backend->kind, XR_CORO_BACKEND_NATIVE);
    ASSERT_FALSE(xr_coro_backend_is_vm(coro));
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
    ASSERT_FALSE(xr_coro_backend_is_vm(coro));

    xr_coro_destroy(coro);
}

TEST(aot_coroutine_uses_aot_backend_without_vm_state_and_maps_done) {
    XrayIsolate isolate;
    memset(&isolate, 0, sizeof(isolate));

    int release_count = 0;
    int trace_count = 0;
    AotTestFrame *frame =
        aot_test_frame_new(AOT_TEST_DONE, xr_int(42), XR_NULL_VAL, &release_count, &trace_count);
    ASSERT_NOT_NULL(frame);

    XrCoroutine *coro = xr_coro_create_aot(&isolate, &aot_test_desc, frame, "aot_done");
    ASSERT_NOT_NULL(coro);
    ASSERT_NOT_NULL(coro->backend);
    ASSERT_EQ_INT(coro->backend->kind, XR_CORO_BACKEND_AOT);
    ASSERT_FALSE(xr_coro_backend_is_vm(coro));
    ASSERT_TRUE((coro->gc_flags & XR_CORO_GC_LIGHTWEIGHT) != 0);

    XrCoroDebugSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    coro->backend->debug_snapshot(coro, &snapshot);
    ASSERT_STR_EQ(snapshot.backend_name, "aot");
    ASSERT_STR_EQ(snapshot.function_name, "aot_test");
    ASSERT_EQ_INT(snapshot.frame_count, 1);

    coro->backend->trace_roots(coro, &trace_count);
    ASSERT_EQ_INT(trace_count, 1);

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
    ASSERT_EQ_INT(XR_TO_INT(result.value), 42);
    ASSERT_EQ_INT(frame->resume_count, 1);
    ASSERT_FALSE(xr_coro_backend_is_vm(coro));

    xr_coro_destroy(coro);
    ASSERT_EQ_INT(release_count, 1);
}

TEST(aot_coroutine_maps_block_error_and_cancel_to_common_run_results) {
    XrayIsolate isolate;
    memset(&isolate, 0, sizeof(isolate));

    int block_release_count = 0;
    AotTestFrame *block_frame =
        aot_test_frame_new(AOT_TEST_BLOCK, XR_NULL_VAL, XR_NULL_VAL, &block_release_count, NULL);
    ASSERT_NOT_NULL(block_frame);

    XrCoroutine *blocked = xr_coro_create_aot(&isolate, &aot_test_desc, block_frame, "aot_block");
    ASSERT_NOT_NULL(blocked);

    XrCoroRunContext run_ctx = {
        .worker = NULL,
        .isolate = &isolate,
    };
    XrCoroRunResult block_result = blocked->backend->resume(blocked, NULL, &run_ctx);
    ASSERT_EQ_INT(block_result.kind, XR_CORO_RUN_BLOCKED);
    ASSERT_EQ_INT(atomic_load(&blocked->coro_state), XR_CORO_STATE_BLOCKED);
    ASSERT_TRUE(xr_coro_flags_has(blocked, XR_CORO_FLG_BLOCKED));
    ASSERT_FALSE(xr_coro_flags_has(blocked, XR_CORO_FLG_RUNNING));

    xr_coro_destroy(blocked);
    ASSERT_EQ_INT(block_release_count, 1);

    int error_release_count = 0;
    AotTestFrame *error_frame =
        aot_test_frame_new(AOT_TEST_ERROR, XR_NULL_VAL, xr_int(77), &error_release_count, NULL);
    ASSERT_NOT_NULL(error_frame);

    XrCoroutine *errored = xr_coro_create_aot(&isolate, &aot_test_desc, error_frame, "aot_error");
    ASSERT_NOT_NULL(errored);
    XrCoroRunResult error_result = errored->backend->resume(errored, NULL, &run_ctx);
    ASSERT_EQ_INT(error_result.kind, XR_CORO_RUN_ERROR);
    ASSERT_TRUE(error_result.error_is_value);
    ASSERT_EQ_INT(XR_TO_INT(error_result.error), 77);
    ASSERT_EQ_INT(error_frame->resume_count, 1);

    xr_coro_destroy(errored);
    ASSERT_EQ_INT(error_release_count, 1);

    int cancel_release_count = 0;
    AotTestFrame *cancel_frame =
        aot_test_frame_new(AOT_TEST_DONE, xr_int(1), XR_NULL_VAL, &cancel_release_count, NULL);
    ASSERT_NOT_NULL(cancel_frame);

    XrCoroutine *cancelled =
        xr_coro_create_aot(&isolate, &aot_test_desc, cancel_frame, "aot_cancel");
    ASSERT_NOT_NULL(cancelled);
    XrCoroEvent cancel_event = {
        .kind = XR_CORO_EVENT_CANCEL,
        .value = XR_NULL_VAL,
        .flags = 0,
    };
    XrCoroRunResult cancel_result = cancelled->backend->resume(cancelled, &cancel_event, &run_ctx);
    ASSERT_EQ_INT(cancel_result.kind, XR_CORO_RUN_CANCELLED);
    ASSERT_EQ_INT(cancel_frame->resume_count, 0);

    xr_coro_destroy(cancelled);
    ASSERT_EQ_INT(cancel_release_count, 1);
}

TEST(aot_coroutine_create_failure_releases_frame) {
    XrayIsolate isolate;
    memset(&isolate, 0, sizeof(isolate));

    int release_count = 0;
    AotTestFrame *frame =
        aot_test_frame_new(AOT_TEST_DONE, xr_int(1), XR_NULL_VAL, &release_count, NULL);
    ASSERT_NOT_NULL(frame);

    XrAotCoroDesc invalid_desc = aot_test_desc;
    invalid_desc.resume = NULL;

    XrCoroutine *coro = xr_coro_create_aot(&isolate, &invalid_desc, frame, "invalid_aot");
    ASSERT_NULL(coro);
    ASSERT_EQ_INT(release_count, 1);
}

TEST(coroutine_recycle_hooks_are_backend_abi_contract) {
    const XrCoroBackendVTable *vm_backend = xr_coro_vm_backend_vtable();
    ASSERT_NOT_NULL(vm_backend);
    ASSERT_EQ_INT(vm_backend->kind, XR_CORO_BACKEND_VM);
    ASSERT_NOT_NULL(vm_backend->prepare_recycle);
    ASSERT_NOT_NULL(vm_backend->reset_reusable);

    XrayIsolate isolate;
    memset(&isolate, 0, sizeof(isolate));

    int counter = 0;
    XrCoroutine *native = xr_coro_create_native(&isolate, native_increment, &counter, "native");
    ASSERT_NOT_NULL(native);
    ASSERT_FALSE(xr_coro_backend_prepare_recycle(native, NULL));
    ASSERT_FALSE(xr_coro_backend_reset_reusable(native));
    xr_coro_destroy(native);

    int release_count = 0;
    AotTestFrame *frame =
        aot_test_frame_new(AOT_TEST_DONE, xr_int(1), XR_NULL_VAL, &release_count, NULL);
    ASSERT_NOT_NULL(frame);
    XrCoroutine *aot = xr_coro_create_aot(&isolate, &aot_test_desc, frame, "aot_no_pool");
    ASSERT_NOT_NULL(aot);
    ASSERT_FALSE(xr_coro_backend_prepare_recycle(aot, NULL));
    ASSERT_FALSE(xr_coro_backend_reset_reusable(aot));
    xr_coro_destroy(aot);
    ASSERT_EQ_INT(release_count, 1);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Native Coroutine Backend");
RUN_TEST(native_coroutine_uses_native_backend_without_vm_state);
RUN_TEST(aot_coroutine_uses_aot_backend_without_vm_state_and_maps_done);
RUN_TEST(aot_coroutine_maps_block_error_and_cancel_to_common_run_results);
RUN_TEST(aot_coroutine_create_failure_releases_frame);
RUN_TEST(coroutine_recycle_hooks_are_backend_abi_contract);

TEST_MAIN_END()
