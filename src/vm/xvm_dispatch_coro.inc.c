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
 * pc, frame, ci, base, R, vmcase, vmbreak, VM_DISPATCH,
 * VM_RUNTIME_ERROR, VM_CURRENT_CORO, TRACE_EXECUTION, ...)
 * provided by the surrounding scope. CMake excludes *.inc.c
 * from the VM_SRC glob.
 *
 * Owns the spawn / await / yield family plus coroutine thread-affinity and
 * coroutine-local opcodes. Heavy variants delegate to dispatch helpers in
 * xvm_coro_ops.c (vm_go / vm_await / ...).
 */

vmcase(OP_GO) {
    TRACE_EXECUTION();
    ci->pc = pc;
    XrDispatchAction _sc_cr = vm_go(isolate, vm_ctx, i, base, ci);
    pc = ci->pc;
    VM_DISPATCH(_sc_cr);
}

vmcase(OP_AWAIT) {
    TRACE_EXECUTION();
    /* Inline fast path: task completed with immediate value.
     * Avoids the out-of-line dispatch helper; one-shot await-go can
     * clear the temporary Task slot and recycle the executor immediately. */
    {
        int _aw_a = GETARG_A(i);
        int _aw_b = GETARG_B(i);
        int _aw_flags = GETARG_C(i);
        bool _aw_discard = (_aw_flags & 0x01) != 0;
        bool _aw_one_shot = (_aw_flags & 0x02) != 0;
        XrValue _aw_tv = base[_aw_b];
        if (xr_value_is_task(_aw_tv)) {
            XrTask *_aw_task = xr_value_to_task(_aw_tv);
            XrRuntime *_aw_runtime = isolate ? (XrRuntime *) isolate->vm.scheduler : NULL;
            if (_aw_one_shot && _aw_runtime) {
                xr_sched_metric_inc(_aw_runtime,
                                    &_aw_runtime->sched_stats.task_one_shot_await_count);
            }
            uint8_t _aw_st = atomic_load_explicit(&_aw_task->state, memory_order_acquire);
            if (_aw_st == XR_TASK_COMPLETED) {
                XrValue _aw_res = _aw_task->result;
                if (!XR_IS_PTR(_aw_res)) {
                    /* A replayed await reaches this fast path after a blocked
                     * registration: drop the waiter-side bookkeeping (wait
                     * state's await_task back-pointer) before the task can be
                     * one-shot destroyed, or it would dangle. */
                    xr_task_finish_await_waiters((XrCoroutine *) VM_CURRENT_CORO);
                    // Immediate value: no deep copy
                    base[_aw_a] = _aw_discard ? xr_null() : _aw_res;
                    XrWorker *_aw_stats_worker = xr_current_worker();
                    if (_aw_stats_worker && _aw_stats_worker->p.runtime) {
                        xr_sched_metric_inc(
                            _aw_stats_worker->p.runtime,
                            &_aw_stats_worker->p.runtime->sched_stats.vm_await_done_fast_count);
                    }
                    if (_aw_one_shot && _aw_a != _aw_b)
                        base[_aw_b] = xr_null();
                    /* Exchange-claim: the completing worker reclaims
                     * immediate-result executors before publishing
                     * COMPLETED, so this is NULL on the hot path; a
                     * non-NULL claim transfers shell ownership to us. */
                    XrCoroutine *_aw_exec = xr_task_claim_executor(_aw_task);
                    if (_aw_exec) {
                        _aw_exec->task = NULL;
                        if (_aw_one_shot && xr_coro_flags_has(_aw_exec, XR_CORO_FLG_DONE) &&
                            !xr_coro_flags_has(_aw_exec, XR_CORO_FLG_MAIN)) {
                            _aw_exec->gc_flags |= XR_CORO_GC_TRIM_BACKEND_STORAGE;
                            XrWorker *_aw_worker = xr_current_worker();
                            if (_aw_worker) {
                                xr_coro_recycle_local(_aw_worker, _aw_exec);
                            } else {
                                xr_coro_destroy(_aw_exec);
                            }
                        }
                    }
                    if (_aw_one_shot && _aw_runtime)
                        (void) xr_task_runtime_try_destroy_detached(_aw_runtime, _aw_task);
                    vmbreak;
                }
            }
        }
    }
    VM_DISPATCH(vm_await(isolate, vm_ctx, i, base, ci, pc));
}

vmcase(OP_AWAIT_TIMEOUT) {
    TRACE_EXECUTION();
    VM_DISPATCH(vm_await_timeout(isolate, vm_ctx, i, base, ci, pc));
}

vmcase(OP_AWAIT_ALL) {
    TRACE_EXECUTION();
    VM_DISPATCH(vm_await_all(isolate, vm_ctx, i, base, ci, pc));
}

vmcase(OP_AWAIT_ANY) {
    TRACE_EXECUTION();
    VM_DISPATCH(vm_await_any(isolate, vm_ctx, i, base, ci, pc));
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
        if (xr_coro_consume_reds(current, hint) <= 0) {
            xr_coro_set_reds(current, XR_CORO_REDUCTIONS);
            frame->pc = pc;
            return XR_VM_YIELD;
        }
    }
    vmbreak;
}

vmcase(OP_GEN_YIELD) {
    /* generator `yield expr`: hand R[A] to the driving iterator and suspend.
     * Generator coroutines are pull-driven synchronously (never scheduled), so
     * the value lives in coro->result until xr_vm_gen_drive reads it. */
    XrCoroutine *current = (XrCoroutine *) VM_CURRENT_CORO;
    if (current != NULL) {
        int a = GETARG_A(i);
        current->result = R(a);
        frame->pc = pc;
        return XR_VM_YIELD;
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

vmcase(OP_CORO_CTRL) {
    // Dispatch: all coro monitoring/diagnostics sub-operations
    vm_coro_ctrl(isolate, vm_ctx, i, base);
    vmbreak;
}
