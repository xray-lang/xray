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
 * VM_RUNTIME_ERROR, VM_DISPATCH_COLD, VM_HANDLER_COUNT,
 * VM_FRAME_COUNT, VM_CURRENT_CORO, TRACE_EXECUTION, checkGC,
 * startfunc label, ...) provided by the surrounding scope.
 * CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns: OP_DEFER, OP_BYTES_NEW, OP_SCOPE_ENTER, OP_SCOPE_EXIT,
 *       OP_TIME_AFTER, OP_SLEEP, OP_SELECT_BLOCK.
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
        vm_ctx->defer_stack = xr_malloc(sizeof(XrValue) * vm_ctx->defer_capacity);
        vm_ctx->defer_frame_marks = xr_malloc(sizeof(int) * vm_ctx->frame_capacity);
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

vmcase(OP_NEWEXCEPTION) {
    /* R[A] = new Exception(R[A+1..A+B])
     * A = result register
     * B = argument count (0..2): message, cause
     *
     * XrException has its own GC type, so the generic
     * "allocate XrInstance + invoke constructor" pipeline cannot serve
     * `new Exception(...)`. The opcode hands the args straight to the
     * native constructor primitive which allocates the right struct
     * shape and surfaces the result as XR_TEXCEPTION.
     */
    int a = GETARG_A(i);
    int nargs = GETARG_B(i);
    if (nargs > 2) {
        VM_RUNTIME_ERROR(XR_ERR_WRONG_ARG_COUNT, "new Exception(...) accepts at most 2 arguments");
    }
    R(a) = xr_exception_user_construct(isolate, xr_null(), &R(a + 1), nargs);
    checkGC(base + a + 1);
    vmbreak;
}

/* === Scope structured concurrency instructions === */

vmcase(OP_SCOPE_ENTER) {
    // Enter structured concurrency scope
    XrCoroutine *current = (XrCoroutine *) VM_CURRENT_CORO;
    if (current) {
        atomic_store(&current->wait_count, 0);
        atomic_store(&current->any_done, false);
    }

    // Create scope context — per-coroutine tracking
    int scope_mode = GETARG_A(i);
    XrScopeContext *scope = (XrScopeContext *) xr_malloc(sizeof(XrScopeContext));
    if (scope) {
        atomic_store(&scope->count, 0);
        scope->mode = (uint8_t) scope_mode;
        atomic_store(&scope->cancel_requested, false);
        atomic_init(&scope->child_lock, false);
        scope->first_error = xr_null();
        // Eager-allocate the supervisor errors[] on the scope owner's
        // GC heap so wake_waiter can push under the scope lock without
        // touching the allocator. The owner is the coroutine running
        // this OP_SCOPE_ENTER (its lifetime brackets the scope block).
        scope->errors = (scope_mode == XR_SCOPE_SUPERVISOR && current)
                            ? xr_array_with_capacity(current, 4)
                            : NULL;
        scope->first_child = NULL;
        scope->owner = current;
        if (current) {
            scope->parent = current->current_scope;
            current->current_scope = scope;
        } else {
            // Main thread fallback: use scheduler global
            XrCoroState *sched = (XrCoroState *) isolate->vm.coro_state;
            if (sched) {
                scope->parent = sched->current_scope;
                sched->current_scope = scope;
            }
        }
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
        XrScopeContext *scope = current->current_scope;
        if (!scope)
            vmbreak;

        if (atomic_load(&scope->count) > 0) {
            // Children still running — block and re-execute on resume
            frame->pc = pc - 1;
            uint32_t old_flags = xr_coro_flags_load(current);
            atomic_store(&current->flags, xr_coro_set_wait_reason_flags(
                                              old_flags, XR_CORO_WAIT_SCOPE >> XR_CORO_WAIT_SHIFT));
            return XR_VM_BLOCKED;
        }
        atomic_store(&current->wait_count, 0);

        // All children done
        if (scope_mode == XR_SCOPE_LINKED && !XR_IS_NULL(scope->first_error)) {
            // linked scope: throw first error
            XrValue err = scope->first_error;
            current->current_scope = scope->parent;
            xr_free(scope);
            XrValue exc = err;
            if (!xr_is_exception(exc)) {
                exc = xr_exception_from_value(isolate, exc);
            }
            savepc();
            xr_vm_unwind_with_trace(isolate, exc);
            if (!xr_vm_is_catch_reachable(isolate))
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
        if (scope_mode == XR_SCOPE_SUPERVISOR) {
            // supervisor scope: write collected errors[] to result_reg
            if (scope->errors && scope->errors->length > 0) {
                base[result_reg] = xr_value_from_array(scope->errors);
            } else {
                XrArray *empty = xr_array_new(current);
                base[result_reg] = empty ? xr_value_from_array(empty) : xr_null();
            }
        }
        current->current_scope = scope->parent;
        xr_free(scope);
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
    /* R[A] = time.after(R[B]) - create Timer Channel
     * A = target register
     * B = timeout register (milliseconds)
     */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue timeout_val = R(b);

    int64_t timeout_ms = 0;
    if (XR_IS_INT(timeout_val)) {
        timeout_ms = XR_TO_INT(timeout_val);
    } else if (XR_IS_FLOAT(timeout_val)) {
        timeout_ms = (int64_t) XR_TO_FLOAT(timeout_val);
    }

    if (timeout_ms < 0)
        timeout_ms = 0;

    // Create Timer Channel
    XrChannel *timer_ch = xr_channel_new_timer(isolate, timeout_ms);
    if (!timer_ch) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "time.after: out of memory");
    }

    // Arm on current worker's timer wheel (no polling).
    {
        XrWorker *_w = xr_current_worker();
        if (_w && _w->p.timer_wheel) {
            xr_channel_timer_arm(timer_ch, _w->p.timer_wheel);
        }
    }

    R(a) = xr_value_from_channel(timer_ch);
    vmbreak;
}

