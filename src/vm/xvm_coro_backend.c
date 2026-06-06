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
 *   VM-shaped execution state, maps VM/JIT/CFunc outcomes into the coroutine
 *   ABI, and keeps interpreter frame details out of the worker hot path.
 */

#include "../coro/xworker_internal.h"
#include "../coro/xblock.h"
#include "../coro/xcoro_backend_ops.h"
#include "../coro/xdeep_copy.h"
#include "../coro/xjit_hooks.h"
#include "../coro/xtask.h"
#include "../coro/xyieldable.h"
#include "../runtime/xexec_frame.h"
#include "../runtime/xvm_call.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/gc/xsystem_heap.h"
#include "../runtime/value/xvalue_format.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../base/xlog.h"
#include "xvm_internal.h"
#include "xvm_coro_state.h"
#include "xvm_resume.h"
#include "xvm_worker_state.h"

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
static void vm_backend_on_safepoint(XrCoroutine *coro);
static void vm_backend_detach_worker_state(XrCoroutine *coro);
static bool vm_backend_in_try_mode(const XrCoroutine *coro);
static bool vm_backend_setup_yield_continuation(XrayIsolate *X, XrCoroutine *coro,
                                                void *continuation, void *user_data);
static bool vm_backend_has_continuation(const XrCoroutine *coro);
static int vm_backend_call_closure(XrayIsolate *X, XrCoroutine *coro, XrClosure *closure,
                                   XrValue *args, int nargs, void *continuation, void *user_ctx,
                                   XrValue *result);

static const XrCoroBackendVTable vm_backend_vtable = {
    .kind = XR_CORO_BACKEND_VM,
    .resume = vm_backend_resume,
    .trace_roots = NULL,
    .prepare_recycle = vm_backend_prepare_recycle,
    .reset_reusable = vm_backend_reset_reusable,
    .on_safepoint = vm_backend_on_safepoint,
    .detach_worker_state = vm_backend_detach_worker_state,
    .is_try_mode = vm_backend_in_try_mode,
    .release = NULL,
    .destroy = vm_backend_destroy,
    .debug_name = vm_backend_debug_name,
    .debug_snapshot = vm_backend_debug_snapshot,
};

static const XrCoroBackendOps vm_backend_ops = {
    .ensure_state = vm_backend_ensure_state,
    .prepare_execution_state = vm_backend_prepare_execution_state,
    .reset_execution_state = vm_backend_reset_execution_state,
    .clear_entry_state = vm_backend_clear_entry_state,
    .reset_entry_state_no_free = vm_backend_reset_entry_state_no_free,
    .bind_closure_entry = vm_backend_bind_closure_entry,
    .bind_cfunc_entry = vm_backend_bind_cfunc_entry,
    .setup_yield_continuation = vm_backend_setup_yield_continuation,
    .has_continuation = vm_backend_has_continuation,
    .call_closure = vm_backend_call_closure,
};

const XrCoroBackendVTable *xr_coro_vm_backend_vtable(void) {
    return &vm_backend_vtable;
}

const XrCoroBackendOps *xr_coro_vm_backend_ops(void) {
    return &vm_backend_ops;
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
    state->entry_type = XR_CORO_ENTRY_CLOSURE;
    state->entry.closure = NULL;
    state->args = NULL;
    state->arg_count = 0;
    for (int i = 0; i < 4; i++)
        state->inline_args[i] = xr_null();
}

static bool vm_backend_ensure_state(XrCoroutine *coro) {
    if (!coro)
        return false;
    if (coro->backend == &vm_backend_vtable)
        coro->backend_ops = &vm_backend_ops;
    if (vm_state_for_coro(coro))
        return true;
    if (coro->backend && coro->backend != &vm_backend_vtable && coro->backend_state)
        return false;
    XrVmCoroState *state = (XrVmCoroState *) xr_calloc(1, sizeof(XrVmCoroState));
    if (!state)
        return false;
    state->ctx.handlers = state->ctx.handler_inline;
    state->ctx.handler_capacity = XR_HANDLER_INLINE_CAP;
    xr_coro_attach_backend(coro, xr_coro_vm_backend_vtable(), state, &vm_backend_ops);
    coro->gc_flags |= XR_CORO_GC_BACKEND_STATE_OWNED;
    return true;
}

