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
#include "base/xglobal_indices.h"
#include "base/xmalloc.h"
#include "coro/xchannel.h"
#include "coro/xaot_coro.h"
#include "coro/xblock.h"
#include "coro/xcoro_pool.h"
#include "coro/xcoroutine.h"
#include "coro/xdeep_copy.h"
#include "coro/xresult_group.h"
#include "coro/xtask.h"
#include "coro/xworker.h"
#include "coro/xwork_queue.h"
#include "coro/xyieldable.h"
#include "runtime/class/xenum.h"
#include "runtime/mem/xalloc_unified.h"
#include "runtime/mem/xobj_destroy_ops.h"
#include "runtime/object/xarray.h"
#include "runtime/xisolate_internal.h"
#include <stdatomic.h>
#include <string.h>

static void native_increment(void *arg) {
    int *value = (int *) arg;
    if (value)
        (*value)++;
}

static _Atomic int64_t aot_par_for_sum;

static _Atomic int64_t aot_par_for_bad_worker_id;
static _Atomic int64_t aot_par_for_seen_mask;
static _Atomic int64_t aot_par_for_lane_begin[8];
static _Atomic int64_t aot_par_for_lane_end[8];
static _Atomic int64_t aot_par_for_lane_calls[8];

static void aot_par_for_reset_lane_records(void) {
    for (int i = 0; i < 8; i++) {
        atomic_store_explicit(&aot_par_for_lane_begin[i], -1, memory_order_relaxed);
        atomic_store_explicit(&aot_par_for_lane_end[i], -1, memory_order_relaxed);
        atomic_store_explicit(&aot_par_for_lane_calls[i], 0, memory_order_relaxed);
    }
}

static void aot_par_for_record_lane_body(struct xrt_closure *closure, int64_t begin, int64_t end,
                                         int64_t worker_id) {
    (void) closure;
    if (worker_id < 0 || worker_id >= 8) {
        atomic_fetch_add_explicit(&aot_par_for_bad_worker_id, 1, memory_order_relaxed);
        return;
    }
    atomic_store_explicit(&aot_par_for_lane_begin[worker_id], begin, memory_order_relaxed);
    atomic_store_explicit(&aot_par_for_lane_end[worker_id], end, memory_order_relaxed);
    atomic_fetch_add_explicit(&aot_par_for_lane_calls[worker_id], 1, memory_order_relaxed);
}

static void aot_par_for_range_sum_body(struct xrt_closure *closure, int64_t begin, int64_t end,
                                       int64_t worker_id) {
    (void) closure;
    if (worker_id < 0 || worker_id >= 8)
        atomic_fetch_add_explicit(&aot_par_for_bad_worker_id, 1, memory_order_relaxed);
    if (worker_id >= 0 && worker_id < 63)
        atomic_fetch_or_explicit(&aot_par_for_seen_mask, (int64_t) 1 << worker_id,
                                 memory_order_relaxed);
    for (int64_t i = begin; i < end; i++)
        atomic_fetch_add_explicit(&aot_par_for_sum, i, memory_order_relaxed);
}

static bool aot_par_reduce_range_sum_body(struct xrt_closure *closure, int64_t begin, int64_t end,
                                          int64_t worker_id, int64_t *out) {
    (void) closure;
    if (!out)
        return false;
    if (worker_id < 0 || worker_id >= 8)
        atomic_fetch_add_explicit(&aot_par_for_bad_worker_id, 1, memory_order_relaxed);
    if (worker_id >= 0 && worker_id < 63)
        atomic_fetch_or_explicit(&aot_par_for_seen_mask, (int64_t) 1 << worker_id,
                                 memory_order_relaxed);
    int64_t sum = 0;
    for (int64_t i = begin; i < end; i++)
        sum += i;
    *out = sum;
    return true;
}

static int64_t aot_par_reduce_i64_add(struct xrt_closure *closure, int64_t acc, int64_t value) {
    (void) closure;
    return acc + value;
}

static bool aot_par_reduce_failing_body(struct xrt_closure *closure, int64_t begin, int64_t end,
                                        int64_t worker_id, int64_t *out) {
    (void) closure;
    (void) begin;
    (void) end;
    (void) worker_id;
    (void) out;
    return false;
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

static void aot_test_configure_runtime_core(struct XrRuntimeCore *core, uint32_t caps,
                                            void *userdata) {
    (void) userdata;
    if ((caps & XR_AOT_CAP_OBJECTS) != 0)
        xr_runtime_core_enable_object_destroy_ops(core);
    if ((caps & XR_AOT_CAP_TASK) != 0)
        xr_runtime_core_enable_task_destroy_ops(core);
    if ((caps & XR_AOT_CAP_CHANNEL) != 0)
        xr_runtime_core_enable_channel_destroy_ops(core);
    if ((caps & XR_AOT_CAP_WORK_QUEUE) != 0)
        xr_runtime_core_enable_work_queue_destroy_ops(core);
    if ((caps & XR_AOT_CAP_RESULT_GROUP) != 0)
        xr_runtime_core_enable_result_group_destroy_ops(core);
}

static void aot_test_runtime_config_init(XrAotRuntimeConfig *cfg) {
    xr_aot_runtime_config_init(cfg);
    cfg->configure_core = aot_test_configure_runtime_core;
}

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

static void aot_test_release(void *raw_frame, struct XrCoroHeap *heap) {
    AotTestFrame *frame = (AotTestFrame *) raw_frame;
    (void) heap;
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
    // xr_aot_frame_alloc uses xr_malloc (uninitialized); AOT codegen zeroes the
    // frame, so mirror that here, otherwise resume_count starts as garbage.
    memset(frame, 0, sizeof(*frame));
    frame->mode = mode;
    frame->release_count = release_count;
    frame->trace_count = trace_count;
    frame->value = value;
    frame->error = error;
    return frame;
}

static XrAotRuntime *aot_test_runtime_new(void) {
    XrAotRuntimeConfig cfg;
    aot_test_runtime_config_init(&cfg);
    cfg.caps = XR_AOT_CAP_CORO;
    cfg.scheduler_workers = 0;
    return xr_aot_runtime_new(&cfg);
}

TEST(native_coroutine_uses_native_backend_without_vm_state) {
    XrVMRuntime isolate;
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
        .backend_ctx = &isolate,
    };
    XrCoroRunResult result = coro->backend->resume(coro, &event, &run_ctx);

    ASSERT_EQ_INT(result.kind, XR_CORO_RUN_DONE);
    ASSERT_EQ_INT(counter, 1);
    ASSERT_FALSE(xr_coro_backend_is_vm(coro));

    xr_coro_destroy(coro);
}

