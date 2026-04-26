/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_exception.inc.c — try / catch / finally / throw dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, vm_ctx,
 * pc, ci, R, savepc, vmcase, vmbreak, VM_RUNTIME_ERROR,
 * VM_HANDLERS, VM_HANDLER_COUNT, VM_INC_HANDLER_COUNT,
 * VM_DEC_HANDLER_COUNT, VM_STACK, VM_STACK_TOP, VM_FRAME_COUNT,
 * VM_SET_EXCEPTION, TRACE_EXECUTION, startfunc label, ...)
 * provided by the surrounding scope. CMake excludes *.inc.c
 * from the VM_SRC glob.
 *
 * Owns: OP_TRY, OP_CATCH, OP_FINALLY, OP_END_TRY, OP_THROW.
 * The companion OP_SPILL / OP_RELOAD pair lives next to these
 * in the source order but is not exception-related; it stays
 * in xvm.c with the rest of the register-window machinery.
 */

vmcase(OP_TRY) {
    // Set exception handler
    TRACE_EXECUTION();
    int catch_offset = GETARG_Bx(i);

    // Read next instruction to get finally offset
    XrInstruction next_i = *pc++;
    int finally_offset = GETARG_Bx(next_i);

    // Lazy allocate / grow exception handler array
    if (VM_HANDLER_COUNT >= vm_ctx->handler_capacity) {
        int new_cap = vm_ctx->handler_capacity == 0 ? 8 : vm_ctx->handler_capacity * 2;
        if (new_cap > XR_EXCEPTION_HANDLERS_MAX)
            new_cap = XR_EXCEPTION_HANDLERS_MAX;
        if (VM_HANDLER_COUNT >= new_cap) {
            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "exception handler nesting too deep");
        }
        XrExceptionHandler *new_h = (XrExceptionHandler *) xr_realloc(
            vm_ctx->handlers, sizeof(XrExceptionHandler) * new_cap);
        if (!new_h) {
            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "failed to allocate exception handlers");
        }
        vm_ctx->handlers = new_h;
        vm_ctx->handler_capacity = new_cap;
    }

    int _hidx = VM_HANDLER_COUNT;
    VM_INC_HANDLER_COUNT;
    XrExceptionHandler *handler = &VM_HANDLERS[_hidx];
    handler->catch_offset = catch_offset;
    handler->finally_offset = finally_offset;
    handler->stack_size = (int) (VM_STACK_TOP - VM_STACK);
    handler->frame_count = VM_FRAME_COUNT;
    handler->exception = xr_null();
    handler->caught = false;
    handler->in_finally = false;
    handler->try_pc = pc - 2;  // Save try instruction position

    vmbreak;
}

vmcase(OP_CATCH) {
    // Catch exception: R[A] = originally thrown value (or exception object)
    TRACE_EXECUTION();
    int a = GETARG_A(i);

    // Get current handler
    if (VM_HANDLER_COUNT > 0) {
        XrExceptionHandler *handler = &VM_HANDLERS[VM_HANDLER_COUNT - 1];

        // Store exception value in specified register
        if (!XR_IS_NULL(handler->exception)) {
            XrValue exc = handler->exception;

            // If wrapped exception, return original value (userData)
            if (XR_IS_EXCEPTION(exc)) {
                XrException *ex = XR_AS_EXCEPTION(exc);
                if (!XR_IS_NULL(ex->userData)) {
                    R(a) = ex->userData;
                } else {
                    R(a) = exc;
                }
            } else {
                R(a) = exc;
            }
            handler->caught = true;
            // Clear consumed exception; if catch rethrows,
            // xr_vm_throw_exception will set a new one.
            handler->exception = xr_null();
            // Also clear the ctx-wide pending-exception
            // slot. Subsequent dispatch hot paths (notably
            // OP_INVOKE / OP_INVOKE_BUILTIN) treat a
            // non-null current_exception as "the most
            // recently called builtin threw"; leaving the
            // caught value in place would cause the next
            // builtin call to spuriously unwind.
            VM_SET_EXCEPTION(xr_null());
        }
    }

    vmbreak;
}

vmcase(OP_FINALLY) {
    // finally block start - mark handler so re-throw propagates outward
    TRACE_EXECUTION();
    if (VM_HANDLER_COUNT > 0) {
        VM_HANDLERS[VM_HANDLER_COUNT - 1].in_finally = true;
    }
    vmbreak;
}

vmcase(OP_END_TRY) {
    // End try-catch-finally block
    TRACE_EXECUTION();

    if (VM_HANDLER_COUNT > 0) {
        XrExceptionHandler *handler = &VM_HANDLERS[VM_HANDLER_COUNT - 1];

        // Check for pending exception that needs re-throw:
        // 1. Uncaught exception (try-finally without catch)
        // 2. Exception thrown during catch, finally just finished
        bool has_pending =
            !XR_IS_NULL(handler->exception) && (!handler->caught || handler->in_finally);
        if (has_pending) {
            XrValue exc = handler->exception;
            VM_DEC_HANDLER_COUNT;  // Pop handler
            xr_vm_throw_exception(isolate, exc);

            // Check if there are upper handlers
            if (VM_HANDLER_COUNT == 0) {
                return XR_VM_RUNTIME_ERROR;
            }
            // Jump to upper handler
            goto startfunc;
        } else {
            // Normal end, pop handler
            VM_DEC_HANDLER_COUNT;
        }
    }

    vmbreak;
}

vmcase(OP_THROW) {
    // Throw exception: throw R[A]
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    XrValue exception = R(a);

    // If not an exception object, wrap automatically
    if (!xr_is_exception(exception)) {
        exception = xr_exception_from_value(isolate, exception);
    }

    // Record full call chain into the exception trace
    // and rewind to the matching handler in one step.
    savepc();

    // Debug hook: check exception breakpoint before unwinding
    {
        XrDebugHooks *_eh = (XrDebugHooks *) isolate->debug_hooks;
        if (_eh && _eh->on_exception) {
            bool _unc = (VM_HANDLER_COUNT == 0);
            const char *_msg =
                XR_IS_EXCEPTION(exception) ? xr_exception_get_message(exception) : "<exception>";
            if (_eh->on_exception(isolate, _msg, _unc) == XR_DBG_ACTION_BREAK) {
                VM_SET_EXCEPTION(exception);
                ci->pc = pc - 1;
                return XR_VM_DEBUG_BREAK;
            }
        }
    }

    // Throw exception (records full call-chain trace
    // before unwinding to the matching handler).
    xr_vm_unwind_with_trace(isolate, exception);

    // Check if uncaught exception
    if (VM_HANDLER_COUNT == 0) {
        // Uncaught exception, return error
        return XR_VM_RUNTIME_ERROR;
    }

    // Jump to catch or finally, continue execution
    goto startfunc;
}
