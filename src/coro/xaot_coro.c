/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_coro.c - AOT coroutine runtime bridge
 */

#include "xaot_coro.h"
#include "xaot_await.h"
#include "xaot_task.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "../base/xchecks.h"
#include "../base/xconstants.h"
#include "../base/xglobal_indices.h"
#include "../base/xmalloc.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/gc/xgc.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/gc/xsystem_heap.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xexception.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xtuple.h"
#include "../runtime/xisolate_internal.h"
#include "../os/os_time.h"
#include "xblock.h"
#include "xchannel_ops.h"
#include "xcoroutine.h"
#include "xcoro_pool.h"
#include "xtask.h"
#include "xworker.h"

typedef struct XrAotCoroState {
    const XrAotCoroDesc *desc;
    void *frame;
} XrAotCoroState;

enum {
    XR_AOT_VALUE_TAG_ARRAY = 15,
};

typedef struct XrAotArrayView {
    int64_t len;
    int64_t cap;
    void *data;
    uint8_t elem_type;
    uint8_t elem_size;
} XrAotArrayView;

static XrAotCoroState *aot_state_from_coro(XrCoroutine *coro) {
    if (!coro || !coro->backend_state)
        return NULL;
    return (XrAotCoroState *) coro->backend_state;
}

static void aot_release_frame(const XrAotCoroDesc *desc, void *frame, XrCoroGC *gc) {
    if (!frame)
        return;
    if (desc && desc->release_frame) {
        desc->release_frame(frame, gc);
        return;
    }
    xr_aot_frame_free(frame);
}

static void aot_release_state(XrCoroutine *coro) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (!state)
        return;

    aot_release_frame(state->desc, state->frame, coro ? coro->coro_gc : NULL);
    xr_free(state);
    coro->backend_state = NULL;
    coro->backend = NULL;
    coro->backend_ops = NULL;
}

static const char *aot_backend_debug_name(const XrCoroutine *coro) {
    const XrAotCoroState *state = coro ? (const XrAotCoroState *) coro->backend_state : NULL;
    if (state && state->desc && state->desc->name)
        return state->desc->name;
    return "aot";
}

static void aot_backend_debug_snapshot(const XrCoroutine *coro, XrCoroDebugSnapshot *snapshot) {
    if (!snapshot)
        return;
    snapshot->backend_name = "aot";
    snapshot->function_name = aot_backend_debug_name(coro);
    snapshot->frame_count = coro && coro->backend_state ? 1 : 0;
    snapshot->in_c_frame = 0;
}

static void aot_backend_trace_roots(XrCoroutine *coro, void *visitor) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (state && state->desc && state->desc->trace_roots)
        state->desc->trace_roots(state->frame, visitor);
}

static void aot_mark_running(XrCoroutine *coro) {
    uint32_t flags = atomic_load_explicit(&coro->flags, memory_order_relaxed);
    atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
    atomic_store_explicit(&coro->flags,
                          (flags & ~(uint32_t) (XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED)) |
                              XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED,
                          memory_order_release);
}

static void aot_mark_blocked(XrCoroutine *coro) {
    uint8_t expected = XR_CORO_STATE_RUNNING;
    if (atomic_compare_exchange_strong_explicit(&coro->coro_state, &expected, XR_CORO_STATE_BLOCKED,
                                                memory_order_release, memory_order_relaxed)) {
        atomic_fetch_and_explicit(&coro->flags, ~(uint32_t) XR_CORO_FLG_RUNNING,
                                  memory_order_relaxed);
        atomic_fetch_or_explicit(&coro->flags, (uint32_t) XR_CORO_FLG_BLOCKED,
                                 memory_order_release);
    }
}

static XrCoroRunResult aot_map_result(XrCoroutine *coro, XrAotResult result) {
    switch (result.kind) {
        case XR_AOT_RUN_DONE:
            coro->result = result.value;
            return xr_coro_run_done(result.value);
        case XR_AOT_RUN_BLOCKED:
            aot_mark_blocked(coro);
            return xr_coro_run_result(XR_CORO_RUN_BLOCKED);
        case XR_AOT_RUN_YIELD:
            return xr_coro_run_result(XR_CORO_RUN_YIELD);
        case XR_AOT_RUN_SPAWN_CHILD:
            return xr_coro_run_spawn_child(result.child);
        case XR_AOT_RUN_CANCELLED:
            return xr_coro_run_result(XR_CORO_RUN_CANCELLED);
        case XR_AOT_RUN_ERROR:
        default:
            return xr_coro_run_error(result.error, result.error_is_value);
    }
}