TEST(aot_coroutine_uses_aot_backend_without_vm_state_and_maps_done) {
    XrAotRuntime *runtime = aot_test_runtime_new();
    ASSERT_NOT_NULL(runtime);

    int release_count = 0;
    int trace_count = 0;
    AotTestFrame *frame =
        aot_test_frame_new(AOT_TEST_DONE, xr_int(42), XR_NULL_VAL, &release_count, &trace_count);
    ASSERT_NOT_NULL(frame);

    XrCoroutine *coro = xr_coro_create_aot(runtime, &aot_test_desc, frame, "aot_done");
    ASSERT_NOT_NULL(coro);
    ASSERT_NOT_NULL(coro->backend);
    ASSERT_EQ_INT(coro->backend->kind, XR_CORO_BACKEND_AOT);
    ASSERT_FALSE(xr_coro_backend_is_vm(coro));
    ASSERT_NULL(xr_coro_vm_owner(coro));
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
        .backend_ctx = runtime,
    };
    XrCoroRunResult result = coro->backend->resume(coro, &event, &run_ctx);

    ASSERT_EQ_INT(result.kind, XR_CORO_RUN_DONE);
    ASSERT_EQ_INT(XR_TO_INT(result.value), 42);
    ASSERT_EQ_INT(frame->resume_count, 1);
    ASSERT_FALSE(xr_coro_backend_is_vm(coro));

    xr_coro_destroy(coro);
    ASSERT_EQ_INT(release_count, 1);
    xr_aot_runtime_delete(runtime);
}

TEST(aot_coroutine_maps_block_error_and_cancel_to_common_run_results) {
    XrAotRuntime *runtime = aot_test_runtime_new();
    ASSERT_NOT_NULL(runtime);

    int block_release_count = 0;
    AotTestFrame *block_frame =
        aot_test_frame_new(AOT_TEST_BLOCK, XR_NULL_VAL, XR_NULL_VAL, &block_release_count, NULL);
    ASSERT_NOT_NULL(block_frame);

    XrCoroutine *blocked = xr_coro_create_aot(runtime, &aot_test_desc, block_frame, "aot_block");
    ASSERT_NOT_NULL(blocked);
    ASSERT_NULL(xr_coro_vm_owner(blocked));

    XrCoroRunContext run_ctx = {
        .worker = NULL,
        .backend_ctx = runtime,
    };
    XrCoroRunResult block_result = blocked->backend->resume(blocked, NULL, &run_ctx);
    ASSERT_EQ_INT(block_result.kind, XR_CORO_RUN_BLOCKED);
    ASSERT_EQ_INT(xr_flag_to_state(atomic_load(&blocked->flags)), XR_CORO_STATE_BLOCKED);
    ASSERT_TRUE(xr_coro_flags_has(blocked, XR_CORO_FLG_BLOCKED));
    ASSERT_FALSE(xr_coro_flags_has(blocked, XR_CORO_FLG_RUNNING));

    xr_coro_destroy(blocked);
    ASSERT_EQ_INT(block_release_count, 1);

    int error_release_count = 0;
    AotTestFrame *error_frame =
        aot_test_frame_new(AOT_TEST_ERROR, XR_NULL_VAL, xr_int(77), &error_release_count, NULL);
    ASSERT_NOT_NULL(error_frame);

    XrCoroutine *errored = xr_coro_create_aot(runtime, &aot_test_desc, error_frame, "aot_error");
    ASSERT_NOT_NULL(errored);
    ASSERT_NULL(xr_coro_vm_owner(errored));
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
        xr_coro_create_aot(runtime, &aot_test_desc, cancel_frame, "aot_cancel");
    ASSERT_NOT_NULL(cancelled);
    ASSERT_NULL(xr_coro_vm_owner(cancelled));
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
    xr_aot_runtime_delete(runtime);
}

TEST(aot_coroutine_create_failure_releases_frame) {
    XrAotRuntime *runtime = aot_test_runtime_new();
    ASSERT_NOT_NULL(runtime);

    int release_count = 0;
    AotTestFrame *frame =
        aot_test_frame_new(AOT_TEST_DONE, xr_int(1), XR_NULL_VAL, &release_count, NULL);
    ASSERT_NOT_NULL(frame);

    XrAotCoroDesc invalid_desc = aot_test_desc;
    invalid_desc.resume = NULL;

    XrCoroutine *coro = xr_coro_create_aot(runtime, &invalid_desc, frame, "invalid_aot");
    ASSERT_NULL(coro);
    ASSERT_EQ_INT(release_count, 1);
    xr_aot_runtime_delete(runtime);
}

TEST(aot_frame_alloc_accepts_zero_state_frames) {
    void *frame = xr_aot_frame_alloc(0);
    ASSERT_NOT_NULL(frame);
    xr_aot_frame_free(frame);
}

TEST(aot_frame_alloc_reuses_small_frames_locally) {
    void *frame = xr_aot_frame_alloc(24);
    ASSERT_NOT_NULL(frame);
    memset(frame, 0xA5, 24);
    xr_aot_frame_free(frame);

    void *reused = xr_aot_frame_alloc(24);
    ASSERT_EQ_PTR(reused, frame);
    xr_aot_frame_free(reused);
}

TEST(aot_runtime_owns_core_without_isolate) {
    char *argv[] = {"alpha", "beta"};
    int userdata = 123;

    XrAotRuntimeConfig cfg;
    aot_test_runtime_config_init(&cfg);
    cfg.caps = XR_AOT_CAP_NONE;
    cfg.argc = 2;
    cfg.argv = argv;
    cfg.file = "main.xr";
    cfg.userdata = &userdata;

    XrAotRuntime *runtime = xr_aot_runtime_new(&cfg);
    ASSERT_NOT_NULL(runtime);
    ASSERT_EQ_INT((int) xr_aot_runtime_caps(runtime), XR_AOT_CAP_NONE);
    ASSERT_NOT_NULL(xr_aot_runtime_core(runtime));
    ASSERT_NULL(xr_aot_runtime_scheduler(runtime));
    ASSERT_NULL(xr_aot_runtime_core(runtime)->fixed_heap.isolate);
    ASSERT_EQ_PTR(xr_aot_runtime_core(runtime)->userdata, &userdata);
    ASSERT_STR_EQ(xr_aot_runtime_core(runtime)->script_info.file, "main.xr");
    ASSERT_EQ_INT(xr_aot_runtime_core(runtime)->script_info.argc, 2);
    ASSERT_EQ_PTR(xr_aot_runtime_core(runtime)->script_info.argv, argv);

    xr_aot_runtime_delete(runtime);
}

