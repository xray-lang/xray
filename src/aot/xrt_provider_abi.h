/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_provider_abi.h - freestanding AOT executor/provider ABI v1
 *
 * Included by xrt_core_freestanding.h only when the verified EntryPlan selected
 * a target runtime provider.  This header deliberately contains declarations
 * and value-only helpers, never a hosted runtime implementation.  The target
 * supplies the referenced symbols; ordinary link resolution is therefore the
 * exact required-symbol verifier for the reachable generated code.
 */

#ifndef XRT_PROVIDER_ABI_H
#define XRT_PROVIDER_ABI_H

#ifndef XRAY_TARGET_RUNTIME_PROVIDER
#error "xrt_provider_abi.h requires XRAY_TARGET_RUNTIME_PROVIDER"
#endif
#if !defined(XRAY_PROVIDER_ABI) || XRAY_PROVIDER_ABI != 1
#error "unsupported Xray freestanding runtime provider ABI"
#endif
#ifndef XRAY_PROVIDER_REQUIRED_CAPS
#error "missing verified provider capability contract"
#endif
#ifndef XRAY_PROVIDER_REQUIRED_HOOKS
#error "missing verified provider hook contract"
#endif
#ifndef XRAY_PROVIDER_TARGET_METADATA_HASH
#error "missing verified provider target metadata hash"
#endif

struct XrCoroHeap;
struct XrRuntime;
struct XrRuntimeCore;

typedef void (*XrAotRuntimeConfigureCoreFn)(struct XrRuntimeCore *core, uint32_t caps,
                                            void *userdata);

typedef enum {
    XR_AOT_CAP_NONE = 0,
    XR_AOT_CAP_CORO = 1u << 0,
    XR_AOT_CAP_TIMER = 1u << 1,
    XR_AOT_CAP_CHANNEL = 1u << 2,
    XR_AOT_CAP_WORK_QUEUE = 1u << 3,
    XR_AOT_CAP_RESULT_GROUP = 1u << 4,
    XR_AOT_CAP_PROCESS = 1u << 5,
    XR_AOT_CAP_TRANSFER = 1u << 6,
    XR_AOT_CAP_TASK = 1u << 7,
    XR_AOT_CAP_OBJECTS = 1u << 8,
    XR_AOT_CAP_ATOMIC = 1u << 9,
    XR_AOT_CAP_COUNTDOWN_LATCH = 1u << 10,
    XR_AOT_CAP_SEMAPHORE = 1u << 11,
    XR_AOT_CAP_EVENT_COUNT = 1u << 12,
} XrAotRuntimeCap;

typedef struct XrAotRuntimeConfig {
    uint32_t caps;
    int scheduler_workers;
    int argc;
    char **argv;
    const char *file;
    void *userdata;
    XrAotRuntimeConfigureCoreFn configure_core;
} XrAotRuntimeConfig;

typedef enum {
    XR_SLOT_NONE = 0,
    XR_SLOT_XVALUE_PTR,
    XR_SLOT_NATIVE_PTR,
    XR_SLOT_AOT_FRAME_OFFSET
} XrSlotKind;

typedef struct XrSlotRef {
    XrSlotKind kind;
    void *base;
    uint32_t offset;
    uint16_t type_id;
} XrSlotRef;

#ifndef XR_REP_TAGGED
#define XR_REP_TAGGED 3
#endif

static inline XrSlotRef xr_slot_none(void) {
    return (XrSlotRef) {XR_SLOT_NONE, NULL, 0, 0};
}

static inline XrSlotRef xr_slot_xvalue_ptr(XrValue *ptr) {
    return (XrSlotRef) {XR_SLOT_XVALUE_PTR, ptr, 0, XR_REP_TAGGED};
}

static inline XrSlotRef xr_slot_native_ptr(void *ptr, uint16_t type_id) {
    return (XrSlotRef) {XR_SLOT_NATIVE_PTR, ptr, 0, type_id};
}

static inline XrSlotRef xr_slot_aot_frame_offset(void *base, uint32_t offset, uint16_t type_id) {
    return (XrSlotRef) {XR_SLOT_AOT_FRAME_OFFSET, base, offset, type_id};
}

typedef XrAotResult (*XrAotResumeFn)(void *frame, const XrAotContext *ctx);
typedef void (*XrAotFrameTraceFn)(void *frame, void *visitor);
typedef void (*XrAotFrameReleaseFn)(void *frame, struct XrCoroHeap *heap);

typedef struct XrAotCoroDesc {
    const char *name;
    size_t frame_size;
    uint32_t root_count;
    uint32_t release_count;
    XrAotResumeFn resume;
    XrAotFrameTraceFn trace_roots;
    XrAotFrameReleaseFn release_frame;
} XrAotCoroDesc;

typedef struct XrAotSpawnResult {
    XrValue task_value;
    struct XrCoroutine *child;
} XrAotSpawnResult;

static inline XrAotResult xr_aot_result(XrAotRunKind kind) {
    XrAotResult result = {0};
    result.kind = kind;
    result.value = XR_NULL_VAL;
    result.error = XR_NULL_VAL;
    return result;
}

static inline XrAotResult xr_aot_done(XrValue value) {
    XrAotResult result = xr_aot_result(XR_AOT_RUN_DONE);
    result.value = value;
    return result;
}

