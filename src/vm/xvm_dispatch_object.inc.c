/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_object.inc.c -- class / instance / field / json /
 *                               property / IC dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, vm_ctx,
 * pc, ci, frame, base, k, R, savepc, vmcase, vmbreak,
 * VM_RUNTIME_ERROR, VM_DISPATCH_COLD, VM_FRAMES, VM_FRAME_COUNT,
 * VM_INC_FRAME_COUNT, VM_BARRIER_*, VM_STACK*, VM_CURRENT_CORO,
 * checkGC, startfunc label, ...) provided by the surrounding
 * scope. CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns:
 *   Class / instance setup:
 *     - OP_CLASS_CREATE_FROM_DESCRIPTOR
 *     - OP_CLINIT_CALL
 *     - OP_ABSTRACT_ERROR
 *     - OP_SET_STORAGE_CTX / OP_TO_SHARED
 *     - OP_MAP_SETKS  (descriptor literal helper)
 *
 *   Field R/W (with field IC):
 *     - OP_GETFIELD / OP_SETFIELD / OP_GETFIELD_IC
 *
 *   Json:
 *     - OP_NEWJSON
 *     - OP_JSON_GET / SET / GETK / SETK
 *     - OP_JSON_INIT / OP_JSON_INIT_I / OP_JSON_INIT_N
 *
 *   Property R/W:
 *     - OP_GETPROP / OP_SETPROP / OP_GETSUPER (placeholder)
 *
 * Method-invocation opcodes (OP_INVOKE / OP_INVOKE_TAIL /
 * OP_SUPERINVOKE / OP_INVOKE_DIRECT / OP_INVOKE_BUILTIN) live in
 * xvm_dispatch_invoke.inc.c so this file stays under the per-file
 * size gate.
 */

/* ========================================================
** OOP Instructions
** ======================================================== */

vmcase(OP_CLASS_CREATE_FROM_DESCRIPTOR) {
    // R[A] optionally holds a runtime-resolved super class
    // (for `extends` whose parent comes from a local, upvalue
    // or imported module member). The codegen side computes
    // the parent into R[A] before this instruction; a non-
    // class value (nil) means "fall back to descriptor-
    // encoded resolution".
    int a = GETARG_A(i);
    int bx = GETARG_Bx(i);
    XrValue desc_val = k[bx];
    XrClassDescriptor *desc = (XrClassDescriptor *) XR_TO_PTR(desc_val);
    XrProto *proto = cl->proto;
    XrClass *super_override = NULL;
    XrValue super_slot = R(a);
    if (XR_IS_CLASS(super_slot)) {
        super_override = XR_TO_CLASS(super_slot);
    }
    XrClass *cls = xr_class_from_descriptor(isolate, desc, proto, cl, base, vm_ctx, super_override);
    R(a) = XR_FROM_PTR(cls);
    vmbreak;
}

vmcase(OP_CLINIT_CALL) {
    // OP_CLINIT_CALL: call static constructor
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrClass *cls = xr_value_to_class(R(a));

    // Skip if class already initialized
    if (cls->flags & XR_CLASS_INITIALIZED) {
        vmbreak;
    }

    // Get class descriptor
    XrProto *proto = cl->proto;
    XrValue desc_val = PROTO_CONSTANT(proto, b);
    XrClassDescriptor *desc = (XrClassDescriptor *) XR_TO_PTR(desc_val);

    // Skip if no static constructor
    if (desc->clinit_proto_index < 0) {
        vmbreak;
    }

    // Get and execute static constructor
    XrProto *clinit_proto = DYNARRAY_GET(&proto->protos, desc->clinit_proto_index, XrProto *);
    XrCoroutine *_clinit_coro = (XrCoroutine *) vm_ctx->current_coro;
    XrClosure *clinit_closure;
    if (_clinit_coro && _clinit_coro->coro_gc) {
        clinit_closure =
            (XrClosure *) xr_coro_gc_newobj(_clinit_coro->coro_gc, XR_TFUNCTION, sizeof(XrClosure));
    } else {
        clinit_closure = (XrClosure *) xr_gc_alloc(&isolate->gc, sizeof(XrClosure), XR_TFUNCTION);
    }
    xr_gc_header_init_type(&clinit_closure->gc, XR_TFUNCTION);
    clinit_closure->proto = clinit_proto;

    cls->flags |= XR_CLASS_INITIALIZED;
    savepc();
    int _fidx = VM_FRAME_COUNT;
    VM_INC_FRAME_COUNT;
    XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
    new_frame->closure = clinit_closure;
    new_frame->pc = PROTO_CODE_BASE(clinit_proto);
    new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);
    goto startfunc;
}