TEST(aot_runtime_creates_scheduler_for_runtime_caps) {
    XrAotRuntimeConfig cfg;
    aot_test_runtime_config_init(&cfg);
    cfg.caps = XR_AOT_CAP_CORO | XR_AOT_CAP_TIMER | XR_AOT_CAP_CHANNEL;
    cfg.scheduler_workers = 0;

    XrAotRuntime *runtime = xr_aot_runtime_new(&cfg);
    ASSERT_NOT_NULL(runtime);
    XrRuntime *scheduler = xr_aot_runtime_scheduler(runtime);
    ASSERT_NOT_NULL(scheduler);
    ASSERT_EQ_PTR(xr_runtime_get_core(scheduler), xr_aot_runtime_core(runtime));
    ASSERT_EQ_PTR(xr_scheduler_host_backend_context(scheduler), runtime);

    xr_aot_runtime_delete(runtime);
}

TEST(aot_runtime_creates_isolate_free_aot_coroutine) {
    XrAotRuntimeConfig cfg;
    aot_test_runtime_config_init(&cfg);
    cfg.caps = XR_AOT_CAP_CORO;
    cfg.scheduler_workers = 0;

    XrAotRuntime *runtime = xr_aot_runtime_new(&cfg);
    ASSERT_NOT_NULL(runtime);

    int release_count = 0;
    AotTestFrame *frame =
        aot_test_frame_new(AOT_TEST_DONE, xr_int(7), XR_NULL_VAL, &release_count, NULL);
    ASSERT_NOT_NULL(frame);

    XrCoroutine *coro = xr_coro_create_aot(runtime, &aot_test_desc, frame, "aot_runtime");
    ASSERT_NOT_NULL(coro);
    ASSERT_NULL(xr_coro_vm_owner(coro));
    ASSERT_EQ_PTR(coro->core, xr_aot_runtime_core(runtime));
    ASSERT_EQ_PTR(coro->scheduler, xr_aot_runtime_scheduler(runtime));

    XrCoroRunContext run_ctx = {
        .worker = NULL,
        .backend_ctx = runtime,
    };
    XrCoroRunResult result = coro->backend->resume(coro, NULL, &run_ctx);
    ASSERT_EQ_INT(result.kind, XR_CORO_RUN_DONE);
    ASSERT_EQ_INT(XR_TO_INT(result.value), 7);
    ASSERT_EQ_INT(frame->resume_count, 1);

    xr_coro_destroy(coro);
    ASSERT_EQ_INT(release_count, 1);
    xr_aot_runtime_delete(runtime);
}

TEST(aot_run_main_uses_runtime_without_isolate) {
    XrAotRuntimeConfig cfg;
    aot_test_runtime_config_init(&cfg);
    cfg.caps = XR_AOT_CAP_CORO;
    cfg.scheduler_workers = 0;

    XrAotRuntime *runtime = xr_aot_runtime_new(&cfg);
    ASSERT_NOT_NULL(runtime);

    int release_count = 0;
    AotTestFrame *frame =
        aot_test_frame_new(AOT_TEST_DONE, xr_int(123), XR_NULL_VAL, &release_count, NULL);
    ASSERT_NOT_NULL(frame);

    XrValue result = xr_aot_run_main(runtime, &aot_test_desc, frame);
    ASSERT_EQ_INT(XR_TO_INT(result), 123);
    ASSERT_EQ_INT(release_count, 1);

    xr_aot_runtime_delete(runtime);
}

TEST(aot_context_builtin_prefers_runtime_table) {
    XrAotRuntimeConfig cfg;
    aot_test_runtime_config_init(&cfg);

    XrAotRuntime *runtime = xr_aot_runtime_new(&cfg);
    ASSERT_NOT_NULL(runtime);

    XrValue expected = xr_int(99);
    xr_aot_runtime_set_builtin(runtime, XR_GLOBAL_VAR_PROCESS, expected);

    XrAotContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.runtime = runtime;
    ASSERT_EQ_INT(XR_TO_INT(xr_aot_get_builtin(&ctx, XR_GLOBAL_VAR_PROCESS)), 99);

    xr_aot_runtime_delete(runtime);
}

TEST(aot_runtime_registers_prelude_enums_without_isolate) {
    XrAotRuntimeConfig cfg;
    aot_test_runtime_config_init(&cfg);
    cfg.caps = XR_AOT_CAP_OBJECTS;

    XrAotRuntime *runtime = xr_aot_runtime_new(&cfg);
    ASSERT_NOT_NULL(runtime);
    ASSERT_NULL(xr_aot_runtime_core(runtime)->fixed_heap.isolate);

    XrAotContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.runtime = runtime;

    ASSERT_TRUE(XR_IS_NULL(xr_aot_runtime_builtin(runtime, XR_GLOBAL_VAR_RECV)));
    XrValue recv_type_value = xr_aot_get_builtin(&ctx, XR_GLOBAL_VAR_RECV);
    ASSERT_TRUE(XR_IS_PTR(recv_type_value));
    XrEnumType *recv_type = XR_TO_ENUM_TYPE(recv_type_value);
    ASSERT_STR_EQ(recv_type->name, "Recv");
    ASSERT_TRUE(recv_type->is_adt);
    ASSERT_EQ_INT(recv_type->payload_counts[0], 1);

    XrValue recv_value = xr_aot_load_builtin_field(&ctx, XR_GLOBAL_VAR_RECV, "Value");
    const char *enum_name = NULL;
    const char *member_name = NULL;
    uint32_t member_index = 99;
    bool is_adt = false;
    int payload_count = -1;
    ASSERT_TRUE(xr_aot_runtime_enum_value_info(recv_value, &enum_name, &member_name, &member_index,
                                               &is_adt, &payload_count));
    ASSERT_STR_EQ(enum_name, "Recv");
    ASSERT_STR_EQ(member_name, "Value");
    ASSERT_EQ_INT((int) member_index, 0);
    ASSERT_TRUE(is_adt);
    ASSERT_EQ_INT(payload_count, 1);

    XrValue task_pending = xr_aot_load_builtin_field(&ctx, XR_GLOBAL_VAR_TASK_RESULT, "Pending");
    ASSERT_EQ_INT(task_pending.tag, 24);
    ASSERT_EQ_INT((int) task_pending.ext, 4);

    xr_aot_runtime_delete(runtime);
}

