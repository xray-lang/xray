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

#include <stdatomic.h>

#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/gc/xgc.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/gc/xsystem_heap.h"
#include "../runtime/object/xtuple.h"
#include "../runtime/xisolate_internal.h"
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

static XrAotCoroState *aot_state_from_coro(XrCoroutine *coro) {
    if (!coro || !coro->backend_state)
        return NULL;
    return (XrAotCoroState *) coro->backend_state;
}

static void aot_release_state(XrCoroutine *coro) {
    XrAotCoroState *state = aot_state_from_coro(coro);
    if (!state)
        return;

    if (state->desc && state->desc->release_frame && state->frame)
        state->desc->release_frame(state->frame);
    xr_free(state);
    coro->backend_state = NULL;
}

static void aot_release_frame(const XrAotCoroDesc *desc, void *frame) {
    if (!frame)
        return;
    if (desc && desc->release_frame) {
        desc->release_frame(frame);
        return;
    }
    xr_aot_frame_free(frame);
}

static const char *aot_backend_debug_name(const XrCoroutine *coro) {
    const XrAotCoroState *state = coro ? (const XrAotCoroState *) coro->backend_state : NULL;
    if (state && state->desc && state->desc->name)
        return state->desc->name;
    return "aot";
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
    .release = aot_release_state,
    .destroy = aot_release_state,
    .debug_name = aot_backend_debug_name,
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

void xr_aot_release_frame_value(XrValue value) {
    (void) value;
}

XrCoroutine *xr_coro_create_aot(XrayIsolate *X, const XrAotCoroDesc *desc, void *frame,
                                const char *name) {
    if (!X || !desc || !desc->resume || !frame) {
        aot_release_frame(desc, frame);
        return NULL;
    }

    XrAotCoroState *state = (XrAotCoroState *) xr_calloc(1, sizeof(XrAotCoroState));
    if (!state) {
        aot_release_frame(desc, frame);
        return NULL;
    }
    state->desc = desc;
    state->frame = frame;

    XrCoroutine *coro = xr_coro_create_empty(X, name ? name : desc->name, false);
    if (!coro) {
        aot_release_frame(desc, frame);
        xr_free(state);
        return NULL;
    }

    coro->backend = &aot_backend_vtable;
    coro->backend_state = state;
    coro->entry_type = XR_CORO_ENTRY_NATIVE;
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
    XrScopeContext *scope = parent ? parent->current_scope : NULL;
    if (!scope && runtime)
        scope = runtime->current_scope;
    if (scope && parent) {
        child->parent_scope = scope;
        while (atomic_exchange_explicit(&scope->child_lock, true, memory_order_acquire)) {
        }
        child->scope_sibling = scope->first_child;
        scope->first_child = child;
        atomic_store_explicit(&scope->child_lock, false, memory_order_release);
        atomic_fetch_add_explicit(&scope->count, 1, memory_order_relaxed);
        if (link_mode == XR_LINK_NONE && scope->mode == XR_SCOPE_LINKED)
            task->link_mode = XR_LINK_LINKED;
    }

    if (task->link_mode == XR_LINK_LINKED && parent && parent->task && !child->parent_scope)
        xr_task_attach_child(parent->task, task);

    result.task_value = xr_value_from_task(task);
    result.child = child;
    if (!parent && runtime) {
        xr_runtime_spawn(runtime, child);
    } else if (parent) {
        parent->pending_spawn = child;
    }
    return result;
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

XrAotResult xr_aot_await_task(const XrAotContext *ctx, XrValue task_value, XrValue *out_value,
                              bool discard_result) {
    if (!ctx || !ctx->coro || !xr_value_is_task(task_value))
        return xr_aot_error(XR_NULL_VAL, false);
    XrTask *task = xr_value_to_task(task_value);
    XrSlotRef out_slot = out_value ? xr_slot_xvalue_ptr(out_value) : xr_slot_none();
    XrCoroBlockResult block =
        xr_coro_await_task_slot(ctx->isolate, ctx->coro, task, out_slot, -1, discard_result);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_READY) {
        aot_recycle_completed_executor(task);
        return xr_aot_result(XR_AOT_RUN_DONE);
    }
    if (block.kind == XR_CORO_BLOCK_CLOSED) {
        if (out_value)
            *out_value = XR_NULL_VAL;
        return xr_aot_result(XR_AOT_RUN_DONE);
    }
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_await_task_resume(const XrAotContext *ctx, XrValue task_value,
                                     XrValue *out_value, bool discard_result) {
    if (!ctx || !ctx->coro || !xr_value_is_task(task_value))
        return xr_aot_error(XR_NULL_VAL, false);
    XrTask *task = xr_value_to_task(task_value);
    XrSlotRef out_slot = out_value ? xr_slot_xvalue_ptr(out_value) : xr_slot_none();
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
    if (block.kind == XR_CORO_BLOCK_TIMEOUT || block.kind == XR_CORO_BLOCK_CLOSED) {
        if (out_value)
            *out_value = XR_NULL_VAL;
        return xr_aot_result(XR_AOT_RUN_DONE);
    }
    return xr_aot_error(XR_NULL_VAL, false);
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

XrAotResult xr_aot_chan_send(const XrAotContext *ctx, XrValue channel_value, XrValue send_value) {
    if (!ctx || !ctx->isolate || !ctx->coro || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrCoroBlockResult block =
        xr_coro_chan_send(ctx->isolate, ctx->coro, ch, send_value, xr_slot_none(), -1);
    if (block.kind == XR_CORO_BLOCK_READY)
        return xr_aot_done(XR_NULL_VAL);
    if (block.kind == XR_CORO_BLOCK_BLOCKED)
        return xr_aot_blocked();
    if (block.kind == XR_CORO_BLOCK_CLOSED)
        return xr_aot_error(XR_NULL_VAL, false);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_chan_send_resume(const XrAotContext *ctx) {
    if (!ctx || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);
    XrCoroBlockResult block = xr_coro_chan_send_resume(ctx->coro, xr_slot_none());
    if (block.kind == XR_CORO_BLOCK_READY)
        return xr_aot_done(XR_NULL_VAL);
    if (block.kind == XR_CORO_BLOCK_CLOSED || block.kind == XR_CORO_BLOCK_TIMEOUT)
        return xr_aot_error(XR_NULL_VAL, false);
    return xr_aot_error(XR_NULL_VAL, false);
}

XrAotResult xr_aot_chan_recv(const XrAotContext *ctx, XrValue channel_value, XrValue *out_value) {
    if (!ctx || !ctx->isolate || !ctx->coro || !out_value || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);
    return xr_aot_chan_recv_slot(ctx, channel_value, xr_slot_xvalue_ptr(out_value));
}

XrAotResult xr_aot_chan_recv_resume(const XrAotContext *ctx, XrValue *out_value) {
    if (!ctx || !ctx->isolate || !ctx->coro || !out_value)
        return xr_aot_error(XR_NULL_VAL, false);
    return xr_aot_chan_recv_slot_resume(ctx, xr_slot_xvalue_ptr(out_value));
}

XrAotResult xr_aot_chan_recv_slot(const XrAotContext *ctx, XrValue channel_value,
                                  XrSlotRef out_slot) {
    if (!ctx || !ctx->isolate || !ctx->coro || !xr_value_is_channel(channel_value))
        return xr_aot_error(XR_NULL_VAL, false);

    XrChannel *ch = xr_value_to_channel(channel_value);
    XrCoroBlockResult block =
        xr_coro_chan_recv(ctx->isolate, ctx->coro, ch, out_slot, xr_slot_none(), -1);
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

XrAotResult xr_aot_chan_recv_slot_resume(const XrAotContext *ctx, XrSlotRef out_slot) {
    if (!ctx || !ctx->isolate || !ctx->coro)
        return xr_aot_error(XR_NULL_VAL, false);

    XrCoroBlockResult block =
        xr_coro_chan_recv_resume(ctx->isolate, ctx->coro, out_slot, xr_slot_none());
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
