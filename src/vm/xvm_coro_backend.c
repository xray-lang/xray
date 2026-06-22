/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_coro_backend.c - VM coroutine backend for the scheduler ABI
 *
 * KEY CONCEPT:
 *   Workers schedule backend-neutral coroutine run results. This file owns the
 *   VM-shaped execution state, maps VM/CFunc outcomes into the coroutine
 *   ABI, and keeps interpreter frame details out of the worker hot path.
 */

#include "../coro/xworker_internal.h"
#include "../coro/xblock.h"
#include "../coro/xdeep_copy.h"
#include "../coro/xtask.h"
#include "../coro/xyieldable.h"
#include "../runtime/xexec_frame.h"
#include "../runtime/xvm_call.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xray_debug_hooks.h"
#include "../runtime/mem/xcoro_heap.h"
#include "../runtime/mem/xsystem_heap.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xvalue_format.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../base/xlog.h"
#include "xvm_internal.h"
#include "xvm_coro_state.h"
#include "xvm_resume.h"
#include "xvm_worker_state.h"
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>

#define XR_VM_CORO_INIT_STACK_SLOTS 64
#define XR_VM_CORO_INIT_FRAME_SLOTS 4

// ========== Forward Declarations ==========

static XrVMResult run_finalize(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                               XrVMContext *ctx, XrVMContext *coro_ctx, XrVMResult result);

static XrVMResult run_first_exec(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                                 XrVMContext *ctx, XrVMContext *coro_ctx);

static XrVMResult run_resume_path(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                                  XrVMContext *ctx, XrVMContext *coro_ctx);

static XrVMResult run_cfunc_coro(XrWorker *worker, XrCoroutine *coro, XrayIsolate *isolate);

static XrVMResult try_recover_via_closure_continuation(XrayIsolate *isolate, XrWorker *worker,
                                                       XrCoroutine *coro, XrVMContext *ctx,
                                                       XrVMContext *coro_ctx);

static XrVMResult vm_backend_resume_on_worker(XrWorker *worker, XrCoroutine *coro);
static XrCoroRunResult vm_backend_resume(XrCoroutine *coro, const XrCoroEvent *event,
                                         const XrCoroRunContext *run_ctx);
static XrCoroRunResult worker_run_result_from_vm(XrCoroutine *coro, XrVMResult result);
static const char *vm_backend_debug_name(const XrCoroutine *coro);
static void vm_backend_debug_snapshot(const XrCoroutine *coro, XrCoroDebugSnapshot *snapshot);
static void vm_backend_destroy(XrCoroutine *coro);
static bool vm_backend_ensure_state(XrCoroutine *coro);
static bool vm_backend_prepare_execution_state(XrCoroutine *coro, XrayIsolate *X, XrWorker *worker,
                                               bool need_storage, bool is_clean);
static void vm_backend_reset_execution_state(XrCoroutine *coro, XrayIsolate *X);
static void vm_backend_clear_entry_state(XrCoroutine *coro);
static void vm_backend_reset_entry_state_no_free(XrCoroutine *coro);
static bool vm_backend_bind_closure_entry(XrCoroutine *coro, XrayIsolate *X, XrClosure *closure,
                                          XrValue *args, int arg_count, bool copy_args);
static bool vm_backend_bind_cfunc_entry(XrCoroutine *coro, XrCoroCFuncEntry cfunc, XrValue *args,
                                        int arg_count);
static bool vm_backend_prepare_recycle(XrCoroutine *coro, XrWorker *worker);
static void vm_backend_reset_reusable(XrCoroutine *coro);
static bool vm_backend_setup_yield_continuation(XrayIsolate *X, XrCoroutine *coro,
                                                void *continuation, void *user_data);
static bool vm_backend_has_continuation(const XrCoroutine *coro);
static XrCFuncResult vm_backend_call_closure(XrayIsolate *X, XrCoroutine *coro, XrClosure *closure,
                                             XrValue *args, int nargs, void *continuation,
                                             void *user_ctx, XrValue *result);

static const XrCoroBackendVTable vm_backend_vtable = {
    .kind = XR_CORO_BACKEND_VM,
    .resume = vm_backend_resume,
    .trace_roots = NULL,
    .prepare_recycle = vm_backend_prepare_recycle,
    .reset_reusable = vm_backend_reset_reusable,
    .setup_yield_continuation = vm_backend_setup_yield_continuation,
    .has_continuation = vm_backend_has_continuation,
    .call_closure = vm_backend_call_closure,
    .ensure_state = vm_backend_ensure_state,
    .prepare_execution_state = vm_backend_prepare_execution_state,
    .reset_execution_state = vm_backend_reset_execution_state,
    .clear_entry_state = vm_backend_clear_entry_state,
    .reset_entry_state_no_free = vm_backend_reset_entry_state_no_free,
    .bind_closure_entry = vm_backend_bind_closure_entry,
    .bind_cfunc_entry = vm_backend_bind_cfunc_entry,
    .release = NULL,
    .destroy = vm_backend_destroy,
    .debug_name = vm_backend_debug_name,
    .debug_snapshot = vm_backend_debug_snapshot,
};

const XrCoroBackendVTable *xr_coro_vm_backend_vtable(void) {
    return &vm_backend_vtable;
}

static const char *vm_backend_debug_name(const XrCoroutine *coro) {
    (void) coro;
    return "vm";
}

static void vm_backend_debug_snapshot(const XrCoroutine *coro, XrCoroDebugSnapshot *snapshot) {
    if (!snapshot)
        return;
    snapshot->backend_name = "vm";
    snapshot->function_name = "?";
    snapshot->frame_count = 0;
    snapshot->in_c_frame = 0;
    if (!coro)
        return;

    const XrVmCoroState *state = xr_coro_maybe_vm_state_const(coro);
    if (!state)
        return;

    const XrVMContext *ctx = &state->ctx;
    snapshot->frame_count = ctx->frame_count;
    if (ctx->frame_count <= 0 || !ctx->frames)
        return;

    const XrBcCallFrame *frame = &ctx->frames[ctx->frame_count - 1];
    snapshot->in_c_frame = (frame->call_status & XR_CALL_C) ? 1 : 0;
    if (frame->closure && frame->closure->proto && frame->closure->proto->name) {
        snapshot->function_name = frame->closure->proto->name->data;
    }
}

static bool consume_select_channel_resume(XrCoroutine *coro) {
    if (!xr_coro_select_wait(coro))
        return false;
    xr_coro_clear_select_wait(coro);
    xr_coro_resume_store(coro, XR_RESUME_OK);
    return true;
}

static void finish_io_resume_tokens(XrCoroutine *coro, int resume_status) {
    if (!coro || !coro->ext)
        return;
    if (resume_status != XR_RESUME_IO_READY && resume_status != XR_RESUME_TIMEOUT)
        return;
    int wait_reason = xr_coro_get_wait_reason(xr_coro_flags_load(coro));
    if (wait_reason != (XR_CORO_WAIT_IO >> XR_CORO_WAIT_SHIFT))
        return;
    xr_io_wait_token_finish(&coro->ext->wait.io_token);
    if (resume_status == XR_RESUME_TIMEOUT)
        xr_timer_wait_token_finish(&coro->ext->wait.timer_token);
}

static void finish_channel_resume_tokens(XrCoroutine *coro) {
    if (!coro || !coro->ext)
        return;
    xr_channel_wait_token_finish(&coro->ext->chan_wait_token);
    atomic_store_explicit((_Atomic(void *) *) &coro->ext->wait_channel, NULL, memory_order_relaxed);
    coro->ext->chan_ok_slot_ref = xr_slot_none();
    coro->ext->chan_resume_delivered = false;
}

static bool is_select_channel_resume(XrCoroutine *coro, int resume_status) {
    if (!xr_coro_select_wait(coro))
        return false;
    return resume_status == XR_RESUME_CHANNEL || resume_status == XR_RESUME_CHANNEL_CLOSED;
}

static XrVmCoroState *vm_state_for_coro(XrCoroutine *coro) {
    return xr_coro_maybe_vm_state(coro);
}

static void vm_entry_reset_no_free(XrVmCoroState *state) {
    if (!state)
        return;
    /* The coroutine owns one reference to its entry closure (taken in
     * vm_backend_bind_closure_entry). This is the single point where the entry
     * transitions back to empty, so release it here — a closure shared by many
     * `go` spawns must stay live for every coroutine that still references it,
     * even when the spawning loop has already dropped its own reference. The
     * field is nulled immediately below, so a repeated reset is a safe no-op
     * (shared closures route through the atomic shared-destroy path, so the
     * exact owning heap does not matter). */
    if (state->entry_type == XR_CORO_ENTRY_CLOSURE && state->entry.closure) {
        XrCoroHeap *owner =
            state->entry_closure_owner ? state->entry_closure_owner : xr_current_coro_heap();
        xr_rc_release(owner, (XrObjHeader *) state->entry.closure);
    }
    state->entry_type = XR_CORO_ENTRY_CLOSURE;
    state->entry.closure = NULL;
    state->entry_closure_owner = NULL;
    state->args = NULL;
    state->arg_count = 0;
    for (int i = 0; i < 4; i++)
        state->inline_args[i] = xr_null();
}

