/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_coro.h - AOT coroutine runtime bridge
 */

#ifndef XAOT_CORO_H
#define XAOT_CORO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xslot_ref.h"

#ifndef XR_FUNC
#define XR_FUNC extern
#endif

#ifndef XRT_VALUE_H
#include "../runtime/value/xvalue.h"
#endif

struct XrCoroutine;
struct XrCoroGC;
struct XrCoroRegistry;
struct XrCoroState;
struct XrArray;
struct XrChannel;
struct XrMap;
struct XrRuntime;
struct XrRuntimeCore;
struct XrTask;

typedef struct XrAotRuntime XrAotRuntime;
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
    XR_AOT_RUN_DONE = 0,
    XR_AOT_RUN_BLOCKED,
    XR_AOT_RUN_YIELD,
    XR_AOT_RUN_SPAWN_CHILD,
    XR_AOT_RUN_ERROR,
    XR_AOT_RUN_CANCELLED
} XrAotRunKind;

typedef struct XrAotResult {
    XrAotRunKind kind;
    XrValue value;
    XrValue error;
    struct XrCoroutine *child;
    bool error_is_value;
} XrAotResult;

typedef struct XrAotVmHostOps {
    XrValue (*get_builtin)(void *host, int32_t index);
    struct XrRuntimeCore *(*runtime_core)(void *host);
    struct XrRuntime *(*scheduler)(void *host);
    struct XrCoroutine *(*current_coro)(void *host);
    XrValue (*intern_string_value)(void *host, const char *data, size_t len);
    struct XrMap *(*new_map)(void *host, struct XrCoroutine *owner);
    struct XrArray *(*new_array)(void *host, struct XrCoroutine *owner);
    struct XrMap *(*main_locals)(void *host);
    struct XrMap *(*ensure_main_locals)(void *host, struct XrCoroutine *owner);
    struct XrCoroState *(*coro_state)(void *host);
    struct XrChannel *(*new_channel)(void *host, uint32_t buffer_size);
    struct XrChannel *(*new_timer_channel)(void *host, int64_t timeout_ms);
    struct XrChannel *(*monitor)(void *host, struct XrCoroRegistry *registry, const char *name);
    XrValue (*exception_new)(void *host, int code, const char *message);
    bool (*is_exception)(void *host, XrValue value);
    XrValue (*exception_from_value)(void *host, XrValue value);
} XrAotVmHostOps;

typedef struct XrAotContext {
    XrAotRuntime *runtime;
    struct XrCoroutine *coro;
    /* Optional VM-only host bridge; pure runtime AOT keeps both fields NULL. */
    const XrAotVmHostOps *vm_host_ops;
    void *vm_host;
    void *worker;
} XrAotContext;

typedef XrAotResult (*XrAotResumeFn)(void *frame, const XrAotContext *ctx);
typedef void (*XrAotFrameTraceFn)(void *frame, void *visitor);
/* Optional hook; when present, it releases owned fields and frees the frame. */
typedef void (*XrAotFrameReleaseFn)(void *frame, struct XrCoroGC *gc);
typedef void (*XrAotRootVisitFn)(XrValue value, void *ctx);

typedef struct XrAotRootVisitor {
    XrAotRootVisitFn visit;
    void *ctx;
} XrAotRootVisitor;

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
    XrAotResult result;
    result.kind = kind;
    result.value = XR_NULL_VAL;
    result.error = XR_NULL_VAL;
    result.child = NULL;
    result.error_is_value = false;
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

XR_FUNC void *xr_aot_frame_alloc(size_t size);
XR_FUNC void xr_aot_frame_free(void *frame);
XR_FUNC void xr_aot_runtime_config_init(XrAotRuntimeConfig *cfg);
XR_FUNC XrAotRuntime *xr_aot_runtime_new(const XrAotRuntimeConfig *cfg);
XR_FUNC void xr_aot_runtime_delete(XrAotRuntime *runtime);
XR_FUNC uint32_t xr_aot_runtime_caps(const XrAotRuntime *runtime);
XR_FUNC struct XrRuntimeCore *xr_aot_runtime_core(XrAotRuntime *runtime);
XR_FUNC struct XrRuntime *xr_aot_runtime_scheduler(XrAotRuntime *runtime);
XR_FUNC void xr_aot_runtime_enable_transfer(XrAotRuntime *runtime);
XR_FUNC XrValue xr_aot_runtime_builtin(const XrAotRuntime *runtime, int32_t index);
XR_FUNC void xr_aot_runtime_set_builtin(XrAotRuntime *runtime, int32_t index, XrValue value);
XR_FUNC void xr_aot_trace_frame_value(void *visitor, XrValue value);
XR_FUNC void xr_aot_release_frame_value(struct XrCoroGC *gc, XrValue value);
XR_FUNC XrValue xr_aot_get_builtin(const XrAotContext *ctx, int32_t index);
XR_FUNC XrValue xr_aot_load_builtin_field(const XrAotContext *ctx, int32_t index,
                                          const char *field);