TEST(aot_runtime_copy_context_uses_core_without_isolate) {
    XrAotRuntimeConfig cfg;
    aot_test_runtime_config_init(&cfg);
    cfg.caps = XR_AOT_CAP_OBJECTS;

    XrAotRuntime *runtime = xr_aot_runtime_new(&cfg);
    ASSERT_NOT_NULL(runtime);
    XrRuntimeCore *core = xr_aot_runtime_core(runtime);
    ASSERT_NOT_NULL(core);
    ASSERT_NULL(core->fixed_heap.isolate);

    XrCoroutine *owner = xr_coro_create_runtime_empty(core, NULL, "copy_owner");
    ASSERT_NOT_NULL(owner);
    XrArray *source = xr_array_with_capacity_typed(owner, 3, XR_ELEM_I64);
    ASSERT_NOT_NULL(source);
    source->length = 3;
    xr_array_set_i64(source, 0, 11);
    xr_array_set_i64(source, 1, 22);
    xr_array_set_i64(source, 2, 33);

    XrValue transit = xr_deep_copy_to_transit_core(core, xr_value_from_array(source));
    ASSERT_TRUE(xr_value_is_array(transit));
    XrArray *copied = xr_value_to_array(transit);
    ASSERT_NOT_NULL(copied);
    ASSERT_TRUE(copied != source);
    ASSERT_TRUE(XR_OBJ_GET_FLAG(&copied->hdr, XR_OBJ_TRANSIT));
    ASSERT_TRUE(XR_OBJ_IS_SHARED(&copied->hdr));
    ASSERT_EQ_INT((int) copied->length, 3);
    ASSERT_EQ_INT((int) xr_array_get_i64(copied, 0), 11);
    ASSERT_EQ_INT((int) xr_array_get_i64(copied, 1), 22);
    ASSERT_EQ_INT((int) xr_array_get_i64(copied, 2), 33);

    xr_chan_transit_release_core(core, transit);
    xr_coro_destroy(owner);
    xr_aot_runtime_delete(runtime);
}

TEST(aot_result_group_uses_runtime_without_isolate) {
    XrAotRuntimeConfig cfg;
    aot_test_runtime_config_init(&cfg);
    cfg.caps = XR_AOT_CAP_CORO | XR_AOT_CAP_RESULT_GROUP | XR_AOT_CAP_OBJECTS;
    cfg.scheduler_workers = 0;

    XrAotRuntime *runtime = xr_aot_runtime_new(&cfg);
    ASSERT_NOT_NULL(runtime);

    XrAotContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.runtime = runtime;

    XrValue group_value = xr_aot_result_group_new(&ctx, 2);
    ASSERT_TRUE(xr_value_is_result_group(group_value));
    XrResultGroup *group = xr_value_to_result_group(group_value);
    ASSERT_EQ_PTR(group->core, xr_aot_runtime_core(runtime));
    ASSERT_EQ_PTR(group->scheduler, xr_aot_runtime_scheduler(runtime));

    ASSERT_TRUE(XR_TO_BOOL(xr_aot_result_group_add(&ctx, group_value, 5)));
    ASSERT_TRUE(XR_TO_BOOL(xr_aot_result_group_add(&ctx, group_value, 7)));

    XrValue received = XR_NULL_VAL;
    ASSERT_TRUE(xr_aot_result_group_try_recv(&ctx, group_value, &received));
    ASSERT_EQ_INT(XR_TO_INT(received), 12);
    ASSERT_FALSE(xr_aot_result_group_try_recv(&ctx, group_value, &received));

    ASSERT_TRUE(XR_TO_BOOL(xr_aot_result_group_reset(&ctx, group_value, 3)));
    ASSERT_EQ_INT((int) group->batch_size, 3);
    ASSERT_TRUE(XR_TO_BOOL(xr_aot_result_group_add(&ctx, group_value, 1)));
    ASSERT_TRUE(XR_TO_BOOL(xr_aot_result_group_add(&ctx, group_value, 2)));
    ASSERT_TRUE(XR_TO_BOOL(xr_aot_result_group_add(&ctx, group_value, 4)));
    ASSERT_TRUE(xr_aot_result_group_try_recv(&ctx, group_value, &received));
    ASSERT_EQ_INT(XR_TO_INT(received), 7);

    xr_obj_destroy_result_group(&group->hdr, NULL);
    xr_aot_runtime_delete(runtime);
}

TEST(aot_work_queue_uses_runtime_owner_without_isolate) {
    XrAotRuntimeConfig cfg;
    aot_test_runtime_config_init(&cfg);
    cfg.caps = XR_AOT_CAP_CORO | XR_AOT_CAP_WORK_QUEUE | XR_AOT_CAP_OBJECTS;
    cfg.scheduler_workers = 0;

    XrAotRuntime *runtime = xr_aot_runtime_new(&cfg);
    ASSERT_NOT_NULL(runtime);

    XrAotContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.runtime = runtime;

    XrValue queue_value = xr_aot_work_queue_new(&ctx, 2, 4);
    ASSERT_TRUE(xr_value_is_work_queue(queue_value));
    XrWorkQueue *queue = xr_value_to_work_queue(queue_value);
    ASSERT_EQ_PTR(queue->core, xr_aot_runtime_core(runtime));
    ASSERT_EQ_PTR(queue->scheduler, xr_aot_runtime_scheduler(runtime));

    ASSERT_TRUE(XR_TO_BOOL(xr_aot_work_queue_push(&ctx, queue_value, xr_int(44), 0)));
    XrValue popped = XR_NULL_VAL;
    ASSERT_TRUE(xr_aot_work_queue_try_pop(&ctx, queue_value, 0, &popped));
    ASSERT_EQ_INT(XR_TO_INT(popped), 44);

    ASSERT_TRUE(XR_TO_BOOL(xr_aot_work_queue_push_sync(queue_value, xr_int(55), 0)));
    ASSERT_TRUE(xr_aot_work_queue_try_pop_sync(queue_value, 0, &popped));
    ASSERT_EQ_INT(XR_TO_INT(popped), 55);

    ASSERT_EQ_INT(XR_TO_INT(xr_aot_work_queue_push_range(&ctx, queue_value, 70, 3, 0)), 3);
    ASSERT_TRUE(xr_aot_work_queue_try_pop(&ctx, queue_value, 0, &popped));
    ASSERT_EQ_INT(XR_TO_INT(popped), 72);
    ASSERT_TRUE(xr_aot_work_queue_try_pop(&ctx, queue_value, 1, &popped));
    ASSERT_EQ_INT(XR_TO_INT(popped), 71);
    ASSERT_TRUE(xr_aot_work_queue_try_pop(&ctx, queue_value, 0, &popped));
    ASSERT_EQ_INT(XR_TO_INT(popped), 70);

    ASSERT_EQ_INT(XR_TO_INT(xr_aot_work_queue_push_range_sync(queue_value, 80, 2, 0)), 2);
    ASSERT_TRUE(xr_aot_work_queue_try_pop_sync(queue_value, 0, &popped));
    ASSERT_EQ_INT(XR_TO_INT(popped), 80);
    ASSERT_TRUE(xr_aot_work_queue_try_pop_sync(queue_value, 1, &popped));
    ASSERT_EQ_INT(XR_TO_INT(popped), 81);

    xr_aot_work_queue_close_sync(queue_value);
    ASSERT_TRUE(XR_TO_BOOL(xr_aot_work_queue_is_closed_sync(queue_value)));

    xr_obj_destroy_work_queue(&queue->hdr, NULL);
    xr_aot_runtime_delete(runtime);
}