static XrCoroRunResult aot_backend_resume(XrCoroutine *coro, const XrCoroEvent *event,
                                          const XrCoroRunContext *run_ctx) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (!coro || !state || !state->desc || !state->desc->resume)
        return xr_coro_run_error(XR_NULL_VAL, false);
    if (event && event->kind == XR_CORO_EVENT_CANCEL)
        return xr_coro_run_result(XR_CORO_RUN_CANCELLED);

    aot_mark_running(coro);

    XrAotContext ctx;
    ctx.coro = coro;
    ctx.isolate = run_ctx && run_ctx->isolate ? run_ctx->isolate : coro->isolate;
    ctx.worker = run_ctx ? (void *) run_ctx->worker : NULL;

    XrAotResult result = state->desc->resume(state->frame, &ctx);
    return aot_map_result(coro, result);
}

static const XrCoroBackendVTable aot_backend_vtable = {
    .kind = XR_CORO_BACKEND_AOT,
    .resume = aot_backend_resume,
    .trace_roots = aot_backend_trace_roots,
    .prepare_recycle = NULL,
    .reset_reusable = NULL,
    .release = aot_release_state,
    .destroy = aot_release_state,
    .debug_name = aot_backend_debug_name,
    .debug_snapshot = aot_backend_debug_snapshot,
};

void *xr_aot_frame_alloc(size_t size) {
    if (size == 0)
        return NULL;
    return xr_calloc(1, size);
}

void xr_aot_frame_free(void *frame) {
    xr_free(frame);
}

void xr_aot_trace_frame_value(void *visitor, XrValue value) {
    if (!visitor || !XR_IS_PTR(value))
        return;
    XrAotRootVisitor *root_visitor = (XrAotRootVisitor *) visitor;
    if (root_visitor->visit)
        root_visitor->visit(value, root_visitor->ctx);
}

void xr_aot_release_frame_value(XrCoroGC *gc, XrValue value) {
    if (!gc || !XR_IS_PTR(value))
        return;
    xr_gc_release_value(gc, value);
}

XrValue xr_aot_get_builtin(const XrAotContext *ctx, int32_t index) {
    if (!ctx || !ctx->isolate || index < 0 || index >= XR_GLOBALS_MAX)
        return XR_NULL_VAL;
    return ctx->isolate->vm.builtins[index];
}

XrValue xr_aot_load_builtin_field(const XrAotContext *ctx, int32_t index, const char *field) {
    if (!field)
        return XR_NULL_VAL;
    if (index != XR_GLOBAL_VAR_PROCESS)
        return XR_NULL_VAL;

    XrValue process = xr_aot_get_builtin(ctx, index);
    if (!XR_IS_INSTANCE(process))
        return XR_NULL_VAL;

    int field_index = -1;
    if (strcmp(field, "file") == 0) {
        field_index = PROCESS_FIELD_FILE;
    } else if (strcmp(field, "args") == 0) {
        field_index = PROCESS_FIELD_ARGS;
    } else if (strcmp(field, "dir") == 0) {
        field_index = PROCESS_FIELD_DIR;
    }
    if (field_index < 0)
        return XR_NULL_VAL;
    return xr_instance_get_field_by_index((XrInstance *) process.ptr, field_index);
}

XrValue xr_aot_time_now(void) {
    return XR_FROM_INT((int64_t) (xr_time_realtime_ns() / 1000000ULL));
}

XrValue xr_aot_time_monotonic(void) {
    return XR_FROM_INT((int64_t) (xr_time_monotonic_ns() / 1000000ULL));
}

XrValue xr_aot_time_nanos(void) {
    return XR_FROM_INT((int64_t) xr_time_monotonic_ns());
}

XrValue xr_aot_time_micros(void) {
    return XR_FROM_INT((int64_t) (xr_time_monotonic_ns() / 1000ULL));
}

XrValue xr_aot_time_clock(void) {
    return XR_FROM_INT((int64_t) (xr_time_process_cpu_ns() / 1000000ULL));
}

