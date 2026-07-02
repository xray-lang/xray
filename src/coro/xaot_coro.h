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
#include "../runtime/value/xtransfer_mode.h"

struct XrCoroutine;
struct XrCoroHeap;
struct XrCoroRegistry;
struct XrCoroState;
struct XrArray;
struct XrChannel;
struct XrMap;
struct XrRuntime;
struct XrRuntimeCore;
struct XrTask;
struct xrt_closure;

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
    XR_AOT_CAP_ATOMIC = 1u << 9,
    XR_AOT_CAP_COUNTDOWN_LATCH = 1u << 10,
    XR_AOT_CAP_SEMAPHORE = 1u << 11,
    XR_AOT_CAP_EVENT_COUNT = 1u << 12,
} XrAotRuntimeCap;

typedef enum {
    XR_AOT_ORDERING_RELAXED = 0,
    XR_AOT_ORDERING_ACQUIRE = 1,
    XR_AOT_ORDERING_RELEASE = 2,
    XR_AOT_ORDERING_ACQUIRE_RELEASE = 3,
    XR_AOT_ORDERING_SEQ_CST = 4,
} XrAotAtomicOrdering;

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
    XR_AOT_RUN_CANCELLED,
    XR_AOT_RUN_GEN_YIELD  // generator `yield expr`: .value carries the yielded element
} XrAotRunKind;

typedef enum {
    XR_AOT_GEN_DRIVE_DONE = 0,
    XR_AOT_GEN_DRIVE_YIELD,
    XR_AOT_GEN_DRIVE_ERROR
} XrAotGenDriveKind;

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
typedef void (*XrAotFrameReleaseFn)(void *frame, struct XrCoroHeap *heap);
typedef void (*XrAotRootVisitFn)(XrValue value, void *ctx);
typedef void (*XrAotParForRangeI64Fn)(struct xrt_closure *closure, int64_t begin, int64_t end,
                                      int64_t worker_id);
typedef bool (*XrAotParReduceRangeI64Fn)(struct xrt_closure *closure, int64_t begin, int64_t end,
                                         int64_t worker_id, int64_t *out);
typedef int64_t (*XrAotParReduceCombineI64Fn)(struct xrt_closure *closure, int64_t acc,
                                              int64_t value);
typedef bool (*XrAotParReduceRangeAggFn)(struct xrt_closure *closure, int64_t begin, int64_t end,
                                         int64_t worker_id, void *out);
typedef void (*XrAotParReduceCombineAggFn)(struct xrt_closure *closure, void *acc,
                                           const void *value);

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

#ifndef XR_AOT_LOOP_POLL_INTERVAL
#define XR_AOT_LOOP_POLL_INTERVAL 64
#endif

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

// Generator value yield: hand `value` to the driving iterator and suspend the
// producer. Distinct from xr_aot_yielded() (cooperative scheduling) so the
// generator drive can read the element and never re-enqueues on a worker.
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
XR_FUNC XrValue xr_aot_runtime_builtin_lazy(XrAotRuntime *runtime, int32_t index);
XR_FUNC void xr_aot_runtime_set_builtin(XrAotRuntime *runtime, int32_t index, XrValue value);
XR_FUNC void xr_aot_trace_frame_value(void *visitor, XrValue value);
XR_FUNC void xr_aot_release_frame_value(struct XrCoroHeap *heap, XrValue value);
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
XR_FUNC void xr_coro_destroy(struct XrCoroutine *coro);

// Pull a generator coroutine to its next yielded value without exposing the
// scheduler/backend ABI to AOT collection helpers.
XR_FUNC XrAotGenDriveKind xr_aot_gen_drive(struct XrCoroutine *coro, XrValue *out,
                                           bool *out_error_is_value);

XR_FUNC XrValue xr_aot_run_main(XrAotRuntime *runtime, const XrAotCoroDesc *desc, void *frame);

XR_FUNC XrAotSpawnResult xr_aot_spawn(const XrAotContext *ctx, const XrAotCoroDesc *desc,
                                      void *frame, int link_mode, bool fire_and_forget,
                                      bool one_shot_await, const char *name);
