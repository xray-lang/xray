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

/*
** Single authoritative VM context resolver.
** See xvm_internal.h for the documented resolution order and contract.
*/
XrVMContext *xr_vm_current_ctx(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "vm_current_ctx: NULL isolate");
    XrWorker *worker = xr_current_worker();
    if (worker && worker->m) {
        XrCoroutine *coro = (XrCoroutine *) worker->m->vm_ctx.current_coro;
        if (coro)
            return &coro->vm_ctx;
        return &worker->m->vm_ctx;
    }
    if (isolate->main_coro) {
        return &((XrCoroutine *) isolate->main_coro)->vm_ctx;
    }
    return &isolate->vm_ctx;
}

/*
** Ensure ctx has room for one new entry frame plus extra_stack slots.
** See xvm_internal.h for contract.
**
** xr_coro_grow_stack reallocates ctx->stack but does NOT update
** ctx->stack_top. We snapshot stack_top as an offset before grow and
** re-derive it afterwards so the invariant stack <= stack_top <= stack+cap
** always holds across this call.
*/
bool xr_vm_prepare_entry(XrVMContext *ctx, int extra_stack) {
    XR_DCHECK(ctx != NULL, "vm_prepare_entry: NULL ctx");
    XR_DCHECK(extra_stack >= 0, "vm_prepare_entry: negative extra_stack");

    int stack_used = (int) (ctx->stack_top - ctx->stack);
    int stack_needed = stack_used + extra_stack;
    int frames_needed = ctx->frame_count + 1;

    bool need_stack = stack_needed > ctx->stack_capacity;
    bool need_frames = frames_needed >= ctx->frame_capacity;

    if (!need_stack && !need_frames) {
        return true;
    }

    XrCoroutine *coro = (XrCoroutine *) ctx->current_coro;
    if (!coro) {
        // Static fallback ctx (isolate->vm_ctx with no main coro): cannot grow.
        // Caller must surface XR_VM_RUNTIME_ERROR; we never silently truncate.
        return false;
    }

    // xr_coro_grow_stack(coro, n>0) grows stack and (when frames near full)
    // also doubles frame capacity. Use a small bump (64) when only frames
    // need to grow so the underlying API contract (extra_slots > 0) holds.
    int extra = need_stack ? (stack_needed - ctx->stack_capacity + 64) : 64;
    if (!xr_coro_grow_stack(coro, extra)) {
        return false;
    }

    // Re-derive stack_top from saved offset — old pointer is now freed.
    ctx->stack_top = ctx->stack + stack_used;
    return true;
}

/* ========== C Code Calling Closure API ========== */