XrCoroutine *xr_coro_create_aot(XrayIsolate *X, const XrAotCoroDesc *desc, void *frame,
                                const char *name) {
    if (!X || !desc || !desc->resume || !frame) {
        aot_release_frame(desc, frame, NULL);
        return NULL;
    }

    XrAotCoroState *state = (XrAotCoroState *) xr_calloc(1, sizeof(XrAotCoroState));
    if (!state) {
        aot_release_frame(desc, frame, NULL);
        return NULL;
    }
    state->desc = desc;
    state->frame = frame;

    XrCoroutine *coro = xr_coro_create_empty(X, name ? name : desc->name);
    if (!coro) {
        aot_release_frame(desc, frame, NULL);
        xr_free(state);
        return NULL;
    }

    xr_coro_attach_backend(coro, &aot_backend_vtable, state, NULL);
    return coro;
}

static void aot_recycle_completed_executor(XrTask *task) {
    if (!task || !task->coro)
        return;
    XrCoroutine *exec = task->coro;
    if (!xr_coro_flags_has(exec, XR_CORO_FLG_DONE))
        return;

    task->coro = NULL;
    exec->task = NULL;
    XrWorker *worker = xr_current_worker();
    if (worker) {
        xr_coro_recycle_local(worker, exec);
    } else {
        xr_coro_destroy(exec);
    }
}

static XrAotResult aot_store_slot_result(XrSlotRef out_slot, XrValue value) {
    if (!xr_slot_store_value(out_slot, value))
        return xr_aot_error(XR_NULL_VAL, false);
    return xr_aot_done(value);
}

static XrArray *aot_tasks_array_from_value(const XrAotContext *ctx, XrValue tasks_value) {
    if (!ctx || !ctx->coro)
        return NULL;
    if (xr_value_is_array(tasks_value))
        return xr_value_to_array(tasks_value);
    if (tasks_value.tag != XR_AOT_VALUE_TAG_ARRAY || !tasks_value.ptr)
        return NULL;

    XrAotArrayView *view = (XrAotArrayView *) tasks_value.ptr;
    if (view->len < 0 || view->len > INT32_MAX)
        return NULL;
    if (view->elem_type >= XR_ELEM_COUNT)
        return NULL;

    XrArray *tasks = xr_array_with_capacity(ctx->coro, (int) view->len);
    if (!tasks)
        return NULL;

    for (int64_t i = 0; i < view->len; i++) {
        XrValue item = XR_NULL_VAL;
        if (view->data)
            item = xr_typed_get(view->data, (int32_t) i, view->elem_type);
        xr_array_push(tasks, item);
    }
    return tasks;
}

static bool aot_all_tasks_done(XrArray *tasks) {
    int count = xr_array_size(tasks);
    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!xr_value_is_task(cv))
            continue;
        if (!xr_task_is_done(xr_value_to_task(cv)))
            return false;
    }
    return true;
}

static XrValue aot_collect_await_all_results(const XrAotContext *ctx, XrArray *tasks) {
    if (!ctx || !ctx->coro || !tasks)
        return XR_NULL_VAL;

    int count = xr_array_size(tasks);
    XrArray *results = xr_array_with_capacity(ctx->coro, count);
    if (!results)
        return XR_NULL_VAL;

    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        XrValue value = XR_NULL_VAL;
        if (xr_value_is_task(cv)) {
            XrTask *task = xr_value_to_task(cv);
            value = xr_coro_await_result_value(ctx->isolate, ctx->coro, task, false);
        }
        xr_array_push(results, value);
    }
    return xr_value_from_array(results);
}

static XrAotResult aot_await_all_ready_result(const XrAotContext *ctx, XrArray *tasks,
                                              XrSlotRef out_slot) {
    if (ctx && ctx->coro)
        xr_task_finish_await_waiters(ctx->coro);
    XrValue results = aot_collect_await_all_results(ctx, tasks);
    return aot_store_slot_result(out_slot, results);
}

static XrAotResult aot_await_any_ready_result(const XrAotContext *ctx, XrArray *tasks,
                                              XrSlotRef out_slot, bool success_only,
                                              bool *found_pending) {
    int count = xr_array_size(tasks);
    int done_count = 0;
    int task_count = 0;
    if (found_pending)
        *found_pending = false;

    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!xr_value_is_task(cv))
            continue;
        task_count++;

        XrTask *task = xr_value_to_task(cv);
        if (!xr_task_is_done(task)) {
            if (found_pending)
                *found_pending = true;
            continue;
        }

        done_count++;
        if (!success_only || XR_IS_NULL(task->error)) {
            if (ctx && ctx->coro)
                xr_task_finish_await_waiters(ctx->coro);
            XrValue value = xr_coro_await_result_value(ctx->isolate, ctx->coro, task, false);
            return aot_store_slot_result(out_slot, value);
        }
    }

    if (task_count == 0 || (success_only && done_count == task_count)) {
        if (ctx && ctx->coro)
            xr_task_finish_await_waiters(ctx->coro);
        return aot_store_slot_result(out_slot, XR_NULL_VAL);
    }
    return xr_aot_result(XR_AOT_RUN_BLOCKED);
}