XR_FUNC XrAotSpawnResult xr_aot_spawn_deferred(const XrAotContext *ctx, const XrAotCoroDesc *desc,
                                               void *frame, int link_mode, bool fire_and_forget,
                                               bool one_shot_await, const char *name);
XR_FUNC bool xr_aot_parallel_for_range_i64(int64_t start, int64_t end, int64_t workers,
                                           XrAotParForRangeI64Fn body, struct xrt_closure *closure);
XR_FUNC bool xr_aot_parallel_reduce_i64(int64_t start, int64_t end, int64_t workers,
                                        int64_t initial, XrAotParReduceRangeI64Fn body,
                                        XrAotParReduceCombineI64Fn combine,
                                        struct xrt_closure *closure, int64_t *out);
XR_FUNC bool xr_aot_parallel_reduce_agg(int64_t start, int64_t end, int64_t workers,
                                        size_t value_size, const void *initial,
                                        XrAotParReduceRangeAggFn body,
                                        XrAotParReduceCombineAggFn combine,
                                        struct xrt_closure *closure, void *out);

// Construct a coroutine-backed iterator over a generator function. The producer
// coroutine is created from desc+frame but NOT scheduled; it is pull-driven by
// the returned iterator's hasNext()/next(). Returns the iterator as an XrValue.
XR_FUNC XrValue xr_aot_gen_iterator_new(const XrAotContext *ctx, const XrAotCoroDesc *desc,
                                        void *frame);

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
                                      XrSlotRef out_slot, int64_t timeout_ms, bool discard_result,
                                      bool one_shot_await);
XR_FUNC XrAotResult xr_aot_await_deferred_task_from_array(const XrAotContext *ctx,
                                                          XrValue tasks_value, int64_t task_index,
                                                          XrValue task_value, XrSlotRef out_slot,
                                                          int64_t timeout_ms, bool discard_result,
                                                          bool one_shot_await);
XR_FUNC XrAotResult xr_aot_await_task_resume(const XrAotContext *ctx, XrSlotRef out_slot,
                                             bool discard_result, bool one_shot_await);
XR_FUNC XrAotResult xr_aot_await_deferred_task_from_array_resume(
    const XrAotContext *ctx, XrValue tasks_value, int64_t task_index, XrSlotRef out_slot,
    bool discard_result, bool one_shot_await);
XR_FUNC void xr_aot_submit_deferred_tasks(const XrAotContext *ctx, XrValue tasks_value);
XR_FUNC void xr_aot_submit_deferred_tasks_cached(const XrAotContext *ctx, XrValue tasks_value);
XR_FUNC void xr_aot_submit_deferred_spawns(const XrAotContext *ctx);
XR_FUNC int64_t xr_aot_await_all_tasks_count(const XrAotContext *ctx, XrValue tasks_value);
XR_FUNC XrAotResult xr_aot_await_all_tasks_wait(const XrAotContext *ctx, XrValue tasks_value);
XR_FUNC XrAotResult xr_aot_await_all_tasks_wait_resume(const XrAotContext *ctx,
                                                       XrValue tasks_value);