vmcase(OP_SLEEP) {
    /* time.sleep(R[A]) - coroutine-friendly sleep
     * A = sleep time (milliseconds, int)
     *
     * Coroutine mode: set wake time, yield CPU
     * Non-coroutine mode: blocking sleep
     */
    int a = GETARG_A(i);
    XrValue val = R(a);

    int64_t milliseconds = 0;
    if (XR_IS_INT(val)) {
        milliseconds = XR_TO_INT(val);
    } else if (XR_IS_FLOAT(val)) {
        milliseconds = (int64_t) XR_TO_FLOAT(val);
    }

    if (milliseconds <= 0) {
        vmbreak;
    }

    // Check if in coroutine
    XrCoroutine *coro = (XrCoroutine *) VM_CURRENT_CORO;
    if (coro) {
        // Coroutine mode: use Timer Wheel for precise timed wake
        XrRuntime *rt = (XrRuntime *) isolate->vm.runtime;
        (void) rt;

        // First set as pure sleep (no fd)
        XrCoroExt *sleep_ext = xr_coro_ensure_ext(coro);
        if (sleep_ext) {
            sleep_ext->yield_info.wait_fd = -1;
            sleep_ext->yield_info.wait_events = 0;
        }
        // Set wait reason in flags
        uint32_t old_flags = xr_coro_flags_load(coro);
        uint32_t new_flags =
            xr_coro_set_wait_reason_flags(old_flags, XR_CORO_WAIT_SLEEP >> XR_CORO_WAIT_SHIFT);
        atomic_store(&coro->flags, new_flags);

        // Add timer to current Worker's Timer Wheel (Per-Worker lock-free)
        XrWorker *worker = xr_current_worker();
        XrTimerWheel *tw = worker ? worker->p.timer_wheel : NULL;

        if (tw) {
            // Use Per-Worker Timer Wheel (lock-free)
            XR_DBG_TIMER("Add timer: coro=%d, ms=%lld, worker=%d, tw=%p", coro->id,
                         (long long) milliseconds, worker->p.id, (void *) tw);
            xr_worker_add_sleep_timer(worker, coro, milliseconds);
        } else {
            /* Timer wheel must exist for all workers.
             * If missing, fall back to blocking sleep to avoid
             * setting timer_active without timer wheel registration. */
            xr_time_sleep_ms((uint64_t) milliseconds);
            vmbreak;
        }

        // Mark as pure sleep (no fd) - already set above

        // Save current instruction address, re-execute on resume
        frame->pc = pc;

        // Return BLOCKED, let scheduler switch to other coroutines
        return XR_VM_BLOCKED;
    }

    // Non-coroutine mode: blocking sleep.
    xr_time_sleep_ms((uint64_t) milliseconds);
    vmbreak;
}

vmcase(OP_SELECT_BLOCK) {
    TRACE_EXECUTION();
    VM_DISPATCH_COLD(vm_select_block(isolate, vm_ctx, i, base, ci, pc));
}