// Abstract method support
vmcase(OP_ABSTRACT_ERROR) {
    // OP_ABSTRACT_ERROR: abstract method call error
    int a = GETARG_A(i);
    XrValue method_name_val = k[a];
    const char *method_name =
        XR_IS_STRING(method_name_val) ? XR_TO_STRING(method_name_val)->data : "<unknown>";
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_CALL, "cannot call abstract method '%s'", method_name);
}

vmcase(OP_SET_STORAGE_CTX) {
    /* OP_SET_STORAGE_CTX: set storage mode context
    ** A = storage mode (0=normal, 1=shared)
    **
    ** For class instance shared support
    ** Set before constructor call, OP_INVOKE reads this context
    */
    int storage_mode = GETARG_A(i);
    isolate->current_storage_mode = (uint8_t) storage_mode;
    vmbreak;
}

vmcase(OP_TO_SHARED) {
    /* OP_TO_SHARED: convert to shared object
    ** A = destination register
    ** B = source register
    **
    ** If already shared, increment reference count
    ** Otherwise deep copy to system heap
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue src = R(b);
    R(a) = xr_to_shared(isolate, src);
    vmbreak;
}

vmcase(OP_MAP_SETKS) {
    // OP_MAP_SETKS: batch set fields
    int a = GETARG_A(i);
    int count = GETARG_B(i);
    XrInstance *inst_obj = xr_value_to_instance(R(a));
    for (int j = 0; j < count; j++) {
        XrValue val = R(a + 1 + j);
        inst_obj->fields[j] = val;
    }
    VM_BARRIER_BACK(inst_obj);
    vmbreak;
}

vmcase(OP_GETFIELD) {
    // OP_GETFIELD: instance field read by index
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int field_idx = GETARG_C(i);

    XrValue inst_val = R(b);
    XrInstance *inst_obj = xr_value_to_instance(inst_val);
    R(a) = inst_obj->fields[field_idx];
    vmbreak;
}

vmcase(OP_SETFIELD) {
    // OP_SETFIELD: instance field write by index
    int a = GETARG_A(i);
    int field_idx = GETARG_B(i);
    int c = GETARG_C(i);

    XrValue inst_val = R(a);
    XrInstance *inst_obj = xr_value_to_instance(inst_val);
    XrValue val = R(c);
    inst_obj->fields[field_idx] = val;
    VM_BARRIER_VAL(inst_obj, val);
    vmbreak;
}

vmcase(OP_GETFIELD_IC) {
    /* R[A] = R[B].K[C] - inline cache field access
    ** Uses field_name_idx as IC key (constant per call site). */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int field_name_idx = GETARG_C(i);

    XrValue inst_val = R(b);
    if (!xr_value_is_instance(inst_val)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "field access requires instance object");
    }

    XrInstance *inst_obj = xr_value_to_instance(inst_val);
    XrClass *cls = inst_obj->klass;

    // Lazily ensure the per-ctx IC table for this proto.
    XrICFieldTable *ic_table = xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
    if (!ic_table) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "OP_GETFIELD_IC: failed to allocate IC table");
    }

    size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
    XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
    XrICField *cache = xr_ic_field_table_get(ic_table, (int) cache_index);
    if (cache) {
        XR_VM_IC_FIELD_BIND(cache, (int) cache_index);
    }

    int field_idx = -1;

    // Fast path: monomorphic IC hit
    if (cache && xr_ic_field_lookup_mono(cache, cls, field_name_idx, &field_idx)) {
        R(a) = inst_obj->fields[field_idx];
        vmbreak;
    }

    // Fast path: polymorphic IC hit
    if (cache && xr_ic_field_lookup_poly(cache, cls, field_name_idx, &field_idx)) {
        R(a) = inst_obj->fields[field_idx];
        vmbreak;
    }

    // Slow path: string lookup
    XrValue field_name_val = K(field_name_idx);
    if (!XR_IS_STRING(field_name_val)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "field name must be a string");
    }
    XrString *field_name = XR_TO_STRING(field_name_val);
    field_idx = xr_class_lookup_field_by_name(isolate, cls, field_name->data);

    if (field_idx < 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "field '%s' not found", field_name->data);
    }

    R(a) = inst_obj->fields[field_idx];

    // Update IC cache
    if (cache) {
        xr_ic_field_update(cache, cls, field_idx, field_name_idx);
    }
    vmbreak;
}