XR_FUNC bool xr_aot_await_all_tasks_collect_into_array(const XrAotContext *ctx, XrValue tasks_value,
                                                       XrValue results_value,
                                                       uint8_t result_elem_type,
                                                       bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_tasks_into_array(const XrAotContext *ctx, XrValue tasks_value,
                                                      XrValue results_value,
                                                      uint8_t result_elem_type,
                                                      bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_tasks_into_array_resume(const XrAotContext *ctx,
                                                             XrValue tasks_value,
                                                             XrValue results_value,
                                                             uint8_t result_elem_type,
                                                             bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_tasks(const XrAotContext *ctx, XrValue tasks_value,
                                           XrSlotRef out_slot, uint8_t result_elem_type,
                                           bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_tasks_resume(const XrAotContext *ctx, XrValue tasks_value,
                                                  XrSlotRef out_slot, uint8_t result_elem_type,
                                                  bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_task_values(const XrAotContext *ctx,
                                                 const XrValue *task_values, int task_count,
                                                 XrSlotRef out_slot, uint8_t result_elem_type,
                                                 bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_task_values_resume(const XrAotContext *ctx,
                                                        const XrValue *task_values, int task_count,
                                                        XrSlotRef out_slot,
                                                        uint8_t result_elem_type,
                                                        bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_task_values_to_slots(
    const XrAotContext *ctx, const XrValue *task_values, int task_count,
    const XrSlotRef *result_slots, uint8_t result_elem_type, bool aggregate_one_shot);
XR_FUNC XrAotResult xr_aot_await_all_task_values_to_slots_resume(
    const XrAotContext *ctx, const XrValue *task_values, int task_count,
    const XrSlotRef *result_slots, uint8_t result_elem_type, bool aggregate_one_shot);

XR_FUNC XrValue xr_aot_channel_new(const XrAotContext *ctx, int64_t buffer_size);
XR_FUNC XrValue xr_aot_chan_try_send(const XrAotContext *ctx, XrValue channel_value,
                                     XrValue send_value);
XR_FUNC XrValue xr_aot_chan_try_send_transfer(const XrAotContext *ctx, XrValue channel_value,
                                              XrValue send_value, uint8_t transfer_mode);
XR_FUNC XrValue xr_aot_chan_try_send_ready(const XrAotContext *ctx, XrValue channel_value,
                                           XrValue send_value);
XR_FUNC XrValue xr_aot_chan_try_send_ready_transfer(const XrAotContext *ctx, XrValue channel_value,
                                                    XrValue send_value, uint8_t transfer_mode);
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
XR_FUNC XrAotRunKind xr_aot_poll_yield_kind_cost(const XrAotContext *ctx, int32_t cost);
XR_FUNC XrAotRunKind xr_aot_poll_yield_kind(const XrAotContext *ctx);
XR_FUNC XrAotResult xr_aot_poll_yield(const XrAotContext *ctx);
XR_FUNC void xr_aot_sync_backedge_heartbeat(void);
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
XR_FUNC bool xr_aot_work_queue_push_bool(const XrAotContext *ctx, XrValue queue_value,
                                         XrValue value, int64_t shard_hint);
XR_FUNC bool xr_aot_work_queue_push_bool_sync(XrValue queue_value, XrValue value,
                                              int64_t shard_hint);
XR_FUNC XrValue xr_aot_work_queue_push_range(const XrAotContext *ctx, XrValue queue_value,
                                             int64_t start, int64_t count, int64_t shard_start);
XR_FUNC XrValue xr_aot_work_queue_push_range_sync(XrValue queue_value, int64_t start, int64_t count,
                                                  int64_t shard_start);
XR_FUNC int64_t xr_aot_work_queue_push_range_i64(const XrAotContext *ctx, XrValue queue_value,
                                                 int64_t start, int64_t count, int64_t shard_start);
XR_FUNC int64_t xr_aot_work_queue_push_range_i64_sync(XrValue queue_value, int64_t start,
                                                      int64_t count, int64_t shard_start);
XR_FUNC bool xr_aot_work_queue_try_pop(const XrAotContext *ctx, XrValue queue_value,
                                       int64_t worker_hint, XrValue *out_value);
XR_FUNC bool xr_aot_work_queue_try_pop_sync(XrValue queue_value, int64_t worker_hint,
                                            XrValue *out_value);
XR_FUNC XrAotResult xr_aot_work_queue_pop(const XrAotContext *ctx, XrValue queue_value,
                                          int64_t worker_hint, XrSlotRef out_slot);
XR_FUNC XrAotResult xr_aot_work_queue_pop_value(const XrAotContext *ctx, XrValue queue_value,
                                                int64_t worker_hint, XrValue *out_value);
XR_FUNC XrAotResult xr_aot_work_queue_pop_i64_optional(const XrAotContext *ctx, XrValue queue_value,
                                                       int64_t worker_hint, int64_t *out_value,
                                                       bool *out_has);
XR_FUNC XrAotResult xr_aot_work_queue_pop_resume(const XrAotContext *ctx, XrSlotRef out_slot);
XR_FUNC XrAotResult xr_aot_work_queue_pop_value_resume(const XrAotContext *ctx, XrValue *out_value);
XR_FUNC XrAotResult xr_aot_work_queue_pop_i64_optional_resume(const XrAotContext *ctx,
                                                              int64_t *out_value, bool *out_has);
XR_FUNC XrValue xr_aot_work_queue_close(const XrAotContext *ctx, XrValue queue_value);
XR_FUNC XrValue xr_aot_work_queue_close_sync(XrValue queue_value);
XR_FUNC void xr_aot_work_queue_close_void(const XrAotContext *ctx, XrValue queue_value);
XR_FUNC void xr_aot_work_queue_close_void_sync(XrValue queue_value);
XR_FUNC XrValue xr_aot_work_queue_length(const XrAotContext *ctx, XrValue queue_value);
XR_FUNC XrValue xr_aot_work_queue_shard_count(const XrAotContext *ctx, XrValue queue_value);
XR_FUNC XrValue xr_aot_work_queue_is_closed(const XrAotContext *ctx, XrValue queue_value);
XR_FUNC XrValue xr_aot_work_queue_is_closed_sync(XrValue queue_value);
XR_FUNC int64_t xr_aot_atomic_ordering_from_value(XrValue value);
XR_FUNC XrValue xr_aot_atomic_new_i64(const XrAotContext *ctx, int64_t initial);
XR_FUNC XrValue xr_aot_atomic_new_f64(const XrAotContext *ctx, double initial);
XR_FUNC XrValue xr_aot_atomic_new_bool(const XrAotContext *ctx, bool initial);
XR_FUNC XrValue xr_aot_atomic_load_value(XrValue atomic_value, int64_t ordering);
XR_FUNC void xr_aot_atomic_store_value(XrValue atomic_value, XrValue value, int64_t ordering);
XR_FUNC int64_t xr_aot_atomic_load_i64(XrValue atomic_value, int64_t ordering);
XR_FUNC double xr_aot_atomic_load_f64(XrValue atomic_value, int64_t ordering);
XR_FUNC bool xr_aot_atomic_load_bool(XrValue atomic_value, int64_t ordering);
XR_FUNC void xr_aot_atomic_store_i64(XrValue atomic_value, int64_t value, int64_t ordering);
XR_FUNC void xr_aot_atomic_store_f64(XrValue atomic_value, double value, int64_t ordering);
XR_FUNC void xr_aot_atomic_store_bool(XrValue atomic_value, bool value, int64_t ordering);
XR_FUNC void xr_aot_atomic_add_i64(XrValue atomic_value, int64_t delta, int64_t ordering);
XR_FUNC void xr_aot_atomic_sub_i64(XrValue atomic_value, int64_t delta, int64_t ordering);
XR_FUNC void xr_aot_atomic_add_f64(XrValue atomic_value, double delta, int64_t ordering);
XR_FUNC void xr_aot_atomic_sub_f64(XrValue atomic_value, double delta, int64_t ordering);
XR_FUNC int64_t xr_aot_atomic_fetch_add_i64(XrValue atomic_value, int64_t delta, int64_t ordering);
XR_FUNC int64_t xr_aot_atomic_fetch_sub_i64(XrValue atomic_value, int64_t delta, int64_t ordering);
XR_FUNC double xr_aot_atomic_fetch_add_f64(XrValue atomic_value, double delta, int64_t ordering);
XR_FUNC double xr_aot_atomic_fetch_sub_f64(XrValue atomic_value, double delta, int64_t ordering);
XR_FUNC int64_t xr_aot_atomic_swap_i64(XrValue atomic_value, int64_t desired, int64_t ordering);
XR_FUNC double xr_aot_atomic_swap_f64(XrValue atomic_value, double desired, int64_t ordering);
XR_FUNC bool xr_aot_atomic_swap_bool(XrValue atomic_value, bool desired, int64_t ordering);
XR_FUNC bool xr_aot_atomic_compare_exchange_i64(XrValue atomic_value, int64_t expected,
                                                int64_t desired, int64_t ordering,
                                                int64_t *out_previous);
XR_FUNC bool xr_aot_atomic_compare_exchange_f64(XrValue atomic_value, double expected,
                                                double desired, int64_t ordering,
                                                double *out_previous);
XR_FUNC bool xr_aot_atomic_compare_exchange_bool(XrValue atomic_value, bool expected, bool desired,
                                                 int64_t ordering, bool *out_previous);
XR_FUNC bool xr_aot_atomic_toggle_bool(XrValue atomic_value, int64_t ordering);
XR_FUNC XrValue xr_aot_result_group_new(const XrAotContext *ctx, int64_t batch_size);
XR_FUNC XrValue xr_aot_result_group_add(const XrAotContext *ctx, XrValue group_value,
                                        int64_t value);
XR_FUNC XrValue xr_aot_result_group_add_sync(XrValue group_value, int64_t value);
XR_FUNC bool xr_aot_result_group_add_bool(const XrAotContext *ctx, XrValue group_value,
                                          int64_t value);
XR_FUNC bool xr_aot_result_group_add_bool_sync(XrValue group_value, int64_t value);
XR_FUNC XrValue xr_aot_result_group_flush(const XrAotContext *ctx, XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_flush_sync(XrValue group_value);
XR_FUNC void xr_aot_result_group_flush_void(const XrAotContext *ctx, XrValue group_value);
XR_FUNC void xr_aot_result_group_flush_void_sync(XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_reset(const XrAotContext *ctx, XrValue group_value,
                                          int64_t batch_size);
XR_FUNC XrValue xr_aot_result_group_reset_sync(XrValue group_value, int64_t batch_size);
XR_FUNC bool xr_aot_result_group_reset_bool(const XrAotContext *ctx, XrValue group_value,
                                            int64_t batch_size);
XR_FUNC bool xr_aot_result_group_reset_bool_sync(XrValue group_value, int64_t batch_size);
XR_FUNC bool xr_aot_result_group_try_recv(const XrAotContext *ctx, XrValue group_value,
                                          XrValue *out_value);
XR_FUNC bool xr_aot_result_group_try_recv_sync(XrValue group_value, XrValue *out_value);
XR_FUNC XrAotResult xr_aot_result_group_recv(const XrAotContext *ctx, XrValue group_value,
                                             XrSlotRef out_slot);
XR_FUNC XrAotResult xr_aot_result_group_recv_value(const XrAotContext *ctx, XrValue group_value,
                                                   XrValue *out_value);
XR_FUNC XrAotResult xr_aot_result_group_recv_i64_optional(const XrAotContext *ctx,
                                                          XrValue group_value, int64_t *out_value,
                                                          bool *out_has);
XR_FUNC XrAotResult xr_aot_result_group_recv_resume(const XrAotContext *ctx, XrSlotRef out_slot);
XR_FUNC XrAotResult xr_aot_result_group_recv_value_resume(const XrAotContext *ctx,
                                                          XrValue *out_value);
XR_FUNC XrAotResult xr_aot_result_group_recv_i64_optional_resume(const XrAotContext *ctx,
                                                                 int64_t *out_value, bool *out_has);
XR_FUNC XrValue xr_aot_result_group_close(const XrAotContext *ctx, XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_close_sync(XrValue group_value);
XR_FUNC void xr_aot_result_group_close_void(const XrAotContext *ctx, XrValue group_value);
XR_FUNC void xr_aot_result_group_close_void_sync(XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_length(const XrAotContext *ctx, XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_pending_count(const XrAotContext *ctx, XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_batch_size(const XrAotContext *ctx, XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_is_closed(const XrAotContext *ctx, XrValue group_value);
XR_FUNC XrValue xr_aot_result_group_is_closed_sync(XrValue group_value);
XR_FUNC XrValue xr_aot_countdown_latch_new(const XrAotContext *ctx, int64_t count);
XR_FUNC XrValue xr_aot_countdown_latch_reset(const XrAotContext *ctx, XrValue latch_value,
                                             int64_t count);
XR_FUNC XrValue xr_aot_countdown_latch_reset_sync(XrValue latch_value, int64_t count);
XR_FUNC bool xr_aot_countdown_latch_reset_bool(const XrAotContext *ctx, XrValue latch_value,
                                               int64_t count);
XR_FUNC bool xr_aot_countdown_latch_reset_bool_sync(XrValue latch_value, int64_t count);
XR_FUNC XrValue xr_aot_countdown_latch_done(const XrAotContext *ctx, XrValue latch_value,
                                            int64_t count);
XR_FUNC XrValue xr_aot_countdown_latch_done_sync(XrValue latch_value, int64_t count);
XR_FUNC int64_t xr_aot_countdown_latch_done_i64(const XrAotContext *ctx, XrValue latch_value,
                                                int64_t count);
XR_FUNC int64_t xr_aot_countdown_latch_done_i64_sync(XrValue latch_value, int64_t count);
XR_FUNC XrValue xr_aot_countdown_latch_try_wait(const XrAotContext *ctx, XrValue latch_value);
XR_FUNC XrValue xr_aot_countdown_latch_try_wait_sync(XrValue latch_value);
XR_FUNC bool xr_aot_countdown_latch_try_wait_bool(const XrAotContext *ctx, XrValue latch_value);
XR_FUNC bool xr_aot_countdown_latch_try_wait_bool_sync(XrValue latch_value);
XR_FUNC XrAotResult xr_aot_countdown_latch_wait(const XrAotContext *ctx, XrValue latch_value,
                                                XrSlotRef out_slot);
XR_FUNC XrAotResult xr_aot_countdown_latch_wait_resume(const XrAotContext *ctx, XrSlotRef out_slot);
XR_FUNC XrValue xr_aot_countdown_latch_close(const XrAotContext *ctx, XrValue latch_value);
XR_FUNC XrValue xr_aot_countdown_latch_close_sync(XrValue latch_value);
XR_FUNC void xr_aot_countdown_latch_close_void(const XrAotContext *ctx, XrValue latch_value);
XR_FUNC void xr_aot_countdown_latch_close_void_sync(XrValue latch_value);
XR_FUNC XrValue xr_aot_countdown_latch_remaining(const XrAotContext *ctx, XrValue latch_value);
XR_FUNC XrValue xr_aot_countdown_latch_is_closed(const XrAotContext *ctx, XrValue latch_value);
XR_FUNC XrValue xr_aot_countdown_latch_is_closed_sync(XrValue latch_value);
XR_FUNC XrValue xr_aot_semaphore_new(const XrAotContext *ctx, int64_t permits);
XR_FUNC XrValue xr_aot_semaphore_release(const XrAotContext *ctx, XrValue semaphore_value,
                                         int64_t count);
XR_FUNC XrValue xr_aot_semaphore_release_sync(XrValue semaphore_value, int64_t count);
XR_FUNC int64_t xr_aot_semaphore_release_i64(const XrAotContext *ctx, XrValue semaphore_value,
                                             int64_t count);
XR_FUNC int64_t xr_aot_semaphore_release_i64_sync(XrValue semaphore_value, int64_t count);
XR_FUNC XrValue xr_aot_semaphore_try_acquire(const XrAotContext *ctx, XrValue semaphore_value);
XR_FUNC XrValue xr_aot_semaphore_try_acquire_sync(XrValue semaphore_value);
XR_FUNC bool xr_aot_semaphore_try_acquire_bool(const XrAotContext *ctx, XrValue semaphore_value);
XR_FUNC bool xr_aot_semaphore_try_acquire_bool_sync(XrValue semaphore_value);
XR_FUNC XrAotResult xr_aot_semaphore_acquire(const XrAotContext *ctx, XrValue semaphore_value,
                                             XrSlotRef out_slot);
XR_FUNC XrAotResult xr_aot_semaphore_acquire_resume(const XrAotContext *ctx, XrSlotRef out_slot);
XR_FUNC XrValue xr_aot_semaphore_close(const XrAotContext *ctx, XrValue semaphore_value);
XR_FUNC XrValue xr_aot_semaphore_close_sync(XrValue semaphore_value);
XR_FUNC void xr_aot_semaphore_close_void(const XrAotContext *ctx, XrValue semaphore_value);
XR_FUNC void xr_aot_semaphore_close_void_sync(XrValue semaphore_value);
XR_FUNC XrValue xr_aot_semaphore_available(const XrAotContext *ctx, XrValue semaphore_value);
XR_FUNC XrValue xr_aot_semaphore_is_closed(const XrAotContext *ctx, XrValue semaphore_value);
XR_FUNC XrValue xr_aot_semaphore_is_closed_sync(XrValue semaphore_value);
XR_FUNC XrValue xr_aot_event_count_new(const XrAotContext *ctx, int64_t epoch);
XR_FUNC XrValue xr_aot_event_count_advance(const XrAotContext *ctx, XrValue event_value,
                                           int64_t step);
XR_FUNC XrValue xr_aot_event_count_advance_sync(XrValue event_value, int64_t step);
XR_FUNC int64_t xr_aot_event_count_advance_i64(const XrAotContext *ctx, XrValue event_value,
                                               int64_t step);
XR_FUNC int64_t xr_aot_event_count_advance_i64_sync(XrValue event_value, int64_t step);
XR_FUNC XrAotResult xr_aot_event_count_wait(const XrAotContext *ctx, XrValue event_value,
                                            int64_t last_epoch, int64_t worker_hint,
                                            XrSlotRef out_slot);
XR_FUNC XrAotResult xr_aot_event_count_wait_resume(const XrAotContext *ctx, XrSlotRef out_slot);
XR_FUNC XrValue xr_aot_event_count_close(const XrAotContext *ctx, XrValue event_value);
XR_FUNC XrValue xr_aot_event_count_close_sync(XrValue event_value);
XR_FUNC void xr_aot_event_count_close_void(const XrAotContext *ctx, XrValue event_value);
XR_FUNC void xr_aot_event_count_close_void_sync(XrValue event_value);
XR_FUNC XrValue xr_aot_event_count_epoch(const XrAotContext *ctx, XrValue event_value);
XR_FUNC XrValue xr_aot_event_count_is_closed(const XrAotContext *ctx, XrValue event_value);
XR_FUNC XrValue xr_aot_event_count_is_closed_sync(XrValue event_value);
XR_FUNC XrValue xr_aot_tuple_get(const XrAotContext *ctx, XrValue tuple_value, uint16_t index);
XR_FUNC XrAotResult xr_aot_chan_send(const XrAotContext *ctx, XrValue channel_value,
                                     XrValue send_value, XrSlotRef result_slot, int64_t timeout_ms);
XR_FUNC XrAotResult xr_aot_chan_send_transfer(const XrAotContext *ctx, XrValue channel_value,
                                              XrValue send_value, XrSlotRef result_slot,
                                              int64_t timeout_ms, uint8_t transfer_mode);
XR_FUNC XrAotResult xr_aot_chan_send_i64(const XrAotContext *ctx, XrValue channel_value,
                                         int64_t send_value);
XR_FUNC XrAotResult xr_aot_chan_send_f64(const XrAotContext *ctx, XrValue channel_value,
                                         double send_value);

static inline XrAotResult xr_aot_chan_send_timeout(const XrAotContext *ctx, XrValue channel_value,
                                                   XrValue send_value, int64_t timeout_ms,
                                                   XrSlotRef result_slot) {
    return xr_aot_chan_send(ctx, channel_value, send_value, result_slot, timeout_ms);
}

static inline XrAotResult xr_aot_chan_send_timeout_transfer(const XrAotContext *ctx,
                                                            XrValue channel_value,
                                                            XrValue send_value, int64_t timeout_ms,
                                                            XrSlotRef result_slot,
                                                            uint8_t transfer_mode) {
    return xr_aot_chan_send_transfer(ctx, channel_value, send_value, result_slot, timeout_ms,
                                     transfer_mode);
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
                                          XrSlotRef out_slot, int64_t timeout_ms,
                                          bool result_value);
XR_FUNC XrAotResult xr_aot_chan_recv_slot_resume(const XrAotContext *ctx, XrSlotRef out_slot,
                                                 bool result_value);
XR_FUNC XrAotResult xr_aot_chan_recv_or_slot(const XrAotContext *ctx, XrValue channel_value,
                                             XrSlotRef out_slot, XrValue default_value);
XR_FUNC XrAotResult xr_aot_chan_recv_or_slot_resume(const XrAotContext *ctx, XrSlotRef out_slot);
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
