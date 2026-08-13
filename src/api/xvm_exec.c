/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_exec.c - VM bytecode execution and lifecycle API
 */

#include "../runtime/xisolate_internal.h"
#include "../runtime/xisolate_api.h"
#include "../base/xchecks.h"
#include "../base/xglobal_indices.h"
#include "../base/xlog.h"
#include "../base/xmalloc.h"
#include "../coro/xcoroutine.h"
#include "../coro/xthread_obj.h"
#include "../coro/xworker.h"
#include "../runtime/closure/xclosure.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/xglobal_dict.h"
#include "../vm/xvm.h"
#include "../vm/xvm_coro_api.h"
#include "../vm/xvm_internal.h"
#include <string.h>

static bool xr_bind_proto_shared_slots_recursive(XrVMRuntime *isolate, XrProto *proto,
                                                 int offset) {
    if (!proto)
        return true;
    proto->shared_offset = offset;
    proto->shared_slots_bound = true;
    proto->shared_slots_owner = isolate;
    int child_count = DYNARRAY_COUNT(&proto->protos);
    for (int i = 0; i < child_count; i++) {
        XrProto *child = DYNARRAY_GET(&proto->protos, i, XrProto *);
        if (!xr_bind_proto_shared_slots_recursive(isolate, child, offset))
            return false;
    }
    return true;
}

bool xr_vm_bind_proto_shared_slots(XrVMRuntime *isolate, XrProto *proto) {
    if (!isolate || !proto)
        return false;
    if (proto->shared_slots_bound) {
        if (proto->shared_slots_owner == isolate)
            return true;
        xr_log_warning("vm", "bytecode shared slots are already bound to another VM runtime");
        return false;
    }
    int offset = isolate->vm.shared.count;
    if (proto->shared_count > 0) {
        xr_shared_array_ensure(&isolate->vm.shared, offset + proto->shared_count - 1);
        for (int i = 0; i < proto->shared_count; i++)
            isolate->vm.shared.data[offset + i] = XR_NULL_VAL;
        isolate->vm.shared.count = offset + proto->shared_count;
    }
    return xr_bind_proto_shared_slots_recursive(isolate, proto, offset);
}

/* ========== Execution API ========== */

// Execute bytecode. A physical root task is created only for scheduler-backed
// execution; the direct VM path keeps the logical root on the native stack.
int xr_execute(XrVMRuntime *isolate, XrProto *proto) {
    XR_DCHECK(isolate != NULL, "xr_execute: NULL isolate");
    XR_DCHECK(proto != NULL, "xr_execute: NULL proto");
    if (proto == NULL) {
        xr_log_warning("vm", "invalid bytecode");
        return -1;
    }
    if (!xr_vm_bind_proto_shared_slots(isolate, proto))
        return -1;

    if (!xr_vm_entry_plan_validate(proto)) {
        xr_log_warning("vm", "bytecode has no verified entry plan");
        return -1;
    }
    const XrEntryPlan *entry_plan = &proto->entry_plan;
    if (entry_plan->root_representation == XR_ROOT_ELIDED) {
        XrExecutionContext *previous =
            xr_exec_context_enter(xr_runtime_core_root_exec(isolate->core_rt));
        XrVMResult result = xr_vm_interpret_proto(isolate, proto);
        XrVMContext *ctx = xr_vm_current_ctx(isolate);
        xr_exec_context_restore(previous);
        if (result == XR_VM_OK && ctx && !XR_IS_NULL(ctx->pending_error)) {
            /* An elided root runs on the native stack with no main coroutine,
             * so run_finalize() — where the scheduler-backed roots report —
             * is never reached. Report here, and clear the channel so a
             * re-entrant host (REPL, embedder) starts clean. */
            xr_report_uncaught_error(isolate, ctx->pending_error, false);
            ctx->pending_error = xr_null();
            return -1;
        }
        return (result == XR_VM_OK) ? 0 : -1;
    }

    XrRuntime *runtime = (XrRuntime *) isolate->vm.scheduler;
    if (!runtime) {
        int workers =
            entry_plan->scheduler_mode == XR_SCHED_MULTI ? isolate->params.scheduler_workers : 1;
        xr_isolate_multicore_init(isolate, workers);
        runtime = (XrRuntime *) isolate->vm.scheduler;
        if (!runtime) {
            xr_log_warning("vm", "entry plan requires an unavailable scheduler");
            return -1;
        }
    }

    XrCoroutine *main_coro = isolate->main_coro;
    if (!main_coro) {
        main_coro = xr_coro_create_bootstrap(isolate);
        if (!main_coro)
            return -1;
        isolate->main_coro = main_coro;
    }

    XrClosure *closure = xr_closure_new(isolate, proto, main_coro);
    if (!closure) {
        xr_log_warning("vm", "failed to create main closure");
        return -1;
    }

    xr_coro_setup_main(main_coro, isolate, closure);

    return xr_main_thread_run(isolate, main_coro);
}

