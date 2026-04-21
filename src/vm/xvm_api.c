/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_api.c - VM public API
 *
 * KEY CONCEPT:
 *   C code calling closures, VM execution, exception handling, Isolate API.
 *   Uses coroutine's vm_ctx for stack/frame access in coroutine mode.
 */

#include "xvm_internal.h"
#include "../coro/xworker.h"
#include "../coro/xcoroutine.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"

/* ========== VM Context Helper ========== */

// Unified ctx retrieval: prefer coroutine ctx > worker ctx > isolate vm
static XrVMContext* xr_vm_get_current_ctx(XrayIsolate *isolate) {
    XrWorker *worker = xr_current_worker();
    if (worker) {
        if (worker->m->vm_ctx.current_coro) {
            return &((XrCoroutine *)worker->m->vm_ctx.current_coro)->vm_ctx;
        }
        return &worker->m->vm_ctx;
    }
    return &isolate->vm_ctx;
}

/* ========== C Code Calling Closure API ========== */

// Call Xray closure from C (for higher-order functions and defer)
XrValue xr_vm_call_closure(XrayIsolate *isolate, XrClosure *closure, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "vm_call_closure: NULL isolate");
    XR_DCHECK(nargs >= 0, "vm_call_closure: negative nargs");
    // Parameter check
    if (closure == NULL || closure->proto == NULL) {
        return xr_null();
    }

    // Check argument count
    if (nargs != closure->proto->numparams) {
        xr_log_warning("vm", "expected %d arguments but got %d",
                closure->proto->numparams, nargs);
        return xr_null();
    }

    // Get execution context: prefer current coroutine, fallback to main
    XrWorker *worker = xr_current_worker();
    XrVMContext *ctx = NULL;
    if (worker && worker->m->current_coro) {
        ctx = &((XrCoroutine *)worker->m->current_coro)->vm_ctx;
    } else {
        // Fallback to main coroutine
        XrCoroutine *main_coro = (XrCoroutine*)isolate->main_coro;
        ctx = &main_coro->vm_ctx;
    }

    // Save current VM state
    XrBcCallFrame *current_frame = (ctx->frame_count > 0)
        ? &ctx->frames[ctx->frame_count - 1]
        : NULL;
    XrValue *safe_stack_top = ctx->stack_top;
    if (current_frame && current_frame->closure && current_frame->closure->proto) {
        XrValue *frame_end = ctx->stack + current_frame->base_offset + current_frame->closure->proto->maxstacksize;
        if (safe_stack_top < frame_end) {
            safe_stack_top = frame_end;
        }
    }
    XrValue *saved_stack_top = safe_stack_top;
    int saved_frame_count = ctx->frame_count;
    int saved_module_base = ctx->module_base_frame;

    // Set module_base_frame for OP_RETURN boundary
    ctx->module_base_frame = saved_frame_count;

    // Reserve slot for function object on stack
    saved_stack_top[0] = xr_null();
    XrValue *func_base = saved_stack_top + 1;

    // Create new call frame
    XR_DCHECK(ctx->frame_count >= 0 && ctx->frame_count < ctx->frame_capacity,
              "vm_call_closure: call frame overflow");
    XrBcCallFrame *frame = &ctx->frames[ctx->frame_count++];
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(closure->proto);
    frame->base_offset = (int)(func_base - ctx->stack);
    XR_DCHECK(frame->base_offset >= 0, "vm_call_closure: negative base_offset");
    frame->flags = 0;
    frame->call_status = 0;
    frame->result_offset = 0;
    frame->u.l.pending_operator_check = false;

    // Copy arguments to stack
    for (int i = 0; i < nargs; i++) {
        func_base[i] = args[i];
    }

    // Update stack top
    ctx->stack_top = ctx->stack + frame->base_offset + closure->proto->maxstacksize;
    XR_DCHECK(ctx->stack_top <= ctx->stack + ctx->stack_capacity,
              "vm_call_closure: stack overflow");

    // Execute bytecode
    XrVMResult exec_result = run(isolate, ctx);

    // Get return value
    XrValue return_value = xr_null();
    if (exec_result == XR_VM_OK) {
        XrValue *return_slot = saved_stack_top;
        return_value = *return_slot;
    } else {
        if (!isolate->suppress_exception_print) {
            xr_log_warning("vm", "xr_vm_call_closure: execution failed with error %d", exec_result);
        }
    }

    // Restore VM state
    ctx->frame_count = saved_frame_count;
    ctx->stack_top = saved_stack_top;
    ctx->module_base_frame = saved_module_base;

    return return_value;
}

