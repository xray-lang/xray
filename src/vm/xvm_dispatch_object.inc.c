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
 * VM_RUNTIME_ERROR, VM_DISPATCH, VM_FRAMES, VM_FRAME_COUNT,
 * VM_INC_FRAME_COUNT, VM_STACK*, VM_CURRENT_CORO,
 * startfunc label, ...) provided by the surrounding
 * scope. CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns:
 *   Class / instance setup:
 *     - OP_CLASS_CREATE_FROM_DESCRIPTOR
 *     - OP_CLINIT_CALL
 *     - OP_SET_STORAGE_CTX
 *     - OP_MAP_SETKS  (descriptor literal helper)
 *
 *   Field R/W (with field IC):
 *     - OP_GETFIELD / OP_SETFIELD / OP_GETFIELD_IC
 *
 *   Json:
 *     - OP_NEWOBJECT
 *     - OP_OBJECT_GET / SET / GETK / SETK
 *     - OP_OBJECT_GETK / SETK (verified structural descriptor dispatch)
 *     - OP_OBJECT_INIT / OP_OBJECT_INIT_I / OP_OBJECT_INIT_N
 *
 *   Property R/W:
 *     - OP_GETPROP / OP_SETPROP / OP_GETSUPER (placeholder)
 *
 * Method-invocation opcodes (OP_INVOKE / OP_INVOKE_TAIL /
 * OP_SUPERINVOKE / OP_INVOKE_DIRECT) live in
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
    XrClosure *clinit_closure = xr_closure_new(isolate, clinit_proto, _clinit_coro);
    if (!clinit_closure) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "OP_CLINIT_CALL: failed to allocate closure");
    }

    cls->flags |= XR_CLASS_INITIALIZED;

    /* Frame depth limit and stack capacity must both be checked before
     * pushing a new frame. Without VM_STACK_CHECK, when stack and frames
     * share a combined slab allocation the callee's register file can
     * silently overflow into frames[] and corrupt frame.closure. */
    if (XR_UNLIKELY(VM_FRAME_COUNT >= XR_FRAMES_MAX)) {
        VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW, "stack overflow: recursion exceeds %d levels",
                         XR_FRAMES_MAX);
    }
    VM_STACK_CHECK(a + 1 + clinit_proto->maxstacksize);

    savepc();
    int _fidx = VM_FRAME_COUNT;
    VM_INC_FRAME_COUNT;
    XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
    new_frame->closure = clinit_closure;
    new_frame->pc = PROTO_CODE_BASE(clinit_proto);
    new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);
    goto startfunc;
}

vmcase(OP_SET_STORAGE_CTX) {
    /* OP_SET_STORAGE_CTX: set storage mode context
    ** A = storage mode (0=normal, 1=shared, 2=owned)
    **
    ** For class instance shared support
    ** Set before constructor call, OP_INVOKE reads this context
    */
    int storage_mode = vm_resolve_allocation_storage_mode(base, (uint8_t) GETARG_A(i));
    atomic_store_explicit(&isolate->current_storage_mode, (uint8_t) storage_mode,
                          memory_order_relaxed);
    vmbreak;
}

vmcase(OP_MAP_SETKS) {
    // OP_MAP_SETKS: batch set fields
    int a = GETARG_A(i);
    int count = GETARG_B(i);
    XrObjectInstance *inst_obj = xr_value_to_instance(R(a));
    for (int j = 0; j < count; j++) {
        XrValue val = R(a + 1 + j);
        if (inst_obj->klass && inst_obj->klass->struct_layout) {
            if (!xr_instance_struct_set_field(isolate, inst_obj, j, val)) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid value for struct field write");
            }
        } else if (XR_IS_ARRAY_REF(val)) {
            if (!vm_store_persistent_array_ref(isolate, &inst_obj->fields[j], val)) {
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                 "failed to materialize fixed array instance field");
            }
        } else {
            inst_obj->fields[j] = val;
        }
    }
    vmbreak;
}