/*
** Call Xray closure from C code.
** Unified implementation for higher-order functions, defer, and coroutines.
**
** @param out_result Optional: if non-NULL, receives XrVMResult status.
**                   If NULL, errors are logged via xr_log_warning.
** @return Return value (xr_null() on failure or when *out_result != XR_VM_OK)
*/
XrValue xr_vm_call_closure(XrayIsolate *isolate, XrClosure *closure, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "vm_call_closure: NULL isolate");
    XR_DCHECK(nargs >= 0, "vm_call_closure: negative nargs");

    if (closure == NULL || closure->proto == NULL) {
        return xr_null();
    }

    XrProto *proto = closure->proto;

    // Argument count validation (aligned with OP_CALL semantics)
    if (proto->is_vararg) {
        if (nargs < proto->min_params) {
            xr_log_warning("vm", "expected at least %d arguments but got %d", proto->min_params,
                           nargs);
            return xr_null();
        }
    } else if (nargs < proto->min_params || nargs > proto->numparams) {
        xr_log_warning("vm", "expected %d..%d arguments but got %d", proto->min_params,
                       proto->numparams, nargs);
        return xr_null();
    }

    // Single authoritative ctx resolver.
    XrVMContext *ctx = xr_vm_current_ctx(isolate);

    // Save current VM state
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

    // Reserve room for: 1 function/return slot + maxstacksize register window.
    // Advance ctx->stack_top to the projected start so prepare_entry computes
    // the right grow amount, then re-derive saved_stack_top via offset since
    // grow may relocate ctx->stack.
    int stack_top_offset = (int) (saved_stack_top - ctx->stack);
    XrValue *prev_stack_top = ctx->stack_top;
    ctx->stack_top = saved_stack_top;
    if (!xr_vm_prepare_entry(ctx, 1 + proto->maxstacksize)) {
        ctx->stack_top = prev_stack_top;
        xr_log_warning("vm", "vm_call_closure: stack/frame growth failed");
        return xr_null();
    }
    saved_stack_top = ctx->stack + stack_top_offset;

    // Set module_base_frame so run() stops when returning past this point
    ctx->module_base_frame = saved_frame_count;

    // Reserve slot for function object (return value) on stack
    saved_stack_top[0] = xr_null();
    XrValue *func_base = saved_stack_top + 1;

    // Create new call frame
    XR_DCHECK(ctx->frame_count >= 0 && ctx->frame_count < ctx->frame_capacity,
              "vm_call_closure: prepare_entry post-condition violated");
    XrBcCallFrame *frame = &ctx->frames[ctx->frame_count++];
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(proto);
    frame->base_offset = (int) (func_base - ctx->stack);
    XR_DCHECK(frame->base_offset >= 0, "vm_call_closure: negative base_offset");
    frame->flags = 0;
    frame->call_status = 0;
    frame->result_offset = 0;
    frame->u.l.pending_operator_check = false;

    // Copy arguments to stack
    for (int i = 0; i < nargs; i++) {
        func_base[i] = args[i];
    }

    if (proto->is_vararg) {
        // Collect extra arguments into rest array (matches OP_CALL vararg path)
        int extra = nargs > proto->numparams ? nargs - proto->numparams : 0;
        XrCoroutine *coro = (XrCoroutine *) ctx->current_coro;
        XrArray *rest = xr_array_new(coro);
        if (extra > 0) {
            for (int j = 0; j < extra; j++) {
                xr_array_push(rest, func_base[proto->numparams + j]);
            }
        }
        func_base[proto->numparams] = xr_value_from_array(rest);
        // Fill missing optional fixed params with null
        for (int j = nargs; j < proto->numparams; j++) {
            func_base[j] = xr_null();
        }
    } else {
        // Fill missing optional arguments with null (default params)
        for (int j = nargs; j < proto->numparams; j++) {
            func_base[j] = xr_null();
        }
    }

    // Update stack top
    ctx->stack_top = ctx->stack + frame->base_offset + proto->maxstacksize;
    XR_DCHECK(ctx->stack_top <= ctx->stack + ctx->stack_capacity,
              "vm_call_closure: stack overflow post-prepare");

    // Execute bytecode. run() may grow the stack via xr_coro_grow_stack,
    // invalidating saved_stack_top — re-derive from offset after return.
    XrVMResult exec_result = run(isolate, ctx);

    XrValue *result_slot = ctx->stack + stack_top_offset;

    // Get return value
    XrValue return_value = xr_null();
    if (exec_result == XR_VM_OK) {
        return_value = result_slot[0];
    } else {
        if (!isolate->suppress_exception_print) {
            xr_log_warning("vm", "xr_vm_call_closure: execution failed with error %d", exec_result);
        }
    }

    // Restore VM state
    ctx->module_base_frame = saved_module_base;
    ctx->frame_count = saved_frame_count;
    ctx->stack_top = result_slot;

    return return_value;
}

/* ========== VM Execution API ========== */

/*
** Execute function prototype
** Uses main coroutine's vm_ctx uniformly
*/
XrVMResult xr_vm_interpret_proto(XrayIsolate *isolate, XrProto *proto) {
    XR_DCHECK(isolate != NULL, "vm_interpret_proto: NULL isolate");
    // proto NULL is an expected hardening case (mirrors the closure NULL
    // handling in xr_vm_call_closure): DCHECK would abort in Debug but is
    // a no-op in Release, so without an explicit guard a Release build
    // would dereference NULL inside xr_closure_new -> Win64 SEGFAULT.
    if (proto == NULL) {
        return XR_VM_RUNTIME_ERROR;
    }
    XrCoroutine *main_coro = (XrCoroutine *) isolate->main_coro;
    XrClosure *closure = xr_closure_new(isolate, proto, main_coro);
    if (closure == NULL) {
        return XR_VM_RUNTIME_ERROR;
    }

    // Single authoritative ctx resolver.
    XrVMContext *ctx = xr_vm_current_ctx(isolate);

    // Initialize stack top to base; prepare_entry then ensures capacity.
    ctx->stack_top = ctx->stack;

    // Reserve room for one entry frame at base + maxstacksize register window.
    if (!xr_vm_prepare_entry(ctx, proto->maxstacksize)) {
        return XR_VM_RUNTIME_ERROR;
    }

    XR_DCHECK(ctx->frame_count >= 0 && ctx->frame_count < ctx->frame_capacity,
              "vm_interpret_proto: prepare_entry post-condition violated");
    XrBcCallFrame *frame = &ctx->frames[ctx->frame_count];
    memset(frame, 0, sizeof(XrBcCallFrame));
    ctx->frame_count++;
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(proto);
    frame->base_offset = 0;

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

    // Single authoritative ctx resolver.
    XrVMContext *ctx = xr_vm_current_ctx(isolate);

    // Create module closure on current coroutine's Immix heap (if any).
    XrCoroutine *coro = (XrCoroutine *) ctx->current_coro;
    XrClosure *closure = xr_closure_new(isolate, proto, coro);
    if (closure == NULL) {
        return XR_VM_RUNTIME_ERROR;
    }

    // Save state as offsets (pointer-stable across grow).
    int saved_frame_count = ctx->frame_count;
    int saved_module_base = ctx->module_base_frame;
    int saved_top_offset = (int) (ctx->stack_top - ctx->stack);

    // Reserve room for one entry frame plus the proto's full register window
    // above the current stack_top. prepare_entry uses ctx->stack_top as the
    // baseline for "extra_stack" computation.
    if (!xr_vm_prepare_entry(ctx, proto->maxstacksize)) {
        return XR_VM_RUNTIME_ERROR;
    }

    // Pointers may have moved; re-derive everything from the saved offset.
    XrValue *module_base = ctx->stack + saved_top_offset;
    int base_offset = saved_top_offset;

    // Mark module boundary — run() stops returning past this frame depth.
    ctx->module_base_frame = saved_frame_count;
    ctx->stack_top = module_base + proto->maxstacksize;

    XR_DCHECK(saved_frame_count + 1 <= ctx->frame_capacity,
              "vm_execute_module: prepare_entry post-condition violated");
    XrBcCallFrame *frame = &ctx->frames[saved_frame_count];
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(proto);
    frame->base_offset = base_offset;
    frame->result_offset = base_offset > 0 ? base_offset - 1 : 0;
    frame->flags = 0;
    frame->call_status = 0;
    frame->u.l.pending_operator_check = false;
    ctx->frame_count = saved_frame_count + 1;

    XrVMResult result = run(isolate, ctx);

    // run() may have grown the stack, so re-derive stack_top via offset.
    ctx->module_base_frame = saved_module_base;
    ctx->frame_count = saved_frame_count;
    ctx->stack_top = ctx->stack + saved_top_offset;

    return result;
}