/*
** Extended closure call (supports blocking return)
** Used for coroutine execution, can handle XR_VM_BLOCKED state
** Uses main coroutine's vm_ctx uniformly
**
** @param out_result Output: execution result status
** @return Return value (only valid when *out_result == XR_VM_OK)
*/
XrValue xr_vm_call_closure_ex(XrayIsolate *isolate, XrClosure *closure,
                               XrValue *args, int nargs, XrVMResult *out_result) {
    XR_DCHECK(isolate != NULL, "vm_call_closure_ex: NULL isolate");
    XR_DCHECK(out_result != NULL, "vm_call_closure_ex: NULL out_result");
    XR_DCHECK(nargs >= 0, "vm_call_closure_ex: negative nargs");
    // Parameter check
    if (closure == NULL || closure->proto == NULL) {
        *out_result = XR_VM_RUNTIME_ERROR;
        return xr_null();
    }

    // Check argument count
    if (nargs != closure->proto->numparams) {
        fprintf(stderr, "Expected %d arguments but got %d\n",
                closure->proto->numparams, nargs);
        *out_result = XR_VM_RUNTIME_ERROR;
        return xr_null();
    }

    // Get main coroutine's vm_ctx
    XrCoroutine *main_coro = (XrCoroutine*)isolate->main_coro;
    XrVMContext *ctx = &main_coro->vm_ctx;

    // Save current VM state (no frame copy needed - use module_base_frame)
    int saved_frame_count = ctx->frame_count;
    int saved_module_base = ctx->module_base_frame;
    XrValue *saved_stack_top = ctx->stack_top;

    // Ensure stack_top is past any active frame
    if (saved_frame_count > 0) {
        XrBcCallFrame *cur = &ctx->frames[saved_frame_count - 1];
        if (cur->closure && cur->closure->proto) {
            XrValue *frame_end = ctx->stack + cur->base_offset + cur->closure->proto->maxstacksize;
            if (saved_stack_top < frame_end) {
                saved_stack_top = frame_end;
            }
        }
    }

    // Use module_base_frame so run() stops when returning past this point
    ctx->module_base_frame = saved_frame_count;

    // Reserve slot for function object on stack
    saved_stack_top[0] = xr_null();
    XrValue *func_base = saved_stack_top + 1;

    // Create new call frame (append, don't reset)
    XR_DCHECK(ctx->frame_count >= 0 && ctx->frame_count < ctx->frame_capacity,
              "vm_call_closure_ex: call frame overflow");
    XrBcCallFrame *frame = &ctx->frames[ctx->frame_count++];
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(closure->proto);
    frame->base_offset = (int)(func_base - ctx->stack);
    XR_DCHECK(frame->base_offset >= 0, "vm_call_closure_ex: negative base_offset");
    frame->flags = 0;
    frame->call_status = 0;
    frame->result_offset = 0;
    frame->u.l.pending_operator_check = false;

    // Copy arguments to stack
    for (int i = 0; i < nargs; i++) {
        func_base[i] = args[i];
    }

    // Update stack top
    ctx->stack_top = ctx->stack + frame->base_offset + closure->proto->maxstacksize;
    XR_DCHECK(ctx->stack_top <= ctx->stack + ctx->stack_capacity,
              "vm_call_closure_ex: stack overflow");

    // Execute bytecode
    XrVMResult exec_result = run(isolate, ctx);
    *out_result = exec_result;

    // Get return value
    XrValue return_value = xr_null();
    if (exec_result == XR_VM_OK) {
        return_value = saved_stack_top[0];
    }

    // Restore VM state (frames are naturally truncated by frame_count restore)
    ctx->module_base_frame = saved_module_base;
    ctx->frame_count = saved_frame_count;
    ctx->stack_top = saved_stack_top;

    return return_value;
}

/* ========== VM Execution API ========== */

/*
** Execute function prototype
** Uses main coroutine's vm_ctx uniformly
*/
XrVMResult xr_vm_interpret_proto(XrayIsolate *isolate, XrProto *proto) {
    XR_DCHECK(isolate != NULL, "vm_interpret_proto: NULL isolate");
    XR_DCHECK(proto != NULL, "vm_interpret_proto: NULL proto");
    // Create top-level closure on main coroutine's Immix heap
    XrCoroutine *main_coro = (XrCoroutine*)isolate->main_coro;
    XrClosure *closure = xr_closure_new(isolate, proto, main_coro);
    if (closure == NULL) {
        return XR_VM_RUNTIME_ERROR;
    }

    // Get vm_ctx: prefer main coroutine, fallback to isolate->vm_ctx
    XrVMContext *ctx = main_coro ? &main_coro->vm_ctx : &isolate->vm_ctx;

    // Initialize stack top
    ctx->stack_top = ctx->stack;

    // Create call frame
    XR_DCHECK(ctx->frame_count >= 0 && ctx->frame_count < ctx->frame_capacity,
              "vm_interpret_proto: call frame overflow");
    XrBcCallFrame *frame = &ctx->frames[ctx->frame_count++];
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(proto);
    frame->base_offset = 0;
    frame->u.l.pending_operator_check = false;

    // Execute (using vm_ctx uniformly)
    return run(isolate, ctx);
}

