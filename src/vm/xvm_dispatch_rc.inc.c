/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_rc.inc.c — reference-counting opcode dispatch (dup/drop/move)
 *
 * NOT a standalone translation unit. Included from inside the dispatch
 * switch in xvm.c; relies on locals (i, R, vmcase, vmbreak, ...) provided
 * by the surrounding scope. CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns: OP_DUP, OP_DROP — the compile-time RC primitives inserted by
 * xi_arc (consuming xi_own ownership analysis). OP_MOVE (ownership
 * transfer) reuses the pre-existing register-move opcode dispatched in
 * xvm.c.
 *
 * OP_DROP releases an owning reference and frees the object on the last
 * reference.
 */

vmcase(OP_DUP) {
    /* dup(R[A]): acquire a new owning reference via the unified RC retain
     * primitive (no-op for scalars / region / managed / dead objects). */
    int a = GETARG_A(i);
    XrValue v = R(a);
    if (XR_IS_PTR(v))
        xr_rc_retain((XrObjHeader *) XR_VALUE_GCPTR(v));
    vmbreak;
}

vmcase(OP_DROP) {
    /* drop(R[A]): release an owning reference via the unified RC release
     * primitive. On the last reference the object is destroyed (and unlinked
     * from cycle_roots); when RC stays > 0 a cycle-candidate is registered as
     * a potential cycle root. Region/managed objects are a no-op. This is the
     * SAME primitive the container runtime uses, so cycle bookkeeping no
     * longer diverges between the compiler-inserted drop and the C runtime. */
    int a = GETARG_A(i);
    XrValue v = R(a);
    if (XR_IS_PTR(v)) {
        XrCoroutine *_co = (XrCoroutine *) VM_CURRENT_CORO;
        xr_rc_release(_co ? _co->coro_gc : NULL, (XrObjHeader *) XR_VALUE_GCPTR(v));
    }
    vmbreak;
}

/* OP_MOVE is dispatched in xvm.c (pre-existing register-move opcode);
 * xi_emit_move reuses it for ownership transfer. No case needed here. */