/*
** Execute source code
*/
XrVMResult xr_vm_interpret(const char *source) {
    (void) source;
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
    if (!xr_value_is_exception(isolate, exception)) {
        return;
    }

    XrVMContext *ctx = xr_vm_current_ctx(isolate);

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
        int pc_offset = (int) (frame->pc - PROTO_CODE_BASE(frame->closure->proto));
        size_t line_count = PROTO_LINE_COUNT(frame->closure->proto);
        if (pc_offset >= 0 && (size_t) pc_offset < line_count) {
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
    XrVMContext *ctx = xr_vm_current_ctx(isolate);

    // Iterative exception propagation loop.
    //
    // Stop at module_base_frame: handlers whose frame_count is at or below
    // that boundary were installed by a caller VM invocation that entered us
    // re-entrantly via xr_vm_call_closure. Running an outer handler inside
    // the inner VM would execute its catch block here, then let the outer
    // VM resume and re-run the same post-catch code.
    //
    // Uncaught within this scope → fall through to the "no handler" tail;
    // callers detect that condition via xr_vm_is_catch_reachable().
    int floor = ctx->module_base_frame > 0 ? ctx->module_base_frame : 0;
    while (ctx->handler_count > 0 && ctx->handlers[ctx->handler_count - 1].frame_count > floor) {
        XrExceptionHandler *handler = &ctx->handlers[ctx->handler_count - 1];

        // Case 1: Already in finally block — pop and propagate outward
        if (handler->in_finally) {
            ctx->handler_count--;
            continue;
        }

        // Case 2: Caught (re-throw in catch block) — run finally if available
        if (handler->caught) {
            if (handler->finally_offset > 0) {
                // Store new exception, jump to finally block
                handler->exception = exception;
                handler->in_finally = true;
                ctx->current_exception = exception;
                ctx->stack_top = ctx->stack + handler->stack_size;
                ctx->frame_count = handler->frame_count;
                if (ctx->frame_count > 0) {
                    XrBcCallFrame *frame = &ctx->frames[ctx->frame_count - 1];
                    frame->pc = PROTO_CODE_BASE(frame->closure->proto) + handler->finally_offset;
                    return;
                }
            }
            // No finally — pop and continue outward
            ctx->handler_count--;
            continue;
        }

        // Case 3: Fresh handler — save exception and jump to catch or finally
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
        XrCoroutine *coro = (XrCoroutine *) ctx->current_coro;
        is_child_coro = !xr_coro_flags_has(coro, XR_CORO_FLG_MAIN);
    }
    if (!is_child_coro && !(isolate && isolate->suppress_exception_print)) {
        fprintf(stderr, "\n[Uncaught Exception]\n");
        xr_exception_print(isolate, exception);
        fprintf(stderr, "\n");
    }
}

bool xr_vm_is_catch_reachable(XrayIsolate *isolate) {
    if (!isolate)
        return false;
    XrVMContext *ctx = xr_vm_current_ctx(isolate);
    if (!ctx || ctx->handler_count <= 0)
        return false;
    int floor = ctx->module_base_frame > 0 ? ctx->module_base_frame : 0;
    return ctx->handlers[ctx->handler_count - 1].frame_count > floor;
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