static bool vm_backend_ensure_state(XrCoroutine *coro) {
    if (!coro)
        return false;
    if (vm_state_for_coro(coro))
        return true;
    if (coro->backend && coro->backend != &vm_backend_vtable && coro->backend_state)
        return false;
    XrVmCoroState *state = (XrVmCoroState *) xr_calloc(1, sizeof(XrVmCoroState));
    if (!state)
        return false;
    state->ctx.handlers = state->ctx.handler_inline;
    state->ctx.handler_capacity = XR_HANDLER_INLINE_CAP;
    xr_coro_attach_backend(coro, xr_coro_vm_backend_vtable(), state);
    coro->gc_flags |= XR_CORO_GC_BACKEND_STATE_OWNED;
    return true;
}

static XrCoroutine *vm_backend_alloc_shell(XrayIsolate *X, bool use_runtime_pool) {
    XrCoroutine *coro = NULL;
    if (use_runtime_pool) {
        XrRuntime *runtime = (XrRuntime *) X->scheduler_runtime;
        if (runtime) {
            coro = xr_coro_pool_get(runtime);
        }
    }
    if (!coro) {
        if (xr_isolate_get_sys_heap(X)) {
            coro = xr_sysheap_alloc_coro(xr_isolate_get_sys_heap(X));
            if (!coro)
                return NULL;
            coro->heap = NULL;
        } else {
            coro = (XrCoroutine *) xr_malloc(sizeof(XrCoroutine));
            if (!coro)
                return NULL;
            memset(coro, 0, sizeof(XrCoroutine));
            coro->hdr.type = XR_TCOROUTINE;
            coro->heap = NULL;
        }
    }
    if (!vm_backend_ensure_state(coro)) {
        xr_coro_discard_uninitialized(coro);
        return NULL;
    }
    return coro;
}

// Create bootstrap main coroutine before script execution.
XrCoroutine *xr_coro_create_bootstrap(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "coro_create_bootstrap: NULL isolate");
    XrCoroutine *coro = vm_backend_alloc_shell(X, false);
    if (!coro)
        return NULL;

    if (!xr_coro_init_shell(coro, X, "main", true)) {
        xr_coro_free(coro);
        xr_coro_discard_uninitialized(coro);
        return NULL;
    }

    if (!coro->heap) {
        coro->heap = xr_coro_heap_create(coro);
        if (!coro->heap) {
            xr_coro_free(coro);
            xr_coro_discard_uninitialized(coro);
            return NULL;
        }
    }

    coro->flags |= XR_CORO_FLG_MAIN;
    vm_backend_reset_entry_state_no_free(coro);
    (void) xr_coro_set_pending_spawn(coro, NULL);
    return coro;
}

void xr_coro_setup_main(XrCoroutine *coro, XrayIsolate *X, XrClosure *closure) {
    XR_DCHECK(coro != NULL, "coro_setup_main: NULL coro");
    XR_DCHECK(X != NULL, "coro_setup_main: NULL isolate");
    XR_DCHECK(closure != NULL, "coro_setup_main: NULL closure");
    bool bound = vm_backend_bind_closure_entry(coro, X, closure, NULL, 0, false);
    XR_CHECK(bound, "coro_setup_main: failed to bind VM closure");
    (void) xr_coro_set_source(coro, closure->proto ? closure->proto->source_file : NULL, 0);
    vm_backend_reset_execution_state(coro, X);
}

void xr_coro_reset_for_call(XrCoroutine *coro, XrayIsolate *X, XrClosure *closure) {
    XR_DCHECK(coro != NULL, "coro_reset_for_call: NULL coro");
    XR_DCHECK(X != NULL, "coro_reset_for_call: NULL isolate");
    XR_DCHECK(closure != NULL, "coro_reset_for_call: NULL closure");

    if (coro->heap) {
        coro->heap->cycle_collection_disabled = 0;
    }

    vm_backend_reset_execution_state(coro, X);

    bool bound = vm_backend_bind_closure_entry(coro, X, closure, NULL, 0, false);
    XR_CHECK(bound, "coro_reset_for_call: failed to bind VM closure");
    (void) xr_coro_set_source(coro, closure->proto ? closure->proto->source_file : NULL, 0);

    coro->result = xr_null();
    coro->error = xr_null();
    atomic_store_explicit(&coro->current_scope, NULL, memory_order_relaxed);
}

XrCoroutine *xr_coro_create_vm_closure(XrayIsolate *X, XrClosure *closure, XrValue *args,
                                       int arg_count, const char *name, const char *file,
                                       int line) {
    XR_DCHECK(X != NULL, "coro_create_vm_closure: NULL isolate");
    XR_DCHECK(closure != NULL, "coro_create_vm_closure: NULL closure");
    XR_DCHECK(arg_count >= 0, "coro_create_vm_closure: negative arg_count");
    XR_DCHECK(arg_count == 0 || args != NULL, "coro_create_vm_closure: NULL args with count > 0");

    XrCoroutine *coro = vm_backend_alloc_shell(X, true);
    if (!coro)
        return NULL;

    if (!xr_coro_init_shell(coro, X, name, true)) {
        xr_coro_free(coro);
        xr_coro_discard_uninitialized(coro);
        return NULL;
    }

    if (!vm_backend_bind_closure_entry(coro, X, closure, args, arg_count, true)) {
        xr_coro_free(coro);
        xr_coro_discard_uninitialized(coro);
        return NULL;
    }
    if (xr_coro_name(coro) && !xr_coro_set_source(coro, file, line)) {
        xr_coro_free(coro);
        xr_coro_discard_uninitialized(coro);
        return NULL;
    }
    if (xr_coro_name(coro) &&
        !xr_coro_set_spawn_origin(coro, (XrCoroutine *) X->vm.current_coro, file, line)) {
        xr_coro_free(coro);
        xr_coro_discard_uninitialized(coro);
        return NULL;
    }

    return coro;
}

XrCoroutine *xr_coro_create_vm_cfunc(XrayIsolate *X, XrCoroCFuncEntry cfunc, XrValue *args,
                                     int argc, const char *name) {
    XrCoroutine *coro = vm_backend_alloc_shell(X, true);
    if (!coro)
        return NULL;

    if (!xr_coro_init_shell(coro, X, name, true)) {
        xr_coro_free(coro);
        xr_coro_discard_uninitialized(coro);
        return NULL;
    }

    if (!vm_backend_bind_cfunc_entry(coro, cfunc, args, argc)) {
        xr_coro_free(coro);
        xr_coro_discard_uninitialized(coro);
        return NULL;
    }

    return coro;
}

static void vm_backend_reset_execution_state(XrCoroutine *coro, XrayIsolate *X) {
    if (!coro)
        return;

    XrVMContext *ctx = xr_coro_vm_ctx(coro);
    ctx->stack_top = ctx->stack;
    ctx->frame_count = 0;
    ctx->module_base_frame = 0;
    ctx->handlers = ctx->handler_inline;
    ctx->handler_count = 0;
    ctx->handler_capacity = XR_HANDLER_INLINE_CAP;
    ctx->current_exception = xr_null();
    ctx->pending_error = xr_null();
    ctx->current_coro = coro;
    ctx->instruction_count = 0;
    ctx->preempt_pending = false;
    ctx->last_nret = 0;
    ctx->defer_count = 0;
    ctx->trace_execution = false;
    ctx->isolate = X;
}

static void vm_backend_reset_entry_state_no_free(XrCoroutine *coro) {
    vm_entry_reset_no_free(vm_state_for_coro(coro));
}

static void vm_backend_clear_entry_state(XrCoroutine *coro) {
    XrVmCoroState *state = vm_state_for_coro(coro);
    if (!state)
        return;
    if (state->args && state->args != state->inline_args)
        xr_free(state->args);
    vm_entry_reset_no_free(state);
}

static bool vm_entry_copy_args(XrCoroutine *coro, XrayIsolate *X, XrVmCoroState *state,
                               XrValue *args, int arg_count, bool copy_args) {
    if (!state || arg_count < 0 || (arg_count > 0 && !args))
        return false;
    state->arg_count = arg_count;
    if (arg_count <= 0) {
        state->args = NULL;
        return true;
    }

    XrValue *dst = state->inline_args;
    if (arg_count > 4) {
        if ((size_t) arg_count > SIZE_MAX / sizeof(XrValue))
            return false;
        dst = (XrValue *) xr_malloc(sizeof(XrValue) * (size_t) arg_count);
        if (!dst)
            return false;
    }

    for (int i = 0; i < arg_count; i++) {
        dst[i] = xr_null();
    }
    state->args = dst;
    for (int i = 0; i < arg_count; i++) {
        dst[i] =
            (copy_args && XR_IS_PTR(args[i])) ? xr_deep_copy_to_coro(X, args[i], coro) : args[i];
    }
    return true;
}