XR_FUNC bool xr_aot_runtime_enum_value_info(XrValue value, const char **enum_name,
                                            const char **member_name, uint32_t *member_index,
                                            bool *is_adt, int *payload_count);
XR_FUNC bool xr_aot_runtime_adt_value_info(XrValue value, const char **enum_name,
                                           const char **member_name, uint32_t *member_index,
                                           int *payload_count);
XR_FUNC XrValue xr_aot_runtime_adt_payload(XrValue value, int index);
XR_FUNC XrValue xr_aot_time_now(void);
XR_FUNC XrValue xr_aot_time_monotonic(void);
XR_FUNC XrValue xr_aot_time_nanos(void);
XR_FUNC XrValue xr_aot_time_micros(void);
XR_FUNC XrValue xr_aot_time_clock(void);
XR_FUNC XrValue xr_aot_coro_op(const XrAotContext *ctx, int32_t sub_op, const XrValue *args,
                               int argc);

XR_FUNC struct XrCoroutine *xr_coro_create_aot(XrAotRuntime *runtime, const XrAotCoroDesc *desc,
                                               void *frame, const char *name);

XR_FUNC XrValue xr_aot_run_main(XrAotRuntime *runtime, const XrAotCoroDesc *desc, void *frame);

XR_FUNC XrAotSpawnResult xr_aot_spawn(const XrAotContext *ctx, const XrAotCoroDesc *desc,
                                      void *frame, int link_mode, bool fire_and_forget,
                                      const char *name);

XR_FUNC XrAotResult xr_aot_sleep(const XrAotContext *ctx, int64_t milliseconds);
XR_FUNC XrAotResult xr_aot_scope_enter(const XrAotContext *ctx, uint8_t scope_mode);
XR_FUNC XrAotResult xr_aot_scope_exit(const XrAotContext *ctx, uint8_t scope_mode,
                                      XrValue *out_value);
XR_FUNC XrValue xr_aot_time_after(const XrAotContext *ctx, int64_t milliseconds);
// Release a select-owned `after` timer channel emitted at the select merge.
// Mirrors the VM OP_CHAN_TIMER_DISPOSE handler.
XR_FUNC void xr_aot_chan_timer_dispose(const XrAotContext *ctx, XrValue ch_value);
XR_FUNC XrAotResult xr_aot_select_block(const XrAotContext *ctx, const XrValue *channel_values,
                                        int channel_count, int case_count);
XR_FUNC XrAotResult xr_aot_await_task(const XrAotContext *ctx, XrValue task_value,
                                      XrSlotRef out_slot, int64_t timeout_ms, bool discard_result);
XR_FUNC XrAotResult xr_aot_await_task_resume(const XrAotContext *ctx, XrSlotRef out_slot,
                                             bool discard_result);

XR_FUNC XrValue xr_aot_channel_new(const XrAotContext *ctx, int64_t buffer_size);
XR_FUNC XrValue xr_aot_chan_try_send(const XrAotContext *ctx, XrValue channel_value,
                                     XrValue send_value);
XR_FUNC XrValue xr_aot_chan_try_send_ready(const XrAotContext *ctx, XrValue channel_value,
                                           XrValue send_value);
XR_FUNC XrValue xr_aot_chan_try_send_sync(XrValue channel_value, XrValue send_value);

static inline XrValue xr_aot_chan_try_send_i64(const XrAotContext *ctx, XrValue channel_value,
                                               int64_t send_value) {
    return xr_aot_chan_try_send(ctx, channel_value, XR_FROM_INT(send_value));
}