vmcase(OP_GETFIELD) {
    // OP_GETFIELD: instance field read by index
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int field_idx = GETARG_C(i);

    XrValue inst_val = R(b);
    XrEnumAggregateValue *enum_agg = xr_value_to_enum_aggregate(inst_val);
    if (enum_agg) {
        R(a) = xr_enum_aggregate_field_get(enum_agg, field_idx);
        vmbreak;
    }
    XrObjectInstance *inst_obj = xr_value_to_instance(inst_val);
    if (xr_weak_instance_field_load(inst_obj, field_idx, &R(a)))
        vmbreak;
    if (inst_obj->klass && inst_obj->klass->struct_layout) {
        if (!xr_instance_struct_get_field(isolate, inst_obj, field_idx, &R(a))) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct field read");
        }
    } else {
        R(a) = inst_obj->fields[field_idx];
    }
    vmbreak;
}

vmcase(OP_SETFIELD) {
    // OP_SETFIELD: instance field write by index
    int a = GETARG_A(i);
    int field_idx = GETARG_B(i);
    int c = GETARG_C(i);

    XrValue inst_val = R(a);
    XrObjectInstance *inst_obj = xr_value_to_instance(inst_val);
    XrValue val = R(c);
    if (xr_weak_instance_field_store(vm_weak_heap(vm_ctx), inst_obj, field_idx, val))
        vmbreak;
    if (inst_obj->klass && inst_obj->klass->struct_layout) {
        if (!xr_instance_struct_set_field(isolate, inst_obj, field_idx, val)) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid value for struct field write");
        }
    } else if (XR_IS_ARRAY_REF(val)) {
        if (!vm_store_persistent_array_ref(isolate, &inst_obj->fields[field_idx], val)) {
            VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                             "failed to materialize fixed array instance field");
        }
    } else {
        inst_obj->fields[field_idx] = val;
    }
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

    XrObjectInstance *inst_obj = xr_value_to_instance(inst_val);
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
        if (xr_weak_instance_field_load(inst_obj, field_idx, &R(a)))
            vmbreak;
        if (cls->struct_layout) {
            if (!xr_instance_struct_get_field(isolate, inst_obj, field_idx, &R(a))) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct field read");
            }
        } else {
            R(a) = inst_obj->fields[field_idx];
        }
        vmbreak;
    }

    // Fast path: polymorphic IC hit
    if (cache && xr_ic_field_lookup_poly(cache, cls, field_name_idx, &field_idx)) {
        if (xr_weak_instance_field_load(inst_obj, field_idx, &R(a)))
            vmbreak;
        if (cls->struct_layout) {
            if (!xr_instance_struct_get_field(isolate, inst_obj, field_idx, &R(a))) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct field read");
            }
        } else {
            R(a) = inst_obj->fields[field_idx];
        }
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

    /* A weak slot is never inline-cached: the cache records an index and the
     * fast paths read that slot raw, which would hand back the handle. */
    if (xr_weak_instance_field_load(inst_obj, field_idx, &R(a)))
        vmbreak;
    if (cls->struct_layout) {
        if (!xr_instance_struct_get_field(isolate, inst_obj, field_idx, &R(a))) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct field read");
        }
    } else {
        R(a) = inst_obj->fields[field_idx];
    }

    // Update IC cache
    if (cache) {
        xr_ic_field_update(cache, cls, field_idx, field_name_idx);
    }
    vmbreak;
}

// Exact structural object instructions.