static bool vm_backend_bind_closure_entry(XrCoroutine *coro, XrayIsolate *X, XrClosure *closure,
                                          XrValue *args, int arg_count, bool copy_args) {
    if (!coro || !closure)
        return false;
    if (!vm_backend_ensure_state(coro))
        return false;
    XrVmCoroState *state = vm_state_for_coro(coro);
    if (!state)
        return false;
    vm_backend_clear_entry_state(coro);
    state->entry_type = XR_CORO_ENTRY_CLOSURE;
    state->entry.closure = closure;
    /* The coroutine takes its own reference to the entry closure so it survives
     * for the coroutine's whole life regardless of what the spawning frame does
     * with its copy (XI_GO only borrows the closure). The spawning coroutine's
     * gc owns the closure block, so record it for the matching release in
     * vm_entry_reset_no_free (the last coroutine to drop the shared closure must
     * return it to the owner's heap, not its own). */
    xr_rc_retain((XrObjHeader *) closure);
    state->entry_closure_owner = xr_current_coro_heap();
    if (!vm_entry_copy_args(coro, X, state, args, arg_count, copy_args)) {
        vm_backend_clear_entry_state(coro);
        return false;
    }
    return true;
}

static bool vm_backend_bind_cfunc_entry(XrCoroutine *coro, XrCoroCFuncEntry cfunc, XrValue *args,
                                        int arg_count) {
    if (!coro || !cfunc)
        return false;
    if (!vm_backend_ensure_state(coro))
        return false;
    XrVmCoroState *state = vm_state_for_coro(coro);
    if (!state)
        return false;
    vm_backend_clear_entry_state(coro);
    state->entry_type = XR_CORO_ENTRY_CFUNC;
    state->entry.cfunc = cfunc;
    if (!vm_entry_copy_args(coro, NULL, state, args, arg_count, false)) {
        vm_backend_clear_entry_state(coro);
        return false;
    }
    return true;
}

static bool vm_backend_alloc_stack_frames(XrCoroutine *coro, XrVMContext *ctx, XrWorker *worker) {
    (void) worker;
    if (!coro || !ctx)
        return false;
    if (ctx->stack)
        return true;

    size_t stack_bytes = sizeof(XrValue) * XR_VM_CORO_INIT_STACK_SLOTS;
    size_t frames_bytes = sizeof(XrBcCallFrame) * XR_VM_CORO_INIT_FRAME_SLOTS;
    char *block = (char *) xr_malloc(stack_bytes + frames_bytes);
    if (!block)
        return false;

    ctx->stack = (XrValue *) block;
    ctx->stack_capacity = XR_VM_CORO_INIT_STACK_SLOTS;
    ctx->frames = (XrBcCallFrame *) (block + stack_bytes);
    ctx->frame_capacity = XR_VM_CORO_INIT_FRAME_SLOTS;
    memset(block, 0, stack_bytes + frames_bytes);
    return true;
}

static void vm_backend_clear_stack_frames(XrVMContext *ctx) {
    if (!ctx || !ctx->stack)
        return;
    memset(ctx->stack, 0, sizeof(XrValue) * (size_t) ctx->stack_capacity);
    if (ctx->frames) {
        memset(ctx->frames, 0, sizeof(XrBcCallFrame) * (size_t) ctx->frame_capacity);
    }
}

static bool vm_backend_prepare_execution_state(XrCoroutine *coro, XrayIsolate *X, XrWorker *worker,
                                               bool need_storage, bool is_clean) {
    XrVmCoroState *state = vm_state_for_coro(coro);
    if (!state)
        return !need_storage;

    XrVMContext *ctx = &state->ctx;
    if (!is_clean) {
        XrValue *saved_stack = ctx->stack;
        int saved_stack_cap = ctx->stack_capacity;
        XrBcCallFrame *saved_frames = ctx->frames;
        int saved_frame_cap = ctx->frame_capacity;
        XrExceptionHandler *saved_handlers = ctx->handlers;
        int saved_handler_cap = ctx->handler_capacity;

        memset(ctx, 0, sizeof(*ctx));
        ctx->stack = saved_stack;
        ctx->stack_capacity = saved_stack_cap;
        ctx->frames = saved_frames;
        ctx->frame_capacity = saved_frame_cap;
        ctx->handlers = saved_handlers ? saved_handlers : ctx->handler_inline;
        ctx->handler_capacity = saved_handler_cap ? saved_handler_cap : XR_HANDLER_INLINE_CAP;
        vm_entry_reset_no_free(state);
    }

    if (need_storage) {
        if (!vm_backend_alloc_stack_frames(coro, ctx, worker))
            return false;
        if (!is_clean)
            vm_backend_clear_stack_frames(ctx);
        ctx->stack_top = ctx->stack;
    }

    ctx->current_coro = coro;
    ctx->isolate = X;
    return true;
}

static void vm_backend_free_stack_frames(XrCoroutine *coro, XrVMContext *ctx) {
    if (!coro || !ctx)
        return;
    if (ctx->stack) {
        char *stack_end = (char *) ctx->stack + sizeof(XrValue) * ctx->stack_capacity;
        bool combined = (ctx->frames && (char *) ctx->frames == stack_end);
        xr_free(ctx->stack);
        if (!combined && ctx->frames) {
            xr_free(ctx->frames);
        }
        ctx->stack = NULL;
        ctx->frames = NULL;
        ctx->stack_capacity = 0;
        ctx->frame_capacity = 0;
    } else if (ctx->frames) {
        xr_free(ctx->frames);
        ctx->frames = NULL;
        ctx->frame_capacity = 0;
    }
}

static void vm_backend_reset_handlers(XrVMContext *ctx) {
    if (!ctx)
        return;
    if (ctx->handlers && ctx->handlers != ctx->handler_inline) {
        xr_free(ctx->handlers);
    }
    ctx->handlers = ctx->handler_inline;
    ctx->handler_count = 0;
    ctx->handler_capacity = XR_HANDLER_INLINE_CAP;
}

static void vm_backend_free_struct_storage(XrVMContext *ctx) {
    if (!ctx)
        return;
    if (ctx->struct_areas) {
        for (int i = 0; i < ctx->struct_areas_cap; i++) {
            if (ctx->struct_areas[i])
                xr_free(ctx->struct_areas[i]);
        }
        xr_free(ctx->struct_areas);
        xr_free(ctx->struct_area_caps);
        ctx->struct_areas = NULL;
        ctx->struct_area_caps = NULL;
        ctx->struct_areas_cap = 0;
    }
    if (ctx->struct_ret_arena) {
        xr_free(ctx->struct_ret_arena);
        ctx->struct_ret_arena = NULL;
        ctx->struct_ret_arena_used = 0;
        ctx->struct_ret_arena_cap = 0;
    }
}

static void vm_backend_free_defer_state(XrVMContext *ctx) {
    if (!ctx)
        return;
    if (ctx->defer_stack) {
        xr_free(ctx->defer_stack);
        ctx->defer_stack = NULL;
    }
    if (ctx->defer_frame_marks) {
        xr_free(ctx->defer_frame_marks);
        ctx->defer_frame_marks = NULL;
    }
    ctx->defer_count = 0;
    ctx->defer_capacity = 0;
}

static bool vm_backend_prepare_recycle(XrCoroutine *coro, XrWorker *worker) {
    (void) worker;
    XrVmCoroState *state = vm_state_for_coro(coro);
    if (!state)
        return false;
    XrVMContext *ctx = &state->ctx;
    bool trim_backend_storage = (coro->gc_flags & XR_CORO_GC_TRIM_BACKEND_STORAGE) != 0;
    coro->gc_flags &= ~XR_CORO_GC_TRIM_BACKEND_STORAGE;
    if (!ctx->stack || !ctx->frames)
        return false;

    ctx->stack[0] = xr_null();
    ctx->stack_top = ctx->stack;
    ctx->frame_count = 0;
    ctx->handler_count = 0;
    vm_backend_free_defer_state(ctx);
    if (trim_backend_storage) {
        vm_backend_free_stack_frames(coro, ctx);
        vm_backend_reset_handlers(ctx);
        vm_backend_free_struct_storage(ctx);
    }
    vm_backend_clear_entry_state(coro);
    return true;
}

static void vm_backend_reset_reusable(XrCoroutine *coro) {
    if (!vm_state_for_coro(coro))
        return;
    vm_backend_clear_entry_state(coro);
}

static bool vm_backend_setup_yield_continuation(XrayIsolate *X, XrCoroutine *coro,
                                                void *continuation, void *user_data) {
    if (!coro || !continuation)
        return false;
    XrBcCallFrame *frames = NULL;
    int frame_count = 0;
    XrVmCoroState *state = vm_state_for_coro(coro);
    XrVMContext *ctx = state ? &state->ctx : NULL;

    if (ctx && ctx->frame_count > 0 && ctx->frames) {
        frames = ctx->frames;
        frame_count = ctx->frame_count;
    } else if (X && X->vm_ctx.frame_count > 0 && X->vm_ctx.frames) {
        frames = X->vm_ctx.frames;
        frame_count = X->vm_ctx.frame_count;
    } else {
        return false;
    }

    XrBcCallFrame *frame = &frames[frame_count - 1];
    int16_t saved_result_slot = frame->cfunc_result_slot;
    frame->u.c.continuation = continuation;
    frame->u.c.continuation_ctx = user_data;
    frame->has_cfunc_result = false;
    frame->cfunc_result = xr_null();
    frame->call_status |= XR_CALL_C | XR_CALL_HAS_CONT | XR_CALL_YIELDED;
    frame->cfunc_result_slot = saved_result_slot >= 0 ? saved_result_slot : -1;
    return true;
}

