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
#include "../coro/xworker.h"
#include "../runtime/closure/xclosure.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/xglobal_dict.h"
#include "../vm/xvm.h"
#include "../vm/xvm_coro_api.h"
#include "../vm/xvm_internal.h"
#include <string.h>

XrVMResult xr_vm_interpret_proto_isolate(XrayIsolate *isolate, XrProto *proto);

/* ========== Execution API ========== */

// Execute bytecode
// main_coro already exists (created as bootstrap during isolate init),
// just upgrade it with the compiled closure.
int xr_execute(XrayIsolate *isolate, XrProto *proto) {
    XR_DCHECK(isolate != NULL, "xr_execute: NULL isolate");
    XR_DCHECK(proto != NULL, "xr_execute: NULL proto");
    if (proto == NULL) {
        xr_log_warning("vm", "invalid bytecode");
        return -1;
    }

    XrRuntime *runtime = (XrRuntime *) isolate->vm.scheduler;
    if (!runtime) {
        XrVMResult result = xr_vm_interpret_proto_isolate(isolate, proto);
        if (result == XR_VM_OK && !XR_IS_NULL(isolate->vm.pending_error)) {
            return -1;
        }
        return (result == XR_VM_OK) ? 0 : -1;
    }

    XrCoroutine *main_coro = isolate->main_coro;
    if (!main_coro) {
        xr_log_warning("vm", "main_coro not initialized");
        return -1;
    }

    XrClosure *closure = xr_closure_new(isolate, proto, main_coro);
    if (!closure) {
        xr_log_warning("vm", "failed to create main closure");
        return -1;
    }

    xr_coro_setup_main(main_coro, isolate, closure);

    return xr_main_thread_run(isolate, main_coro);
}

// Free bytecode
void xr_free_code(XrayIsolate *isolate, XrProto *proto) {
    (void) isolate;
    if (proto != NULL) {
        xr_vm_proto_free(proto);
    }
}

/* ========== VM Lifecycle ========== */

// Initialize global variables table
static void init_globals(XrayIsolate *isolate) {
    isolate->vm.builtin_count = 0;
    for (int i = 0; i < 256; i++) {
        isolate->vm.builtins[i] = xr_null();
    }

    xr_shared_array_init(&isolate->vm.shared);

    /* Name-keyed top-level binding dict.  XrMap allocation lives on
     * the fixed heap, so the main coroutine — already constructed in
     * xray_isolate_new before xr_vm_init — must exist here. */
    XR_DCHECK(isolate->main_coro != NULL, "init_globals: main_coro must precede globals dict");
    isolate->vm.globals = (XrGlobalDict *) xr_malloc(sizeof(XrGlobalDict));
    XR_CHECK(isolate->vm.globals != NULL, "init_globals: dict allocation failed");
    xr_global_dict_init(isolate->vm.globals, isolate->main_coro);

    // Core class registration is done in isolate_init_full() (xisolate_full.c)
    // because isolate->core is NULL at this point.

    // User globals start from index XR_USER_GLOBALS_START
    if (isolate->vm.builtin_count < XR_USER_GLOBALS_START) {
        isolate->vm.builtin_count = XR_USER_GLOBALS_START;
    }
}

// Initialize coroutine state
static void init_coro_state(XrayIsolate *isolate) {
    XrCoroState *sched = (XrCoroState *) xr_malloc(sizeof(XrCoroState));
    if (sched) {
        xr_coro_state_init(sched);
    }
    isolate->vm.coro_state = sched;
    isolate->vm.current_coro = NULL;
}

// Initialize unified VM context
static void init_vm_context(XrayIsolate *isolate) {
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

    // Defer stack is per-context (lazy-allocated on first OP_DEFER)
    ctx->defer_stack = NULL;
    ctx->defer_count = 0;
    ctx->defer_capacity = 0;
    ctx->defer_frame_marks = NULL;
}

// Initialize VM execution engine
int xr_vm_init(XrayIsolate *isolate) {
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
void xr_vm_cleanup(XrayIsolate *isolate) {
    if (isolate->vm.strings_map != NULL) {
        xr_hashmap_free(isolate->vm.strings_map);
        isolate->vm.strings_map = NULL;
    }

    /* Globals dict struct is xr_malloc'd; the underlying XrMap nodes
     * are fixed heap-owned and reclaimed by xr_fixed_heap_cleanup later. */
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

    // Cleanup main-thread defer stack (per vm_ctx)
    if (isolate->vm_ctx.defer_stack != NULL) {
        xr_free(isolate->vm_ctx.defer_stack);
        isolate->vm_ctx.defer_stack = NULL;
    }
    if (isolate->vm_ctx.defer_frame_marks != NULL) {
        xr_free(isolate->vm_ctx.defer_frame_marks);
        isolate->vm_ctx.defer_frame_marks = NULL;
    }

    // Static-fallback VM context (used by xr_vm_current_ctx when no coro
    // is active) carries its own per-proto IC tables; release them here
    // alongside the rest of the embedded VM state.
    xr_vm_ctx_free_ic_tables(&isolate->vm_ctx);
}