static inline XrValue xr_aot_chan_try_send_f64(const XrAotContext *ctx, XrValue channel_value,
                                               double send_value) {
    return xr_aot_chan_try_send(ctx, channel_value, XR_FROM_FLOAT(send_value));
}

static inline XrValue xr_aot_chan_try_send_sync_i64(XrValue channel_value, int64_t send_value) {
    return xr_aot_chan_try_send_sync(channel_value, XR_FROM_INT(send_value));
}

static inline XrValue xr_aot_chan_try_send_sync_f64(XrValue channel_value, double send_value) {
    return xr_aot_chan_try_send_sync(channel_value, XR_FROM_FLOAT(send_value));
}

static inline XrValue xr_aot_chan_try_send_ready_i64(const XrAotContext *ctx, XrValue channel_value,
                                                     int64_t send_value) {
    return xr_aot_chan_try_send_ready(ctx, channel_value, XR_FROM_INT(send_value));
}

static inline XrValue xr_aot_chan_try_send_ready_f64(const XrAotContext *ctx, XrValue channel_value,
                                                     double send_value) {
    return xr_aot_chan_try_send_ready(ctx, channel_value, XR_FROM_FLOAT(send_value));
}

XR_FUNC XrValue xr_aot_chan_try_recv(const XrAotContext *ctx, XrValue channel_value);
XR_FUNC XrValue xr_aot_chan_try_recv_sync(XrValue channel_value);
XR_FUNC XrAotResult xr_aot_poll_yield(const XrAotContext *ctx);
XR_FUNC bool xr_aot_send_is_sent(XrValue send_value);
XR_FUNC bool xr_aot_recv_is_value(XrValue recv_value);
XR_FUNC XrValue xr_aot_recv_payload(XrValue recv_value);
XR_FUNC XrValue xr_aot_chan_close(const XrAotContext *ctx, XrValue channel_value);
XR_FUNC XrValue xr_aot_chan_close_sync(XrValue channel_value);
XR_FUNC XrValue xr_aot_chan_length(const XrAotContext *ctx, XrValue channel_value);
XR_FUNC XrValue xr_aot_chan_capacity(const XrAotContext *ctx, XrValue channel_value);
XR_FUNC XrValue xr_aot_chan_is_closed(const XrAotContext *ctx, XrValue channel_value);
XR_FUNC XrValue xr_aot_chan_is_closed_sync(XrValue channel_value);
XR_FUNC XrValue xr_aot_work_queue_new(const XrAotContext *ctx, int64_t shard_count,
                                      int64_t shard_capacity);
XR_FUNC XrValue xr_aot_work_queue_push(const XrAotContext *ctx, XrValue queue_value, XrValue value,
                                       int64_t shard_hint);
XR_FUNC XrValue xr_aot_work_queue_push_sync(XrValue queue_value, XrValue value, int64_t shard_hint);
XR_FUNC bool xr_aot_work_queue_try_pop(const XrAotContext *ctx, XrValue queue_value,
                                       int64_t worker_hint, XrValue *out_value);
XR_FUNC bool xr_aot_work_queue_try_pop_sync(XrValue queue_value, int64_t worker_hint,
                                            XrValue *out_value);
XR_FUNC XrAotResult xr_aot_work_queue_pop(const XrAotContext *ctx, XrValue queue_value,
                                          int64_t worker_hint, XrSlotRef out_slot);
XR_FUNC XrAotResult xr_aot_work_queue_pop_resume(const XrAotContext *ctx, XrSlotRef out_slot);
XR_FUNC XrValue xr_aot_work_queue_close(const XrAotContext *ctx, XrValue queue_value);
XR_FUNC XrValue xr_aot_work_queue_close_sync(XrValue queue_value);
XR_FUNC XrValue xr_aot_work_queue_length(const XrAotContext *ctx, XrValue queue_value);
XR_FUNC XrValue xr_aot_work_queue_shard_count(const XrAotContext *ctx, XrValue queue_value);
XR_FUNC XrValue xr_aot_work_queue_is_closed(const XrAotContext *ctx, XrValue queue_value);
XR_FUNC XrValue xr_aot_work_queue_is_closed_sync(XrValue queue_value);
XR_FUNC XrValue xr_aot_result_group_new(const XrAotContext *ctx, int64_t batch_size);
XR_FUNC XrValue xr_aot_result_group_add(const XrAotContext *ctx, XrValue group_value,
                                        int64_t value);