// Json dynamic object instructions (V2 zero-copy design)

vmcase(OP_NEWJSON) {
    /* OP_NEWJSON: create Json object
    ** A = destination register
    ** B = Shape constant index
    ** C = storage mode (0=normal, 1=shared)
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int storage_mode = GETARG_C(i);
    XrValue cls_val = k[b];
    // Class stored as integer pointer (not GC managed; lives with isolate)
    XrClass *cls = (XrClass *) (intptr_t) XR_TO_INT(cls_val);
    XrJson *json;
    if (storage_mode != 0 && isolate->sys_heap) {
        // shared: allocate on system heap
        size_t size = xr_json_size(cls);
        json = (XrJson *) xr_sysheap_alloc_shared(isolate->sys_heap, size, XR_TJSON);
        if (json) {
            xr_json_init_inplace(json, cls);
            XR_GC_SET_STORAGE(&json->gc, storage_mode);
            if (storage_mode == XR_GC_STORAGE_SHARED) {
                xr_shared_set_refc(&json->gc, 1);
            }
        }
    } else {
        // normal: allocate on coroutine heap
        json = xr_json_new_with_class(VM_CURRENT_CORO, cls);
    }

    R(a) = xr_json_value(json);
    if (storage_mode == 0)
        checkGC(base + a + 1);
    vmbreak;
}

vmcase(OP_JSON_GET) {
    // OP_JSON_GET: read field by index
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrJson *json = xr_value_to_json(R(b));
    R(a) = xr_instance_get_dynamic_field(json, (uint16_t) c);
    vmbreak;
}

vmcase(OP_JSON_SET) {
    // OP_JSON_SET: write field by index
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrJson *json = xr_value_to_json(R(a));
    XrValue val = R(c);
    xr_instance_set_dynamic_field(isolate, json, (uint16_t) b, val);
    VM_BARRIER_VAL(json, val);
    vmbreak;
}

vmcase(OP_JSON_GETK) {
    // OP_JSON_GETK: read field by Symbol
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);  // Local symbol index
    XrJson *json = xr_value_to_json(R(b));
    R(a) = xr_json_get(isolate, json, (SymbolId) PROTO_SYMBOL(cl->proto, c));
    vmbreak;
}

vmcase(OP_JSON_SETK) {
    // OP_JSON_SETK: write field by Symbol (supports zero-copy conversion)
    int a = GETARG_A(i);
    int b = GETARG_B(i);  // Local symbol index
    int c = GETARG_C(i);
    XrJson *json = xr_value_to_json(R(a));
    XrValue val = R(c);
    if (!xr_json_set(isolate, json, (SymbolId) PROTO_SYMBOL(cl->proto, b), val)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "cannot add property to sealed Json object");
    }
    VM_BARRIER_VAL(json, val);
    vmbreak;
}

vmcase(OP_JSON_INIT) {
    // OP_JSON_INIT: direct index write during initialization
    int a = GETARG_A(i);
    int b = GETARG_B(i);  // Field index
    int c = GETARG_C(i);
    XrJson *json = xr_value_to_json(R(a));
    XrValue val = R(c);
    xr_instance_set_dynamic_field(isolate, json, (uint16_t) b, val);
    VM_BARRIER_VAL(json, val);
    vmbreak;
}

vmcase(OP_JSON_INIT_I) {
    // OP_JSON_INIT_I: init field with immediate integer
    int a = GETARG_A(i);
    int b = GETARG_B(i);   // Field index
    int c = GETARG_sC(i);  // Signed immediate value
    XrJson *json = xr_value_to_json(R(a));
    xr_instance_set_dynamic_field(isolate, json, (uint16_t) b, xr_int(c));
    vmbreak;
}

vmcase(OP_JSON_INIT_N) {
    // OP_JSON_INIT_N: init field with null
    int a = GETARG_A(i);
    int b = GETARG_B(i);  // Field index
    XrJson *json = xr_value_to_json(R(a));
    xr_instance_set_dynamic_field(isolate, json, (uint16_t) b, xr_null());
    vmbreak;
}

vmcase(OP_JSON_DECODE) {
    /* OP_JSON_DECODE: typed JSON deserialization
    ** A = destination register (result: sealed Json or null)
    ** B = data register (string to parse)
    ** C = Class constant index (pre-built from compile-time field names)
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    XrValue data = R(b);
    XrValue cls_val = k[c];
    XrClass *cls = (XrClass *) (intptr_t) XR_TO_INT(cls_val);
    XR_DCHECK(cls != NULL, "OP_JSON_DECODE: null class");

    /* Accept string (parse first) or Json object (validate directly) */
    XrJson *src = NULL;
    if (XR_IS_STRING(data)) {
        XrString *str = XR_TO_STRING(data);
        XrValue parsed = xr_json_parse_from_cstr(isolate, str->data, str->length);
        if (XR_IS_NULL(parsed) || !xr_value_is_json(parsed)) {
            R(a) = xr_null();
            vmbreak;
        }
        src = xr_value_to_json(parsed);
    } else if (xr_value_is_json(data)) {
        src = xr_value_to_json(data);
    } else {
        R(a) = xr_null();
        vmbreak;
    }
    uint16_t field_count = cls->field_count;

    XrJson *result = xr_json_new_with_class(VM_CURRENT_CORO, cls);
    if (!result) {
        R(a) = xr_null();
        vmbreak;
    }

    bool valid = true;
    for (uint16_t fi = 0; fi < field_count; fi++) {
        const char *fname = cls->fields[fi].name;
        int sym = cls->fields[fi].symbol;
        if (!fname) {
            valid = false;
            break;
        }
        XrValue field_val = xr_json_get_by_key(isolate, src, fname);
        if (XR_IS_NULL(field_val) && !xr_json_has_field(isolate, src, sym)) {
            valid = false;
            break;
        }
        xr_instance_set_dynamic_field(isolate, result, fi, field_val);
    }

    R(a) = valid ? xr_json_value(result) : xr_null();
    checkGC(base + a + 1);
    vmbreak;
}

