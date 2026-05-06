/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_coro.inc.c — coroutine opcode dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, vm_ctx,
 * pc, frame, ci, base, R, vmcase, vmbreak, VM_DISPATCH_COLD,
 * VM_RUNTIME_ERROR, VM_CURRENT_CORO, TRACE_EXECUTION, ...)
 * provided by the surrounding scope. CMake excludes *.inc.c
 * from the VM_SRC glob.
 *
 * Owns the spawn / await / yield family plus the coroutine
 * thread-affinity, coroutine-local, and priority opcodes that
 * sit alongside them. Heavy variants delegate to cold-path
 * helpers (vm_spawn_cont / vm_await / vm_await_timeout / ...).
 */

vmcase(OP_SPAWN_CONT) {
    TRACE_EXECUTION();
    ci->pc = pc;
    int _sc_cr = vm_spawn_cont(isolate, vm_ctx, i, base, ci);
    pc = ci->pc;
    VM_DISPATCH_COLD(_sc_cr);
}

vmcase(OP_AWAIT) {
    TRACE_EXECUTION();
    /* Inline fast path: task completed with immediate value.
     * Avoids noinline cold path call and defers executor recycle
     * to next pool_get — matching channel recv hot path perf. */
    {
        int _aw_a = GETARG_A(i);
        XrValue _aw_tv = base[GETARG_B(i)];
        if (xr_value_is_task(_aw_tv)) {
            XrTask *_aw_task = xr_value_to_task(_aw_tv);
            uint8_t _aw_st = atomic_load_explicit(&_aw_task->state, memory_order_acquire);
            if (_aw_st == XR_TASK_COMPLETED) {
                XrValue _aw_res = _aw_task->result;
                if (!XR_IS_PTR(_aw_res)) {
                    // Immediate value: no deep copy
                    base[_aw_a] = GETARG_C(i) ? xr_null() : _aw_res;
                    /* Detach executor only — do NOT recycle.
                     * Task lives on executor's Immix heap;
                     * parent's tasks array still references it.
                     * Recycling frees the Immix block, causing
                     * use-after-free when parent's GC scans
                     * the dangling Task pointer. */
                    XrCoroutine *_aw_exec = _aw_task->coro;
                    if (_aw_exec) {
                        _aw_task->coro = NULL;
                        _aw_exec->task = NULL;
                    }
                    vmbreak;
                }
            }
        }
    }
    VM_DISPATCH_COLD(vm_await(isolate, vm_ctx, i, base, ci, pc));
}

vmcase(OP_AWAIT_TIMEOUT) {
    TRACE_EXECUTION();
    VM_DISPATCH_COLD(vm_await_timeout(isolate, vm_ctx, i, base, ci, pc));
}

vmcase(OP_AWAIT_ALL) {
    TRACE_EXECUTION();
    VM_DISPATCH_COLD(vm_await_all(isolate, vm_ctx, i, base, ci, pc));
}

vmcase(OP_AWAIT_ANY) {
    TRACE_EXECUTION();
    VM_DISPATCH_COLD(vm_await_any(isolate, vm_ctx, i, base, ci, pc));
}

vmcase(OP_YIELD) {
    /* yield - cooperatively yield execution to the scheduler
     *
     * A=0: immediate yield (user explicit `yield` statement)
     * A>0: hint yield (compiler-inserted, e.g. select default path)
     *      Deducts A from reductions; only yields when reductions <= 0.
     *      This avoids context-switch storms while still ensuring
     *      fairness within a bounded number of iterations.
     */
    XrCoroutine *current = (XrCoroutine *) VM_CURRENT_CORO;
    if (current != NULL) {
        int hint = GETARG_A(i);
        if (hint == 0) {
            // Immediate yield
            frame->pc = pc;
            return XR_VM_YIELD;
        }
        // Hint yield: accelerate next scheduling point
        current->reductions -= hint;
        if (current->reductions <= 0) {
            current->reductions = XR_CORO_REDUCTIONS;
            frame->pc = pc;
            return XR_VM_YIELD;
        }
    }
    vmbreak;
}

vmcase(OP_CANCELLED) {
    // R[A] = cancelled() - check if cancelled
    int a = GETARG_A(i);
    R(a) = xr_bool(false);  // Default: not cancelled
    vmbreak;
}