TEST(aot_channel_uses_runtime_owner_without_isolate) {
    XrAotRuntimeConfig cfg;
    aot_test_runtime_config_init(&cfg);
    cfg.caps = XR_AOT_CAP_CORO | XR_AOT_CAP_CHANNEL | XR_AOT_CAP_OBJECTS;
    cfg.scheduler_workers = 0;

    XrAotRuntime *runtime = xr_aot_runtime_new(&cfg);
    ASSERT_NOT_NULL(runtime);

    XrAotContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.runtime = runtime;
    ctx.coro = xr_coro_create_runtime_empty(xr_aot_runtime_core(runtime),
                                            xr_aot_runtime_scheduler(runtime), "aot_channel_owner");
    ASSERT_NOT_NULL(ctx.coro);
    ASSERT_NULL(ctx.vm_host_ops);
    ASSERT_NULL(ctx.vm_host);

    XrValue channel_value = xr_aot_channel_new(&ctx, 2);
    ASSERT_TRUE(xr_value_is_channel(channel_value));
    XrChannel *channel = xr_value_to_channel(channel_value);
    ASSERT_EQ_PTR(channel->core, xr_aot_runtime_core(runtime));
    ASSERT_EQ_PTR(channel->scheduler, xr_aot_runtime_scheduler(runtime));
    ASSERT_NULL(channel->vm_host_isolate);

    ASSERT_TRUE(XR_TO_BOOL(xr_aot_chan_try_send_ready(&ctx, channel_value, xr_int(77))));

    int64_t received = 0;
    int64_t ok = 0;
    XrAotResult recv =
        xr_aot_chan_recv_pair_i64(&ctx, channel_value, xr_slot_native_ptr(&received, XR_REP_I64),
                                  xr_slot_native_ptr(&ok, XR_REP_I64));
    ASSERT_EQ_INT(recv.kind, XR_AOT_RUN_DONE);
    ASSERT_EQ_INT((int) received, 77);
    ASSERT_EQ_INT((int) ok, 1);

    xr_aot_chan_close(&ctx, channel_value);
    ASSERT_TRUE(XR_TO_BOOL(xr_aot_chan_is_closed(&ctx, channel_value)));

    xr_coro_destroy(ctx.coro);
    xr_obj_destroy_channel(&channel->gc_header, NULL);
    xr_aot_runtime_delete(runtime);
}

TEST(aot_task_await_uses_runtime_owner_without_isolate) {
    XrAotRuntimeConfig cfg;
    aot_test_runtime_config_init(&cfg);
    cfg.caps = XR_AOT_CAP_CORO | XR_AOT_CAP_TASK;
    cfg.scheduler_workers = 0;

    XrAotRuntime *runtime = xr_aot_runtime_new(&cfg);
    ASSERT_NOT_NULL(runtime);

    XrAotContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.runtime = runtime;
    ctx.coro = xr_coro_create_runtime_empty(xr_aot_runtime_core(runtime),
                                            xr_aot_runtime_scheduler(runtime), "aot_task_parent");
    ASSERT_NOT_NULL(ctx.coro);
    ASSERT_NULL(ctx.vm_host_ops);
    ASSERT_NULL(ctx.vm_host);

    XrCoroutine *child = xr_coro_create_runtime_empty(
        xr_aot_runtime_core(runtime), xr_aot_runtime_scheduler(runtime), "aot_task_child");
    ASSERT_NOT_NULL(child);
    XrTask *task = xr_task_create(xr_aot_runtime_scheduler(runtime), ctx.coro, child);
    ASSERT_NOT_NULL(task);
    xr_task_complete(task, xr_int(91));

    int64_t result = 0;
    XrAotResult await = xr_aot_await_task(
        &ctx, xr_value_from_task(task), xr_slot_native_ptr(&result, XR_REP_I64), -1, false, false);
    ASSERT_EQ_INT(await.kind, XR_AOT_RUN_DONE);
    ASSERT_EQ_INT((int) result, 91);
    ASSERT_NULL(ctx.vm_host_ops);
    ASSERT_NULL(ctx.vm_host);

    xr_coro_destroy(ctx.coro);
    xr_aot_runtime_delete(runtime);
}

TEST(runtime_task_one_shot_destroy_unlinks_in_constant_time) {
    XrAotRuntime *runtime = aot_test_runtime_new();
    ASSERT_NOT_NULL(runtime);
    XrRuntime *scheduler = xr_aot_runtime_scheduler(runtime);
    ASSERT_NOT_NULL(scheduler);

    XrCoroutine exec1 = {0};
    XrCoroutine exec2 = {0};
    XrCoroutine exec3 = {0};
    XrTask *t1 = xr_task_create(scheduler, NULL, &exec1);
    XrTask *t2 = xr_task_create(scheduler, NULL, &exec2);
    XrTask *t3 = xr_task_create(scheduler, NULL, &exec3);
    ASSERT_NOT_NULL(t1);
    ASSERT_NOT_NULL(t2);
    ASSERT_NOT_NULL(t3);
    ASSERT_EQ_PTR(scheduler->task_list, t3);
    ASSERT_EQ_PTR(t3->runtime_next, t2);
    ASSERT_EQ_PTR(t2->runtime_next, t1);
    ASSERT_EQ_PTR(t3->runtime_prev_link, &scheduler->task_list);
    ASSERT_EQ_PTR(t2->runtime_prev_link, &t3->runtime_next);
    ASSERT_EQ_PTR(t1->runtime_prev_link, &t2->runtime_next);
    ASSERT_EQ_INT((int) scheduler->task_count, 3);

    atomic_store_explicit(&t1->state, XR_TASK_COMPLETED, memory_order_release);
    atomic_store_explicit(&t2->state, XR_TASK_COMPLETED, memory_order_release);
    atomic_store_explicit(&t3->state, XR_TASK_COMPLETED, memory_order_release);
    atomic_store_explicit(&t1->coro, NULL, memory_order_release);
    atomic_store_explicit(&t2->coro, NULL, memory_order_release);
    atomic_store_explicit(&t3->coro, NULL, memory_order_release);
    atomic_store_explicit(&t1->completer_done, 1, memory_order_release);
    atomic_store_explicit(&t2->completer_done, 1, memory_order_release);
    atomic_store_explicit(&t3->completer_done, 1, memory_order_release);
    exec1.task = NULL;
    exec2.task = NULL;
    exec3.task = NULL;

    ASSERT_TRUE(xr_task_runtime_try_destroy_detached(scheduler, t2));
    ASSERT_EQ_PTR(scheduler->task_list, t3);
    ASSERT_EQ_PTR(t3->runtime_next, t1);
    ASSERT_EQ_PTR(t1->runtime_prev_link, &t3->runtime_next);
    ASSERT_EQ_INT((int) scheduler->task_count, 2);

    ASSERT_TRUE(xr_task_runtime_try_destroy_detached(scheduler, t3));
    ASSERT_EQ_PTR(scheduler->task_list, t1);
    ASSERT_EQ_PTR(t1->runtime_next, NULL);
    ASSERT_EQ_PTR(t1->runtime_prev_link, &scheduler->task_list);
    ASSERT_EQ_INT((int) scheduler->task_count, 1);

    ASSERT_TRUE(xr_task_runtime_try_destroy_detached(scheduler, t1));
    ASSERT_EQ_PTR(scheduler->task_list, NULL);
    ASSERT_EQ_INT((int) scheduler->task_count, 0);

    xr_aot_runtime_delete(runtime);
}