/* ========================================================
** Property access instructions
** ======================================================== */

vmcase(OP_GETPROP) {
    // R[A] = R[B].Symbol[C] - Get property/method
    // Use Symbol direct dispatch, 10x performance improvement
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    XrValue obj = R(b);
    int prop_symbol = PROTO_SYMBOL(cl->proto, c);  // Dereference local index → global symbol

    // Fixed array .length
    if (XR_IS_ARRAY_REF(obj) && prop_symbol == SYMBOL_LENGTH) {
        R(a) = XR_FROM_INT((int64_t) XR_ARRAY_REF_ELEM_COUNT(obj));
        vmbreak;
    }

    // Stack-allocated struct field access
    if (XR_IS_STRUCT_REF(obj)) {
        uint8_t *sptr = (uint8_t *) xr_to_struct_ptr(obj);
        XrClass *scls = *(XrClass **) sptr;
        int fidx = xr_class_lookup_field(scls, prop_symbol);
        if (fidx >= 0 && scls->struct_layout && fidx < scls->struct_layout->field_count) {
            XrStructFieldLayout *sf = &scls->struct_layout->fields[fidx];
            uint8_t *fp = sptr + 8 + sf->offset;
            switch (sf->native_type) {
                case XR_NATIVE_I64:
                    R(a) = XR_FROM_INT(*(int64_t *) fp);
                    break;
                case XR_NATIVE_F64:
                    R(a) = XR_FROM_FLOAT(*(double *) fp);
                    break;
                case XR_NATIVE_BOOL:
                    R(a).descriptor = 0;
                    R(a).i = *(uint8_t *) fp ? 1 : 0;
                    R(a).tag = XR_TAG_BOOL;
                    break;
                case XR_NATIVE_I32:
                    R(a) = XR_FROM_INT((int64_t) *(int32_t *) fp);
                    break;
                case XR_NATIVE_F32:
                    R(a) = XR_FROM_FLOAT((double) *(float *) fp);
                    break;
                case XR_NATIVE_STRING: {
                    XrString *s = *(XrString **) fp;
                    R(a) = s ? XR_FROM_STR(s) : xr_null();
                    break;
                }
                default:
                    R(a) = xr_null();
                    break;
            }
            vmbreak;
        }
        // Field not found: might be a method, fall through to cold path
    }

    // Fast path: Instance is the most common target, skip if-else chain
    if (xr_value_is_instance(obj))
        goto getprop_instance;

    // Json fast path with Shape IC
    // Json (XR_TJSON) values are dynamic-layout instances; route them
    // through the unified instance path below — same dispatch as user
    // classes, no separate shape IC needed.
    if (xr_value_is_json(obj))
        goto getprop_instance;

    // Cold path: all non-instance, non-json type dispatch
    {
        savepc();
        int _cr =
            vm_getprop_type_dispatch(isolate, vm_ctx, obj, prop_symbol, base, a, b, frame, pc);
        if (_cr == VM_COLD_BREAK)
            vmbreak;
        if (_cr == VM_COLD_STARTFUNC)
            goto startfunc;
        if (_cr == VM_COLD_ERROR) {
            if (!xr_vm_is_catch_reachable(isolate))
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
        // VM_COLD_CONTINUE: fall through to instance path
    }

getprop_instance:;
    // XrJson (XR_TJSON) shares XrInstance layout, but xr_value_to_instance
    // only matches XR_TINSTANCE heap_type. Use direct cast for both.
    XrInstance *inst = (XrInstance *) XR_TO_PTR(obj);

    // Dynamic-layout fast path: hidden-class instance, lookup may miss
    // (returns null), no getter dispatch, no method fallback.
    if (inst->klass->flags & XR_CLASS_DYNAMIC_LAYOUT) {
        int field_index_d = xr_class_lookup_field(inst->klass, prop_symbol);
        if (field_index_d >= 0) {
            R(a) = xr_instance_get_dynamic_field(inst, (uint16_t) field_index_d);
        } else {
            R(a) = xr_null();
        }
        vmbreak;
    }

    // Cold path: getter method lookup
    {
        int _cr =
            vm_getprop_instance_getter(isolate, vm_ctx, inst, obj, prop_symbol, base, a, frame, pc);
        if (_cr == VM_COLD_BREAK)
            vmbreak;
        if (_cr == VM_COLD_STARTFUNC)
            goto startfunc;
        if (_cr == VM_COLD_ERROR) {
            if (!xr_vm_is_catch_reachable(isolate))
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
        // VM_COLD_CONTINUE: no getter, fall through to field access
    }

    // No getter: access as regular field

    // Field access Inline Cache optimization (per-ctx)
    XrICFieldTable *ic_table_g = xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
    if (!ic_table_g) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "OP_GETPROP: failed to allocate IC table");
    }

    // Get IC for current instruction
    size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
    XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
    XrICField *cache = xr_ic_field_table_get(ic_table_g, cache_index);
    if (cache) {
        XR_VM_IC_FIELD_BIND(cache, (int) cache_index);
    }

    XrClass *inst_class = inst->klass;
    int field_index = -1;

    // Fast path 1: Monomorphic IC hit (verify symbol match)
    if (cache && xr_ic_field_lookup_mono(cache, inst_class, prop_symbol, &field_index)) {
        // Monomorphic hit: direct field access!
        R(a) = inst->fields[field_index];
        vmbreak;
    }

    // Fast path 2: Polymorphic IC hit (verify symbol match)
    if (cache && xr_ic_field_lookup_poly(cache, inst_class, prop_symbol, &field_index)) {
        // Polymorphic hit: direct field access!
        R(a) = inst->fields[field_index];
        vmbreak;
    }

    // Slow path: Symbol lookup for field index
    field_index = xr_class_lookup_field(inst_class, prop_symbol);

    if (field_index >= 0) {
        // Get instance field count for bounds check
        uint32_t inst_field_count = xr_class_instance_field_count(inst_class);

        // Bounds check: prevent out-of-bounds access
        if ((uint32_t) field_index >= inst_field_count) {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "field index out of bounds: %d >= %d",
                             field_index, inst_field_count);
        }

        // Field exists: access and update IC
        R(a) = inst->fields[field_index];

        // Update IC cache (pass symbol)
        if (cache) {
            xr_ic_field_update(cache, inst_class, field_index, prop_symbol);
        }
    } else {
        // Field not found: try method lookup for method reference
        XrMethod *_m = xr_class_lookup_method(inst_class, prop_symbol);
        if (_m && _m->as.closure) {
            R(a) = XR_FROM_PTR(_m->as.closure);
        } else {
            const char *class_name = inst_class->name ? inst_class->name : "?";
            XrSymbolTable *_st2 = (XrSymbolTable *) isolate->symbol_table;
            const char *_pn = xr_symbol_get_name_in_table(_st2, prop_symbol);
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "field '%s' not declared in class '%s'",
                             _pn ? _pn : "?", class_name);
        }
    }
    vmbreak;
}