vmcase(OP_NEWOBJECT) {
    /* OP_NEWOBJECT: create a structural object
    ** A = destination register
    ** B = Shape constant index
    ** C = storage mode (0=normal, 1=shared, 2=owned)
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int storage_mode = vm_resolve_allocation_storage_mode(base, (uint8_t) GETARG_C(i));
    XrValue cls_val = k[b];
    // Class stored as integer pointer (not GC managed; lives with isolate)
    XrClass *cls = (XrClass *) (intptr_t) XR_TO_INT(cls_val);
    XrObjectInstance *object;
    if (storage_mode != 0 && xr_isolate_get_sys_heap(isolate)) {
        // System storage: allocate on system heap
        size_t size = xr_object_instance_size(cls);
        object = (XrObjectInstance *) xr_sysheap_alloc_storage(xr_isolate_get_sys_heap(isolate),
                                                               size, XR_TINSTANCE, storage_mode);
        if (object) {
            xr_object_instance_init_inplace(object, cls);
            XR_OBJ_SET_STORAGE(&object->hdr, storage_mode);
            if (storage_mode == XR_OBJ_STORAGE_SHARED) {
                xr_shared_set_refc(&object->hdr, 1);
            }
        }
    } else {
        // normal: allocate on coroutine heap
        object = xr_object_instance_new_with_class(VM_CURRENT_CORO, cls);
    }

    R(a) = xr_object_instance_value(object);
    vmbreak;
}

vmcase(OP_OBJECT_GET) {
    // OP_OBJECT_GET: read field by index
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrObjectInstance *json = xr_value_to_object_instance(R(b));
    R(a) = xr_instance_get_dynamic_field(json, (uint16_t) c);
    vmbreak;
}

vmcase(OP_OBJECT_SET) {
    // OP_OBJECT_SET: write field by index
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrObjectInstance *json = xr_value_to_object_instance(R(a));
    XrValue val = R(c);
    xr_instance_set_dynamic_field(isolate, json, (uint16_t) b, val);
    vmbreak;
}

vmcase(OP_OBJECT_GETK) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrObjectInstance *object = xr_value_to_object_instance(R(b));
    SymbolId symbol = (SymbolId) PROTO_SYMBOL(cl->proto, c);
    int ordinal = object && object->klass ? xr_class_lookup_field(object->klass, (int) symbol) : -1;
    if (ordinal < 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY,
                         "verified object access plan does not cover the runtime shape");
    }
    R(a) = xr_instance_get_dynamic_field(object, (uint16_t) ordinal);
    vmbreak;
}

vmcase(OP_OBJECT_SETK) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrObjectInstance *object = xr_value_to_object_instance(R(a));
    SymbolId symbol = (SymbolId) PROTO_SYMBOL(cl->proto, b);
    int ordinal = object && object->klass ? xr_class_lookup_field(object->klass, (int) symbol) : -1;
    if (ordinal < 0 || !xr_instance_set_dynamic_field(isolate, object, (uint16_t) ordinal, R(c))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY,
                         "verified object access plan does not cover the runtime shape");
    }
    vmbreak;
}

vmcase(OP_OBJECT_MERGE) {
    // OP_OBJECT_MERGE copies every field of the source structural object into
    // the destination structural object during object spread.
    // Copied values are retained; later writes override earlier ones.
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    if (!xr_value_has_object_shape(R(b))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "object spread source must be an object");
    }
    XrObjectInstance *dst = xr_value_to_object_instance(R(a));
    XrObjectInstance *src = xr_value_to_object_instance(R(b));
    if (!xr_object_instance_merge(isolate, dst, src)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY,
                         "object spread fields do not match the exact destination shape");
    }
    vmbreak;
}

vmcase(OP_OBJECT_INIT) {
    // OP_OBJECT_INIT: direct index write during initialization
    int a = GETARG_A(i);
    int b = GETARG_B(i);  // Field index
    int c = GETARG_C(i);
    XrObjectInstance *object = xr_value_to_object_instance(R(a));
    XrValue val = R(c);
    xr_instance_set_dynamic_field(isolate, object, (uint16_t) b, val);
    vmbreak;
}

vmcase(OP_OBJECT_INIT_I) {
    // OP_OBJECT_INIT_I: init field with immediate integer
    int a = GETARG_A(i);
    int b = GETARG_B(i);   // Field index
    int c = GETARG_sC(i);  // Signed immediate value
    XrObjectInstance *object = xr_value_to_object_instance(R(a));
    xr_instance_set_dynamic_field(isolate, object, (uint16_t) b, xr_int(c));
    vmbreak;
}

vmcase(OP_OBJECT_INIT_N) {
    // OP_OBJECT_INIT_N: init field with null
    int a = GETARG_A(i);
    int b = GETARG_B(i);  // Field index
    XrObjectInstance *object = xr_value_to_object_instance(R(a));
    xr_instance_set_dynamic_field(isolate, object, (uint16_t) b, xr_null());
    vmbreak;
}

vmcase(OP_JSON_DECODE) vmcase(OP_JSON_DECODE_IGNORE) vmcase(OP_JSON_DECODE_REQUIRE) {
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
    bool ignore_unknown_fields = GET_OPCODE(i) == OP_JSON_DECODE_IGNORE;
    bool require = GET_OPCODE(i) == OP_JSON_DECODE_REQUIRE;
    XR_DCHECK(cls != NULL, "OP_JSON_DECODE: null class");
    bool root_schema = (cls->flags & XR_CLASS_JSON_DECODE_ROOT) != 0;
    const XrJsonDecodeSchema *schema = root_schema && cls->field_count == 1 && cls->fields
                                           ? &cls->fields[0].json_decode_schema
                                           : NULL;

    /* JSON.decode consumes an already parsed JSON.Value. In particular, a
     * Json string is data, not a second JSON source text. JSON.parse<T> owns
     * the direct token-stream path in OP_JSON_PARSE_TYPED. */
    XrMap *src = NULL;
    if (!root_schema && XR_IS_MAP(data)) {
        src = XR_TO_MAP(data);
    } else if (!root_schema && xr_value_has_object_shape(data)) {
        R(a) = xr_json_decode_struct_instance_with_class(isolate, VM_CURRENT_CORO,
                                                         xr_value_to_object_instance(data), cls,
                                                         ignore_unknown_fields);
        vmbreak;
    } else if (root_schema && schema) {
        XrValue decoded = xr_null();
        if (!xr_json_decode_value_with_schema(isolate, VM_CURRENT_CORO, data, schema,
                                              ignore_unknown_fields, &decoded)) {
            if (require) {
                savepc();
                VM_RUNTIME_ERROR(XR_ERR_JSON_INVALID,
                                 "JSON.require: value does not match the requested type");
            }
            R(a) = xr_null();
            vmbreak;
        }
        R(a) = decoded;
        vmbreak;
    } else {
        R(a) = xr_null();
        vmbreak;
    }
    R(a) = xr_json_decode_struct_object_with_class(isolate, VM_CURRENT_CORO, src, cls,
                                                   ignore_unknown_fields);
    vmbreak;
}