static bool vm_backend_has_continuation(const XrCoroutine *coro) {
    const XrVmCoroState *state = xr_coro_maybe_vm_state_const(coro);
    if (!state || state->ctx.frame_count == 0 || !state->ctx.frames)
        return false;
    const XrBcCallFrame *frame = &state->ctx.frames[state->ctx.frame_count - 1];
    return (frame->call_status & XR_CALL_HAS_CONT) && frame->u.c.continuation;
}

static XrCFuncResult vm_backend_call_closure(XrayIsolate *X, XrCoroutine *coro, XrClosure *closure,
                                             XrValue *args, int nargs, void *continuation,
                                             void *user_ctx, XrValue *result) {
    (void) X;
    (void) result;
    if (!coro || !closure || !closure->proto || !continuation)
        return XR_CFUNC_ERROR;
    XrVMContext *ctx = xr_coro_vm_ctx(coro);
    XrProto *proto = closure->proto;

    XR_DCHECK(ctx->frame_count > 0, "yield_call_closure: no active frame");
    XrBcCallFrame *caller = &ctx->frames[ctx->frame_count - 1];

    int closure_base_offset;
    if (caller->closure && caller->closure->proto) {
        closure_base_offset = caller->base_offset + caller->closure->proto->maxstacksize;
    } else {
        closure_base_offset = (int) (ctx->stack_top - ctx->stack);
    }

    int needed = closure_base_offset + proto->maxstacksize + 1;
    if (needed > ctx->stack_capacity || ctx->frame_count + 1 >= ctx->frame_capacity) {
        int extra = needed - ctx->stack_capacity + 64;
        if (extra < 64)
            extra = 64;
        if (!xr_coro_grow_stack(coro, extra))
            return XR_CFUNC_ERROR;
        caller = &ctx->frames[ctx->frame_count - 1];
    }

    int return_slot_offset = closure_base_offset - 1;
    if (return_slot_offset >= 0 && return_slot_offset < ctx->stack_capacity) {
        ctx->stack[return_slot_offset] = xr_null();
    }

    caller->call_status |= XR_CALL_CLOSURE_PENDING | XR_CALL_HAS_CONT | XR_CALL_C;
    caller->u.c.continuation = continuation;
    caller->u.c.continuation_ctx = user_ctx;
    caller->has_cfunc_result = false;
    caller->cfunc_result_slot = (int16_t) (return_slot_offset - caller->base_offset);

    XrBcCallFrame *frame = &ctx->frames[ctx->frame_count++];
    memset(frame, 0, sizeof(XrBcCallFrame));
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(proto);
    frame->base_offset = closure_base_offset;

    XrValue *closure_base = ctx->stack + closure_base_offset;
    int copy_count = nargs < proto->numparams ? nargs : proto->numparams;
    for (int i = 0; i < copy_count; i++) {
        closure_base[i] = args[i];
    }
    for (int i = copy_count; i < proto->maxstacksize; i++) {
        closure_base[i] = xr_null();
    }

    ctx->stack_top = ctx->stack + closure_base_offset + proto->maxstacksize;
    return XR_CFUNC_CALL_CLOSURE;
}

static void vm_backend_destroy(XrCoroutine *coro) {
    XrVmCoroState *state = vm_state_for_coro(coro);
    if (!coro || !state)
        return;
    XrVMContext *ctx = &state->ctx;

    vm_backend_free_stack_frames(coro, ctx);
    vm_backend_reset_handlers(ctx);
    vm_backend_free_struct_storage(ctx);
    vm_backend_free_defer_state(ctx);
    xr_vm_ctx_free_ic_tables(ctx);
    vm_backend_clear_entry_state(coro);

    if (coro->gc_flags & XR_CORO_GC_BACKEND_STATE_OWNED) {
        xr_free(state);
        coro->backend_state = NULL;
        coro->backend = NULL;
        coro->gc_flags &= ~XR_CORO_GC_BACKEND_STATE_OWNED;
    }
}

bool xr_coro_grow_stack(XrCoroutine *coro, int extra_slots) {
    if (!coro)
        return false;
    XrVMContext *ctx = xr_coro_vm_ctx(coro);
    if (!ctx->stack)
        return false;
    XR_DCHECK(extra_slots > 0, "grow_stack: non-positive extra_slots");
    XR_DCHECK(ctx->stack_capacity > 0, "grow_stack: zero stack_capacity");

    int new_capacity = ctx->stack_capacity + extra_slots;
    if (new_capacity > 1024 * 1024)
        return false;

    char *stack_end = (char *) ctx->stack + sizeof(XrValue) * ctx->stack_capacity;
    bool combined = ((char *) ctx->frames == stack_end);

    if (combined) {
        XrValue *new_stack = (XrValue *) xr_malloc(sizeof(XrValue) * new_capacity);
        if (!new_stack)
            return false;
        memcpy(new_stack, ctx->stack, sizeof(XrValue) * ctx->stack_capacity);
        memset(new_stack + ctx->stack_capacity, 0, sizeof(XrValue) * extra_slots);

        XrBcCallFrame *new_frames =
            (XrBcCallFrame *) xr_malloc(sizeof(XrBcCallFrame) * ctx->frame_capacity);
        if (!new_frames) {
            xr_free(new_stack);
            return false;
        }
        memcpy(new_frames, ctx->frames, sizeof(XrBcCallFrame) * ctx->frame_count);

        xr_free(ctx->stack);
        ctx->stack = new_stack;
        ctx->stack_capacity = new_capacity;
        ctx->frames = new_frames;
    } else {
        XrValue *new_stack = (XrValue *) xr_realloc(ctx->stack, sizeof(XrValue) * new_capacity);
        if (!new_stack)
            return false;
        memset(new_stack + ctx->stack_capacity, 0, sizeof(XrValue) * extra_slots);
        ctx->stack = new_stack;
        ctx->stack_capacity = new_capacity;
    }

    if (ctx->frame_count + 8 >= ctx->frame_capacity) {
        int new_frame_cap = ctx->frame_capacity * 2;
        XrBcCallFrame *new_frames =
            (XrBcCallFrame *) xr_realloc(ctx->frames, sizeof(XrBcCallFrame) * new_frame_cap);
        if (!new_frames)
            return false;
        ctx->frames = new_frames;

        // defer_frame_marks is indexed by frame index and lazily allocated to
        // frame_capacity; grow it in lockstep, otherwise deep recursion under an
        // active defer overruns it (startfunc records a mark per frame entry).
        if (ctx->defer_frame_marks) {
            int *new_marks =
                (int *) xr_realloc(ctx->defer_frame_marks, sizeof(int) * new_frame_cap);
            if (!new_marks)
                return false;
            for (int j = ctx->frame_capacity; j < new_frame_cap; j++)
                new_marks[j] = 0;
            ctx->defer_frame_marks = new_marks;
        }

        ctx->frame_capacity = new_frame_cap;
    }

    return true;
}

static XrCoroRunResult worker_run_result_from_vm(XrCoroutine *coro, XrVMResult result) {
    XrValue value = coro ? coro->result : XR_NULL_VAL;
    XrValue error = coro ? coro->error : XR_NULL_VAL;
    bool error_is_value = coro ? coro->error_is_value : false;

    switch (result) {
        case XR_VM_OK:
            return xr_coro_run_done(value);
        case XR_VM_BLOCKED:
            return xr_coro_run_result(XR_CORO_RUN_BLOCKED);
        case XR_VM_YIELD:
            return xr_coro_run_result(XR_CORO_RUN_YIELD);
        case XR_VM_GO_CHILD:
            return xr_coro_run_spawn_child(xr_coro_take_pending_spawn(coro));
        case XR_VM_CANCELLED:
            return xr_coro_run_result(XR_CORO_RUN_CANCELLED);
        case XR_VM_DEBUG_BREAK:
            return xr_coro_run_result(XR_CORO_RUN_DEBUG_BREAK);
        case XR_VM_COMPILE_ERROR:
        case XR_VM_RUNTIME_ERROR:
        default:
            return xr_coro_run_error(error, error_is_value);
    }
}

static XrCoroRunResult vm_backend_resume(XrCoroutine *coro, const XrCoroEvent *event,
                                         const XrCoroRunContext *run_ctx) {
    (void) event;
    if (!run_ctx || !run_ctx->worker)
        return xr_coro_run_error(XR_NULL_VAL, false);
    run_ctx->worker->p.vm_settled_coro = NULL;
    XrVMResult result = vm_backend_resume_on_worker(run_ctx->worker, coro);
    /* In-dispatch direct switches may settle execution on another coroutine;
     * the run result describes the coroutine that ran LAST, so map it from
     * that coroutine's fields (result/error/pending spawn). The field stays
     * published for the worker loop's own result handling, which consumes
     * (re-clears) it. */
    XrCoroutine *settled = run_ctx->worker->p.vm_settled_coro;
    if (settled && settled != coro)
        coro = settled;
    return worker_run_result_from_vm(coro, result);
}