TEST(runtime_task_one_shot_destroy_reuses_task_handle_locally) {
    XrAotRuntime *runtime = aot_test_runtime_new();
    ASSERT_NOT_NULL(runtime);
    XrRuntime *scheduler = xr_aot_runtime_scheduler(runtime);
    ASSERT_NOT_NULL(scheduler);

    XrCoroutine exec1 = {0};
    XrTask *task = xr_task_create(scheduler, NULL, &exec1);
    ASSERT_NOT_NULL(task);
    atomic_store_explicit(&task->state, XR_TASK_COMPLETED, memory_order_release);
    atomic_store_explicit(&task->coro, NULL, memory_order_release);
    atomic_store_explicit(&task->completer_done, 1, memory_order_release);
    exec1.task = NULL;
    ASSERT_TRUE(xr_task_runtime_try_destroy_detached(scheduler, task));
    ASSERT_EQ_PTR(scheduler->task_list, NULL);
    ASSERT_EQ_INT((int) scheduler->task_count, 0);

    XrCoroutine exec2 = {0};
    XrTask *reused = xr_task_create(scheduler, NULL, &exec2);
    ASSERT_EQ_PTR(reused, task);
    ASSERT_EQ_PTR(exec2.task, reused);
    ASSERT_EQ_PTR(xr_task_executor_peek(reused), &exec2);
    ASSERT_EQ_INT(atomic_load_explicit(&reused->state, memory_order_acquire), XR_TASK_ACTIVE);
    ASSERT_EQ_INT(reused->flags, XR_TASK_FLG_RUNTIME_OWNED);
    ASSERT_EQ_PTR(reused->runtime_prev_link, &scheduler->task_list);
    ASSERT_EQ_PTR(scheduler->task_list, reused);
    ASSERT_EQ_INT((int) scheduler->task_count, 1);

    atomic_store_explicit(&reused->state, XR_TASK_COMPLETED, memory_order_release);
    atomic_store_explicit(&reused->coro, NULL, memory_order_release);
    atomic_store_explicit(&reused->completer_done, 1, memory_order_release);
    exec2.task = NULL;
    ASSERT_TRUE(xr_task_runtime_try_destroy_detached(scheduler, reused));
    xr_aot_runtime_delete(runtime);
}

TEST(runtime_task_deferred_registry_batches_one_shot_handles) {
    XrAotRuntime *runtime = aot_test_runtime_new();
    ASSERT_NOT_NULL(runtime);
    XrRuntime *scheduler = xr_aot_runtime_scheduler(runtime);
    ASSERT_NOT_NULL(scheduler);

    XrCoroutine exec1 = {0};
    XrCoroutine exec2 = {0};
    XrTask *t1 = xr_task_create_deferred_registry(scheduler, NULL, &exec1);
    XrTask *t2 = xr_task_create_deferred_registry(scheduler, NULL, &exec2);
    ASSERT_NOT_NULL(t1);
    ASSERT_NOT_NULL(t2);
    ASSERT_EQ_INT((int) scheduler->task_count, 0);
    ASSERT_EQ_PTR(scheduler->task_list, NULL);
    ASSERT_TRUE((t1->flags & XR_TASK_FLG_DEFERRED_REGISTRY) != 0);
    ASSERT_TRUE((t2->flags & XR_TASK_FLG_DEFERRED_REGISTRY) != 0);

    XrCoroutine *batch[] = {&exec1, &exec2};
    ASSERT_EQ_INT((int) xr_task_runtime_register_deferred_batch(scheduler, batch, 2), 2);
    ASSERT_EQ_INT((int) scheduler->task_count, 2);
    ASSERT_TRUE((t1->flags & XR_TASK_FLG_DEFERRED_REGISTRY) == 0);
    ASSERT_TRUE((t2->flags & XR_TASK_FLG_DEFERRED_REGISTRY) == 0);
    ASSERT_EQ_INT((int) xr_task_runtime_register_deferred_batch(scheduler, batch, 2), 0);
    ASSERT_EQ_INT((int) scheduler->task_count, 2);

    atomic_store_explicit(&t1->state, XR_TASK_COMPLETED, memory_order_release);
    atomic_store_explicit(&t2->state, XR_TASK_COMPLETED, memory_order_release);
    atomic_store_explicit(&t1->coro, NULL, memory_order_release);
    atomic_store_explicit(&t2->coro, NULL, memory_order_release);
    atomic_store_explicit(&t1->completer_done, 1, memory_order_release);
    atomic_store_explicit(&t2->completer_done, 1, memory_order_release);
    exec1.task = NULL;
    exec2.task = NULL;
    ASSERT_TRUE(xr_task_runtime_try_destroy_detached(scheduler, t1));
    ASSERT_TRUE(xr_task_runtime_try_destroy_detached(scheduler, t2));
    ASSERT_EQ_INT((int) scheduler->task_count, 0);

    XrCoroutine exec4 = {0};
    XrCoroutine exec5 = {0};
    XrTask *t4 = xr_task_create_deferred_registry(scheduler, NULL, &exec4);
    XrTask *t5 = xr_task_create_deferred_registry(scheduler, NULL, &exec5);
    ASSERT_NOT_NULL(t4);
    ASSERT_NOT_NULL(t5);
    XrTask *task_batch[] = {t4, t5};
    ASSERT_EQ_INT((int) xr_task_runtime_register_deferred_tasks(scheduler, task_batch, 2), 2);
    ASSERT_EQ_INT((int) scheduler->task_count, 2);
    ASSERT_TRUE((t4->flags & XR_TASK_FLG_DEFERRED_REGISTRY) == 0);
    ASSERT_TRUE((t5->flags & XR_TASK_FLG_DEFERRED_REGISTRY) == 0);
    ASSERT_EQ_INT((int) xr_task_runtime_register_deferred_tasks(scheduler, task_batch, 2), 0);

    atomic_store_explicit(&t4->state, XR_TASK_COMPLETED, memory_order_release);
    atomic_store_explicit(&t5->state, XR_TASK_COMPLETED, memory_order_release);
    atomic_store_explicit(&t4->coro, NULL, memory_order_release);
    atomic_store_explicit(&t5->coro, NULL, memory_order_release);
    atomic_store_explicit(&t4->completer_done, 1, memory_order_release);
    atomic_store_explicit(&t5->completer_done, 1, memory_order_release);
    exec4.task = NULL;
    exec5.task = NULL;
    ASSERT_TRUE(xr_task_runtime_try_destroy_detached(scheduler, t4));
    ASSERT_TRUE(xr_task_runtime_try_destroy_detached(scheduler, t5));
    ASSERT_EQ_INT((int) scheduler->task_count, 0);

    XrCoroutine exec3 = {0};
    XrTask *t3 = xr_task_create_deferred_registry(scheduler, NULL, &exec3);
    ASSERT_NOT_NULL(t3);
    ASSERT_EQ_INT((int) scheduler->task_count, 0);
    ASSERT_TRUE(xr_task_destroy_deferred_unregistered(t3));
    ASSERT_EQ_PTR(exec3.task, NULL);
    ASSERT_EQ_INT((int) scheduler->task_count, 0);

    xr_aot_runtime_delete(runtime);
}