vmcase(OP_SETPROP) {
    /* === OP_SETPROP R[A].symbol(B) = R[C] ===
    ** Use Symbol direct dispatch, 10x performance improvement */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    XrValue obj = R(a);
    int prop_symbol = PROTO_SYMBOL(cl->proto, b);  // Dereference local index → global symbol
    XrValue value = R(c);

    // Json (XR_TJSON) values are dynamic-layout instances; the cold-path
    // type dispatch returns VM_COLD_CONTINUE for them and the regular
    // instance path below (with its DYNAMIC_LAYOUT branch) handles all
    // semantics — including transitions on field add and sealed errors.

    // Cold path: non-instance type dispatch (Map/Module/Class/null)
    {
        savepc();
        int _cr =
            vm_setprop_type_dispatch(isolate, vm_ctx, obj, prop_symbol, value, base, a, frame, pc);
        if (_cr == VM_COLD_BREAK)
            vmbreak;
        if (_cr == VM_COLD_STARTFUNC)
            goto startfunc;
        if (_cr == VM_COLD_ERROR) {
            if (!xr_vm_is_catch_reachable(isolate))
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
        // VM_COLD_CONTINUE: fall through to instance path
    }

    // XrJson (XR_TJSON) shares XrInstance layout — direct cast needed
    XrInstance *inst_s = (XrInstance *) XR_TO_PTR(obj);

    // Dynamic-layout fast path: hidden-class instance, missing field creates
    // a class transition. Shared objects cannot create new transitions.
    if (inst_s->klass->flags & XR_CLASS_DYNAMIC_LAYOUT) {
        int field_index_d = xr_class_lookup_field(inst_s->klass, prop_symbol);
        if (field_index_d < 0) {
            // Adding a new field: forbid on shared objects
            if (XR_GC_GET_STORAGE(&inst_s->gc) == XR_GC_STORAGE_SHARED) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY,
                                 "cannot add field to shared dynamic object");
            }
            XrSymbolTable *_st_sd = (XrSymbolTable *) isolate->symbol_table;
            const char *fname = xr_symbol_get_name_in_table(_st_sd, prop_symbol);
            // Sealed dynamic objects reject new fields with a clear error
            if (inst_s->klass->flags & XR_CLASS_DYNAMIC_SEALED) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "cannot add field '%s' to sealed object",
                                 fname ? fname : "?");
            }
            XrClass *next = xr_class_transition_get_or_create(isolate, inst_s->klass, prop_symbol,
                                                              fname ? fname : "?");
            if (!next) {
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                 "OP_SETPROP: dynamic transition allocation failed");
            }
            inst_s->klass = next;
            field_index_d = xr_class_lookup_field(next, prop_symbol);
            XR_DCHECK(field_index_d >= 0, "transition: new field not registered");
        }
        if (!xr_instance_set_dynamic_field(isolate, inst_s, (uint16_t) field_index_d, value)) {
            VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "OP_SETPROP: dynamic overflow alloc failed");
        }
        VM_BARRIER_VAL(inst_s, value);
        vmbreak;
    }

    // Cold path: setter method lookup
    {
        int _cr = vm_setprop_instance_setter(isolate, vm_ctx, inst_s, obj, prop_symbol, value, base,
                                             c, frame, pc);
        if (_cr == VM_COLD_STARTFUNC)
            goto startfunc;
        if (_cr == VM_COLD_ERROR) {
            if (!xr_vm_is_catch_reachable(isolate))
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
        // VM_COLD_CONTINUE: no setter, fall through to field access
    }

    // No setter: assign as regular field
    XrClass *inst_class = inst_s->klass;

    // Field access Inline Cache optimization (per-ctx)
    XrICFieldTable *ic_table_s = xr_vm_ctx_ensure_ic_fields(vm_ctx, frame->closure->proto);
    if (!ic_table_s) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "OP_SETPROP: failed to allocate IC table");
    }

    size_t cache_index = pc - PROTO_CODE_BASE(frame->closure->proto) - 1;
    XR_VM_IC_ASSERT_INDEX(cache_index, frame->closure->proto);
    XrICField *cache = xr_ic_field_table_get(ic_table_s, cache_index);
    if (cache) {
        XR_VM_IC_FIELD_BIND(cache, (int) cache_index);
    }

    int field_index = -1;

    // Fast path 1: Monomorphic IC hit (verify symbol match)
    if (cache && xr_ic_field_lookup_mono(cache, inst_class, prop_symbol, &field_index)) {
        inst_s->fields[field_index] = value;
        VM_BARRIER_VAL(inst_s, value);
        vmbreak;
    }

    // Fast path 2: Polymorphic IC hit (verify symbol match)
    if (cache && xr_ic_field_lookup_poly(cache, inst_class, prop_symbol, &field_index)) {
        inst_s->fields[field_index] = value;
        VM_BARRIER_VAL(inst_s, value);
        vmbreak;
    }

    // Slow path: Symbol lookup for field index
    field_index = xr_class_lookup_field(inst_class, prop_symbol);

    if (field_index >= 0) {
        inst_s->fields[field_index] = value;
        VM_BARRIER_VAL(inst_s, value);

        // Update IC cache (pass symbol)
        if (cache) {
            xr_ic_field_update(cache, inst_class, field_index, prop_symbol);
        }
    } else {
        // Field not found: generate error message
        XrSymbolTable *_st2 = (XrSymbolTable *) isolate->symbol_table;
        const char *_pn = xr_symbol_get_name_in_table(_st2, prop_symbol);
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "field '%s' not declared in class '%s'",
                         _pn ? _pn : "?", inst_class->name);
    }
    vmbreak;
}

vmcase(OP_GETSUPER) {
    // GETSUPER: Get superclass method (not implemented)
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_METHOD, "OP_GETSUPER not yet implemented");
}
