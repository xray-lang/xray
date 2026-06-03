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
    /* dup(R[A]): acquire a new owning reference. No-op for scalars and
     * region-allocated objects (handled inside xr_obj_dup). */
    int a = GETARG_A(i);
    XrValue v = R(a);
    if (XR_IS_PTR(v)) {
        XrObjHeader *o = (XrObjHeader *) XR_VALUE_GCPTR(v);
        xr_obj_dup(o);
    }
    vmbreak;
}

vmcase(OP_DROP) {
    /* drop(R[A]): release an owning reference. On the last reference the
     * object's destructor runs and its memory returns to the RC freelist
     * (shared objects route to xr_shared_destroy). Region objects are a
     * no-op (handled in xr_obj_drop_is_last). */
    int a = GETARG_A(i);
    XrValue v = R(a);
    if (XR_IS_PTR(v)) {
        XrObjHeader *o = (XrObjHeader *) XR_VALUE_GCPTR(v);
        if (xr_obj_drop_is_last(o)) {
            XrCoroutine *_co = (XrCoroutine *) VM_CURRENT_CORO;
            xr_coro_gc_rc_destroy(_co ? _co->coro_gc : NULL, o);
        }
    }
    vmbreak;
}

/* OP_MOVE is dispatched in xvm.c (pre-existing register-move opcode);
 * xi_emit_move reuses it for ownership transfer. No case needed here. */