// ========== Yieldable C-function coroutine ==========

// First call of a cfunc coroutine: build the C frame, run the body.
static XrVMResult run_cfunc_first_exec(XrayIsolate *isolate, XrCoroutine *coro,
                                       XrVMContext *coro_ctx, uint32_t cur_flags) {
    XrVmCoroState *vm_state = vm_state_for_coro(coro);
    if (!vm_state || vm_state->entry_type != XR_CORO_ENTRY_CFUNC || !vm_state->entry.cfunc)
        return XR_VM_RUNTIME_ERROR;

    (void) cur_flags;
    xr_coro_flags_swap(coro, XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED,
                       XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED);

    // Initialize first frame (for Yieldable support).
    coro_ctx->frame_count = 1;
    XrBcCallFrame *frame = &coro_ctx->frames[0];
    memset(frame, 0, sizeof(XrBcCallFrame));
    frame->closure = NULL;
    frame->pc = NULL;
    frame->base_offset = 1;  // Reserve stack[0] for return value.
    frame->flags = 0;
    frame->u.l.pending_operator_check = false;
    frame->call_status = XR_CALL_C;
    frame->u.c.continuation = NULL;
    frame->u.c.continuation_ctx = NULL;
    frame->cfunc_result_slot = 0;
    frame->has_cfunc_result = false;

    XrValue *base = coro_ctx->stack + frame->base_offset;
    for (int i = 0; i < vm_state->arg_count && i < 4; i++) {
        base[i] = vm_state->args[i];
    }
    coro_ctx->stack_top = coro_ctx->stack + 1 + vm_state->arg_count;

    XrValue cfunc_result = xr_null();
    XrCFuncResult status =
        vm_state->entry.cfunc(isolate, vm_state->args, vm_state->arg_count, &cfunc_result);
    switch (status) {
        case XR_CFUNC_DONE:
            coro_ctx->stack[0] = cfunc_result;
            return XR_VM_OK;
        case XR_CFUNC_BLOCKED:
            return XR_VM_BLOCKED;
        case XR_CFUNC_YIELD:
            return XR_VM_YIELD;
        case XR_CFUNC_CALL_CLOSURE:
            // Closure frame pushed by xr_call_closure, execute via VM.
            coro_ctx->module_base_frame = 0;
            return run(isolate, coro_ctx);
        case XR_CFUNC_WOULD_BLOCK:
            return XR_VM_RUNTIME_ERROR;
        default:
            return XR_VM_RUNTIME_ERROR;
    }
}

// Resume a previously-suspended cfunc coroutine. Includes an inline fast
// path for the common "single C frame with continuation" case (HTTP/WS
// handlers); falls back to VM continuation unroll otherwise.
static XrVMResult run_cfunc_resume(XrayIsolate *isolate, XrCoroutine *coro, XrVMContext *coro_ctx,
                                   uint32_t cur_flags) {
    (void) cur_flags;
    xr_coro_transition_to_running(coro);

    int resume_status = xr_coro_resume_load(coro);
    if (!resume_status)
        resume_status = XR_RESUME_IO_READY;
    finish_io_resume_tokens(coro, resume_status);

    // Inline fast path: single C frame with continuation.
    if (coro_ctx->frame_count == 1) {
        XrBcCallFrame *frame = &coro_ctx->frames[0];
        uint8_t need = XR_CALL_C | XR_CALL_HAS_CONT | XR_CALL_YIELDED;
        if ((frame->call_status & need) == need && frame->u.c.continuation) {
            XrContinuation cont = (XrContinuation) frame->u.c.continuation;
            void *user_ctx = frame->u.c.continuation_ctx;
            XrValue cfunc_result;
            // Fast-path resume: I/O / timer wakeup. resume_value carries no data
            // for these events (status alone is informative).
            XrCFuncResult status = cont(isolate, resume_status, xr_null(), user_ctx, &cfunc_result);
            switch (status) {
                case XR_CFUNC_DONE:
                    coro_ctx->stack[0] = cfunc_result;
                    frame->call_status &= ~(XR_CALL_C | XR_CALL_HAS_CONT | XR_CALL_YIELDED);
                    frame->u.c.continuation = NULL;
                    coro_ctx->frame_count = 0;
                    return XR_VM_OK;
                case XR_CFUNC_BLOCKED:
                    return XR_VM_BLOCKED;
                case XR_CFUNC_YIELD:
                    return XR_VM_YIELD;
                case XR_CFUNC_CALL_CLOSURE:
                    coro_ctx->module_base_frame = 0;
                    return run(isolate, coro_ctx);
                case XR_CFUNC_WOULD_BLOCK:
                    return XR_VM_RUNTIME_ERROR;
                default:
                    return XR_VM_RUNTIME_ERROR;
            }
        }
    }

    // Slow path: full unroll.
    XrVMResult result = xr_vm_coro_resume_with_unroll(isolate, coro, resume_status);
    if (result != XR_VM_OK)
        return result;

    // Single-frame result short-circuit after successful unroll.
    if (coro_ctx->frame_count == 1) {
        XrBcCallFrame *top = &coro_ctx->frames[0];
        if (top->has_cfunc_result) {
            coro_ctx->stack[0] = top->cfunc_result;
            coro_ctx->frame_count = 0;
            return XR_VM_OK;
        }
    }
    coro_ctx->module_base_frame = 0;
    if (coro_ctx->frame_count == 0)
        return XR_VM_OK;
    return run(isolate, coro_ctx);
}

// Execute Yieldable C function coroutine (supports I/O wait and rescheduling).
//
// First-exec and resume are factored into helpers above so the
// orchestration here stays small and obvious.
static XrVMResult run_cfunc_coro(XrWorker *worker, XrCoroutine *coro, XrayIsolate *isolate) {
    XrVMContext *ctx = xr_vm_machine_ctx(worker->m, isolate);
    if (!ctx)
        return XR_VM_RUNTIME_ERROR;
    XrVMContext *coro_ctx = xr_coro_vm_ctx(coro);

    ctx->current_coro = coro;
    coro_ctx->current_coro = coro;

    uint32_t cur_flags = atomic_load_explicit(&coro->flags, memory_order_relaxed);
    XrVMResult result;
    if (!(cur_flags & XR_CORO_FLG_STARTED)) {
        result = run_cfunc_first_exec(isolate, coro, coro_ctx, cur_flags);
    } else {
        result = run_cfunc_resume(isolate, coro, coro_ctx, cur_flags);
    }

    /* If a user closure called via xr_call_closure threw uncaught,
     * route the exception to the deepest pending C continuation so the
     * native state machine can clean up (send 500, close fd, free buffers)
     * before the coroutine dies. Without this hook, every framework-level
     * cfunc coroutine would leak resources whenever a handler throws. */
    while (result == XR_VM_RUNTIME_ERROR) {
        XrVMResult recovered =
            try_recover_via_closure_continuation(isolate, worker, coro, ctx, coro_ctx);
        if (recovered == XR_VM_RUNTIME_ERROR)
            break;
        result = recovered;
    }

    // ========== Shared Result Handling ==========
    // Handle result
    if (result == XR_VM_OK) {
        // Result MUST be stored before DONE flag (release). Otherwise a
        // parent doing acquire-load on DONE may see DONE=true but read a
        // stale (null) result on weakly-ordered architectures (ARM64).
        coro->result = coro_ctx->stack[0];
        xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
    } else if (result == XR_VM_BLOCKED) {
        (void) xr_coro_finalize_blocked_suspend(coro);
    } else if (result == XR_VM_YIELD) {
        xr_coro_transition_to_ready(coro);
    }

    ctx->current_coro = NULL;
    return result;
}

// ========== Worker Coroutine Execution ==========
//
// Executes directly on the coroutine's own VM value stack — no state copying.
//   - Each coroutine owns an independent XrVMContext stack and frame array.
//   - No native stack switching is performed.
//   - Eliminates state-copy race conditions across stealing.
//
// xr_coro_run_on_worker delegates execution to per-mode helpers,
// a thin dispatch shell that hands to run_first_exec / run_resume_path /
// run_cfunc_coro / run_finalize. See bottom of file for the shell.