/*
** Execute module code (does not reset VM state)
** Used to execute module initialization code during module loading
**
** Under coroutine architecture: uses Worker's vm_ctx to access coroutine stack and frames
*/
XrVMResult xr_vm_execute_module(XrayIsolate *isolate, XrProto *proto) {
    XR_DCHECK(isolate != NULL, "vm_execute_module: NULL isolate");
    XR_DCHECK(proto != NULL, "vm_execute_module: NULL proto");
    // Create module closure on current coroutine's Immix heap
    XrWorker *_mod_worker = xr_current_worker();
    XrCoroutine *_mod_coro = (_mod_worker && _mod_worker->m->vm_ctx.current_coro)
        ? (XrCoroutine *)_mod_worker->m->vm_ctx.current_coro : NULL;
    XrClosure *closure = xr_closure_new(isolate, proto, _mod_coro);
    if (closure == NULL) {
        return XR_VM_RUNTIME_ERROR;
    }

    // Unified ctx: coroutine ctx > worker ctx > isolate vm
    XrVMContext *ctx = xr_vm_get_current_ctx(isolate);

    // Save current state
    int saved_frame_count = ctx->frame_count;
    int saved_module_base = ctx->module_base_frame;
    XrValue *saved_stack_top = ctx->stack_top;

    // Set base frame count for module execution, run() stops when returning to this count
    ctx->module_base_frame = saved_frame_count;

    // Allocate space for module after current stack top
    XrValue *module_base = ctx->stack_top;
    int base_offset = (int)(module_base - ctx->stack);
    int needed_top = base_offset + proto->maxstacksize;

    // Ensure stack has enough space for module code (critical for coroutine stacks
    // which start at 128 slots - module init code often needs more)
    XrCoroutine *coro = ctx->current_coro ? (XrCoroutine *)ctx->current_coro : NULL;
    if (needed_top > ctx->stack_capacity) {
        if (coro) {
            int extra = needed_top - ctx->stack_capacity + 64;
            XrValue *old_stack = ctx->stack;
            int old_cap = ctx->stack_capacity;
            if (!xr_coro_grow_stack(coro, extra)) {
                ctx->module_base_frame = saved_module_base;
                return XR_VM_RUNTIME_ERROR;
            }
            // ctx IS &coro->vm_ctx, grow updates it in place
            (void)old_cap;
            // Recalculate module_base after potential realloc
            module_base = ctx->stack + base_offset;
            saved_stack_top = ctx->stack + (saved_stack_top - old_stack);
        } else {
            ctx->module_base_frame = saved_module_base;
            return XR_VM_RUNTIME_ERROR;
        }
    }

    // Ensure frame capacity
    if (saved_frame_count + 1 >= ctx->frame_capacity) {
        if (coro) {
            // ctx IS &coro->vm_ctx, grow updates it in place
            if (!xr_coro_grow_stack(coro, 0)) {
                ctx->module_base_frame = saved_module_base;
                return XR_VM_RUNTIME_ERROR;
            }
        } else {
            ctx->module_base_frame = saved_module_base;
            return XR_VM_RUNTIME_ERROR;
        }
    }

    // Update stack_top so run() knows the valid range
    ctx->stack_top = module_base + proto->maxstacksize;

    // Create new call frame
    XrBcCallFrame *frame = &ctx->frames[saved_frame_count];
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(proto);
    frame->base_offset = base_offset;
    frame->result_offset = base_offset > 0 ? base_offset - 1 : 0;
    frame->flags = 0;
    frame->call_status = 0;
    frame->u.l.pending_operator_check = false;

    // Update frame count
    ctx->frame_count = saved_frame_count + 1;

    // Execute module code (pass current context)
    XrVMResult result = run(isolate, ctx);

    // Restore state
    ctx->module_base_frame = saved_module_base;
    ctx->frame_count = saved_frame_count;
    ctx->stack_top = saved_stack_top;

    return result;
}

