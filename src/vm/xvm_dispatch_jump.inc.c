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
 * frame, ci, base, vm_worker, R, vmcase, vmbreak, savepc, xr_vm_is_truthy,
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
    ** Also serves as a scheduling/cancellation safepoint. */
    if (offset < 0) {
        /* Host-supplied wall-clock deadline. Checked here (back-edge) so a
        ** tight infinite loop cannot wedge either a coroutine or a taskless
        ** direct root. */
        if (XR_UNLIKELY(xr_isolate_check_deadline(isolate))) {
            xr_runtime_error(isolate, "execution deadline exceeded");
            frame->pc = pc - 1;
            return XR_VM_RUNTIME_ERROR;
        }

        if (vm_ctx && vm_ctx->current_coro) {
            XrCoroutine *coro = (XrCoroutine *) vm_ctx->current_coro;
            xr_worker_bump_heartbeat(vm_worker);

            /* Legacy tracing-GC hook; currently a no-op. Scheduling and
            ** cancellation are handled by the reduction check below. */
            VM_GC_SAFEPOINT();

            if (xr_coro_consume_reds(coro, 1) <= 0) {
                if (xr_coro_flags_has(coro, XR_CORO_FLG_CANCEL_REQUESTED)) {
                    return XR_VM_CANCELLED;
                }
                xr_coro_set_reds(coro, XR_CORO_REDUCTIONS);
                if (XR_LIKELY(isolate->vm.scheduler != NULL)) {
                    frame->pc = pc - 1;
                    return XR_VM_YIELD;
                }
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