TEST(runtime_deferred_array_submit_cache_tracks_content_version) {
    XrAotRuntime *runtime = aot_test_runtime_new();
    ASSERT_NOT_NULL(runtime);
    XrRuntime *scheduler = xr_aot_runtime_scheduler(runtime);
    ASSERT_NOT_NULL(scheduler);

    XrCoroutine parent = {0};
    XrCoroutine exec1 = {0};
    XrCoroutine exec2 = {0};
    XrTask *t1 = xr_task_create_deferred_registry(scheduler, NULL, &exec1);
    XrTask *t2 = xr_task_create_deferred_registry(scheduler, NULL, &exec2);
    ASSERT_NOT_NULL(t1);
    ASSERT_NOT_NULL(t2);
    xr_coro_flags_set(&exec1, XR_CORO_FLG_DEFERRED_SUBMIT);
    xr_coro_flags_set(&exec2, XR_CORO_FLG_DEFERRED_SUBMIT);

    XrArray tasks;
    memset(&tasks, 0, sizeof(tasks));
    xr_obj_header_init_type(&tasks.hdr, XR_TARRAY);
    xr_array_init_inplace(&tasks, 4, XR_ELEM_ANY);
    ASSERT_NOT_NULL(tasks.data);
    uint64_t initial_version = tasks.content_version;
    xr_array_push(&tasks, xr_value_from_task(t1));
    xr_array_push(&tasks, xr_value_from_task(t2));
    ASSERT_TRUE(tasks.content_version != initial_version);

    uint64_t batch_version = tasks.content_version;
    xr_coro_submit_deferred_array_tasks_cached(&parent, &tasks);
    ASSERT_EQ_UINT(tasks.deferred_submit_version, batch_version);
    ASSERT_EQ_UINT(xr_coro_flags_load(&exec1) & XR_CORO_FLG_DEFERRED_SUBMIT, 0);
    ASSERT_EQ_UINT(xr_coro_flags_load(&exec2) & XR_CORO_FLG_DEFERRED_SUBMIT, 0);
    ASSERT_EQ_INT((int) scheduler->task_count, 0);

    xr_coro_flags_set(&exec1, XR_CORO_FLG_DEFERRED_SUBMIT);
    xr_coro_submit_deferred_array_tasks_cached(&parent, &tasks);
    ASSERT_TRUE((xr_coro_flags_load(&exec1) & XR_CORO_FLG_DEFERRED_SUBMIT) != 0);
    xr_coro_flags_clear(&exec1, XR_CORO_FLG_DEFERRED_SUBMIT);

    XrCoroutine exec3 = {0};
    XrTask *t3 = xr_task_create_deferred_registry(scheduler, NULL, &exec3);
    ASSERT_NOT_NULL(t3);
    xr_coro_flags_set(&exec3, XR_CORO_FLG_DEFERRED_SUBMIT);
    xr_array_push(&tasks, xr_value_from_task(t3));
    uint64_t mutated_version = tasks.content_version;
    ASSERT_TRUE(mutated_version != batch_version);
    xr_coro_submit_deferred_array_tasks_cached(&parent, &tasks);
    ASSERT_EQ_UINT(tasks.deferred_submit_version, mutated_version);
    ASSERT_EQ_UINT(xr_coro_flags_load(&exec3) & XR_CORO_FLG_DEFERRED_SUBMIT, 0);
    ASSERT_EQ_INT((int) scheduler->task_count, 0);

    ASSERT_TRUE(xr_task_destroy_deferred_unregistered(t1));
    ASSERT_TRUE(xr_task_destroy_deferred_unregistered(t2));
    ASSERT_TRUE(xr_task_destroy_deferred_unregistered(t3));
    xr_array_clear(&tasks);
    xr_free(tasks.data);
    xr_aot_runtime_delete(runtime);
}

TEST(coroutine_recycle_hooks_are_backend_abi_contract) {
    const XrCoroBackendVTable *vm_backend = xr_coro_vm_backend_vtable();
    ASSERT_NOT_NULL(vm_backend);
    ASSERT_EQ_INT(vm_backend->kind, XR_CORO_BACKEND_VM);
    ASSERT_NOT_NULL(vm_backend->prepare_recycle);
    ASSERT_NOT_NULL(vm_backend->reset_reusable);
    ASSERT_NOT_NULL(vm_backend->setup_yield_continuation);
    ASSERT_NOT_NULL(vm_backend->has_continuation);
    ASSERT_NOT_NULL(vm_backend->call_closure);
    ASSERT_NOT_NULL(vm_backend->ensure_state);
    ASSERT_NOT_NULL(vm_backend->prepare_execution_state);
    ASSERT_NOT_NULL(vm_backend->reset_execution_state);
    ASSERT_NOT_NULL(vm_backend->clear_entry_state);
    ASSERT_NOT_NULL(vm_backend->bind_closure_entry);
    ASSERT_NOT_NULL(vm_backend->bind_cfunc_entry);

    XrVMRuntime isolate;
    memset(&isolate, 0, sizeof(isolate));

    int counter = 0;
    XrCoroutine *native = xr_coro_create_native(&isolate, native_increment, &counter, "native");
    ASSERT_NOT_NULL(native);
    ASSERT_FALSE(xr_coro_backend_prepare_recycle(native, NULL));
    ASSERT_FALSE(xr_coro_backend_reset_reusable(native));
    ASSERT_FALSE(xr_coro_has_continuation(native));
    ASSERT_NULL(native->backend->setup_yield_continuation);
    ASSERT_NULL(native->backend->call_closure);
    ASSERT_NULL(native->backend->prepare_execution_state);
    ASSERT_NULL(native->backend->bind_closure_entry);
    xr_coro_destroy(native);

    int release_count = 0;
    AotTestFrame *frame =
        aot_test_frame_new(AOT_TEST_DONE, xr_int(1), XR_NULL_VAL, &release_count, NULL);
    ASSERT_NOT_NULL(frame);
    XrAotRuntime *runtime = aot_test_runtime_new();
    ASSERT_NOT_NULL(runtime);
    XrCoroutine *aot = xr_coro_create_aot(runtime, &aot_test_desc, frame, "aot_no_pool");
    ASSERT_NOT_NULL(aot);
    ASSERT_NULL(xr_coro_vm_owner(aot));
    ASSERT_TRUE(xr_coro_backend_prepare_recycle(aot, NULL));
    ASSERT_TRUE(xr_coro_backend_reset_reusable(aot));
    ASSERT_FALSE(xr_coro_has_continuation(aot));
    ASSERT_NULL(aot->backend->setup_yield_continuation);
    ASSERT_NULL(aot->backend->call_closure);
    ASSERT_NULL(aot->backend->prepare_execution_state);
    ASSERT_NULL(aot->backend->bind_closure_entry);
    xr_coro_destroy(aot);
    ASSERT_EQ_INT(release_count, 1);
    xr_aot_runtime_delete(runtime);
}