static inline XrAotResult xr_aot_blocked(void) {
    return xr_aot_result(XR_AOT_RUN_BLOCKED);
}

static inline XrAotResult xr_aot_yielded(void) {
    return xr_aot_result(XR_AOT_RUN_YIELD);
}

static inline XrAotResult xr_aot_gen_yielded(XrValue value) {
    XrAotResult result = xr_aot_result(XR_AOT_RUN_GEN_YIELD);
    result.value = value;
    return result;
}

static inline XrAotResult xr_aot_spawn_child(struct XrCoroutine *child) {
    XrAotResult result = xr_aot_result(XR_AOT_RUN_SPAWN_CHILD);
    result.child = child;
    return result;
}

static inline XrAotResult xr_aot_error(XrValue error, bool error_is_value) {
    XrAotResult result = xr_aot_result(XR_AOT_RUN_ERROR);
    result.error = error;
    result.error_is_value = error_is_value;
    return result;
}

/* Provider-owned allocation, task, suspension, and root-executor surface. */
typedef struct XrAotRuntimeInfo {
    int64_t live_bytes;
    double live_kb;
    int64_t live_objects;
    int64_t finalizer_count;
    int64_t blocks;
    int64_t free_blocks;
    int64_t full_blocks;
} XrAotRuntimeInfo;

XR_FUNC void *xr_aot_frame_alloc(size_t size);
XR_FUNC void xr_aot_frame_free(void *frame);
XR_FUNC void xr_aot_runtime_config_init(XrAotRuntimeConfig *cfg);
XR_FUNC XrAotRuntime *xr_aot_runtime_new(const XrAotRuntimeConfig *cfg);
XR_FUNC void xr_aot_runtime_delete(XrAotRuntime *runtime);
XR_FUNC int64_t xr_aot_runtime_collect_cycles(const XrAotContext *ctx);
XR_FUNC void xr_aot_runtime_disable_cycle_collection(const XrAotContext *ctx);
XR_FUNC void xr_aot_runtime_enable_cycle_collection(const XrAotContext *ctx);
XR_FUNC int64_t xr_aot_runtime_live_bytes(const XrAotContext *ctx);
XR_FUNC int64_t xr_aot_runtime_live_objects(const XrAotContext *ctx);
XR_FUNC XrAotRuntimeInfo xr_aot_runtime_info(const XrAotContext *ctx);
XR_FUNC int64_t xr_aot_test_yield_simple(void);
XR_FUNC int64_t xr_aot_test_yield_add(int64_t a, int64_t b);
XR_FUNC int64_t xr_aot_test_yield_sync(void);
XR_FUNC int64_t xr_aot_test_yield_blocking_sleep(int64_t milliseconds);
XR_FUNC void xr_aot_test_yield_counter_inc(void);
XR_FUNC int64_t xr_aot_test_yield_counter_get(void);
XR_FUNC int64_t xr_aot_test_yield_counter_reset(void);
XR_FUNC XrValue xr_aot_run_main(XrAotRuntime *runtime, const XrAotCoroDesc *desc, void *frame);
XR_FUNC bool xr_aot_root_descriptor_begin(XrAotRuntime *runtime);
XR_FUNC bool xr_aot_root_descriptor_end(XrAotRuntime *runtime);
XR_FUNC XrAotSpawnResult xr_aot_spawn(const XrAotContext *ctx, const XrAotCoroDesc *desc,
                                      void *frame, int link_mode, bool fire_and_forget,
                                      bool one_shot_await, bool result_copy_shared,
                                      const char *name);
XR_FUNC XrAotSpawnResult xr_aot_spawn_deferred(const XrAotContext *ctx, const XrAotCoroDesc *desc,
                                               void *frame, int link_mode, bool fire_and_forget,
                                               bool one_shot_await, bool result_copy_shared,
                                               const char *name);
XR_FUNC XrAotResult xr_aot_await_task(const XrAotContext *ctx, XrValue task_value,
                                      XrSlotRef out_slot, int64_t timeout_ms, bool discard_result,
                                      bool one_shot_await);
XR_FUNC XrAotResult xr_aot_await_task_resume(const XrAotContext *ctx, XrSlotRef out_slot,
                                             bool discard_result, bool one_shot_await);
XR_FUNC XrAotResult xr_aot_sleep(const XrAotContext *ctx, int64_t milliseconds);
XR_FUNC XrValue xr_aot_time_after(const XrAotContext *ctx, int64_t milliseconds);
XR_FUNC XrAotResult xr_aot_poll_yield(const XrAotContext *ctx);
XR_FUNC void xr_aot_trace_frame_value(void *visitor, XrValue value);
XR_FUNC void xr_aot_release_frame_value(struct XrCoroHeap *heap, XrValue value);

/* Objects created by reachable coroutine lowering are provider-owned too. */
XR_FUNC XrValue xrt_closure_new(const XrAotCallableDesc *callable, int nupvals);
XR_FUNC void xr_runtime_core_enable_object_destroy_ops(struct XrRuntimeCore *core);
XR_FUNC void xr_runtime_core_enable_task_destroy_ops(struct XrRuntimeCore *core);

/* Provider values already use the freestanding XrValue representation. */
static inline XrValue xr_aot_bridge_value_to_xrt(XrValue value) {
    return value;
}

#endif /* XRT_PROVIDER_ABI_H */
