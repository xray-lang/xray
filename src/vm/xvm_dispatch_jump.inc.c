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

        /* Host-supplied wall-clock deadline. Checked here (back-edge) so a
        ** tight infinite loop in user code cannot wedge the embedder. The
        ** deadline is opt-in: when no deadline is armed the call is a
        ** single load + compare and the branch predictor pins it firmly
        ** to "no". */
        if (XR_UNLIKELY(xr_isolate_check_deadline(isolate))) {
            xr_runtime_error(isolate, "execution deadline exceeded");
            frame->pc = pc - 1;
            return XR_VM_RUNTIME_ERROR;
        }

        if (xr_coro_consume_reds(coro, 1) <= 0) {
            if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
                return XR_VM_CANCELLED;
            }
            xr_coro_set_reds(coro, XR_CORO_REDUCTIONS);
            /* Embedders that did not boot the multicore runtime (e.g. the
             * minimal MCP runner, embedded REPLs, unit tests) have no
             * scheduler to resume a yielded main coroutine — returning
             * XR_VM_YIELD here would abandon execution mid-loop. In that
             * mode just refill the budget and keep running; the deadline
             * check above is the only thing that bounds tight loops, and
             * it is sufficient because it is checked on every back-edge. */
            if (XR_LIKELY(isolate->vm.runtime != NULL)) {
                frame->pc = pc - 1;
                return XR_VM_YIELD;
            }
        }
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