vmcase(OP_JSON_PARSE_TYPED) vmcase(OP_JSON_PARSE_TYPED_IGNORE) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue text = R(b);
    XrClass *cls = (XrClass *) (intptr_t) XR_TO_INT(k[c]);
    bool ignore_unknown_fields = GET_OPCODE(i) == OP_JSON_PARSE_TYPED_IGNORE;
    XR_DCHECK(cls != NULL, "OP_JSON_PARSE_TYPED: null target class");
    bool root_schema = (cls->flags & XR_CLASS_JSON_DECODE_ROOT) != 0;
    const XrJsonDecodeSchema *schema = root_schema && cls->field_count == 1 && cls->fields
                                           ? &cls->fields[0].json_decode_schema
                                           : NULL;

    XrJsonTypedParseError error;
    XrValue parsed = xr_null();
    bool ok = false;
    if (XR_IS_STRING(text)) {
        XrString *str = XR_TO_STRING(text);
        ok = root_schema
                 ? schema && xr_json_parse_typed_value_from_cstr(
                                 isolate, VM_CURRENT_CORO, str->data, str->length, schema,
                                 ignore_unknown_fields, &parsed, &error)
                 : xr_json_parse_typed_object_from_cstr(isolate, VM_CURRENT_CORO, str->data,
                                                        str->length, cls, ignore_unknown_fields,
                                                        &parsed, &error);
    } else {
        memset(&error, 0, sizeof(error));
        snprintf(error.path, sizeof(error.path), "$");
        snprintf(error.expected, sizeof(error.expected), "string");
        snprintf(error.actual, sizeof(error.actual), "non_string");
    }
    if (!ok) {
        savepc();
        XrValue exc = xr_panic_info_newf(
            isolate, XR_ERR_JSON_INVALID, "JSON.parse<T>: path %s expected %s, got %s",
            error.path[0] ? error.path : "$", error.expected[0] ? error.expected : "valid JSON",
            error.actual[0] ? error.actual : "invalid");
        xr_vm_unwind_with_trace(isolate, exc);
        if (!xr_vm_is_catch_reachable(isolate))
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }
    R(a) = parsed;
    vmbreak;
}