// Recover from an uncaught exception by routing it to the deepest C frame
// that called xr_call_closure. The pending continuation is invoked
// with XR_RESUME_CLOSURE_ERROR so the C state machine can release resources
// (e.g. an HTTP server can send a 500 response and close the connection)
// instead of leaking the coroutine + any fds/buffers it owned.
//
// Returns the new XrVMResult to use for finalization, or XR_VM_RUNTIME_ERROR
// if no pending closure frame existed (caller falls through to error path).
static XrVMResult try_recover_via_closure_continuation(XrayIsolate *isolate, XrWorker *worker,
                                                       XrCoroutine *coro, XrVMContext *ctx,
                                                       XrVMContext *coro_ctx) {
    (void) worker;
    /* Find the deepest frame waiting on xr_call_closure completion. */
    int target_fc = -1;
    for (int i = coro_ctx->frame_count - 1; i >= 0; i--) {
        if (coro_ctx->frames[i].call_status & XR_CALL_CLOSURE_PENDING) {
            target_fc = i;
            break;
        }
    }
    if (target_fc < 0)
        return XR_VM_RUNTIME_ERROR; /* no recovery possible */

    XrBcCallFrame *frame = &coro_ctx->frames[target_fc];
    XrContinuation cont = (XrContinuation) frame->u.c.continuation;
    void *user_ctx = frame->u.c.continuation_ctx;
    if (!cont)
        return XR_VM_RUNTIME_ERROR;

    /* Pop the closure frame(s) above the pending C frame and adjust stack_top
     * so the C continuation sees a coherent state. */
    coro_ctx->frame_count = target_fc + 1;
    if (frame->closure && frame->closure->proto) {
        coro_ctx->stack_top =
            coro_ctx->stack + frame->base_offset + frame->closure->proto->maxstacksize;
    }

    /* Hand the exception over to the continuation. Capture the value before
     * clearing VM-wide pending exception so subsequent dispatch starts
     * clean and the cont call sees a coherent state. */
    XrValue exc_value = coro_ctx->current_exception;
    coro_ctx->current_exception = xr_null();
    coro_ctx->pending_error = xr_null();
    frame->call_status &= ~XR_CALL_CLOSURE_PENDING;

    ctx->current_coro = coro;

    XrValue cresult = xr_null();
    XrCFuncResult cstatus = cont(isolate, XR_RESUME_CLOSURE_ERROR, exc_value, user_ctx, &cresult);

    /* Translate continuation outcome back into a VM result. The continuation
     * may have yielded (e.g. queued an async write of a 500 response) or
     * pushed another closure (e.g. invoking a user-level error handler). */
    switch (cstatus) {
        case XR_CFUNC_DONE:
            if (coro_ctx->stack_capacity > 0)
                coro_ctx->stack[0] = cresult;
            return XR_VM_OK;
        case XR_CFUNC_YIELD:
            return XR_VM_YIELD;
        case XR_CFUNC_BLOCKED:
            return XR_VM_BLOCKED;
        case XR_CFUNC_CALL_CLOSURE:
            coro_ctx->module_base_frame = 0;
            return run(isolate, coro_ctx);
        case XR_CFUNC_WOULD_BLOCK:
            return XR_VM_RUNTIME_ERROR;
        case XR_CFUNC_ERROR:
        default:
            return XR_VM_RUNTIME_ERROR;
    }
}

// ========== run_finalize: Result Handling ==========
//
// Centralises the post-run state transitions and result/error copy-outs that
// every execution path in xr_coro_run_on_worker must perform.
// Assumes ctx->current_coro == coro; clears it before returning (except for
// continuation-stealing / debug-break which leave the caller in charge).
static XrVMResult run_finalize(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                               XrVMContext *ctx, XrVMContext *coro_ctx, XrVMResult result) {
    (void) worker;
    /* Iteratively drain pending closure continuations on uncaught exception:
     * each invocation may complete cleanly, yield, block, or re-throw. We loop
     * to avoid recursion and to surface the final state once recovery settles. */
    while (result == XR_VM_RUNTIME_ERROR) {
        XrVMResult recovered =
            try_recover_via_closure_continuation(isolate, worker, coro, ctx, coro_ctx);
        if (recovered == XR_VM_RUNTIME_ERROR)
            break; /* no more pending continuations, or continuation re-threw */
        result = recovered;
    }
    if (result == XR_VM_GO_CHILD) {
        // Continuation stealing: parent saved state, child ready to run inline.
        ctx->current_coro = NULL;
        return result;
    }
    if (result == XR_VM_DEBUG_BREAK) {
        (void) xr_coro_try_transition_to_blocked(coro);
        // Don't clear current_coro — caller handles debug break.
        ctx->current_coro = NULL;
        return result;
    }
    if (result == XR_VM_BLOCKED || result == XR_VM_YIELD) {
        if (result == XR_VM_YIELD) {
            xr_coro_transition_to_ready(coro);
        } else {
            (void) xr_coro_finalize_blocked_suspend(coro);
        }
        if (result == XR_VM_BLOCKED && coro->ext &&
            atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
            XR_DBG_CORO("coro id=%d timer blocked, waiting for Timer Wheel callback", coro->id);
        }
    } else if (result == XR_VM_OK && XR_IS_NULL(coro_ctx->pending_error)) {
        coro->result = coro_ctx->stack[0];
        XR_DBG_CORO("run_on_worker: coro id=%d completed, result tag=%u", coro->id,
                    coro_ctx->stack[0].tag);
    } else {
        /* Error: capture the original Exception object so linked-scope
         * rethrow preserves it (avoids wrapping into Exception.data). */
        coro->result = xr_null();
        xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
        XrValue exc = xr_null();
        /* Determine which channel the failure came from so linked scopes
         * can re-raise on the matching channel in the parent. */
        coro->error_is_value = !XR_IS_NULL(coro_ctx->pending_error);
        if (!XR_IS_NULL(coro_ctx->pending_error)) {
            exc = coro_ctx->pending_error;
        } else if (!XR_IS_NULL(coro_ctx->current_exception)) {
            exc = coro_ctx->current_exception;
        }
        if (!XR_IS_NULL(exc)) {
            coro->error = exc;
        } else {
            XrString *s = xr_string_intern(isolate, "coroutine error", 15, 0);
            coro->error = xr_string_value(s);
        }
        /* Uncaught value-return error diagnostic. Panic faults print during
         * unwind; errors of an awaited or linked coroutine are surfaced by the
         * awaiting/parent task, so reprinting them here would double-report.
         * Two cases ARE printed because their error would otherwise vanish
         * with no observer:
         *   - the main coroutine (top-level program), and
         *   - a statement-form fire-and-forget `go f()`. The codegen only
         *     allocates an XrTask when the result/Task state is user-visible
         *     (awaited handle, or a linked/scoped child whose error propagates
         *     to a parent), so coro->task == NULL is exactly the case where no
         *     observer exists. Silently swallowing such an error once disguised
         *     a deterministic compiler miscompile as a scheduler "lost wakeup",
         *     so surfacing it is worth a line on stderr. */
        if (coro->error_is_value && !(isolate && isolate->suppress_exception_print) &&
            !XR_IS_NULL(coro->error)) {
            bool is_main = xr_coro_flags_has(coro, XR_CORO_FLG_MAIN);
            bool dropped_fire_and_forget = !is_main && coro->task == NULL;
            if (is_main || dropped_fire_and_forget) {
                XrString *msg = xr_value_to_string(isolate, coro->error);
                fprintf(stderr, "\n[Uncaught Error%s] %s\n", is_main ? "" : " in go coroutine",
                        msg ? msg->data : "<error>");
            }
        }
        coro_ctx->current_exception = xr_null();
        coro_ctx->pending_error = xr_null();
    }

    ctx->current_coro = NULL;
    XR_DBG_CORO("run_on_worker: return result=%d, coro id=%d", result, coro->id);
    return result;
}

// ========== run_dispatch_and_finalize: Top-Level run() Entry ==========
//
// Single wrapper for every top-level interpreter entry (worker dispatch with
// no native caller frame between run() and the worker loop). Arms the
// in-dispatch direct-switch admission flag, and — because a direct switch
// moves execution to another coroutine WITHOUT exiting run() — re-resolves
// the coroutine that actually settled before finalizing, so result/error
// copy-outs and state transitions land on the right coroutine.
//
// Nested run() entries (xr_call_closure, module exec, cfunc continuation
// closures) must NOT use this wrapper: a switch there would let the next
// coroutine return through a foreign native frame.
static XrVMResult run_dispatch_and_finalize(XrayIsolate *isolate, XrWorker *worker,
                                            XrCoroutine *coro, XrVMContext *ctx,
                                            XrVMContext *coro_ctx) {
    /* Worker-local admission flag: written only by the owner thread, so it
     * stays race-free even when blocked coroutines from the switch chain are
     * re-owned by other workers mid-run(). */
    worker->p.vm_direct_switch_ok = true;
    XrVMResult result = run(isolate, coro_ctx);
    worker->p.vm_direct_switch_ok = false;
    /* The machine ctx current_coro is maintained by the direct-switch helper,
     * so after run() it names the coroutine that actually ran last. */
    XrCoroutine *settled = (XrCoroutine *) ctx->current_coro;
    if (settled && settled != coro) {
        coro = settled;
        coro_ctx = xr_coro_vm_ctx(settled);
    }
    /* Publish for vm_backend_resume and the worker result handling; both
     * consume it on their side (vm_backend_resume clears at entry, the
     * worker loop re-clears after reading). */
    worker->p.vm_settled_coro = coro;
    return run_finalize(isolate, worker, coro, ctx, coro_ctx, result);
}