// Run a closure to completion on the scheduler and return its value. This is the
// synchronous bridge for C callers (the package-manager CLI) that must invoke a
// suspending stdlib coroutine — http.request and the net transports beneath it —
// and collect its result. See xr_vm_run_closure_blocking's contract in
// xvm_coro_api.h. Reuses one main coroutine so a command may drive several
// requests in sequence, exactly as the test runner reuses its physical root.
XrValue xr_vm_run_closure_blocking(XrVMRuntime *isolate, XrClosure *closure, XrValue *args,
                                   int nargs, XrValue *out_error) {
    if (out_error)
        *out_error = xr_null();
    if (isolate == NULL || closure == NULL || closure->proto == NULL)
        return xr_null();

    // A scheduler-backed runtime is required to service I/O suspensions. The
    // package-manager CLI creates one before driving requests; bootstrap a
    // single-worker runtime defensively for any other caller.
    XrRuntime *runtime = (XrRuntime *) isolate->vm.scheduler;
    if (!runtime) {
        xr_isolate_multicore_init(isolate, 1);
        runtime = (XrRuntime *) isolate->vm.scheduler;
        if (!runtime) {
            xr_log_warning("vm", "run_closure_blocking: scheduler unavailable");
            return xr_null();
        }
    }

    XrCoroutine *main_coro = xr_isolate_get_main_coro(isolate);
    if (!main_coro) {
        main_coro = xr_coro_create_bootstrap(isolate);
        if (!main_coro) {
            xr_log_warning("vm", "run_closure_blocking: main coroutine allocation failed");
            return xr_null();
        }
        xr_isolate_set_main_coro(isolate, main_coro);
    }

    // A thrown error is a value this entry hands back through out_error for the
    // caller to report; suppress the runtime's own uncaught-error print so the
    // package client owns the diagnostic instead of emitting it twice.
    bool saved_suppress = isolate->suppress_exception_print;
    isolate->suppress_exception_print = true;
    xr_coro_reset_for_call_args(main_coro, isolate, closure, args, nargs);
    xr_main_thread_run(isolate, main_coro);
    isolate->suppress_exception_print = saved_suppress;

    if (xr_coro_flags_has(main_coro, XR_CORO_FLG_DONE) &&
        !xr_coro_flags_has(main_coro, XR_CORO_FLG_CANCELLED) && XR_IS_NULL(main_coro->error)) {
        return main_coro->result;
    }
    if (out_error && !XR_IS_NULL(main_coro->error))
        *out_error = main_coro->error;
    return xr_null();
}

// Free bytecode
void xr_free_code(XrVMRuntime *isolate, XrProto *proto) {
    if (proto != NULL) {
        xr_thread_obj_drain_isolate(isolate);
        xr_instruction_unit_free(proto);
    }
}

/* ========== VM Lifecycle ========== */

// Initialize global variables table
static void init_globals(XrVMRuntime *isolate) {
    isolate->vm.builtin_count = 0;
    for (int i = 0; i < 256; i++) {
        isolate->vm.builtins[i] = xr_null();
    }

    xr_shared_array_init(&isolate->vm.shared);

    /* Name-keyed top-level bindings are module storage on the fixed heap;
     * creating a VM never materializes a task merely to host globals. */
    isolate->vm.globals = (XrGlobalDict *) xr_malloc(sizeof(XrGlobalDict));
    XR_CHECK(isolate->vm.globals != NULL, "init_globals: dict allocation failed");
    xr_global_dict_init(isolate->vm.globals, isolate);

    // Core class registration is done in isolate_init_full() (xisolate_full.c)
    // because isolate->core is NULL at this point.

    // User globals start from index XR_USER_GLOBALS_START
    if (isolate->vm.builtin_count < XR_USER_GLOBALS_START) {
        isolate->vm.builtin_count = XR_USER_GLOBALS_START;
    }
}

