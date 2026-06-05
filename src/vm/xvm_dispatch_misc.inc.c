/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_misc.inc.c — defer / bytes / scope / time / sleep dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, vm_ctx,
 * pc, frame, ci, base, R, savepc, vmcase, vmbreak,
 * VM_RUNTIME_ERROR, VM_DISPATCH, VM_HANDLER_COUNT,
 * VM_FRAME_COUNT, VM_CURRENT_CORO, TRACE_EXECUTION, checkGC,
 * startfunc label, ...) provided by the surrounding scope.
 * CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns: OP_DEFER, OP_BYTES_NEW, OP_SCOPE_ENTER, OP_SCOPE_EXIT,
 *       OP_TIME_AFTER, OP_SLEEP, OP_SELECT_BLOCK dispatch.
 */

vmcase(OP_DEFER) {
    /* OP_DEFER A B - push closure and args to defer stack
     * A = closure register
     * B = argument count (args at R[A+1]..R[A+B])
     *
     * defer stack storage format (each entry):
     *   [0] = closure
     *   [1] = argument count (integer)
     *   [2..n+1] = argument values
     */
    int a = GETARG_A(i);
    int b = GETARG_B(i);  // Argument count
    XrValue closure_val = R(a);

    // Required stack space: closure + arg count + arg values
    int needed = 2 + b;

    // Lazy allocate per-context defer stack
    if (vm_ctx->defer_stack == NULL) {
        vm_ctx->defer_capacity = XR_DEFER_ENTRIES_MAX;
        XR_MALLOC_OR_ABORT(vm_ctx->defer_stack, sizeof(XrValue) * vm_ctx->defer_capacity,
                           "vm defer_stack init");
        XR_MALLOC_OR_ABORT(vm_ctx->defer_frame_marks, sizeof(int) * vm_ctx->frame_capacity,
                           "vm defer_frame_marks init");
        // Zero-init all slots.  Active frames whose startfunc ran before this
        // allocation get mark 0, which is correct because no OP_DEFER could
        // have fired before this first lazy allocation.
        for (int j = 0; j < vm_ctx->frame_capacity; j++) {
            vm_ctx->defer_frame_marks[j] = 0;
        }
    }

    // Capacity expansion check
    while (vm_ctx->defer_count + needed > vm_ctx->defer_capacity) {
        vm_ctx->defer_capacity *= 2;
        XR_REALLOC_OR_ABORT(vm_ctx->defer_stack, sizeof(XrValue) * (size_t) vm_ctx->defer_capacity,
                            "vm defer_stack grow");
    }

    // Push to defer stack: closure + arg count + args
    vm_ctx->defer_stack[vm_ctx->defer_count++] = closure_val;
    vm_ctx->defer_stack[vm_ctx->defer_count++] = xr_int(b);
    for (int j = 0; j < b; j++) {
        vm_ctx->defer_stack[vm_ctx->defer_count++] = R(a + 1 + j);
    }
    vmbreak;
}

vmcase(OP_BYTES_NEW) {
    /* R[A] = Bytes(R[A+1..A+B]) - create Array<uint8>
     * A = result register
     * B = argument count
     * C = storage_mode (0=normal, 1=shared)
     */
    int a = GETARG_A(i);
    int nargs = GETARG_B(i);
    int storage_mode = GETARG_C(i);

    int32_t len = 0;
    uint8_t fill_val = 0;
    bool has_fill = false;
    XrArray *src_arr = NULL;

    if (nargs == 0) {
        len = 0;
    } else if (nargs == 1) {
        XrValue arg = R(a + 1);
        if (XR_IS_INT(arg)) {
            len = (int32_t) XR_TO_INT(arg);
            if (len < 0)
                len = 0;
            has_fill = true;
            fill_val = 0;
        } else if (XR_IS_ARRAY(arg)) {
            src_arr = XR_TO_ARRAY(arg);
            len = src_arr->length;
        } else {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Bytes(n): n must be integer or array");
        }
    } else if (nargs == 2) {
        XrValue arg1 = R(a + 1);
        XrValue arg2 = R(a + 2);
        if (!XR_IS_INT(arg1) || !XR_IS_INT(arg2)) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Bytes(n, value): both args must be integers");
        }
        len = (int32_t) XR_TO_INT(arg1);
        if (len < 0)
            len = 0;
        fill_val = (uint8_t) (XR_TO_INT(arg2) & 0xFF);
        has_fill = true;
    } else {
        VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "Bytes() requires 0, 1 or 2 arguments");
    }

    XrArray *arr = NULL;
    if (storage_mode != 0 && isolate->sys_heap) {
        // Shared: allocate on system heap
        arr = (XrArray *) xr_sysheap_alloc_shared(isolate->sys_heap, sizeof(XrArray), XR_TARRAY);
        if (arr) {
            xr_array_init_inplace(arr, len > 0 ? len : 4, XR_ELEM_U8);
            XR_GC_SET_STORAGE(&arr->gc, XR_GC_STORAGE_SHARED);
            xr_shared_set_refc(&arr->gc, 1);
        }
    } else {
        arr = xr_array_with_capacity_typed(VM_CURRENT_CORO, len > 0 ? len : 0, XR_ELEM_U8);
    }

    if (arr) {
        if (src_arr) {
            // Copy from source array
            uint8_t *dst = (uint8_t *) arr->data;
            for (int32_t j = 0; j < len; j++) {
                XrValue elem = ((XrValue *) src_arr->data)[j];
                dst[j] = XR_IS_INT(elem) ? (uint8_t) (XR_TO_INT(elem) & 0xFF) : 0;
            }
            arr->length = len;
        } else if (has_fill && len > 0) {
            memset(arr->data, fill_val, len);
            arr->length = len;
        }
    }

    R(a) = arr ? xr_value_from_array(arr) : xr_null();
    if (storage_mode == 0)
        checkGC(base + a + 1);
    vmbreak;
}