// ========== In-Dispatch Direct Coroutine Switch ==========
//
// When a VM coroutine blocks on a channel/await op inside run() and a
// just-woken partner sits in this worker's LIFO slot, swap the interpreted
// context and keep executing instead of exiting run() through the worker
// loop (run_finalize -> worker result handling -> backend vtable -> run()
// re-entry). This removes the per-switch constant cost that dominates
// serial message chains (pingpong/ring/pipeline).
//
// Admission is deliberately narrow; every excluded shape falls back to the
// ordinary XR_VM_BLOCKED slow path with unchanged semantics.
XR_FUNC XrVMContext *xr_vm_try_direct_switch(XrayIsolate *isolate, XrVMContext *cur_ctx) {
#if !XR_VM_DIRECT_SWITCH
    (void) isolate;
    (void) cur_ctx;
    return NULL;
#else
    XrWorker *worker = xr_current_worker();
    if (!worker)
        return NULL;
    XrProc *p = &worker->p;
    if (!p->vm_direct_switch_ok || p->direct_switch_budget <= 0)
        return NULL;

    /* Peek before any side effect: the LIFO slot is consumed only by the
     * owning worker thread, so a non-destructive admission check on the
     * pointee is race-free. */
    XrCoroutine *next = atomic_load_explicit(&p->lifo_slot, memory_order_relaxed);
    if (!next)
        return NULL;
    XrCoroutine *cur = (XrCoroutine *) cur_ctx->current_coro;
    if (!cur || cur == next || xr_coro_flags_has(cur, XR_CORO_FLG_MAIN))
        return NULL;
    /* Cfunc-entry coroutines carry continuation state machines on their
     * frame stack; keep them on the protocol path (both sides). */
    XrVmCoroState *cur_state = vm_state_for_coro(cur);
    if (!cur_state || cur_state->entry_type == XR_CORO_ENTRY_CFUNC)
        return NULL;
    /* Debugger sessions expect scheduler-visible suspend/resume edges. */
    XrDebugHooks *hooks = (XrDebugHooks *) isolate->debug_hooks;
    if (hooks && hooks->is_enabled && hooks->is_enabled(isolate))
        return NULL;

    /* Partner admission. The acquire flags load pairs with the waker's
     * release state transition, making the partner's saved frame and any
     * delivered value slots visible before we touch its context. */
    uint32_t nflags = atomic_load_explicit(&next->flags, memory_order_acquire);
    if (!(nflags & XR_CORO_FLG_STARTED) ||
        (nflags & (XR_CORO_FLG_DONE | XR_CORO_FLG_CANCELLED | XR_CORO_FLG_CANCEL_REQUESTED |
                   XR_CORO_FLG_MAIN)))
        return NULL;
    if (!next->backend || next->backend->kind != XR_CORO_BACKEND_VM)
        return NULL;
    XrVmCoroState *next_state = vm_state_for_coro(next);
    if (!next_state || next_state->entry_type == XR_CORO_ENTRY_CFUNC)
        return NULL;
    XrVMContext *next_ctx = &next_state->ctx;
    if (next_ctx->frame_count <= 0 || !next_ctx->frames || !next_ctx->stack)
        return NULL;
    XrBcCallFrame *tf = &next_ctx->frames[next_ctx->frame_count - 1];
    if ((tf->call_status & XR_CALL_C) || !tf->closure || !tf->closure->proto)
        return NULL;
    int resume = xr_coro_resume_load(next);
    bool chan_resume = (resume == XR_RESUME_CHANNEL);
    if (resume != XR_RESUME_OK && resume != XR_RESUME_CONTINUATION && !chan_resume)
        return NULL;
    if (chan_resume && xr_coro_select_wait(next))
        return NULL;

    /* Pop through the gated LIFO entry point so the backlog anti-starvation
     * gate still applies; the gate may flush the slot and return NULL. */
    XrCoroutine *popped = xr_worker_try_pop_lifo(worker, false);
    if (!popped)
        return NULL;
    XR_DCHECK(popped == next, "direct_switch: LIFO slot changed under owner");

    /* Blocked-side bookkeeping for cur — replaces what run_finalize and the
     * worker BLOCKED result handling would have done had run() exited. If
     * the post-check reports cur already re-readied by a concurrent waker,
     * cur is owned elsewhere and must not be touched further. */
    (void) xr_coro_finalize_blocked_suspend(cur);
    if (!worker_process_blocked(worker, cur))
        cur->spawn_burst_count = 0;

    /* Resume-side state for next — mirrors the backend resume paths for the
     * admitted shapes. */
    next->next = NULL;
    next->prev = NULL;
    xr_coro_transition_to_running(next);
    if (chan_resume) {
        tf->call_status &= ~XR_CALL_YIELDED;
        XrCoroExt *ext = next->ext;
        if (ext && ext->chan_resume_delivered) {
            /* Resume-with-value delivery: value+ok already written by the
             * waker; skip the instruction replay and continue from the next
             * instruction. */
            ext->chan_resume_delivered = false;
            ext->chan_ok_slot_ref = xr_slot_none();
            tf->pc += 1;
            xr_coro_resume_store(next, XR_RESUME_OK);
            xr_channel_wait_token_finish(&ext->chan_wait_token);
        }
    } else if (resume == XR_RESUME_CONTINUATION) {
        xr_coro_resume_store(next, XR_RESUME_OK);
    } else {
        /* Plain ready (yielded earlier): clear the replay marker exactly
         * like the unroll path does for a bytecode top frame. */
        tf->call_status &= ~XR_CALL_YIELDED;
    }

    /* Worker/machine bookkeeping. */
    XrMachine *m = worker->m;
    atomic_store_explicit(&m->current_coro, next, memory_order_relaxed);
    atomic_store_explicit(&m->heartbeat,
                          atomic_load_explicit(&m->heartbeat, memory_order_relaxed) + 1,
                          memory_order_relaxed);
    XrVMContext *mctx = xr_vm_machine_ctx(m, isolate);
    if (mctx)
        mctx->current_coro = next;
    next_ctx->current_coro = next;
    next_ctx->module_base_frame = 0;
    p->direct_switch_budget--;
    p->stats.vm_direct_switch_count++;
    p->stats.executed_count++;
    p->yield_streak = 0;

    /* Periodic lightweight housekeeping, mirroring fast re-dispatch: keep
     * cross-worker deliveries and timers visible during long chains. */
    if ((p->direct_switch_budget & 7) == 0) {
        worker_drain_inbox(worker);
        worker_pull_inject(worker, XR_FAST_DISPATCH_INJECT_BATCH);
    }
    if ((p->direct_switch_budget & 15) == 0) {
        int64_t now = xr_monotonic_ticks();
        if (p->timer_wheel &&
            (xr_timer_cancel_pending(p->timer_wheel) || now > xr_proc_last_timer_tick(p))) {
            xr_bump_timers(p->timer_wheel, now);
            p->stats.timer_bump_count++;
            if (now > xr_proc_last_timer_tick(p))
                xr_proc_set_last_timer_tick(p, now);
        }
    }
    return next_ctx;
#endif
}

// ========== run_first_exec: Frame Setup + Interpreter ==========
//
// Builds the coroutine's first bytecode frame (VM stack/frame/args), then runs
// the interpreter.
//
// Precondition: caller has already set RUNNING|STARTED on coro->flags and
// bound current_coro on both ctx and coro_ctx. VM state must carry a
// non-NULL closure with a non-NULL proto (validated upstream).
static XrVMResult run_first_exec(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                                 XrVMContext *ctx, XrVMContext *coro_ctx) {
    (void) worker;
    XrVmCoroState *vm_state = vm_state_for_coro(coro);
    if (!vm_state || vm_state->entry_type != XR_CORO_ENTRY_CLOSURE)
        return XR_VM_RUNTIME_ERROR;
    XrClosure *closure = vm_state->entry.closure;
    if (!closure || !closure->proto)
        return XR_VM_RUNTIME_ERROR;
    XrProto *proto = closure->proto;

    // Ensure stack capacity covers the entry function's register file.
    // The module init proto may require more slots than the initial 64.
    int needed = 1 + proto->maxstacksize;  // 1 reserved return slot + registers
    if (needed > coro_ctx->stack_capacity) {
        int extra = needed - coro_ctx->stack_capacity + 64;
        if (!xr_coro_grow_stack(coro, extra)) {
            return XR_VM_RUNTIME_ERROR;
        }
    }

    // Initialize frame directly on coroutine stack.
    XrValue *stack_base = coro_ctx->stack;
    stack_base[0] = xr_null();  // Reserved return slot.
    XrValue *func_base = stack_base + 1;

    for (int i = 0; i < vm_state->arg_count; i++) {
        func_base[i] = vm_state->args[i];
    }

    coro_ctx->frame_count = 1;
    XrBcCallFrame *frame = &coro_ctx->frames[0];
    memset(frame, 0, sizeof(XrBcCallFrame));
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(proto);
    frame->base_offset = (int) (func_base - coro_ctx->stack);
    frame->flags = 0;
    frame->u.l.pending_operator_check = false;
    frame->call_status = 0;
    frame->u.c.continuation = NULL;
    frame->u.c.continuation_ctx = NULL;
    frame->cfunc_result_slot = -1;
    frame->has_cfunc_result = false;

    coro_ctx->stack_top = coro_ctx->stack + frame->base_offset + proto->maxstacksize;
    coro_ctx->module_base_frame = 0;

    return run_dispatch_and_finalize(isolate, worker, coro, ctx, coro_ctx);
}