XrValue xr_aot_run_main(XrayIsolate *X, const XrAotCoroDesc *desc, void *frame) {
    XrCoroutine *main_coro = xr_coro_create_aot(X, desc, frame, desc ? desc->name : "main");
    if (!main_coro)
        return XR_NULL_VAL;
    xr_main_thread_run(X, main_coro);
    XrValue result = main_coro->result;
    xr_coro_destroy(main_coro);
    return result;
}

XrAotSpawnResult xr_aot_spawn(const XrAotContext *ctx, const XrAotCoroDesc *desc, void *frame,
                              int priority, int link_mode, bool fire_and_forget, const char *name) {
    XrAotSpawnResult result;
    result.task_value = XR_NULL_VAL;
    result.child = NULL;

    if (!ctx || !ctx->isolate || !desc || !frame)
        return result;

    XrCoroutine *child = xr_coro_create_aot(ctx->isolate, desc, frame, name);
    if (!child)
        return result;
    if (priority != CORO_PRIORITY_NORMAL)
        xr_coro_set_priority(child, priority);

    XrTask *task = xr_task_create(ctx->coro, child);
    if (!task) {
        xr_coro_destroy(child);
        return result;
    }
    task->link_mode = (uint8_t) link_mode;

    if (fire_and_forget)
        child->gc_flags |= XR_CORO_GC_RECYCLABLE;

    XrRuntime *runtime = (XrRuntime *) ctx->isolate->vm.runtime;
    XrCoroutine *parent = ctx->coro;
    if (parent && !xr_coro_set_pending_spawn(parent, child)) {
        xr_coro_destroy(child);
        return result;
    }

    XrScopeContext *scope = parent ? parent->current_scope : NULL;
    if (!scope && runtime)
        scope = runtime->current_scope;
    if (scope && parent) {
        if (!xr_coro_set_parent_scope(child, scope)) {
            (void) xr_coro_set_pending_spawn(parent, NULL);
            xr_coro_destroy(child);
            return result;
        }
        while (atomic_exchange_explicit(&scope->child_lock, true, memory_order_acquire)) {
        }
        if (!xr_coro_set_scope_sibling(child, scope->first_child)) {
            atomic_store_explicit(&scope->child_lock, false, memory_order_release);
            (void) xr_coro_set_pending_spawn(parent, NULL);
            xr_coro_destroy(child);
            return result;
        }
        scope->first_child = child;
        atomic_store_explicit(&scope->child_lock, false, memory_order_release);
        atomic_fetch_add_explicit(&scope->count, 1, memory_order_relaxed);
        if (link_mode == XR_LINK_NONE && scope->mode == XR_SCOPE_LINKED)
            task->link_mode = XR_LINK_LINKED;
    }

    if (task->link_mode == XR_LINK_LINKED && parent && parent->task && !xr_coro_parent_scope(child))
        xr_task_attach_child(parent->task, task);

    result.task_value = xr_value_from_task(task);
    result.child = child;
    if (!parent && runtime) {
        xr_runtime_spawn(runtime, child);
    }
    return result;
}

XrValue xr_aot_coro_set_priority(const XrAotContext *ctx, XrValue target_value,
                                 XrValue priority_value) {
    (void) ctx;
    XrCoroutine *coro = NULL;
    if (xr_value_is_task(target_value)) {
        XrTask *task = xr_value_to_task(target_value);
        coro = task ? task->coro : NULL;
    } else if (xr_value_is_coro(target_value)) {
        coro = xr_value_to_coro(target_value);
    }
    if (!coro)
        return XR_NULL_VAL;

    XrCoroPriority new_prio = CORO_PRIORITY_NORMAL;
    if (XR_IS_INT(priority_value)) {
        int prio_int = (int) XR_TO_INT(priority_value);
        if (prio_int >= 0 && prio_int < XR_CORO_PRIORITY_COUNT)
            new_prio = (XrCoroPriority) prio_int;
    }

    xr_coro_set_priority(coro, new_prio);
    return XR_NULL_VAL;
}