XR_FUNC XrValue xr_aot_result_group_add_sync(XrValue group_value, int64_t value);
XR_FUNC XrValue xr_aot_result_group_flush(const XrAotContext *ctx, XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_flush_sync(XrValue group_value);
XR_FUNC bool xr_aot_result_group_try_recv(const XrAotContext *ctx, XrValue group_value,
                                          XrValue *out_value);
XR_FUNC bool xr_aot_result_group_try_recv_sync(XrValue group_value, XrValue *out_value);
XR_FUNC XrAotResult xr_aot_result_group_recv(const XrAotContext *ctx, XrValue group_value,
                                             XrSlotRef out_slot);
XR_FUNC XrAotResult xr_aot_result_group_recv_resume(const XrAotContext *ctx, XrSlotRef out_slot);
XR_FUNC XrValue xr_aot_result_group_close(const XrAotContext *ctx, XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_close_sync(XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_length(const XrAotContext *ctx, XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_pending_count(const XrAotContext *ctx, XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_batch_size(const XrAotContext *ctx, XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_is_closed(const XrAotContext *ctx, XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_is_closed_sync(XrValue group_value);
XR_FUNC XrValue xr_aot_tuple_get(const XrAotContext *ctx, XrValue tuple_value, uint16_t index);
XR_FUNC XrAotResult xr_aot_chan_send(const XrAotContext *ctx, XrValue channel_value,
                                     XrValue send_value, XrSlotRef result_slot, int64_t timeout_ms);
XR_FUNC XrAotResult xr_aot_chan_send_i64(const XrAotContext *ctx, XrValue channel_value,
                                         int64_t send_value);
XR_FUNC XrAotResult xr_aot_chan_send_f64(const XrAotContext *ctx, XrValue channel_value,
                                         double send_value);

static inline XrAotResult xr_aot_chan_send_timeout(const XrAotContext *ctx, XrValue channel_value,
                                                   XrValue send_value, int64_t timeout_ms,
                                                   XrSlotRef result_slot) {
    return xr_aot_chan_send(ctx, channel_value, send_value, result_slot, timeout_ms);
}

static inline XrAotResult xr_aot_chan_send_timeout_i64(const XrAotContext *ctx,
                                                       XrValue channel_value, int64_t send_value,
                                                       int64_t timeout_ms, XrSlotRef result_slot) {
    return xr_aot_chan_send(ctx, channel_value, XR_FROM_INT(send_value), result_slot, timeout_ms);
}

static inline XrAotResult xr_aot_chan_send_timeout_f64(const XrAotContext *ctx,
                                                       XrValue channel_value, double send_value,
                                                       int64_t timeout_ms, XrSlotRef result_slot) {
    return xr_aot_chan_send(ctx, channel_value, XR_FROM_FLOAT(send_value), result_slot, timeout_ms);
}

XR_FUNC XrAotResult xr_aot_chan_send_resume(const XrAotContext *ctx, XrSlotRef result_slot,
                                            bool result_value);
XR_FUNC XrAotResult xr_aot_chan_recv_slot(const XrAotContext *ctx, XrValue channel_value,
                                          XrSlotRef out_slot, int64_t timeout_ms);
XR_FUNC XrAotResult xr_aot_chan_recv_slot_resume(const XrAotContext *ctx, XrSlotRef out_slot,
                                                 bool result_value);
XR_FUNC XrAotResult xr_aot_chan_recv_pair(const XrAotContext *ctx, XrValue channel_value,
                                          XrSlotRef value_slot, XrSlotRef ok_slot,
                                          int64_t timeout_ms);
XR_FUNC XrAotResult xr_aot_chan_recv_pair_i64(const XrAotContext *ctx, XrValue channel_value,
                                              XrSlotRef value_slot, XrSlotRef ok_slot);
XR_FUNC XrAotResult xr_aot_chan_recv_pair_f64(const XrAotContext *ctx, XrValue channel_value,
                                              XrSlotRef value_slot, XrSlotRef ok_slot);
XR_FUNC XrAotResult xr_aot_chan_recv_pair_resume(const XrAotContext *ctx, XrSlotRef value_slot,
                                                 XrSlotRef ok_slot);

#endif  // XAOT_CORO_H
