/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_closure.inc.c — closure / shared / cell opcode dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, cl,
 * R, vmcase, vmbreak, ...) provided by the surrounding scope.
 * CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns:
 *   Globals / shared:
 *     OP_GETBUILTIN          — read-only builtin global O(1)
 *     OP_GETSHARED           — read shared variable O(1)
 *     OP_SETSHARED           — write shared variable, decref old
 *
 *   Closures:
 *     OP_CLOSURE             — build closure, populate upvals[]
 *     OP_UPVAL_GET           — read upvalue from closure
 *
 *   Cells (mutable captures):
 *     OP_CELL_NEW            — alloc cell, optional initializer
 *     OP_CELL_GET            — read cell value
 *     OP_CELL_SET            — write cell value
 */

vmcase(OP_GETBUILTIN) {
    // OP_GETBUILTIN: read-only builtin global O(1)
    int a = GETARG_A(i);
    int builtin_index = GETARG_Bx(i);

    if (builtin_index < isolate->vm.builtin_count) {
        R(a) = isolate->vm.builtins[builtin_index];
    } else {
        R(a) = xr_null();
    }
    vmbreak;
}

vmcase(OP_GETSHARED) {
    // OP_GETSHARED: get shared variable O(1)
    int a = GETARG_A(i);
    int shared_index = GETARG_Bx(i) + cl->proto->shared_offset;
    R(a) = xr_shared_array_get(&isolate->vm.shared, shared_index);

    vmbreak;
}

vmcase(OP_SETSHARED) {
    // OP_SETSHARED: set shared variable O(1)
    int a = GETARG_A(i);
    int shared_index = GETARG_Bx(i) + cl->proto->shared_offset;

    // Decref old value if it's a shared object
    XrValue old_val = xr_shared_array_get(&isolate->vm.shared, shared_index);
    if (XR_IS_PTR(old_val)) {
        XrGCHeader *old_obj = XR_VALUE_GCPTR(old_val);
        if (old_obj && XR_GC_IS_SHARED(old_obj)) {
            int new_refc = xr_shared_decref(old_obj);
            if (new_refc == 0) {
                xr_shared_destroy(old_obj);
            }
        }
    }

    xr_shared_array_set(&isolate->vm.shared, shared_index, R(a));
    vmbreak;
}

vmcase(OP_GETGLOBAL) {
    /* OP_GETGLOBAL: name-keyed top-level read.
     * Bx is a constant-pool index pointing to an interned XrString
     * with the binding's source-level name; the runtime looks up
     * isolate->vm.globals by that name.  Unbound names read as null
     * (matches the analyzer's contract that unresolved top-level
     * references are diagnosed at compile time, so a runtime miss is
     * either a stale closure or a deliberate optional probe). */
    int a = GETARG_A(i);
    int kx = GETARG_Bx(i);
    XrValue name_val = PROTO_CONST_FAST(cl->proto, kx);
    if (XR_IS_STRING(name_val) && isolate->vm.globals) {
        R(a) = xr_global_dict_get(isolate->vm.globals, XR_TO_STRING(name_val));
    } else {
        R(a) = xr_null();
    }
    vmbreak;
}

vmcase(OP_SETGLOBAL) {
    /* OP_SETGLOBAL: name-keyed top-level write.  Companion of
     * OP_GETGLOBAL.  Insert-or-overwrite under the interned name in
     * Bx.  Unlike OP_SETSHARED there is no per-slot decref dance:
     * the old XrValue lives in a GC-tracked map node and is reclaimed
     * on the next sweep when no longer referenced. */
    int a = GETARG_A(i);
    int kx = GETARG_Bx(i);
    XrValue name_val = PROTO_CONST_FAST(cl->proto, kx);
    XR_DCHECK(XR_IS_STRING(name_val), "OP_SETGLOBAL: K[Bx] must be a string");
    XR_DCHECK(isolate->vm.globals != NULL, "OP_SETGLOBAL: globals dict not initialized");
    xr_global_dict_set(isolate->vm.globals, XR_TO_STRING(name_val), R(a));
    vmbreak;
}

vmcase(OP_CLOSURE) {
    /* OP_CLOSURE: create closure, populate upvals[] from proto descriptors.
    ** All captures are BY_VALUE: const → raw value, let → cell ref.
    ** Sources: SRC_REG (from register) or SRC_UPVAL (from enclosing upvals[]). */
    int a = GETARG_A(i);
    int bx = GETARG_Bx(i);
    XrProto *proto = PROTO_PROTO(cl->proto, bx);
    XrClosure *closure = xr_closure_new(isolate, proto, (XrCoroutine *) vm_ctx->current_coro);
    int nuv = DYNARRAY_COUNT(&proto->upvalues);
    for (int j = 0; j < nuv; j++) {
        UpvalInfo *uv = &DYNARRAY_GET(&proto->upvalues, j, UpvalInfo);
        if (uv->source == UPVAL_SRC_REG) {
            closure->upvals[j] = R(uv->index);
        } else if (uv->source == UPVAL_SRC_UPVAL) {
            int idx = uv->index;
            closure->upvals[j] = (idx < cl->upval_count) ? cl->upvals[idx] : xr_null();
        } else {
            closure->upvals[j] = xr_null();  // fallback
        }
    }
    R(a) = xr_value_from_closure(closure);
    checkGC(base + a + 1);
    vmbreak;
}

vmcase(OP_UPVAL_GET) {
    /* OP_UPVAL_GET: R[A] = cl->upvals[B]
    ** Flat upvalue read from current closure's upvals array.
    ** Used for BY_VALUE captured variables (const, loop vars). */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    R(a) = (b < cl->upval_count) ? cl->upvals[b] : xr_null();
    vmbreak;
}

vmcase(OP_CELL_NEW) {
    /* OP_CELL_NEW: R[A] = new_cell(R[A])
    ** Wraps current register value in a heap-allocated XrCell (32B).
    ** Used for captured mutable let vars. */
    int a = GETARG_A(i);
    XrCell *cell = xr_cell_new(isolate, (XrCoroutine *) vm_ctx->current_coro);
    cell->value = R(a);
    R(a) = xr_make_ptr_val(cell);
    checkGC(base + a + 1);
    vmbreak;
}

vmcase(OP_CELL_GET) {
    /* OP_CELL_GET: R[A] = cell_deref(R[B])
    ** Read value from XrCell. */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrCell *cell = (XrCell *) R(b).ptr;
    R(a) = cell ? cell->value : xr_null();
    vmbreak;
}

vmcase(OP_CELL_SET) {
    /* OP_CELL_SET: cell_store(R[A], R[B])
    ** Write value into XrCell. */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrCell *cell = (XrCell *) R(a).ptr;
    if (cell)
        cell->value = R(b);
    vmbreak;
}