XrAotResult xr_aot_sleep(const XrAotContext *ctx, int64_t milliseconds) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    XrCoroBlockResult block = xr_coro_sleep(ctx->coro, milliseconds);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY)
        return xr_aot_result(XR_AOT_RUN_DONE);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_scope_enter(const XrAotContext *ctx, uint8_t scope_mode) {
    if (!ctx || !ctx->isolate || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroBlockResult block = xr_coro_scope_enter(ctx->isolate, ctx->coro, scope_mode);
    if (block.kind == XR_CORO_BLOCK_READY)
        return xr_aot_done(XR_NULL_VAL);
    return xr_aot_error(block.value, block.ok);
}

XrAotResult xr_aot_scope_exit(const XrAotContext *ctx, uint8_t scope_mode, XrValue *out_value) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroBlockResult block = xr_coro_scope_exit(ctx->coro, scope_mode);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY) {
        if (out_value)
            *out_value = block.value;
        return xr_aot_done(block.value);
    }
    if (block.kind == XR_CORO_BLOCK_NO_CORO) {
        if (out_value)
            *out_value = XR_NULL_VAL;
        return xr_aot_done(XR_NULL_VAL);
    }
    return xr_aot_error(block.value, block.ok);
}

XrValue xr_aot_time_after(const XrAotContext *ctx, int64_t milliseconds) {
    if (!ctx || !ctx->isolate)
        return XR_NULL_VAL;
    if (milliseconds < 0)
        milliseconds = 0;

    XrChannel *timer_ch = xr_channel_new_timer(ctx->isolate, milliseconds);
    if (!timer_ch)
        return XR_NULL_VAL;

    XrWorker *worker = ctx->worker ? (XrWorker *) ctx->worker : xr_current_worker();
    if (worker && worker->p.timer_wheel)
        xr_channel_timer_arm(timer_ch, worker->p.timer_wheel);
    return xr_value_from_channel(timer_ch);
}

XrAotResult xr_aot_select_block(const XrAotContext *ctx, const XrValue *channel_values,
                                int channel_count, int case_count) {
    if (!ctx || !ctx->isolate || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroBlockResult block = xr_coro_select_block(ctx->isolate, ctx->coro, channel_values,
                                                   channel_count, NULL, case_count);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY || block.kind == XR_CORO_BLOCK_NO_CORO)
        return xr_aot_done(XR_NULL_VAL);
    return xr_aot_error(block.value, block.ok);
}

XrAotResult xr_aot_await_task(const XrAotContext *ctx, XrValue task_value, XrSlotRef out_slot,
                              int64_t timeout_ms, bool discard_result) {
    if (!ctx || !ctx->coro || !xr_value_is_task(task_value))
        return xr_aot_error(XR_NULL_VAL, false);
    XrTask *task = xr_value_to_task(task_value);
    XrCoroBlockResult block = xr_coro_await_task_slot(ctx->isolate, ctx->coro, task, out_slot,
                                                      timeout_ms, discard_result);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY) {
        aot_recycle_completed_executor(task);
        return xr_aot_result(XR_AOT_RUN_DONE);
    }
    if (block.kind == XR_CORO_BLOCK_CLOSED)
        return xr_aot_result(XR_AOT_RUN_DONE);
    if (block.kind == XR_CORO_BLOCK_TIMEOUT)
        return aot_store_slot_result(out_slot, XR_NULL_VAL);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_await_task_resume(const XrAotContext *ctx, XrSlotRef out_slot,
                                     bool discard_result) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    XrCoroWaitState *wait = xr_coro_wait_state(ctx->coro);
    XrTask *task = wait ? atomic_load_explicit(&wait->await_task, memory_order_acquire) : NULL;
    if (!task)
        return xr_aot_error(XR_NULL_VAL, false);
    XrCoroBlockResult block =
        xr_coro_await_task_resume_slot(ctx->isolate, ctx->coro, task, out_slot, discard_result);
    if (block.kind == XR_CORO_BLOCK_NOT_RESUMED)
        block =
            xr_coro_await_task_slot(ctx->isolate, ctx->coro, task, out_slot, -1, discard_result);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY) {
        aot_recycle_completed_executor(task);
        return xr_aot_result(XR_AOT_RUN_DONE);
    }
    if (block.kind == XR_CORO_BLOCK_TIMEOUT || block.kind == XR_CORO_BLOCK_CLOSED)
        return xr_aot_result(XR_AOT_RUN_DONE);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrValue xr_aot_task_cancel(const XrAotContext *ctx, XrValue task_value) {
    if (!xr_value_is_task(task_value))
        return xr_null();
    XrTask *task = xr_value_to_task(task_value);
    XrCoroutine *coro = task->coro;
    if (coro && !xr_task_is_done(task)) {
        xr_coro_cancel(coro);
        xr_task_cancel(task);
        XrayIsolate *isolate = ctx ? ctx->isolate : NULL;
        if (!isolate)
            isolate = coro->isolate;
        if (isolate)
            xr_coro_wake_waiter(isolate, coro);
    }
    return xr_null();
}