// ========== run_resume_path: Continuation + Unroll ==========
//
// Handles every resume case except the inline channel-resume fast path that
// xr_coro_run_on_worker handles directly.  Supports:
//   - XR_RESUME_CONTINUATION / XR_RESUME_DEBUG (run() directly)
//   - Default unroll via VM continuation unroll then run()
static XrVMResult run_resume_path(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                                  XrVMContext *ctx, XrVMContext *coro_ctx) {
    (void) worker;
    xr_coro_transition_to_running(coro);
    XR_DBG_CORO("run_on_worker: resuming coro id=%d", coro->id);

    XrVMResult result;

    // Continuation stealing resume: vm_ctx already set, just call run().
    if (xr_coro_resume_load(coro) == XR_RESUME_CONTINUATION) {
        xr_coro_resume_store(coro, 0);
        coro_ctx->current_coro = coro;
        coro_ctx->module_base_frame = 0;
        return run_dispatch_and_finalize(isolate, worker, coro, ctx, coro_ctx);
    }

    // Debug break resumption: no unroll needed.
    if (xr_coro_resume_load(coro) == XR_RESUME_DEBUG) {
        xr_coro_resume_store(coro, 0);
        coro_ctx->current_coro = coro;
        coro_ctx->module_base_frame = 0;
        return run_dispatch_and_finalize(isolate, worker, coro, ctx, coro_ctx);
    }

    // Default: unroll then run.
    int resume_status = xr_coro_resume_load(coro) ? xr_coro_resume_load(coro) : XR_RESUME_IO_READY;
    finish_io_resume_tokens(coro, resume_status);
    XrVMResult unroll_result = xr_vm_coro_resume_with_unroll(isolate, coro, resume_status);
    XR_DBG_CORO("run_on_worker: coro id=%d, unroll result=%d, frame_count=%d", coro->id,
                unroll_result, coro_ctx->frame_count);

    if (unroll_result == XR_VM_OK) {
        coro_ctx->module_base_frame = 0;
        if (coro_ctx->frame_count == 0) {
            XR_DBG_CORO("run_on_worker: frame_count=0, coroutine completed");
            result = XR_VM_OK;
        } else {
            XR_DBG_CORO("run_on_worker: continue bytecode execution, frame_count=%d",
                        coro_ctx->frame_count);
            return run_dispatch_and_finalize(isolate, worker, coro, ctx, coro_ctx);
        }
        XR_DBG_CORO("run_on_worker: bytecode execution complete, result=%d", result);
    } else if (unroll_result == XR_VM_BLOCKED) {
        result = XR_VM_BLOCKED;
    } else if (unroll_result == XR_VM_YIELD) {
        result = XR_VM_YIELD;
    } else {
        result = XR_VM_RUNTIME_ERROR;
    }

    return run_finalize(isolate, worker, coro, ctx, coro_ctx, result);
}

// ========== Thin Dispatch Shell ==========
//
// vm_backend_resume_on_worker is the VM backend dispatch hub combining
// entry checks, channel-resume fast path, first-execution setup, unroll
// resume, and result finalization. It is now a thin dispatch shell that
// hands off to run_first_exec / run_resume_path / run_cfunc_coro /
// run_finalize as appropriate.
static XrVMResult vm_backend_resume_on_worker(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro)
        return XR_VM_RUNTIME_ERROR;

    if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
        return XR_VM_CANCELLED;
    }

    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        return XR_VM_OK;
    }

    XR_DCHECK(worker->p.runtime != NULL, "worker thread: runtime is NULL");
    XrayIsolate *isolate = (XrayIsolate *) xr_scheduler_host_backend_context(worker->p.runtime);
    XR_DCHECK(isolate != NULL, "worker thread: VM scheduler host isolate is NULL");

    XrVMContext *ctx = xr_vm_machine_ctx(worker->m, isolate);
    if (!ctx)
        return XR_VM_RUNTIME_ERROR;
    XrVmCoroState *vm_state = vm_state_for_coro(coro);
    if (!vm_state)
        return XR_VM_RUNTIME_ERROR;
    XrVMContext *coro_ctx = &vm_state->ctx;

    uint32_t _fast_flags = xr_coro_flags_load(coro);
    int _fast_resume = xr_coro_resume_load(coro);

    // ========== Channel Resume Fast Path ==========
    // Most common resume case: channel wake of a started coroutine. Acquire-
    // load of flags establishes happens-before with sender's flag swap so
    // recv_slot is visible before we enter run().
    if ((_fast_resume == XR_RESUME_CHANNEL || is_select_channel_resume(coro, _fast_resume)) &&
        (_fast_flags & XR_CORO_FLG_STARTED)) {
        ctx->current_coro = coro;
        coro_ctx->current_coro = coro;
        coro->next = NULL;
        coro->prev = NULL;
        xr_coro_transition_to_running(coro);
        consume_select_channel_resume(coro);

        // VM bytecode channel resume
        if (coro_ctx->frame_count > 0) {
            XrBcCallFrame *tf = &coro_ctx->frames[coro_ctx->frame_count - 1];
            if (tf->call_status & XR_CALL_C) {
                // C continuation: fall through to the full unroll path.
                return run_resume_path(isolate, worker, coro, ctx, coro_ctx);
            }
            tf->call_status &= ~XR_CALL_YIELDED;

            // Resume-with-value delivery: the waker already stored value+ok
            // into the register slots, so skip the instruction replay and
            // continue from the next instruction (frame->pc still points at
            // the parked channel instruction). Clearing resume_status keeps
            // the recv head protocol check on its no-resume fast branch.
            XrCoroExt *ext = coro->ext;
            if (ext && ext->chan_resume_delivered) {
                ext->chan_resume_delivered = false;
                ext->chan_ok_slot_ref = xr_slot_none();
                tf->pc += 1;
                xr_coro_resume_store(coro, XR_RESUME_OK);
                xr_channel_wait_token_finish(&ext->chan_wait_token);
            }
        }
        coro_ctx->module_base_frame = 0;
        return run_dispatch_and_finalize(isolate, worker, coro, ctx, coro_ctx);
    }

    // ========== Cfunc First-Run + Resume Fast Path ==========
    // Must check BEFORE closure path: entry union overlaps.
    if (vm_state->entry_type == XR_CORO_ENTRY_CFUNC && vm_state->entry.cfunc) {
        return run_cfunc_coro(worker, coro, isolate);
    }

    // ========== Closure First Execution Fast Path ==========
    if (!(_fast_flags & XR_CORO_FLG_STARTED) && _fast_resume == 0) {
        XrClosure *closure = vm_state->entry.closure;
        if (closure && closure->proto) {
            ctx->current_coro = coro;
            coro_ctx->current_coro = coro;
            coro->next = NULL;
            coro->prev = NULL;
            xr_coro_flags_swap(coro, XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED,
                               XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED);
            return run_first_exec(isolate, worker, coro, ctx, coro_ctx);
        }
    }

    // ========== Slow Path ==========
    XR_DBG_FRAME("run_on_worker: coro=%p, id=%d, stack=%p, frames=%p, frame_count=%d",
                 (void *) coro, coro->id, (void *) xr_coro_vm_ctx(coro)->stack,
                 (void *) xr_coro_vm_ctx(coro)->frames, xr_coro_vm_ctx(coro)->frame_count);

    if (ctx->isolate != isolate) {
        ctx->isolate = isolate;
    }

    // Cfunc (after isolate check): same as fast path.
    if (vm_state->entry_type == XR_CORO_ENTRY_CFUNC && vm_state->entry.cfunc) {
        return run_cfunc_coro(worker, coro, isolate);
    }

    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        return XR_VM_OK;
    }

    if (!xr_coro_vm_ctx(coro)->stack || !xr_coro_vm_ctx(coro)->frames) {
        xr_log_warning("coro", "coroutine stack not allocated (stack=%p, frames=%p, coro=%p)",
                       (void *) xr_coro_vm_ctx(coro)->stack, (void *) xr_coro_vm_ctx(coro)->frames,
                       (void *) coro);
        coro->result = xr_null();
        xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
        return XR_VM_RUNTIME_ERROR;
    }

    ctx->current_coro = coro;
    coro_ctx->current_coro = coro;
    coro->next = NULL;
    coro->prev = NULL;

    // First execution slow path: closure must be validated before run_first_exec.
    uint32_t _flags_snap = atomic_load_explicit(&coro->flags, memory_order_relaxed);
    if (!(_flags_snap & XR_CORO_FLG_STARTED)) {
        xr_coro_flags_swap(coro, XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED,
                           XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED);

        XrClosure *_slow_cl = vm_state->entry.closure;
        if (!_slow_cl || !_slow_cl->proto) {
            xr_log_warning("coro", "coroutine closure invalid (closure=%p, coro=%p)",
                           (void *) _slow_cl, (void *) coro);
            coro->result = xr_null();
            xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
            return XR_VM_RUNTIME_ERROR;
        }
        return run_first_exec(isolate, worker, coro, ctx, coro_ctx);
    }

    return run_resume_path(isolate, worker, coro, ctx, coro_ctx);
}