vmcase(OP_JSON_PARSE_WITH_REST) vmcase(OP_JSON_PARSE_WITH_REST_IGNORE) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue text = R(b);
    XrClass *wrapper_class = (XrClass *) (intptr_t) XR_TO_INT(k[c]);
    bool ignore_nested_unknown_fields = GET_OPCODE(i) == OP_JSON_PARSE_WITH_REST_IGNORE;
    XR_DCHECK(wrapper_class != NULL, "OP_JSON_PARSE_WITH_REST: null wrapper class");
    int value_index = xr_class_lookup_field_by_name(isolate, wrapper_class, "value");
    const XrJsonDecodeSchema *value_schema =
        value_index >= 0 && value_index < wrapper_class->field_count
            ? &wrapper_class->fields[value_index].json_decode_schema
            : NULL;
    XrClass *target_class = NULL;
    if (value_schema &&
        xr_json_value_kind_base(value_schema->value_kind) == XR_JSON_VALUE_STRUCT_OBJECT) {
        target_class = (XrClass *) value_schema->target_descriptor;
    } else if (value_schema &&
               xr_json_value_kind_base(value_schema->value_kind) == XR_JSON_VALUE_CLASS_INSTANCE) {
        XrString *class_name = (XrString *) value_schema->target_descriptor;
        target_class = class_name ? xr_class_lookup_by_name(isolate, class_name->data) : NULL;
    }

    XrJsonTypedParseError error;
    XrValue parsed = xr_null();
    bool ok = false;
    if (XR_IS_STRING(text) && target_class) {
        XrString *str = XR_TO_STRING(text);
        ok = xr_json_parse_typed_object_with_rest_from_cstr(
            isolate, VM_CURRENT_CORO, str->data, str->length, target_class, wrapper_class,
            ignore_nested_unknown_fields, &parsed, &error);
    } else {
        memset(&error, 0, sizeof(error));
        snprintf(error.path, sizeof(error.path), "$");
        snprintf(error.expected, sizeof(error.expected), target_class ? "string" : "object target");
        snprintf(error.actual, sizeof(error.actual),
                 target_class ? "non_string" : "invalid_target");
    }
    if (!ok) {
        savepc();
        XrValue exc = xr_panic_info_newf(
            isolate, XR_ERR_JSON_INVALID, "JSON.parseWithRest<T>: path %s expected %s, got %s",
            error.path[0] ? error.path : "$", error.expected[0] ? error.expected : "valid JSON",
            error.actual[0] ? error.actual : "invalid");
        xr_vm_unwind_with_trace(isolate, exc);
        if (!xr_vm_is_catch_reachable(isolate))
            return XR_VM_RUNTIME_ERROR;
        goto startfunc;
    }
    R(a) = parsed;
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

    // Fixed array .length/.size
    if (XR_IS_ARRAY_REF(obj) && (prop_symbol == SYMBOL_LENGTH || prop_symbol == SYMBOL_SIZE)) {
        R(a) = XR_FROM_INT((int64_t) XR_ARRAY_REF_ELEM_COUNT(obj));
        vmbreak;
    }

    // Frame-local Slice .length/.size
    if (XR_IS_SLICE_REF(obj) && (prop_symbol == SYMBOL_LENGTH || prop_symbol == SYMBOL_SIZE)) {
        XrSliceView *span = XR_TO_SLICE_REF(obj);
        R(a) = XR_FROM_INT(span ? span->length : 0);
        vmbreak;
    }

    // Stack-allocated struct field access
    if (XR_IS_AGG_REF(obj) && !XR_IS_ARRAY_REF(obj) && !XR_IS_SLICE_REF(obj)) {
        XrAggregateLayout *slayout = NULL;
        uint8_t *payload = xr_struct_ref_payload(isolate, obj, &slayout);
        int fidx = xr_struct_layout_field_index(isolate, slayout, prop_symbol);
        if (fidx >= 0 && fidx < slayout->field_count) {
            XrAggregateFieldLayout *sf = &slayout->fields[fidx];
            if (sf->is_flexible) {
                R(a) = xr_null();
                vmbreak;
            }
            uint8_t *fp = payload ? payload + sf->offset : NULL;
            if (!xr_struct_read_field_value(isolate, fp, sf, &R(a)))
                R(a) = xr_null();
            vmbreak;
        }
        // Field not found: might be a method, fall through to type dispatch
    }

    // Fast path: Instance is the most common target, skip if-else chain.
    // Enum types/values/aggregates use type dispatch for member resolution.
    if (xr_value_is_instance(obj) && !XR_IS_ENUM_TYPE(obj) && !XR_IS_ENUM_CTOR(obj) &&
        !xr_value_is_enum_aggregate(obj))
        goto getprop_instance;

    // Dispatch: all non-instance types
    {
        savepc();
        XrDispatchAction _cr =
            vm_getprop_type_dispatch(isolate, vm_ctx, obj, prop_symbol, base, a, b, frame, pc);
        if (_cr == XR_DISP_NEXT) {
            VM_REBIND_AFTER_NATIVE_CALL();
            vmbreak;
        }
        if (_cr == XR_DISP_RESTART)
            goto startfunc;
        if (_cr == XR_DISP_FATAL)
            return XR_VM_RUNTIME_ERROR;
        if (_cr == XR_DISP_RAISE) {
            if (!xr_vm_is_catch_reachable(isolate))
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
        // XR_DISP_FALLTHROUGH: fall through to instance path
    }

getprop_instance:;
    // XrObjectInstance shares XrObjectInstance layout; direct cast works.
    XrObjectInstance *inst = (XrObjectInstance *) XR_TO_PTR(obj);

    /* Every path below resolves the property through the instance class: the
     * dynamic-layout probe, the getter dispatch, and the inline caches all
     * dereference it. A classless instance therefore has no property surface at
     * all, and answering that as a runtime error is what keeps it from becoming
     * a null dereference inside those paths. */
    if (!inst->klass) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "property access on an object with no class");
    }

    /* A weak slot stores an XrWeakHandle, not the target, so it cannot go
     * through the ordinary field paths (nor be inline-cached as one — the
     * cache records an index and reads the slot raw). Decided by one class-
     * level bit, so a class with no weak field keeps every fast path. */
    if (inst->klass->flags & XR_CLASS_HAS_WEAK_FIELDS) {
        int weak_idx = xr_class_lookup_field(inst->klass, prop_symbol);
        if (weak_idx >= 0 && xr_weak_instance_field_load(inst, weak_idx, &R(a)))
            vmbreak;
    }

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

    // Dispatch: getter method lookup. The class summary includes inherited
    // accessors, so ordinary field-only classes avoid the helper call entirely.
    if (inst->klass->flags & XR_CLASS_HAS_GETTERS) {
        XrDispatchAction _cr =
            vm_getprop_instance_getter(isolate, vm_ctx, inst, obj, prop_symbol, base, a, frame, pc);
        if (_cr == XR_DISP_NEXT) {
            VM_REBIND_AFTER_NATIVE_CALL();
            vmbreak;
        }
        if (_cr == XR_DISP_RESTART)
            goto startfunc;
        if (_cr == XR_DISP_FATAL)
            return XR_VM_RUNTIME_ERROR;
        if (_cr == XR_DISP_RAISE) {
            if (!xr_vm_is_catch_reachable(isolate))
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
        // XR_DISP_FALLTHROUGH: no getter, fall through to field access
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
        if (inst_class->struct_layout) {
            if (!xr_instance_struct_get_field(isolate, inst, field_index, &R(a))) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct field read");
            }
        } else {
            R(a) = inst->fields[field_index];
        }
        vmbreak;
    }

    // Fast path 2: Polymorphic IC hit (verify symbol match)
    if (cache && xr_ic_field_lookup_poly(cache, inst_class, prop_symbol, &field_index)) {
        // Polymorphic hit: direct field access!
        if (inst_class->struct_layout) {
            if (!xr_instance_struct_get_field(isolate, inst, field_index, &R(a))) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct field read");
            }
        } else {
            R(a) = inst->fields[field_index];
        }
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
        if (inst_class->struct_layout) {
            if (!xr_instance_struct_get_field(isolate, inst, field_index, &R(a))) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct field read");
            }
        } else {
            R(a) = inst->fields[field_index];
        }

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
            XrSymbolTable *_st2 = (XrSymbolTable *) isolate->core_rt->symbol_table;
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

    // JSON.Values are dynamic-layout instances; the per-type
    // dispatch returns XR_DISP_FALLTHROUGH for them and the regular
    // instance path below (with its DYNAMIC_LAYOUT branch) handles all
    // semantics — including transitions on field add and sealed errors.

    // Dispatch: non-instance type dispatch (Map/Module/Class/null)
    {
        savepc();
        XrDispatchAction _cr =
            vm_setprop_type_dispatch(isolate, vm_ctx, obj, prop_symbol, value, base, a, frame, pc);
        if (_cr == XR_DISP_NEXT) {
            VM_REBIND_AFTER_NATIVE_CALL();
            vmbreak;
        }
        if (_cr == XR_DISP_RESTART)
            goto startfunc;
        if (_cr == XR_DISP_FATAL)
            return XR_VM_RUNTIME_ERROR;
        if (_cr == XR_DISP_RAISE) {
            if (!xr_vm_is_catch_reachable(isolate))
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
        // XR_DISP_FALLTHROUGH: fall through to instance path
    }

    // XrObjectInstance shares XrObjectInstance layout — direct cast works
    XrObjectInstance *inst_s = (XrObjectInstance *) XR_TO_PTR(obj);

    /* Same reasoning as OP_GETPROP: a weak slot holds a handle. */
    if (inst_s->klass && (inst_s->klass->flags & XR_CLASS_HAS_WEAK_FIELDS)) {
        int weak_idx = xr_class_lookup_field(inst_s->klass, prop_symbol);
        if (weak_idx >= 0 &&
            xr_weak_instance_field_store(vm_weak_heap(vm_ctx), inst_s, weak_idx, value))
            vmbreak;
    }

    // Dynamic-layout fast path: hidden-class instance, missing field creates
    // a class transition. Shared objects cannot create new transitions.
    if (inst_s->klass->flags & XR_CLASS_DYNAMIC_LAYOUT) {
        int field_index_d = xr_class_lookup_field(inst_s->klass, prop_symbol);
        if (field_index_d < 0) {
            // Adding a new field: forbid on shared objects
            if (XR_OBJ_GET_STORAGE(&inst_s->hdr) == XR_OBJ_STORAGE_SHARED) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY,
                                 "cannot add field to shared dynamic object");
            }
            XrSymbolTable *_st_sd = (XrSymbolTable *) isolate->core_rt->symbol_table;
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
        vmbreak;
    }

    // Dispatch: setter method lookup. See the getter-side class-summary gate.
    if (inst_s->klass->flags & XR_CLASS_HAS_SETTERS) {
        XrDispatchAction _cr = vm_setprop_instance_setter(isolate, vm_ctx, inst_s, obj, prop_symbol,
                                                          value, base, c, frame, pc);
        if (_cr == XR_DISP_NEXT) {
            VM_REBIND_AFTER_NATIVE_CALL();
            vmbreak;
        }
        if (_cr == XR_DISP_RESTART)
            goto startfunc;
        if (_cr == XR_DISP_FATAL)
            return XR_VM_RUNTIME_ERROR;
        if (_cr == XR_DISP_RAISE) {
            if (!xr_vm_is_catch_reachable(isolate))
                return XR_VM_RUNTIME_ERROR;
            goto startfunc;
        }
        // XR_DISP_FALLTHROUGH: no setter, fall through to field access
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
        if (inst_class->struct_layout) {
            if (!xr_instance_struct_set_field(isolate, inst_s, field_index, value)) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid value for struct field write");
            }
        } else if (XR_IS_ARRAY_REF(value)) {
            if (!vm_store_persistent_array_ref(isolate, &inst_s->fields[field_index], value)) {
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                 "failed to materialize fixed array instance field");
            }
        } else {
            inst_s->fields[field_index] = value;
        }
        vmbreak;
    }

    // Fast path 2: Polymorphic IC hit (verify symbol match)
    if (cache && xr_ic_field_lookup_poly(cache, inst_class, prop_symbol, &field_index)) {
        if (inst_class->struct_layout) {
            if (!xr_instance_struct_set_field(isolate, inst_s, field_index, value)) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid value for struct field write");
            }
        } else if (XR_IS_ARRAY_REF(value)) {
            if (!vm_store_persistent_array_ref(isolate, &inst_s->fields[field_index], value)) {
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                 "failed to materialize fixed array instance field");
            }
        } else {
            inst_s->fields[field_index] = value;
        }
        vmbreak;
    }

    // Slow path: Symbol lookup for field index
    field_index = xr_class_lookup_field(inst_class, prop_symbol);

    if (field_index >= 0) {
        if (inst_class->struct_layout) {
            if (!xr_instance_struct_set_field(isolate, inst_s, field_index, value)) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid value for struct field write");
            }
        } else if (XR_IS_ARRAY_REF(value)) {
            if (!vm_store_persistent_array_ref(isolate, &inst_s->fields[field_index], value)) {
                VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                                 "failed to materialize fixed array instance field");
            }
        } else {
            inst_s->fields[field_index] = value;
        }

        // Update IC cache (pass symbol)
        if (cache) {
            xr_ic_field_update(cache, inst_class, field_index, prop_symbol);
        }
    } else {
        // Field not found: generate error message
        XrSymbolTable *_st2 = (XrSymbolTable *) isolate->core_rt->symbol_table;
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