XrValue xr_aot_task_done(const XrAotContext *ctx, XrValue task_value) {
    (void) ctx;
    if (!xr_value_is_task(task_value))
        return xr_bool(false);
    return xr_bool(xr_task_is_done(xr_value_to_task(task_value)));
}

XrValue xr_aot_task_cancelled(const XrAotContext *ctx, XrValue task_value) {
    (void) ctx;
    if (!xr_value_is_task(task_value))
        return xr_bool(false);
    return xr_bool(xr_task_is_cancelled(xr_value_to_task(task_value)));
}

XrValue xr_aot_task_result(const XrAotContext *ctx, XrValue task_value) {
    if (!xr_value_is_task(task_value))
        return xr_null();
    XrTask *task = xr_value_to_task(task_value);
    if (!xr_task_is_done(task) || !XR_IS_NULL(task->error))
        return xr_null();
    return xr_coro_await_result_value(ctx ? ctx->isolate : NULL, ctx ? ctx->coro : NULL, task,
                                      false);
}

XrValue xr_aot_task_error(const XrAotContext *ctx, XrValue task_value) {
    if (!xr_value_is_task(task_value))
        return xr_null();
    XrTask *task = xr_value_to_task(task_value);
    if (atomic_load_explicit(&task->state, memory_order_acquire) != XR_TASK_FAILED ||
        XR_IS_NULL(task->error))
        return xr_null();

    XrayIsolate *isolate = ctx ? ctx->isolate : NULL;
    XrValue err = task->error;
    if (isolate && xr_value_is_exception(isolate, err)) {
        const char *message = xr_exception_get_message(isolate, err);
        if (!message)
            return xr_null();
        XrString *s = xr_string_intern(isolate, message, strlen(message), 0);
        return s ? xr_string_value(s) : xr_null();
    }
    return err;
}

XrAotResult xr_aot_await_all_tasks(const XrAotContext *ctx, XrValue tasks_value,
                                   XrSlotRef out_slot) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrArray *tasks = aot_tasks_array_from_value(ctx, tasks_value);
    if (!tasks)
        return xr_aot_error(XR_NULL_VAL, false);

    if (aot_all_tasks_done(tasks))
        return aot_await_all_ready_result(ctx, tasks, out_slot);

    XrCoroBlockResult block = xr_coro_await_all_tasks(ctx->coro, tasks);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY)
        return aot_await_all_ready_result(ctx, tasks, out_slot);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_await_all_tasks_resume(const XrAotContext *ctx, XrValue tasks_value,
                                          XrSlotRef out_slot) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrArray *tasks = aot_tasks_array_from_value(ctx, tasks_value);
    if (!tasks)
        return xr_aot_error(XR_NULL_VAL, false);
    if (aot_all_tasks_done(tasks))
        return aot_await_all_ready_result(ctx, tasks, out_slot);

    XrCoroBlockResult block = xr_coro_await_all_tasks(ctx->coro, tasks);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY)
        return aot_await_all_ready_result(ctx, tasks, out_slot);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_await_any_task(const XrAotContext *ctx, XrValue tasks_value, XrSlotRef out_slot,
                                  bool success_only) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrArray *tasks = aot_tasks_array_from_value(ctx, tasks_value);
    if (!tasks)
        return xr_aot_error(XR_NULL_VAL, false);

    bool found_pending = false;
    XrAotResult ready =
        aot_await_any_ready_result(ctx, tasks, out_slot, success_only, &found_pending);
    if (ready.kind != XR_AOT_RUN_BLOCKED)
        return ready;
    if (!found_pending)
        return aot_store_slot_result(out_slot, XR_NULL_VAL);

    XrCoroBlockResult block = xr_coro_await_any_task(ctx->coro, tasks, success_only);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY)
        return aot_store_slot_result(out_slot, block.value);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_await_any_task_resume(const XrAotContext *ctx, XrSlotRef out_slot) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    xr_task_finish_await_waiters(ctx->coro);
    return aot_store_slot_result(out_slot, ctx->coro->result);
}

