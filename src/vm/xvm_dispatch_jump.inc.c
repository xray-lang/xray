/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_jump.inc.c — control-flow opcode dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, pc,
 * frame, ci, base, R, vmcase, vmbreak, savepc, xr_vm_is_truthy,
 * VM_CURRENT_CORO, ...) provided by the surrounding scope.
 * CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns:
 *   OP_JMP      — unconditional jump with reductions / GC safe
 *                 point on backward edges
 *   OP_TEST     — branch on truthiness (skip-next if !R[A] xor C)
 *   OP_TESTSET  — assign-then-branch (R[A] = R[B] if condition)
 */

/* ========================================================
** Control Flow Instructions
** ======================================================== */

vmcase(OP_JMP) {
    // JMP sJ: pc += sJ
    int offset = GETARG_sJ(i);

    /* Reductions check on backward jumps to prevent
    ** infinite loops from starving other coroutines.
    ** Performance impact < 3%, enables fair scheduling.
    ** Also serves as GC safe point for per-coroutine GC. */
    if (offset < 0 && vm_ctx && vm_ctx->current_coro) {
        XrCoroutine *coro = (XrCoroutine *) vm_ctx->current_coro;

        /* GC safe point: check and trigger GC at loop back-edge.
        ** Stack is consistent here (between instructions). */
        VM_GC_SAFEPOINT();

        if (--coro->reductions <= 0) {
            if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
                return XR_VM_CANCELLED;
            }
            coro->reductions = XR_CORO_REDUCTIONS;
            frame->pc = pc - 1;
            return XR_VM_YIELD;
        }

#ifdef XRAY_HAS_JIT
        /* OSR: try entering JIT code at this loop header.
         *
         * Priority order (eliminates atomic contention):
         *   1. jit_entry set → direct OSR (zero atomics)
         *   2. bg compilation done → install + OSR
         *   3. bg compilation in progress → skip (no fetch_add)
         *   4. not yet queued → count via fetch_add, trigger at threshold
         */
        if (isolate->vm.jit) {
            XrProto *_osr_proto = cl->proto;
            bool _do_osr = false;

            if (_osr_proto->jit_entry) {
                // Already compiled: attempt OSR on every back-edge
                _do_osr = true;
            } else {
                void *_pend =
                    atomic_load_explicit(&_osr_proto->jit_entry_pending, memory_order_acquire);
                if (_pend && (uintptr_t) _pend > 1) {
                    // Background compilation done → install + try OSR
                    xm_jit_install_bg_result(_osr_proto);
                    _do_osr = (_osr_proto->jit_entry != NULL);
                } else if (!_pend) {
                    // Not yet queued: count and trigger at threshold
                    if (atomic_fetch_add_explicit(&_osr_proto->exec_count, 1,
                                                  memory_order_relaxed) +
                            1 ==
                        (uint32_t) isolate->vm.jit_threshold) {
                        _do_osr = true;
                    }
                }
                // _pend == sentinel (0x1): compilation in progress, skip
            }

            if (_do_osr) {
                uint32_t _target_pc = (uint32_t) (pc + offset - PROTO_CODE_BASE(_osr_proto));
                coro->jit_ctx->call_closure = cl;
                coro->jit_ctx->osr_deopt_pc = -1;
                XrValue _osr_result;
                int _osr_rc = xm_jit_osr_trigger(
                    isolate->vm.jit, _osr_proto, coro, _target_pc, base, _osr_proto->maxstacksize,
                    _osr_proto->return_type_info
                        ? xr_type_to_slot_type(_osr_proto->return_type_info)
                        : XR_SLOT_ANY,
                    &_osr_result);
                if (_osr_rc == XM_JIT_OK) {
                    _osr_proto->osr_pending = false;
                    vm_ctx->last_nret = 1;
                    if (ci->base_offset > 0) {
                        XrValue *ret_slot = (ci->call_status & XR_CALL_KEEP_FUNC)
                                                ? VM_STACK + ci->result_offset
                                                : VM_STACK + ci->base_offset - 1;
                        *ret_slot = _osr_result;
                    }
                    VM_DEC_FRAME_COUNT;
                    if (VM_MODULE_BASE >= 0 && VM_FRAME_COUNT == VM_MODULE_BASE)
                        return XR_VM_OK;
                    ci = &VM_FRAMES[VM_FRAME_COUNT - 1];
                    if (!ci->closure || !ci->closure->proto)
                        return XR_VM_OK;
                    VM_SET_STACK_TOP(VM_STACK + ci->base_offset + ci->closure->proto->maxstacksize);
                    goto startfunc;
                }
                if (_osr_rc == XM_JIT_SUSPEND) {
                    savepc();
                    return XR_VM_BLOCKED;
                }
                if (coro->jit_ctx->osr_deopt_pc >= 0) {
                    pc = PROTO_CODE_BASE(_osr_proto) + coro->jit_ctx->osr_deopt_pc;
                    coro->jit_ctx->osr_deopt_pc = -1;
                    offset = 0;
                }
            }
        }
#endif
    }

    pc += offset;
    vmbreak;
}

vmcase(OP_TEST) {
    // TEST A k: if (bool(R[A])) != k then pc++
    XrValue va = R(GETARG_A(i));
    int k_flag = GETARG_B(i);
    bool truthy = vm_is_truthy(va);
    if (truthy != k_flag)
        pc++;
    vmbreak;
}

vmcase(OP_TESTSET) {
    /* OP_TESTSET: logical && / || always returns bool.
     * Previously returned the original operand.
     * Now returns xr_bool() for type consistency. */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int k_flag = GETARG_C(i);
    bool truthy = vm_is_truthy(R(b));
    if (truthy != k_flag) {
        pc++;  // Skip next instruction
    } else {
        R(a) = xr_bool(truthy);
    }
    vmbreak;
}