/* === Scope structured concurrency instructions === */

vmcase(OP_SCOPE_ENTER) {
    // Enter structured concurrency scope
    XrCoroutine *current = (XrCoroutine *) VM_CURRENT_CORO;
    int scope_mode = GETARG_A(i);
    XrCoroBlockResult enter_result = xr_coro_scope_enter(isolate, current, (uint8_t) scope_mode);
    if (enter_result.kind == XR_CORO_BLOCK_ERROR) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "scope: out of memory");
    }
    vmbreak;
}

vmcase(OP_SCOPE_EXIT) {
    /* Exit structured concurrency scope.
     * A = scope_mode, B = result_reg (supervisor: errors[]) */
    int scope_mode = GETARG_A(i);
    int result_reg = GETARG_B(i);
    XrCoroutine *current = (XrCoroutine *) VM_CURRENT_CORO;

    if (current) {
        XrCoroBlockResult scope_result = xr_coro_scope_exit(current, (uint8_t) scope_mode);
        if (scope_result.kind == XR_CORO_BLOCK_BLOCKED) {
            frame->pc = pc - 1;
            return XR_VM_BLOCKED;
        }
        if (scope_result.kind == XR_CORO_BLOCK_NO_CORO) {
            vmbreak;
        }
        if (scope_result.kind == XR_CORO_BLOCK_ERROR) {
            XrValue err = scope_result.value;
            if (scope_result.ok) {
                /* Child failed via the value-return channel (user
                 * `throw <enum>`).  Re-raise on the same channel so the
                 * parent's `catch (e)` observes it.  The lowerer emits an
                 * XI_ERR_CHECK right after the scope block to route this to
                 * the catch (inside try) or propagate it (fallible fn). */
                vm_ctx->pending_error = err;
                vmbreak;
            }
            /* Child failed via the panic channel — re-raise as a panic so the
             * parent's `catch panic` observes it. */
            XrValue exc = err;
            if (!xr_value_is_exception(isolate, exc)) {
                exc = xr_exception_from_value(isolate, exc);
            }
            savepc();
            xr_vm_unwind_with_trace(isolate, exc);
            if (!xr_vm_is_catch_reachable(isolate))
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
        if (scope_mode == XR_SCOPE_SUPERVISOR) {
            base[result_reg] = scope_result.value;
        }
    } else {
        // Main thread fallback
        XrCoroState *sched = (XrCoroState *) isolate->vm.coro_state;
        if (!sched || !sched->current_scope)
            vmbreak;

        XrScopeContext *scope = sched->current_scope;
        int spin = 0;
        while (atomic_load(&scope->count) > 0) {
            if (++spin > 1000) {
                spin = 0;
                xr_thread_yield();
            }
        }
        if (scope_mode == XR_SCOPE_LINKED && !XR_IS_NULL(scope->first_error)) {
            XrValue err = scope->first_error;
            bool err_is_value = scope->first_error_is_value;
            sched->current_scope = scope->parent;
            xr_free(scope);
            if (err_is_value) {
                vm_ctx->pending_error = err;
                vmbreak;
            }
            XrValue exc = err;
            if (!xr_value_is_exception(isolate, exc)) {
                exc = xr_exception_from_value(isolate, exc);
            }
            savepc();
            xr_vm_unwind_with_trace(isolate, exc);
            if (!xr_vm_is_catch_reachable(isolate))
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
        if (scope_mode == XR_SCOPE_SUPERVISOR) {
            // Main thread: no coro for array alloc, use null
            if (scope->errors && scope->errors->length > 0) {
                base[result_reg] = xr_value_from_array(scope->errors);
            } else {
                base[result_reg] = xr_null();
            }
        }
        sched->current_scope = scope->parent;
        xr_free(scope);
    }
    vmbreak;
}

/* === Time / sleep / select-block === */

vmcase(OP_TIME_AFTER) {
    VM_DISPATCH(vm_time_dispatch(isolate, vm_ctx, i, base, frame, pc));
}

vmcase(OP_SLEEP) {
    VM_DISPATCH(vm_time_dispatch(isolate, vm_ctx, i, base, frame, pc));
}

vmcase(OP_SELECT_BLOCK) {
    TRACE_EXECUTION();
    VM_DISPATCH(vm_select_block(isolate, vm_ctx, i, base, ci, pc));
}