XrValue xr_aot_channel_new(const XrAotContext *ctx, int64_t buffer_size) {
    if (!ctx || !ctx->isolate)
        return XR_NULL_VAL;
    if (buffer_size < 0)
        buffer_size = 0;
    XrChannel *ch = xr_channel_new(ctx->isolate, (uint32_t) buffer_size);
    return ch ? xr_value_from_channel(ch) : XR_NULL_VAL;
}

XrValue xr_aot_chan_try_send(const XrAotContext *ctx, XrValue channel_value, XrValue send_value) {
    if (!ctx || !ctx->isolate || !xr_value_is_channel(channel_value))
        return xr_bool(false);
    XrChannel *ch = xr_value_to_channel(channel_value);
    return xr_bool(xr_chan_try_send(ctx->isolate, ch, send_value));
}

XrValue xr_aot_chan_try_recv_value(const XrAotContext *ctx, XrValue channel_value) {
    if (!ctx || !ctx->isolate || !xr_value_is_channel(channel_value))
        return XR_NULL_VAL;

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrValue recv_value = XR_NULL_VAL;
    bool ok = xr_chan_try_recv(ctx->isolate, ch, &recv_value, ctx->coro);
    return ok ? recv_value : XR_NULL_VAL;
}

XrValue xr_aot_chan_try_recv(const XrAotContext *ctx, XrValue channel_value) {
    if (!ctx || !ctx->isolate || !xr_value_is_channel(channel_value))
        return XR_NULL_VAL;

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrValue recv_value = XR_NULL_VAL;
    bool ok = xr_chan_try_recv(ctx->isolate, ch, &recv_value, ctx->coro);
    XrTuple *tuple = xr_tuple_new(ctx->coro, 2);
    if (!tuple)
        return XR_NULL_VAL;
    xr_tuple_set(tuple, 0, recv_value);
    xr_tuple_set(tuple, 1, xr_bool(ok));
    return xr_value_from_tuple(tuple);
}

XrValue xr_aot_chan_close(const XrAotContext *ctx, XrValue channel_value) {
    if (!ctx || !ctx->isolate || !xr_value_is_channel(channel_value))
        return XR_NULL_VAL;
    XrChannel *ch = xr_value_to_channel(channel_value);
    xr_channel_close(ch);
    return XR_NULL_VAL;
}

XrValue xr_aot_chan_is_closed(const XrAotContext *ctx, XrValue channel_value) {
    if (!ctx || !xr_value_is_channel(channel_value))
        return xr_bool(false);
    return xr_bool(xr_channel_is_closed(xr_value_to_channel(channel_value)));
}

XrValue xr_aot_tuple_get(const XrAotContext *ctx, XrValue tuple_value, uint16_t index) {
    (void) ctx;
    if (!xr_value_is_tuple(tuple_value))
        return XR_NULL_VAL;
    return xr_tuple_get(xr_value_to_tuple(tuple_value), index);
}

static XrAotResult aot_load_slot_done(XrSlotRef slot) {
    XrValue value = XR_NULL_VAL;
    if (!xr_slot_load_value(slot, &value))
        return xr_aot_error(XR_NULL_VAL, false);
    return xr_aot_done(value);
}

static XrAotResult aot_store_recv_timeout_tuple(const XrAotContext *ctx, XrSlotRef out_slot,
                                                XrValue value, bool ok) {
    if (out_slot.kind == XR_SLOT_NONE)
        return xr_aot_done(XR_NULL_VAL);

    XrTuple *tuple = xr_tuple_new(ctx ? ctx->coro : NULL, 2);
    if (!tuple)
        return xr_aot_error(XR_NULL_VAL, false);
    xr_tuple_set(tuple, 0, ok ? value : XR_NULL_VAL);
    xr_tuple_set(tuple, 1, xr_bool(ok));

    XrValue tuple_value = xr_value_from_tuple(tuple);
    if (!xr_slot_store_value(out_slot, tuple_value))
        return xr_aot_error(XR_NULL_VAL, false);
    return xr_aot_done(tuple_value);
}