TEST(aot_parallel_for_range_i64_runs_static_lanes) {
    atomic_store_explicit(&aot_par_for_bad_worker_id, 0, memory_order_relaxed);
    aot_par_for_reset_lane_records();
    ASSERT_TRUE(xr_aot_parallel_for_range_i64(0, 16, 4, aot_par_for_record_lane_body, NULL));
    ASSERT_EQ_INT(atomic_load_explicit(&aot_par_for_bad_worker_id, memory_order_relaxed), 0);
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ_INT(atomic_load_explicit(&aot_par_for_lane_begin[i], memory_order_relaxed),
                      i * 4);
        ASSERT_EQ_INT(atomic_load_explicit(&aot_par_for_lane_end[i], memory_order_relaxed),
                      (i + 1) * 4);
        ASSERT_EQ_INT(atomic_load_explicit(&aot_par_for_lane_calls[i], memory_order_relaxed), 1);
    }

    atomic_store_explicit(&aot_par_for_sum, 0, memory_order_relaxed);
    atomic_store_explicit(&aot_par_for_bad_worker_id, 0, memory_order_relaxed);
    atomic_store_explicit(&aot_par_for_seen_mask, 0, memory_order_relaxed);
    ASSERT_TRUE(xr_aot_parallel_for_range_i64(0, 1000, 8, aot_par_for_range_sum_body, NULL));
    ASSERT_EQ_INT(atomic_load_explicit(&aot_par_for_sum, memory_order_relaxed), 499500);
    ASSERT_EQ_INT(atomic_load_explicit(&aot_par_for_bad_worker_id, memory_order_relaxed), 0);
    ASSERT_TRUE(atomic_load_explicit(&aot_par_for_seen_mask, memory_order_relaxed) != 0);

    atomic_store_explicit(&aot_par_for_sum, 0, memory_order_relaxed);
    atomic_store_explicit(&aot_par_for_bad_worker_id, 0, memory_order_relaxed);
    atomic_store_explicit(&aot_par_for_seen_mask, 0, memory_order_relaxed);
    ASSERT_TRUE(xr_aot_parallel_for_range_i64(10, 14, 1, aot_par_for_range_sum_body, NULL));
    ASSERT_EQ_INT(atomic_load_explicit(&aot_par_for_sum, memory_order_relaxed), 46);
    ASSERT_EQ_INT(atomic_load_explicit(&aot_par_for_bad_worker_id, memory_order_relaxed), 0);
    ASSERT_EQ_INT(atomic_load_explicit(&aot_par_for_seen_mask, memory_order_relaxed), 1);
}

TEST(aot_parallel_reduce_i64_runs_range_reducer) {
    atomic_store_explicit(&aot_par_for_bad_worker_id, 0, memory_order_relaxed);
    atomic_store_explicit(&aot_par_for_seen_mask, 0, memory_order_relaxed);

    int64_t result = -1;
    ASSERT_TRUE(xr_aot_parallel_reduce_i64(0, 1000, 8, 10, aot_par_reduce_range_sum_body,
                                           aot_par_reduce_i64_add, NULL, &result));
    ASSERT_EQ_INT(result, 499510);
    ASSERT_EQ_INT(atomic_load_explicit(&aot_par_for_bad_worker_id, memory_order_relaxed), 0);
    ASSERT_TRUE(atomic_load_explicit(&aot_par_for_seen_mask, memory_order_relaxed) != 0);

    result = -1;
    ASSERT_TRUE(xr_aot_parallel_reduce_i64(5, 5, 8, 123, aot_par_reduce_range_sum_body,
                                           aot_par_reduce_i64_add, NULL, &result));
    ASSERT_EQ_INT(result, 123);

    result = 77;
    ASSERT_FALSE(xr_aot_parallel_reduce_i64(0, 16, 4, 0, aot_par_reduce_failing_body,
                                            aot_par_reduce_i64_add, NULL, &result));
    ASSERT_EQ_INT(result, 77);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Native Coroutine Backend");
RUN_TEST(native_coroutine_uses_native_backend_without_vm_state);
RUN_TEST(aot_coroutine_uses_aot_backend_without_vm_state_and_maps_done);
RUN_TEST(aot_coroutine_maps_block_error_and_cancel_to_common_run_results);
RUN_TEST(aot_coroutine_create_failure_releases_frame);
RUN_TEST(aot_frame_alloc_accepts_zero_state_frames);
RUN_TEST(aot_frame_alloc_reuses_small_frames_locally);
RUN_TEST(aot_runtime_owns_core_without_isolate);
RUN_TEST(aot_runtime_creates_scheduler_for_runtime_caps);
RUN_TEST(aot_runtime_creates_isolate_free_aot_coroutine);
RUN_TEST(aot_run_main_uses_runtime_without_isolate);
RUN_TEST(aot_context_builtin_prefers_runtime_table);
RUN_TEST(aot_runtime_registers_prelude_enums_without_isolate);
RUN_TEST(aot_runtime_copy_context_uses_core_without_isolate);
RUN_TEST(aot_result_group_uses_runtime_without_isolate);
RUN_TEST(aot_work_queue_uses_runtime_owner_without_isolate);
RUN_TEST(aot_channel_uses_runtime_owner_without_isolate);
RUN_TEST(aot_task_await_uses_runtime_owner_without_isolate);
RUN_TEST(runtime_task_one_shot_destroy_unlinks_in_constant_time);
RUN_TEST(runtime_task_one_shot_destroy_reuses_task_handle_locally);
RUN_TEST(runtime_task_deferred_registry_batches_one_shot_handles);
RUN_TEST(runtime_deferred_array_submit_cache_tracks_content_version);
RUN_TEST(coroutine_recycle_hooks_are_backend_abi_contract);
RUN_TEST(aot_parallel_for_range_i64_runs_static_lanes);
RUN_TEST(aot_parallel_reduce_i64_runs_range_reducer);

TEST_MAIN_END()
