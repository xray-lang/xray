/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_exception.inc.c — panic handler + value-return error dispatch
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
 * Owns: OP_TRY, OP_CATCH, OP_END_TRY, OP_THROW (panic channel),
 *       OP_ERR_SET, OP_ERR_RETURN, OP_ERR_CHECK, OP_ERR_HAS,
 *       OP_ERR_CATCH (value-return error channel).
 */

vmcase(OP_TRY) {
    /* Push a panic handler. Bx = absolute PC of the catch block. */
    TRACE_EXECUTION();
    int catch_offset = GETARG_Bx(i);

    /* Grow handler array if needed (inline→heap on first overflow) */
    if (VM_HANDLER_COUNT >= vm_ctx->handler_capacity) {
        int new_cap = vm_ctx->handler_capacity * 2;
        if (new_cap > XR_EXCEPTION_HANDLERS_MAX)
            new_cap = XR_EXCEPTION_HANDLERS_MAX;
        if (VM_HANDLER_COUNT >= new_cap) {
            VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "exception handler nesting too deep");
        }
        XrExceptionHandler *new_h;
        if (vm_ctx->handlers == vm_ctx->handler_inline) {
            new_h = (XrExceptionHandler *) xr_malloc(sizeof(XrExceptionHandler) * new_cap);
            if (!new_h) {
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "failed to allocate exception handlers");
            }
            memcpy(new_h, vm_ctx->handler_inline, sizeof(XrExceptionHandler) * VM_HANDLER_COUNT);
        } else {
            new_h = (XrExceptionHandler *) xr_realloc(vm_ctx->handlers,
                                                      sizeof(XrExceptionHandler) * new_cap);
            if (!new_h) {
                VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "failed to allocate exception handlers");
            }
        }
        vm_ctx->handlers = new_h;
        vm_ctx->handler_capacity = new_cap;
    }

    int _hidx = VM_HANDLER_COUNT;
    VM_INC_HANDLER_COUNT;
    XrExceptionHandler *handler = &VM_HANDLERS[_hidx];
    handler->catch_offset = (uint32_t) catch_offset;
    handler->stack_size = (int) (VM_STACK_TOP - VM_STACK);
    handler->frame_count = VM_FRAME_COUNT;
    handler->exception = xr_null();
    handler->caught = false;

    vmbreak;
}

vmcase(OP_CATCH) {
    /* Bind the caught panic Exception into R[A].
     * Always delivers the full Exception object (user accesses
     * .message, .code, .stackTrace, .data as needed). */
    TRACE_EXECUTION();
    int a = GETARG_A(i);

    if (VM_HANDLER_COUNT > 0) {
        XrExceptionHandler *handler = &VM_HANDLERS[VM_HANDLER_COUNT - 1];

        if (!XR_IS_NULL(handler->exception)) {
            R(a) = handler->exception;
            handler->caught = true;
            handler->exception = xr_null();
            /* Clear ctx-wide slot so OP_INVOKE doesn't spuriously
             * treat it as "builtin just panicked". */
            VM_SET_EXCEPTION(xr_null());
        }
    }

    vmbreak;
}

vmcase(OP_END_TRY) {
    /* Pop the panic handler. If an uncaught panic is pending (thrown
     * inside the catch block itself), propagate it outward. */
    TRACE_EXECUTION();

    if (VM_HANDLER_COUNT > 0) {
        XrExceptionHandler *handler = &VM_HANDLERS[VM_HANDLER_COUNT - 1];

        /* Uncaught: exception still pending and not consumed by OP_CATCH */
        bool has_pending = !XR_IS_NULL(handler->exception) && !handler->caught;
        if (has_pending) {
            XrValue exc = handler->exception;
            VM_DEC_HANDLER_COUNT;
            xr_vm_throw_exception(isolate, exc);

            if (!xr_vm_is_catch_reachable(isolate)) {
                return XR_VM_RUNTIME_ERROR;
            }
            goto startfunc;
        } else {
            VM_DEC_HANDLER_COUNT;
        }
    }

    vmbreak;
}

vmcase(OP_THROW) {
    /* Throw exception: throw R[A] */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    XrValue exception = R(a);

    /* Strict throw is enforced by the analyzer so source-level
     * `throw <e>` always produces an Exception. Defensive wrap
     * only fires for bytecode that bypassed the analyzer. */
    if (!xr_value_is_exception(isolate, exception)) {
        exception = xr_exception_from_value(isolate, exception);
    }

    savepc();

    /* Debug hook: exception breakpoint */
    {
        XrDebugHooks *_eh = (XrDebugHooks *) isolate->debug_hooks;
        if (_eh && _eh->on_exception) {
            bool _unc = (VM_HANDLER_COUNT == 0);
            const char *_msg = xr_value_is_exception(isolate, exception)
                                   ? xr_exception_get_message(isolate, exception)
                                   : "<exception>";
            if (_eh->on_exception(isolate, _msg, _unc) == XR_DBG_ACTION_BREAK) {
                VM_SET_EXCEPTION(exception);
                ci->pc = pc - 1;
                return XR_VM_DEBUG_BREAK;
            }
        }
    }

    /* Record stack trace and unwind to nearest handler. */
    xr_vm_unwind_with_trace(isolate, exception);

    if (!xr_vm_is_catch_reachable(isolate)) {
        return XR_VM_RUNTIME_ERROR;
    }

    goto startfunc;
}

/* ========== Value-Return Error Channel ========== */

vmcase(OP_ERR_SET) {
    /* Set pending_error without returning. Used for throw inside
     * try body: CFG jumps to the catch block next. */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    vm_ctx->pending_error = R(a);
    vmbreak;
}

vmcase(OP_ERR_RETURN) {
    /* Set pending_error and return from the function.
     * Pure value-return: no handler stack, no unwind. */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    vm_ctx->pending_error = R(a);
    vm_ctx->last_nret = 0;
    goto return_with_defer;
}

vmcase(OP_ERR_CHECK) {
    /* After a fallible call outside a local catch: if pending_error
     * is set, propagate by returning (pure value-return). */
    TRACE_EXECUTION();
    if (!XR_IS_NULL(vm_ctx->pending_error)) {
        vm_ctx->last_nret = 0;
        goto return_with_defer;
    }
    vmbreak;
}

vmcase(OP_ERR_HAS) {
    /* R[A] = !IS_NULL(pending_error).
     * Used inside try body to branch into the catch block. */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    R(a) = xr_bool(!XR_IS_NULL(vm_ctx->pending_error));
    vmbreak;
}

vmcase(OP_ERR_CATCH) {
    /* Bind pending_error into R[A] and clear the error channel. */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    R(a) = vm_ctx->pending_error;
    vm_ctx->pending_error = xr_null();
    vmbreak;
}