vmcase(OP_LOCK_THREAD) {
    // Coro.lockThread() - pin coro to current worker
    XrCoroutine *coro = (XrCoroutine *) VM_CURRENT_CORO;
    if (coro) {
        XrCoroExt *lext = xr_coro_ensure_ext(coro);
        if (lext) {
            int old_count = atomic_fetch_add(&lext->lock_count, 1);
            if (old_count == 0) {
                XrWorker *worker = xr_current_worker();
                lext->locked_worker = worker ? worker->p.id : 0;
            }
        }
    }
    vmbreak;
}

vmcase(OP_UNLOCK_THREAD) {
    // Coro.unlockThread() - unpin coro
    XrCoroutine *coro = (XrCoroutine *) VM_CURRENT_CORO;
    if (coro && coro->ext) {
        int old_count = atomic_fetch_sub(&coro->ext->lock_count, 1);
        if (old_count <= 1) {
            atomic_store(&coro->ext->lock_count, 0);
            coro->ext->locked_worker = -1;
        }
    }
    vmbreak;
}

vmcase(OP_SET_LOCAL) {
    // Coro.setLocal(R[A], R[B])
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue key = R(a);
    XrValue value = R(b);
    XrCoroutine *current = (XrCoroutine *) VM_CURRENT_CORO;
    if (!current) {
        if (!isolate->vm.main_locals) {
            isolate->vm.main_locals = xr_map_new(VM_CURRENT_CORO);
        }
        xr_map_set(isolate->vm.main_locals, key, value);
    } else {
        XrCoroExt *lext = xr_coro_ensure_ext(current);
        if (lext) {
            if (!lext->locals) {
                lext->locals = xr_map_new(VM_CURRENT_CORO);
            }
            xr_map_set(lext->locals, key, value);
        }
    }
    vmbreak;
}

vmcase(OP_GET_LOCAL) {
    // R[A] = Coro.getLocal(R[B])
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue key = R(b);
    XrCoroutine *current = (XrCoroutine *) VM_CURRENT_CORO;
    XrMap *locals = NULL;
    if (!current) {
        locals = isolate->vm.main_locals;
    } else {
        locals = current->ext ? current->ext->locals : NULL;
    }
    if (locals) {
        bool found;
        XrValue result = xr_map_get(locals, key, &found);
        R(a) = found ? result : xr_null();
    } else {
        R(a) = xr_null();
    }
    vmbreak;
}

vmcase(OP_SET_PRIORITY) {
    // Coro.setPriority(R[A], R[B])
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue task_val = R(a);
    XrValue prio_val = R(b);
    XrCoroutine *coro = NULL;
    if (xr_value_is_task(task_val)) {
        XrTask *_t = xr_value_to_task(task_val);
        coro = _t->coro;
    } else if (xr_value_is_coro(task_val)) {
        coro = xr_value_to_coro(task_val);
    }
    if (!coro) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "setPriority: task or coroutine object required");
    }
    XrCoroPriority new_prio = CORO_PRIORITY_NORMAL;
    if (XR_IS_INT(prio_val)) {
        int prio_int = (int) XR_TO_INT(prio_val);
        if (prio_int >= 0 && prio_int < XR_CORO_PRIORITY_COUNT) {
            new_prio = (XrCoroPriority) prio_int;
        }
    }
    int old_prio = xr_coro_get_priority(xr_coro_flags_load(coro));
    if (xr_coro_flags_has(coro, XR_CORO_FLG_READY) && old_prio != (int) new_prio) {
        XrCoroState *sched = (XrCoroState *) isolate->vm.coro_state;
        if (sched) {
            xr_sched_remove(sched, coro);
            uint32_t old_flags = xr_coro_flags_load(coro);
            atomic_store(&coro->flags, xr_coro_set_priority_flags(old_flags, new_prio));
            xr_sched_enqueue(sched, coro);
        }
    } else {
        uint32_t old_flags = xr_coro_flags_load(coro);
        atomic_store(&coro->flags, xr_coro_set_priority_flags(old_flags, new_prio));
    }
    vmbreak;
}

vmcase(OP_CORO_CTRL) {
    // Cold path: all coro monitoring/diagnostics sub-operations
    vm_coro_ctrl(isolate, vm_ctx, i, base);
    vmbreak;
}