XrAotResult xr_aot_chan_send(const XrAotContext *ctx, XrValue channel_value, XrValue send_value,
                             XrSlotRef result_slot, int64_t timeout_ms) {
    if (!ctx || !ctx->isolate || !ctx->coro || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrCoroBlockResult block =
        xr_coro_chan_send(ctx->isolate, ctx->coro, ch, send_value, result_slot, timeout_ms);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY || block.kind == XR_CORO_BLOCK_TIMEOUT ||
        block.kind == XR_CORO_BLOCK_CLOSED || block.kind == XR_CORO_BLOCK_NO_CORO) {
        if (timeout_ms >= 0)
            return aot_load_slot_done(result_slot);
        if (block.kind == XR_CORO_BLOCK_READY)
            return xr_aot_done(XR_NULL_VAL);
    }
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_chan_send_resume(const XrAotContext *ctx, XrSlotRef result_slot,
                                    bool status_result) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    XrCoroBlockResult block = xr_coro_chan_send_resume(ctx->coro, result_slot);
    if (block.kind == XR_CORO_BLOCK_READY || block.kind == XR_CORO_BLOCK_TIMEOUT ||
        block.kind == XR_CORO_BLOCK_CLOSED) {
        if (!status_result && block.kind == XR_CORO_BLOCK_READY)
            return xr_aot_done(XR_NULL_VAL);
        if (status_result)
            return aot_load_slot_done(result_slot);
    }
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_chan_recv_slot(const XrAotContext *ctx, XrValue channel_value,
                                  XrSlotRef out_slot, int64_t timeout_ms, bool tuple_result) {
    if (!ctx || !ctx->isolate || !ctx->coro || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrCoroBlockResult block =
        xr_coro_chan_recv(ctx->isolate, ctx->coro, ch, out_slot, xr_slot_none(), timeout_ms);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (tuple_result) {
        if (block.kind == XR_CORO_BLOCK_READY)
            return aot_store_recv_timeout_tuple(ctx, out_slot, block.value, true);
        if (block.kind == XR_CORO_BLOCK_CLOSED || block.kind == XR_CORO_BLOCK_TIMEOUT ||
            block.kind == XR_CORO_BLOCK_NO_CORO)
            return aot_store_recv_timeout_tuple(ctx, out_slot, XR_NULL_VAL, false);
        return xr_aot_error(XR_NULL_VAL, false);
    }
    if (block.kind == XR_CORO_BLOCK_READY) {
        XrValue value = XR_NULL_VAL;
        (void) xr_slot_load_value(out_slot, &value);
        return xr_aot_done(value);
    }
    if (block.kind == XR_CORO_BLOCK_CLOSED) {
        (void) xr_slot_store_value(out_slot, XR_NULL_VAL);
        return xr_aot_done(XR_NULL_VAL);
    }
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_chan_recv_slot_resume(const XrAotContext *ctx, XrSlotRef out_slot,
                                         bool tuple_result) {
    if (!ctx || !ctx->isolate || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    if (!tuple_result && out_slot.kind == XR_SLOT_NONE)
        out_slot = ctx->coro->ext ? ctx->coro->ext->recv_slot_ref : xr_slot_none();
    XrCoroBlockResult block =
        xr_coro_chan_recv_resume(ctx->isolate, ctx->coro, out_slot, xr_slot_none());
    if (tuple_result) {
        if (block.kind == XR_CORO_BLOCK_READY)
            return aot_store_recv_timeout_tuple(ctx, out_slot, block.value, true);
        if (block.kind == XR_CORO_BLOCK_CLOSED || block.kind == XR_CORO_BLOCK_TIMEOUT)
            return aot_store_recv_timeout_tuple(ctx, out_slot, XR_NULL_VAL, false);
        return xr_aot_error(XR_NULL_VAL, false);
    }
    if (block.kind == XR_CORO_BLOCK_READY) {
        XrValue value = XR_NULL_VAL;
        (void) xr_slot_load_value(out_slot, &value);
        return xr_aot_done(value);
    }
    if (block.kind == XR_CORO_BLOCK_CLOSED || block.kind == XR_CORO_BLOCK_TIMEOUT) {
        (void) xr_slot_store_value(out_slot, XR_NULL_VAL);
        return xr_aot_done(XR_NULL_VAL);
    }
    return xr_aot_error(XR_NULL_VAL, false);
}