/*
** Execute source code
*/
XrVMResult xr_vm_interpret(const char *source) {
    (void)source;
    // This function is now just for API compatibility
    // Currently requires xcompiler module
    fprintf(stderr, "XrCompiler not yet implemented\n");
    return XR_VM_COMPILE_ERROR;
}


/* ========== Exception Handling Implementation ========== */

/*
** Add current stack frame to exception's stack trace
**
** Under coroutine architecture: uses Worker's vm_ctx to access frames
*/
void xr_vm_add_stacktrace(XrayIsolate *isolate, XrValue exception) {
    XR_DCHECK(isolate != NULL, "vm_add_stacktrace: NULL isolate");
    if (!xr_is_exception(exception)) {
        return;
    }

    XrVMContext *ctx = xr_vm_get_current_ctx(isolate);

    int frame_count = ctx->frame_count;
    XrBcCallFrame *frames = ctx->frames;

    if (frame_count == 0) {
        return;
    }

    // Get current frame
    XrBcCallFrame *frame = &frames[frame_count - 1];

    // Get function name
    const char *func_name = "?";
    int line = 0;

    if (frame->closure && frame->closure->proto) {
        if (frame->closure->proto->name) {
            func_name = frame->closure->proto->name->data;
        }

        // Calculate current line number (from lineinfo)
        int pc_offset = (int)(frame->pc - PROTO_CODE_BASE(frame->closure->proto));
        size_t line_count = PROTO_LINE_COUNT(frame->closure->proto);
        if (pc_offset >= 0 && (size_t)pc_offset < line_count) {
            line = PROTO_LINE(frame->closure->proto, pc_offset);
        }
    }

    // Add stack frame
    xr_exception_add_frame(isolate, exception, func_name, line);
}

/*
** Throw exception (stack unwinding)
**
** Uses current coroutine's vm_ctx to access exception handlers.
** Iterative approach avoids stack overflow on deeply nested try/catch.
*/
void xr_vm_throw_exception(XrayIsolate *isolate, XrValue exception) {
    XR_DCHECK(isolate != NULL, "vm_throw_exception: NULL isolate");
    XrVMContext *ctx = xr_vm_get_current_ctx(isolate);

    // Iterative exception propagation loop
    while (ctx->handler_count > 0) {
        XrExceptionHandler *handler = &ctx->handlers[ctx->handler_count - 1];

        // Skip handlers that already caught an exception (re-throw in catch block)
        if (handler->caught) {
            ctx->handler_count--;
            continue;
        }

        // Save exception object
        handler->exception = exception;
        ctx->current_exception = exception;

        // Stack unwinding: restore stack top
        ctx->stack_top = ctx->stack + handler->stack_size;

        // Restore frame count
        ctx->frame_count = handler->frame_count;

        // Get restored frame and jump to handler
        if (ctx->frame_count > 0) {
            XrBcCallFrame *frame = &ctx->frames[ctx->frame_count - 1];

            if (handler->catch_offset > 0) {
                frame->pc = PROTO_CODE_BASE(frame->closure->proto) + handler->catch_offset;
                return;
            } else if (handler->finally_offset > 0) {
                frame->pc = PROTO_CODE_BASE(frame->closure->proto) + handler->finally_offset;
                return;
            }
        }

        ctx->handler_count--;
    }

    // No handler found - uncaught exception
    ctx->current_exception = exception;

    /* Child coroutine: error isolated to coro->error, suppress print.
    ** Main coroutine or non-coroutine: print (fatal, program terminates). */
    bool is_child_coro = false;
    if (ctx->current_coro) {
        XrCoroutine *coro = (XrCoroutine *)ctx->current_coro;
        is_child_coro = !xr_coro_flags_has(coro, XR_CORO_FLG_MAIN);
    }
    if (!is_child_coro && !(isolate && isolate->suppress_exception_print)) {
        fprintf(stderr, "\n[Uncaught Exception]\n");
        xr_exception_print(isolate, exception);
        fprintf(stderr, "\n");
    }
}

/* ==========  Isolate API ========== */

/*
** Execute bytecode on Isolate
** Thin wrapper: delegates to xr_vm_interpret_proto which operates on isolate directly.
*/
XrVMResult xr_vm_interpret_proto_isolate(XrayIsolate *isolate, XrProto *proto) {
    if (isolate == NULL || proto == NULL) {
        return XR_VM_RUNTIME_ERROR;
    }

    // Simplified: directly call xr_vm_interpret_proto, it operates on isolate->vm directly
    // Previous copy operation was unnecessary since xr_vm_interpret_proto uses isolate directly
    return xr_vm_interpret_proto(isolate, proto);
}