static XrCoroutine *vm_backend_alloc_shell(XrayIsolate *X, bool use_runtime_pool) {
    XrCoroutine *coro = NULL;
    if (use_runtime_pool) {
        XrRuntime *runtime = (XrRuntime *) X->vm.runtime;
        if (runtime) {
            coro = xr_coro_pool_get(runtime);
        }
    }
    if (!coro) {
        if (X->sys_heap) {
            coro = xr_sysheap_alloc_coro(X->sys_heap);
            if (!coro)
                return NULL;
            coro->coro_gc = NULL;
        } else {
            coro = (XrCoroutine *) xr_malloc(sizeof(XrCoroutine));
            if (!coro)
                return NULL;
            memset(coro, 0, sizeof(XrCoroutine));
            coro->gc.type = XR_TCOROUTINE;
            coro->coro_gc = NULL;
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

    if (!coro->coro_gc) {
        XrCoroGCConfig main_config = {
            .gc_threshold = XR_MAIN_CORO_GC_THRESHOLD,
            .gc_pause = XR_MAIN_CORO_GC_PAUSE,
            .gc_stepmul = XR_MAIN_CORO_GC_STEPMUL,
        };
        coro->coro_gc = xr_coro_gc_create(coro, &main_config);
        if (!coro->coro_gc) {
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

    if (coro->coro_gc) {
        coro->coro_gc->gc_disabled = 0;
    }

    vm_backend_reset_execution_state(coro, X);

    bool bound = vm_backend_bind_closure_entry(coro, X, closure, NULL, 0, false);
    XR_CHECK(bound, "coro_reset_for_call: failed to bind VM closure");
    (void) xr_coro_set_source(coro, closure->proto ? closure->proto->source_file : NULL, 0);

    coro->result = xr_null();
    coro->error = xr_null();
    coro->current_scope = NULL;
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

static XrJitScratch *vm_worker_jit_scratch(XrWorker *worker) {
    if (!worker || !XR_JIT_AVAILABLE() || !xr_jit_hooks->worker_scratch)
        return NULL;
    return (XrJitScratch *) xr_jit_hooks->worker_scratch(worker->p.backend_worker_storage);
}

static void vm_bind_worker_jit_scratch(XrWorker *worker, XrJitCoroState *jit_state) {
    if (!jit_state)
        return;
    XrJitScratch *scratch = vm_worker_jit_scratch(worker);
    jit_state->scratch = scratch;
    if (scratch && worker && worker->m && xr_jit_hooks->worker_set_heartbeat) {
        xr_jit_hooks->worker_set_heartbeat(worker->p.backend_worker_storage, &worker->m->heartbeat);
    }
}

XrJitCoroState *xr_coro_ensure_jit_state(XrCoroutine *coro) {
    if (!coro || !vm_backend_ensure_state(coro))
        return NULL;
    XrVmCoroState *state = vm_state_for_coro(coro);
    if (!state)
        return NULL;
    if (state->jit_state)
        return state->jit_state;
    state->jit_state = (XrJitCoroState *) xr_calloc(1, sizeof(XrJitCoroState));
    return state->jit_state;
}

XrJitCoroState *xr_coro_prepare_jit_state(XrCoroutine *coro) {
    XrJitCoroState *jit_state = xr_coro_ensure_jit_state(coro);
    if (!jit_state)
        return NULL;
    XrWorker *worker = xr_current_worker();
    vm_bind_worker_jit_scratch(worker, jit_state);
    return jit_state->scratch ? jit_state : NULL;
}

static void vm_backend_on_safepoint(XrCoroutine *coro) {
    XrJitCoroState *jit_state = xr_coro_peek_jit_state(coro);
    if (jit_state && jit_state->scratch && jit_state->scratch->heartbeat_ptr) {
        atomic_fetch_add_explicit(jit_state->scratch->heartbeat_ptr, 1, memory_order_relaxed);
    }
}

static void vm_backend_detach_worker_state(XrCoroutine *coro) {
    XrJitCoroState *jit_state = xr_coro_peek_jit_state(coro);
    if (jit_state) {
        jit_state->scratch = NULL;
    }
}

static bool vm_backend_in_try_mode(const XrCoroutine *coro) {
    const XrVmCoroState *state = xr_coro_maybe_vm_state_const(coro);
    return state && state->jit_state && state->jit_state->try_mode;
}

bool xr_coro_set_jit_try_mode(XrCoroutine *coro, bool enabled) {
    if (!coro)
        return false;
    XrJitCoroState *current = xr_coro_peek_jit_state(coro);
    if (!enabled && !current)
        return true;
    XrJitCoroState *state = enabled ? xr_coro_ensure_jit_state(coro) : current;
    if (!state)
        return false;
    state->try_mode = enabled;
    return true;
}

static void vm_coro_reset_jit_state(XrCoroutine *coro) {
    XrJitCoroState *jit_state = xr_coro_peek_jit_state(coro);
    if (!jit_state)
        return;
    XrJitSuspendState *suspend = jit_state->suspend;
    memset(jit_state, 0, sizeof(*jit_state));
    jit_state->suspend = suspend;
}

static void vm_coro_free_jit_state(XrCoroutine *coro) {
    XrVmCoroState *state = vm_state_for_coro(coro);
    if (!state || !state->jit_state)
        return;
    XrJitCoroState *jit_state = state->jit_state;
    if (jit_state->suspend)
        xr_free(jit_state->suspend);
    xr_free(jit_state);
    state->jit_state = NULL;
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
        vm_coro_reset_jit_state(coro);
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
    } else if (ctx->frames) {
        xr_free(ctx->frames);
        ctx->frames = NULL;
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
    if (!ctx->stack || !ctx->frames)
        return false;

    ctx->stack[0] = xr_null();
    ctx->stack_top = ctx->stack;
    ctx->frame_count = 0;
    ctx->handler_count = 0;
    vm_backend_free_defer_state(ctx);
    vm_backend_clear_entry_state(coro);
    vm_coro_reset_jit_state(coro);
    return true;
}

static void vm_backend_reset_reusable(XrCoroutine *coro) {
    if (!vm_state_for_coro(coro))
        return;
    vm_backend_clear_entry_state(coro);
    vm_coro_reset_jit_state(coro);
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
    int16_t saved_result_slot = frame->u.c.result_slot;
    frame->u.c.continuation = continuation;
    frame->u.c.continuation_ctx = user_data;
    frame->u.c.has_cfunc_result = false;
    frame->u.c.cfunc_result = xr_null();
    frame->call_status |= XR_CALL_C | XR_CALL_HAS_CONT | XR_CALL_YIELDED;
    frame->u.c.result_slot = saved_result_slot >= 0 ? saved_result_slot : -1;
    return true;
}

static bool vm_backend_has_continuation(const XrCoroutine *coro) {
    const XrVmCoroState *state = xr_coro_maybe_vm_state_const(coro);
    if (!state || state->ctx.frame_count == 0 || !state->ctx.frames)
        return false;
    const XrBcCallFrame *frame = &state->ctx.frames[state->ctx.frame_count - 1];
    return (frame->call_status & XR_CALL_HAS_CONT) && frame->u.c.continuation;
}

static int vm_backend_call_closure(XrayIsolate *X, XrCoroutine *coro, XrClosure *closure,
                                   XrValue *args, int nargs, void *continuation, void *user_ctx,
                                   XrValue *result) {
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
    caller->u.c.has_cfunc_result = false;
    caller->u.c.result_slot = (int16_t) (return_slot_offset - caller->base_offset);

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
    vm_coro_free_jit_state(coro);
    vm_backend_clear_entry_state(coro);

    if (coro->gc_flags & XR_CORO_GC_BACKEND_STATE_OWNED) {
        xr_free(state);
        coro->backend_state = NULL;
        coro->backend = NULL;
        coro->backend_ops = NULL;
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
    XrVMResult result = vm_backend_resume_on_worker(run_ctx->worker, coro);
    return worker_run_result_from_vm(coro, result);
}

// ========== Yieldable C-function coroutine ==========

// First call of a cfunc coroutine: build the C frame, run the body.
static XrVMResult run_cfunc_first_exec(XrayIsolate *isolate, XrCoroutine *coro,
                                       XrVMContext *coro_ctx, uint32_t cur_flags) {
    XrVmCoroState *vm_state = vm_state_for_coro(coro);
    if (!vm_state || vm_state->entry_type != XR_CORO_ENTRY_CFUNC || !vm_state->entry.cfunc)
        return XR_VM_RUNTIME_ERROR;

    atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
    atomic_store_explicit(&coro->flags,
                          (cur_flags & ~(XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED)) |
                              XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED,
                          memory_order_release);

    // Initialize first frame (for Yieldable support).
    coro_ctx->frame_count = 1;
    XrBcCallFrame *frame = &coro_ctx->frames[0];
    frame->closure = NULL;
    frame->pc = NULL;
    frame->base_offset = 1;  // Reserve stack[0] for return value.
    frame->flags = 0;
    frame->u.l.pending_operator_check = false;
    frame->call_status = XR_CALL_C;
    frame->u.c.continuation = NULL;
    frame->u.c.continuation_ctx = NULL;
    frame->u.c.result_slot = 0;
    frame->u.c.has_cfunc_result = false;

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
    atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
    atomic_store_explicit(&coro->flags,
                          (cur_flags & ~(XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED)) |
                              XR_CORO_FLG_RUNNING,
                          memory_order_release);

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
        if (top->u.c.has_cfunc_result) {
            coro_ctx->stack[0] = top->u.c.cfunc_result;
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
        // Channel blocks: BLOCKED already set inside xr_channel_recv/send
        // under lock.  Non-channel blocks (await,
        // timer): coro is still RUNNING.  CAS handles both correctly.
        uint8_t _exp = XR_CORO_STATE_RUNNING;
        if (atomic_compare_exchange_strong_explicit(&coro->coro_state, &_exp, XR_CORO_STATE_BLOCKED,
                                                    memory_order_release, memory_order_relaxed)) {
            atomic_fetch_and_explicit(&coro->flags, ~(uint32_t) XR_CORO_FLG_RUNNING,
                                      memory_order_relaxed);
            atomic_fetch_or_explicit(&coro->flags, (uint32_t) XR_CORO_FLG_BLOCKED,
                                     memory_order_release);
        }
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
        xr_coro_flags_clear(coro, XR_CORO_FLG_RUNNING);
        xr_coro_flags_set(coro, XR_CORO_FLG_BLOCKED);
        // Don't clear current_coro — caller handles debug break.
        ctx->current_coro = NULL;
        return result;
    }
    if (result == XR_VM_BLOCKED || result == XR_VM_YIELD) {
        if (result == XR_VM_YIELD) {
            xr_coro_flags_clear(coro, XR_CORO_FLG_RUNNING);
            xr_coro_flags_set(coro, XR_CORO_FLG_READY);
        } else {
            // Channel blocks set BLOCKED under lock; await/timer leave RUNNING.
            // CAS handles both: only swap if still RUNNING.
            uint8_t _exp = XR_CORO_STATE_RUNNING;
            if (atomic_compare_exchange_strong_explicit(&coro->coro_state, &_exp,
                                                        XR_CORO_STATE_BLOCKED, memory_order_release,
                                                        memory_order_relaxed)) {
                atomic_fetch_and_explicit(&coro->flags, ~(uint32_t) XR_CORO_FLG_RUNNING,
                                          memory_order_relaxed);
                atomic_fetch_or_explicit(&coro->flags, (uint32_t) XR_CORO_FLG_BLOCKED,
                                         memory_order_release);
            }
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
        /* Top-level uncaught value-return error: print diagnostic.
         * Panic faults print during unwind; child-coro errors are
         * isolated and surfaced by the awaiting parent. */
        if (coro->error_is_value && xr_coro_flags_has(coro, XR_CORO_FLG_MAIN) &&
            !(isolate && isolate->suppress_exception_print) && !XR_IS_NULL(coro->error)) {
            XrString *msg = xr_value_to_string(isolate, coro->error);
            fprintf(stderr, "\n[Uncaught Error] %s\n", msg ? msg->data : "<error>");
        }
        coro_ctx->current_exception = xr_null();
        coro_ctx->pending_error = xr_null();
    }

    ctx->current_coro = NULL;
    XR_DBG_CORO("run_on_worker: return result=%d, coro id=%d", result, coro->id);
    return result;
}

// ========== run_first_exec: Frame Setup + JIT Entry + Interpreter ==========
//
// Builds the coroutine's first bytecode frame (VM stack/frame/args), then tries
// the JIT entry fast path (if proto has compiled code and hasn't deopted)
// before falling back to the interpreter.
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
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(proto);
    frame->base_offset = (int) (func_base - coro_ctx->stack);
    frame->flags = 0;
    frame->u.l.pending_operator_check = false;
    frame->call_status = 0;
    frame->u.c.continuation = NULL;
    frame->u.c.continuation_ctx = NULL;
    frame->u.c.result_slot = -1;
    frame->u.c.has_cfunc_result = false;

    coro_ctx->stack_top = coro_ctx->stack + frame->base_offset + proto->maxstacksize;
    coro_ctx->module_base_frame = 0;

    XrVMResult result;

    // JIT fast path for coroutine entry: call compiled code directly.
    // go-spawned coroutines bypass OP_CALL and would otherwise always run
    // in the interpreter even when JIT code exists. deopt_count==0 guard
    // prevents replaying a proto whose first coroutine deopted.
    if (XR_JIT_AVAILABLE() && proto->numparams == vm_state->arg_count) {
        if (!proto->jit_entry) {
            void *pending = atomic_load_explicit(&proto->jit_entry_pending, memory_order_acquire);
            if (pending && (uintptr_t) pending > 1) {
                xr_jit_hooks->install_bg_result(proto);
            }
        }
        if (proto->jit_entry && proto->deopt_count < 20) {
            XrJitCoroState *jit_state = xr_coro_prepare_jit_state(coro);
            if (jit_state) {
                jit_state->scratch->call_proto = proto;
                jit_state->scratch->call_closure = closure;
                jit_state->scratch->call_base_offset = (int32_t) (func_base - coro_ctx->stack);
                XrValue jit_result;
                int _jrc =
                    xr_jit_hooks->call(proto->jit_entry, coro, func_base, vm_state->arg_count,
                                       proto->return_type_info, &jit_result);
                if (_jrc == XR_JIT_OK) {
                    coro_ctx->stack[0] = jit_result;
                    return run_finalize(isolate, worker, coro, ctx, coro_ctx, XR_VM_OK);
                }
                if (_jrc == XR_JIT_SUSPEND) {
                    return run_finalize(isolate, worker, coro, ctx, coro_ctx, XR_VM_BLOCKED);
                }
                // JIT deopt: disable fast path for this proto.
                proto->deopt_count++;
            }
        }
    }

    result = run(isolate, coro_ctx);
    return run_finalize(isolate, worker, coro, ctx, coro_ctx, result);
}

// ========== run_jit_resume: Extracted JIT Resume Logic ==========
//
// Prepares resume state (channel recv value or await task result) in
// jit_suspend, then re-enters compiled code via xm_jit_resume.
//
// Returns: XR_JIT_OK, XR_JIT_SUSPEND, XR_JIT_DEOPT (fall through), or
// -1 for channel-close (caller should clear jit_resume_entry and deopt).
static int run_jit_resume(XrayIsolate *isolate, XrCoroutine *coro, XrVMContext *coro_ctx,
                          XrValue *jit_result_out) {
    XrJitCoroState *jit_state = xr_coro_jit_state(coro);
    XR_DCHECK(jit_state->resume_entry != NULL, "run_jit_resume: no resume entry");
    XR_DCHECK(jit_state->scratch != NULL, "run_jit_resume: no jit_ctx");
    XR_DCHECK(XR_JIT_AVAILABLE(), "run_jit_resume: JIT hooks not registered");

    int resume_reason = xr_coro_resume_load(coro);

    // Channel close wake: deopt to bytecode (rare edge case).
    if (resume_reason == XR_RESUME_CHANNEL_CLOSED) {
        jit_state->resume_entry = NULL;
        jit_state->resume_proto = NULL;
        return -1;
    }

    // Channel recv resume: copy value from recv_slot (stack[0]) to
    // jit_suspend.result where the JIT continuation reads it.
    if (resume_reason == XR_RESUME_CHANNEL) {
        XrValue rv = coro_ctx->stack[0];
        if (XR_IS_PTR(rv) && xr_value_needs_copy(rv)) {
            rv = xr_deep_copy_to_coro(isolate, rv, coro);
        }
        jit_state->suspend->result = rv.i;
        jit_state->suspend->result_tag = rv.tag;
    }

    // AWAIT resume: xr_task_wake_waiter only marks coro ready but does
    // NOT propagate the result — do it here to avoid stale register reads.
    if (resume_reason != XR_RESUME_CHANNEL && resume_reason != XR_RESUME_CHANNEL_CLOSED) {
        XrCoroWaitState *wait = xr_coro_wait_state(coro);
        XrTask *await_task =
            wait ? atomic_load_explicit(&wait->await_task, memory_order_acquire) : NULL;
        if (await_task) {
            uint8_t tstate = atomic_load_explicit(&await_task->state, memory_order_acquire);
            XrValue res = xr_null();
            if (tstate == XR_TASK_COMPLETED) {
                res = xr_coro_await_result_value(isolate, coro, await_task, false);
            }
            jit_state->suspend->result = res.i;
            jit_state->suspend->result_tag = res.tag;
            atomic_store_explicit(&wait->await_task, NULL, memory_order_relaxed);
        }
    }

    xr_coro_resume_store(coro, XR_RESUME_OK);
    return xr_jit_hooks->resume(coro, jit_result_out);
}

// ========== run_resume_path: JIT Resume + Continuation + Unroll ==========
//
// Handles every resume case except the inline channel-resume fast path that
// xr_coro_run_on_worker handles directly.  Supports:
//   - JIT suspend/resume (run_jit_resume re-enters compiled code)
//   - XR_RESUME_CONTINUATION / XR_RESUME_DEBUG (run() directly)
//   - Default unroll via VM continuation unroll then run()
static XrVMResult run_resume_path(XrayIsolate *isolate, XrWorker *worker, XrCoroutine *coro,
                                  XrVMContext *ctx, XrVMContext *coro_ctx) {
    (void) worker;
    xr_coro_transition_to_running(coro);
    XR_DBG_CORO("run_on_worker: resuming coro id=%d", coro->id);

    XrVMResult result;

    XrJitCoroState *jit_state = xr_coro_peek_jit_state(coro);
    if (XR_JIT_AVAILABLE() && jit_state && jit_state->resume_entry && jit_state->scratch) {
        XrValue jit_result;
        int jrc = run_jit_resume(isolate, coro, coro_ctx, &jit_result);
        if (jrc == XR_JIT_OK) {
            coro_ctx->stack[0] = jit_result;
            return run_finalize(isolate, worker, coro, ctx, coro_ctx, XR_VM_OK);
        }
        if (jrc == XR_JIT_SUSPEND) {
            return run_finalize(isolate, worker, coro, ctx, coro_ctx, XR_VM_BLOCKED);
        }
        // -1 (channel close) or XR_JIT_DEOPT: fall through to interpreter.
    }

    // Continuation stealing resume: vm_ctx already set, just call run().
    if (xr_coro_resume_load(coro) == XR_RESUME_CONTINUATION) {
        xr_coro_resume_store(coro, 0);
        coro_ctx->current_coro = coro;
        coro_ctx->module_base_frame = 0;
        result = run(isolate, coro_ctx);
        return run_finalize(isolate, worker, coro, ctx, coro_ctx, result);
    }

    // Debug break resumption: no unroll needed.
    if (xr_coro_resume_load(coro) == XR_RESUME_DEBUG) {
        xr_coro_resume_store(coro, 0);
        coro_ctx->current_coro = coro;
        coro_ctx->module_base_frame = 0;
        result = run(isolate, coro_ctx);
        return run_finalize(isolate, worker, coro, ctx, coro_ctx, result);
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
            result = run(isolate, coro_ctx);
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
// entry checks, channel-resume fast path, first-execution setup, JIT+unroll
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
    XR_DCHECK(worker->p.runtime->isolate != NULL, "worker thread: isolate is NULL");

    XrayIsolate *isolate = worker->p.runtime->isolate;
    XrVMContext *ctx = xr_vm_machine_ctx(worker->m, isolate);
    if (!ctx)
        return XR_VM_RUNTIME_ERROR;
    XrVmCoroState *vm_state = vm_state_for_coro(coro);
    if (!vm_state)
        return XR_VM_RUNTIME_ERROR;
    XrVMContext *coro_ctx = &vm_state->ctx;

    XrJitCoroState *bound_jit_state = xr_coro_peek_jit_state(coro);
    vm_bind_worker_jit_scratch(worker, bound_jit_state);

    uint32_t _fast_flags = xr_coro_flags_load(coro);
    int _fast_resume = xr_coro_resume_load(coro);

    // ========== Channel Resume Fast Path ==========
    // Most common resume case: channel wake of a started coroutine. Acquire-
    // load of flags establishes happens-before with sender's flag swap so
    // recv_slot is visible before we enter run().
    // Handles both VM bytecode and JIT coroutines (the redundant separate
    // !jit_resume_entry exclusion — JIT now uses run_jit_resume directly).
    if ((_fast_resume == XR_RESUME_CHANNEL || is_select_channel_resume(coro, _fast_resume)) &&
        (_fast_flags & XR_CORO_FLG_STARTED)) {
        ctx->current_coro = coro;
        coro_ctx->current_coro = coro;
        coro->next = NULL;
        coro->prev = NULL;
        xr_coro_transition_to_running(coro);
        bool select_resume = consume_select_channel_resume(coro);

        // JIT channel resume: propagate recv_slot → jit_suspend.result,
        // then re-enter compiled code directly (no detour via run_resume_path).
        XrJitCoroState *jit_state = xr_coro_peek_jit_state(coro);
        if (!select_resume && XR_JIT_AVAILABLE() && jit_state && jit_state->resume_entry &&
            jit_state->scratch) {
            XrValue jit_result;
            int jrc = run_jit_resume(isolate, coro, coro_ctx, &jit_result);
            if (jrc == XR_JIT_OK) {
                coro_ctx->stack[0] = jit_result;
                return run_finalize(isolate, worker, coro, ctx, coro_ctx, XR_VM_OK);
            }
            if (jrc == XR_JIT_SUSPEND) {
                return run_finalize(isolate, worker, coro, ctx, coro_ctx, XR_VM_BLOCKED);
            }
            // Deopt or channel_closed (-1): jit_resume_entry already cleared.
            // Deopt needs full unroll recovery → delegate to run_resume_path.
            return run_resume_path(isolate, worker, coro, ctx, coro_ctx);
        }

        // VM bytecode channel resume
        if (coro_ctx->frame_count > 0) {
            XrBcCallFrame *tf = &coro_ctx->frames[coro_ctx->frame_count - 1];
            if (tf->call_status & XR_CALL_C) {
                // C continuation: fall through to the full unroll path.
                return run_resume_path(isolate, worker, coro, ctx, coro_ctx);
            }
            tf->call_status &= ~XR_CALL_YIELDED;
        }
        coro_ctx->module_base_frame = 0;
        XrVMResult r = run(isolate, coro_ctx);
        return run_finalize(isolate, worker, coro, ctx, coro_ctx, r);
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
            atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
            atomic_store_explicit(&coro->flags,
                                  (_fast_flags & ~(XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED)) |
                                      XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED,
                                  memory_order_release);
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
        atomic_store_explicit(&coro->coro_state, XR_CORO_STATE_RUNNING, memory_order_release);
        atomic_store_explicit(
            &coro->flags,
            (_flags_snap & ~(uint32_t) (XR_CORO_FLG_READY | XR_CORO_FLG_BLOCKED)) |
                XR_CORO_FLG_RUNNING | XR_CORO_FLG_STARTED,
            memory_order_release);

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