// Initialize coroutine state
static void init_coro_state(XrVMRuntime *isolate) {
    XrCoroState *sched = (XrCoroState *) xr_malloc(sizeof(XrCoroState));
    if (sched) {
        xr_coro_state_init(sched);
    }
    isolate->vm.coro_state = sched;
    isolate->vm.current_coro = NULL;
}

// Initialize unified VM context
static void init_vm_context(XrVMRuntime *isolate) {
    XrVMContext *ctx = &isolate->vm_ctx;
    ctx->stack = isolate->vm.stack;
    ctx->stack_top = isolate->vm.stack_top;
    ctx->stack_capacity = XR_STACK_MAX;
    ctx->frames = isolate->vm.frames;
    ctx->frame_count = isolate->vm.frame_count;
    ctx->frame_capacity = XR_FRAMES_MAX;
    ctx->module_base_frame = isolate->vm.module_base_frame;
    ctx->handlers = isolate->vm.exception_handlers;
    ctx->handler_count = isolate->vm.handler_count;
    ctx->handler_capacity = XR_EXCEPTION_HANDLERS_MAX;
    ctx->current_exception = isolate->vm.current_exception;
    ctx->current_coro = isolate->vm.current_coro;
    ctx->trace_execution = isolate->vm.trace_execution;
    ctx->isolate = isolate;
    ctx->cleanup_depth = 0;
    ctx->cancellation_cleanup_active = false;
}

// Initialize VM execution engine
int xr_vm_init(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "vm_init: NULL isolate");
    isolate->vm.stack_top = isolate->vm.stack;
    for (int i = 0; i < XR_STACK_MAX; i++) {
        isolate->vm.stack[i] = xr_null();
    }

    // Initialize call frames
    isolate->vm.frame_count = 0;
    isolate->vm.module_base_frame = -1;
    memset(isolate->vm.frames, 0, sizeof(XrBcCallFrame) * XR_FRAMES_MAX);

    // Initialize exception handling
    isolate->vm.handler_count = 0;
    isolate->vm.current_exception = xr_null();
    isolate->vm.pending_error = xr_null();

    // Initialize string intern table
    isolate->vm.strings_map = xr_hashmap_new();
    if (isolate->vm.strings_map == NULL) {
        xr_log_warning("vm", "failed to create string intern table");
        return -1;
    }
    isolate->vm.trace_execution = isolate->params.trace_execution;

    init_globals(isolate);
    init_coro_state(isolate);

    init_vm_context(isolate);

    return 0;
}

// Cleanup VM execution engine
void xr_vm_cleanup(XrVMRuntime *isolate) {
    if (isolate->vm.strings_map != NULL) {
        xr_hashmap_free(isolate->vm.strings_map);
        isolate->vm.strings_map = NULL;
    }

    /* init_globals() owns the backing storage for captured/shared slots;
     * release it from the same VM lifecycle path. */
    xr_shared_array_free(&isolate->vm.shared);

    /* Globals dict struct is xr_malloc'd; the underlying XrMap nodes are
     * reclaimed by the fixed heap after its teardown finalizers have run. */
    if (isolate->vm.globals != NULL) {
        xr_global_dict_destroy(isolate->vm.globals);
        xr_free(isolate->vm.globals);
        isolate->vm.globals = NULL;
    }

    // Cleanup coroutine state
    if (isolate->vm.coro_state != NULL) {
        xr_coro_state_destroy((XrCoroState *) isolate->vm.coro_state);
        xr_free(isolate->vm.coro_state);
        isolate->vm.coro_state = NULL;
    }

    // Static-fallback VM context (used by xr_vm_current_ctx when no coro
    // is active) carries its own per-proto IC tables; release them here
    // alongside the rest of the embedded VM state.
    xr_vm_ctx_free_ic_tables(&isolate->vm_ctx);
}
