/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_collection.inc.c — array / map / set / range / string-builder
 *                                  + array R/W + map R/W + substring + index
 *                                  + slice dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, vm_ctx,
 * pc, frame, ci, base, R, k, savepc, vmcase, vmbreak,
 * VM_RUNTIME_ERROR, VM_STACK, VM_FRAMES, VM_FRAME_COUNT,
 * VM_INC_FRAME_COUNT,
 * VM_CURRENT_CORO, startfunc label, ...) provided by
 * the surrounding scope. CMake excludes *.inc.c from the
 * VM_SRC glob.
 *
 * Owns:
 *   Container creation:
 *     - OP_NEWARRAY / OP_NEWMAP / OP_NEWSET
 *     - OP_NEWRANGE / OP_RANGE_UNPACK / OP_NEWSTRINGBUILDER
 *   Array R/W:
 *     - OP_ARRAY_GET / GETC / SET / SETC / PUSH / LEN / INIT
 *   Map R/W:
 *     - OP_MAP_GET / GETK / SET / SETK / INCREMENT
 *   String:
 *     - OP_SUBSTRING / OP_STR_REPEAT
 *   Generic:
 *     - OP_INDEX_GET / OP_INDEX_SET / OP_SLICE
 *
 * Inlined ARRAY_GET_NOCHECK and StringBuilder helpers stay in
 * xvm.c next to the rest of the data-manipulation hot path.
 */

/* ========================================================
** Container Creation Instructions
** ======================================================== */

#define VM_ARRAY_CHECK_STORABLE(arr, val)                                                          \
    do {                                                                                           \
        if (!xr_typed_value_is_storable((val), (arr)->elem_type)) {                                \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, (arr)->elem_type == XR_ELEM_RUNE                \
                                                       ? "Array<char> element must be char"        \
                                                       : "typed array element must be numeric");   \
        }                                                                                          \
    } while (0)

#define VM_SLICE_CHECK_STORABLE(span_value, val)                                                   \
    do {                                                                                           \
        uint8_t _slice_elem_type = XR_SLICE_REF_ELEM_TYPE(span_value);                             \
        if (!xr_typed_value_is_storable((val), _slice_elem_type)) {                                \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, _slice_elem_type == XR_ELEM_RUNE                \
                                                       ? "Slice<char> element must be char"        \
                                                       : "typed span element must be numeric");    \
        }                                                                                          \
    } while (0)

#define VM_SLICE_SLOT(slot_value)                                                                  \
    ((XrSliceView *) (vm_ctx->struct_areas[VM_FRAME_COUNT - 1] +                                   \
                      (uint16_t) (XR_TO_INT(slot_value)) * 16u))

#define VM_SLICE_GET_ELEMENT(span_value, span, idx)                                                \
    (XR_SLICE_REF_LAYOUT_ID(span_value) != 0                                                       \
         ? xr_aggregate_ref((uint8_t *) (span)->data +                                             \
                                (size_t) (idx) * XR_SLICE_REF_ELEM_SIZE(span_value),               \
                            XR_SLICE_REF_LAYOUT_ID(span_value))                                    \
         : xr_typed_get((span)->data, (int32_t) (idx), XR_SLICE_REF_ELEM_TYPE(span_value)))

#define VM_SLICE_SET_ELEMENT(span_value, span, idx, val)                                           \
    do {                                                                                           \
        if (XR_SLICE_REF_IS_READONLY(span_value)) {                                                \
            VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, "cannot write through readonly Slice");      \
        }                                                                                          \
        (void) xr_typed_set((span)->data, (int32_t) (idx), (val),                                  \
                            XR_SLICE_REF_ELEM_TYPE(span_value));                                   \
    } while (0)

vmcase(OP_NEWARRAY) {
    /* OP_NEWARRAY: create array
    ** A = destination register
    ** B = capacity/initial element count
    ** C = (elem_tid << 2) | storage_mode
    **     storage_mode: bits 0-1 (0=normal, 1=shared, 2=owned)
    **     elem_tid:     bits 2-7 (XrTypeId, 0=any)
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c_field = GETARG_C(i);
    int storage_mode = vm_resolve_allocation_storage_mode(base, (uint8_t) (c_field & 0x03));
    uint8_t elem_tid = (uint8_t) (c_field >> 2);
    uint8_t elem_type = xr_tid_to_elem_type(elem_tid);

    XrArray *array;
    if (storage_mode != 0 && xr_isolate_get_sys_heap(isolate)) {
        // System storage: allocate on system heap
        array = (XrArray *) xr_sysheap_alloc_storage(xr_isolate_get_sys_heap(isolate),
                                                     sizeof(XrArray), XR_TARRAY, storage_mode);
        if (array) {
            xr_array_init_inplace(array, b > 0 ? b : 4, elem_type);
            // Set storage mode
            XR_OBJ_SET_STORAGE(&array->hdr, storage_mode);
            if (storage_mode == XR_OBJ_STORAGE_SHARED) {
                xr_shared_set_refc(&array->hdr, 1);
            }
        }
    } else {
        // Root-elided execution uses fixed-lifetime local storage without
        // materializing a coroutine solely as an allocation host.
        if (!VM_CURRENT_CORO) {
            array = vm_root_array_new(isolate, b > 0 ? b : 4, elem_type);
        } else if (elem_type != XR_ELEM_ANY) {
            array = xr_array_with_capacity_typed(VM_CURRENT_CORO, b > 0 ? b : 4, elem_type);
        } else {
            array = (b > 0) ? xr_array_with_capacity(VM_CURRENT_CORO, b)
                            : xr_array_new(VM_CURRENT_CORO);
        }
    }

    if (array) {
        array->elem_tid = elem_tid;
        /* Preset length to the literal element count for both typed and ANY
         * arrays; lower_array_literal then overwrites slots 0..b-1 via
         * OP_INDEX_SET. Typed element storage is numeric (not GC-scanned), so
         * its uninitialized bytes are harmless. ANY storage holds tagged
         * XrValue slots that the GC scans, and the data buffer is not zeroed on
         * allocation, so null-initialize the presented slots to keep the array
         * safe to scan before the fills land. This makes array index-set strict
         * (in-bounds overwrite) instead of relying on an append-at-length path. */
        array->length = b;
        if (elem_type == XR_ELEM_ANY && b > 0 && array->data) {
            memset(array->data, 0, (size_t) b * sizeof(XrValue));
        }
    }
    R(a) = xr_value_from_array(array);
    vmbreak;
}

vmcase(OP_ARRAY_NEW_CAP) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c_field = GETARG_C(i);
    XrValue cap_value = R(b);
    if (!XR_IS_INT(cap_value)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Array capacity must be an integer");
    }
    int32_t cap = (int32_t) XR_TO_INT(cap_value);
    if (cap < 0)
        cap = 0;

    int storage_mode = vm_resolve_allocation_storage_mode(base, (uint8_t) (c_field & 0x03));
    uint8_t elem_tid = (uint8_t) (c_field >> 2);
    uint8_t elem_type = xr_tid_to_elem_type(elem_tid);

    XrArray *array;
    if (storage_mode != 0 && xr_isolate_get_sys_heap(isolate)) {
        array = (XrArray *) xr_sysheap_alloc_storage(xr_isolate_get_sys_heap(isolate),
                                                     sizeof(XrArray), XR_TARRAY, storage_mode);
        if (array) {
            xr_array_init_inplace(array, cap, elem_type);
            XR_OBJ_SET_STORAGE(&array->hdr, storage_mode);
            if (storage_mode == XR_OBJ_STORAGE_SHARED)
                xr_shared_set_refc(&array->hdr, 1);
        }
    } else {
        array = xr_array_with_capacity_typed(VM_CURRENT_CORO, cap, elem_type);
    }

    if (array)
        array->elem_tid = elem_tid;
    R(a) = array ? xr_value_from_array(array) : xr_null();
    vmbreak;
}

vmcase(OP_ARRAY_NEW_LEN) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c_field = GETARG_C(i);
    XrValue length_value = R(b);
    if (!XR_IS_INT(length_value)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Array length must be an integer");
    }
    int32_t length = (int32_t) XR_TO_INT(length_value);
    if (length < 0)
        length = 0;

    int storage_mode = vm_resolve_allocation_storage_mode(base, (uint8_t) (c_field & 0x03));
    uint8_t elem_tid = (uint8_t) (c_field >> 2);
    uint8_t elem_type = xr_tid_to_elem_type(elem_tid);

    XrArray *array;
    if (storage_mode != 0 && xr_isolate_get_sys_heap(isolate)) {
        array = (XrArray *) xr_sysheap_alloc_storage(xr_isolate_get_sys_heap(isolate),
                                                     sizeof(XrArray), XR_TARRAY, storage_mode);
        if (array) {
            xr_array_init_inplace(array, length, elem_type);
            XR_OBJ_SET_STORAGE(&array->hdr, storage_mode);
            if (storage_mode == XR_OBJ_STORAGE_SHARED)
                xr_shared_set_refc(&array->hdr, 1);
        }
    } else {
        array = xr_array_with_capacity_typed(VM_CURRENT_CORO, length, elem_type);
    }

    if (array) {
        array->elem_tid = elem_tid;
        array->length = length;
        if (elem_type == XR_ELEM_ANY && length > 0 && array->data)
            memset(array->data, 0, (size_t) length * sizeof(XrValue));
    }
    R(a) = array ? xr_value_from_array(array) : xr_null();
    vmbreak;
}

vmcase(OP_NEWTUPLE) {
    /* OP_NEWTUPLE: build a new tuple from B consecutive elements
    ** A = destination register
    ** B = arity (== element count)
    ** C = storage mode (0=normal, 1=shared, 2=transfer)
    **
    ** Elements are read from R[A+1..A+B] (same convention as the
    ** untyped path of OP_NEWARRAY).  Tuples are immutable from user
    ** code, so the construction-then-publish pattern is the only
    ** writer of element slots.
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int storage_mode = vm_resolve_allocation_storage_mode(base, (uint8_t) (GETARG_C(i) & 0x03));
    XrTuple *tup = xr_tuple_new_storage(VM_CURRENT_CORO, (uint16_t) b, (uint8_t) storage_mode);
    if (tup) {
        for (int j = 0; j < b; j++)
            xr_tuple_set(tup, (uint16_t) j, R(a + 1 + j));
    }
    R(a) = xr_value_from_tuple(tup);
    vmbreak;
}

vmcase(OP_TUPLE_GET) {
    /* OP_TUPLE_GET: read a tuple field by compile-time index
    ** A = destination register
    ** B = tuple register
    ** C = zero-based field index (0..arity-1)
    **
    ** The analyzer has already bounds-checked C against the tuple's
    ** static arity, so out-of-range access here is impossible from
    ** legal source.  xr_tuple_get keeps a defensive guard regardless
    ** (it returns null on bad input rather than dereferencing past
    ** the flexible array).
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue tv = R(b);
    if (!xr_value_is_tuple(tv)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "OP_TUPLE_GET: receiver is not a tuple");
    }
    R(a) = xr_tuple_get((XrTuple *) XR_TO_PTR(tv), (uint16_t) c);
    vmbreak;
}

vmcase(OP_NEWMAP) {
    /* OP_NEWMAP: create Map
    ** A = destination register
    ** B = capacity hint
    ** C = (key_kind << 8) | (value_tid << 3) | flags
    **     flags bits0-1: storage mode (0=normal, 1=shared, 2=owned)
    **     value_tid: bits 3-7 (5 bits, XrTypeId 0-31)
    **     key_kind:  bits 8-9 (2 bits: 0=any, 1=string, 2=int)
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    int storage_mode = vm_resolve_allocation_storage_mode(base, (uint8_t) (c & 0x03));
    uint8_t value_tid = (uint8_t) ((c >> 3) & 0x1F);
    int key_kind = (c >> 8) & 0x03;
    uint8_t key_tid = (key_kind == 1) ? XR_TID_STRING : (key_kind == 2) ? XR_TID_I64 : 0;

    XrMap *map;
    if (storage_mode != 0 && xr_isolate_get_sys_heap(isolate)) {
        // System storage: allocate on system heap
        map = (XrMap *) xr_sysheap_alloc_storage(xr_isolate_get_sys_heap(isolate), sizeof(XrMap),
                                                 XR_TMAP, storage_mode);
        if (map) {
            xr_map_init_inplace(map, b > 0 ? b : 8);
            // Set storage mode
            XR_OBJ_SET_STORAGE(&map->hdr, storage_mode);
            if (storage_mode == XR_OBJ_STORAGE_SHARED) {
                xr_shared_set_refc(&map->hdr, 1);
            }
        }
    } else {
        map = VM_CURRENT_CORO ? ((b > 0) ? xr_map_with_capacity(VM_CURRENT_CORO, b)
                                         : xr_map_new(VM_CURRENT_CORO))
                              : vm_root_map_new(isolate, b > 0 ? (uint32_t) b : 0);
    }

    if (map) {
        map->key_tid = key_tid;
        map->value_tid = value_tid;
    }

    R(a) = xr_value_from_map(map);
    vmbreak;
}

vmcase(OP_NEWSET) {
    /* OP_NEWSET: create Set
    ** A = destination register
    ** B = (elem_tid << 3) | flags
    **     flags bits0-1: storage mode (0=normal, 1=shared, 2=owned)
    **     elem_tid: bits 3-7 (XrTypeId, 0=any)
    ** C = init mode (0=empty, 1=from array in R[A+1])
    */
    int a = GETARG_A(i);
    int b_arg = GETARG_B(i);
    int init_mode = GETARG_C(i);
    int storage_mode = vm_resolve_allocation_storage_mode(base, (uint8_t) (b_arg & 0x03));
    uint8_t elem_tid = (uint8_t) ((b_arg >> 3) & 0x1F);

    XrSet *set;
    if (init_mode == 1 && XR_IS_ARRAY(R(a + 1)) && storage_mode != 0 &&
        xr_isolate_get_sys_heap(isolate)) {
        // Initialize from array on system heap
        set = (XrSet *) xr_sysheap_alloc_storage(xr_isolate_get_sys_heap(isolate), sizeof(XrSet),
                                                 XR_TSET, storage_mode);
        if (set) {
            xr_set_init_inplace(set);
            XR_OBJ_SET_STORAGE(&set->hdr, storage_mode);
            if (storage_mode == XR_OBJ_STORAGE_SHARED)
                xr_shared_set_refc(&set->hdr, 1);
            XrArray *arr = XR_TO_ARRAY(R(a + 1));
            int32_t len = arr->length;
            XrValue *elems = (XrValue *) arr->data;
            for (int32_t j = 0; j < len; j++)
                xr_set_add(set, elems[j]);
        }
    } else if (init_mode == 1 && XR_IS_ARRAY(R(a + 1))) {
        XrArray *arr = XR_TO_ARRAY(R(a + 1));
        if (VM_CURRENT_CORO) {
            set = xr_set_from_array(VM_CURRENT_CORO, arr);
        } else {
            set = vm_root_set_new(isolate);
            if (set) {
                XrValue *elements = (XrValue *) arr->data;
                for (int32_t j = 0; j < arr->length; j++)
                    xr_set_add(set, elements[j]);
            }
        }
    } else if (storage_mode != 0 && xr_isolate_get_sys_heap(isolate)) {
        // System storage: allocate on system heap
        set = (XrSet *) xr_sysheap_alloc_storage(xr_isolate_get_sys_heap(isolate), sizeof(XrSet),
                                                 XR_TSET, storage_mode);
        if (set) {
            xr_set_init_inplace(set);
            XR_OBJ_SET_STORAGE(&set->hdr, storage_mode);
            if (storage_mode == XR_OBJ_STORAGE_SHARED) {
                xr_shared_set_refc(&set->hdr, 1);
            }
        }
    } else {
        set = VM_CURRENT_CORO ? xr_set_new(VM_CURRENT_CORO) : vm_root_set_new(isolate);
    }

    if (set) {
        set->elem_tid = elem_tid;
    }

    R(a) = xr_value_from_set(set);
    vmbreak;
}

vmcase(OP_NEWRANGE) {
    /* OP_NEWRANGE: create lazy Range object
    ** R[A] = Range(R[B], R[C])
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    int64_t start_val = XR_TO_INT(R(b));
    int64_t end_val = XR_TO_INT(R(c));
    XrRangeCore core =
        XR_RANGE_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_RANGE_HI, XR_SEM_OWNER_ID_SHARED_RANGE_LO,
                             XR_SEM_CONSUMER_VM, start_val, end_val, false);
    R(a) = xr_range_from_core(isolate, core);
    vmbreak;
}

vmcase(OP_NEWRANGE_INCLUSIVE) {
    /* OP_NEWRANGE_INCLUSIVE: create lazy inclusive Range object
    ** R[A] = Range inclusive(R[B], R[C])
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    int64_t start_val = XR_TO_INT(R(b));
    int64_t end_val = XR_TO_INT(R(c));
    XrRangeCore core =
        XR_RANGE_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_RANGE_HI, XR_SEM_OWNER_ID_SHARED_RANGE_LO,
                             XR_SEM_CONSUMER_VM, start_val, end_val, true);
    R(a) = xr_range_from_core(isolate, core);
    vmbreak;
}

vmcase(OP_RANGE_UNPACK) {
    /* OP_RANGE_UNPACK: extract Range fields for standard loop
    ** R[A]   = start (loop variable initial value)
    ** R[A+1] = end   (loop termination bound)
    ** R[A+2] = step
    ** B = source register containing Range object
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue rv = R(b);
    XrRange *rng = xr_value_get_range_body(isolate, rv);
    if (!rng) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "for-in expected Range object");
    }
    XR_DCHECK(rng->step != 0, "OP_RANGE_UNPACK: Range step is zero");
    R(a) = xr_int(rng->start);     // start (loop variable)
    R(a + 1) = xr_int(rng->end);   // end (bound)
    R(a + 2) = xr_int(rng->step);  // step
    vmbreak;
}

vmcase(OP_NEWSTRINGBUILDER) {
    /* OP_NEWSTRINGBUILDER: create StringBuilder
    ** A = destination register
    ** B = storage mode (0=normal, 1=shared, 2=owned)
    */
    int a = GETARG_A(i);
    int storage_mode = vm_resolve_allocation_storage_mode(base, (uint8_t) GETARG_B(i));

    XrStringBuilder *sb;
    if (storage_mode != 0 && xr_isolate_get_sys_heap(isolate)) {
        // System storage: allocate on system heap
        sb = (XrStringBuilder *) xr_sysheap_alloc_storage(
            xr_isolate_get_sys_heap(isolate), sizeof(XrStringBuilder), XR_TINSTANCE, storage_mode);
        if (sb) {
            sb->klass = isolate->core->stringBuilderClass;
            xr_stringbuilder_init_inplace(sb);
            XR_OBJ_SET_STORAGE(&sb->hdr, storage_mode);
            if (storage_mode == XR_OBJ_STORAGE_SHARED) {
                xr_shared_set_refc(&sb->hdr, 1);
            }
        }
    } else {
        // normal: allocate on coroutine heap
        sb = xr_stringbuilder_new(VM_CURRENT_CORO);
    }

    R(a) = xr_stringbuilder_value(sb);
    vmbreak;
}

/* ========================================================
** Array Operation Instructions
** ======================================================== */

vmcase(OP_ARRAY_GET) {
    // OP_ARRAY_GET: array dynamic index read
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue obj_val = R(b);
    // Struct inline fixed array dynamic index
    if (XR_IS_ARRAY_REF(obj_val)) {
        uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
        uint32_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        int64_t idx = XR_TO_INT(R(c));
        if ((uint64_t) idx < (uint64_t) ecount) {
            uint8_t *bp = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(xr_target_data_layout_host(), etype);
            uint8_t *ep = bp + idx * es;
            switch (etype) {
                case XR_NATIVE_I64:
                    R(a) = XR_FROM_INT(*(int64_t *) ep);
                    break;
                case XR_NATIVE_U64:
                    R(a) = XR_FROM_INT((int64_t) *(uint64_t *) ep);
                    break;
                case XR_NATIVE_ISIZE:
                    R(a) = XR_FROM_INT((int64_t) *(ptrdiff_t *) ep);
                    break;
                case XR_NATIVE_USIZE:
                    R(a) = XR_FROM_INT((int64_t) *(size_t *) ep);
                    break;
                case XR_NATIVE_F64:
                    R(a) = XR_FROM_FLOAT(*(double *) ep);
                    break;
                case XR_NATIVE_BOOL:
                    R(a) = *(uint8_t *) ep ? XR_TRUE_VAL : XR_FALSE_VAL;
                    break;
                case XR_NATIVE_VALUE:
                    R(a) = *(XrValue *) ep;
                    break;
                case XR_NATIVE_I32:
                    R(a) = XR_FROM_INT((int64_t) *(int32_t *) ep);
                    break;
                case XR_NATIVE_U32:
                    R(a) = XR_FROM_INT((int64_t) *(uint32_t *) ep);
                    break;
                case XR_NATIVE_I16:
                    R(a) = XR_FROM_INT((int64_t) *(int16_t *) ep);
                    break;
                case XR_NATIVE_U16:
                    R(a) = XR_FROM_INT((int64_t) *(uint16_t *) ep);
                    break;
                case XR_NATIVE_I8:
                    R(a) = XR_FROM_INT((int64_t) *(int8_t *) ep);
                    break;
                case XR_NATIVE_U8:
                    R(a) = XR_FROM_INT((int64_t) *(uint8_t *) ep);
                    break;
                case XR_NATIVE_F32:
                    R(a) = XR_FROM_FLOAT((double) *(float *) ep);
                    break;
                default:
                    R(a) = xr_null();
                    break;
            }
        } else {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "fixed array index out of range: %lld (length %u)", (long long) idx,
                             (unsigned) ecount);
        }
        vmbreak;
    }
    if (!XR_IS_ARRAY(obj_val)) {
        if (xr_value_is_instance(obj_val)) {
            XrObjectInstance *_inst = xr_value_to_instance(obj_val);
            XrClass *_cls = xr_instance_get_class(_inst);
            if (XCLASS_HAS_OP(_cls, XR_OP_INDEX_FLAG)) {
                XrMethod *_m = xr_class_lookup_method(_cls, SYMBOL_OP_INDEX);
                if (_m && _m->type == XMETHOD_OPERATOR && _m->as.closure) {
                    XrClosure *_cl = _m->as.closure;
                    XrProto *_p = _cl->proto;
                    VM_STACK_CHECK(a + 1 + _p->maxstacksize);
                    R(a + 1) = obj_val;
                    R(a + 2) = R(c);
                    savepc();
                    int _fi = VM_FRAME_COUNT;
                    VM_INC_FRAME_COUNT;
                    XrBcCallFrame *_nf = &VM_FRAMES[_fi];
                    _nf->closure = _cl;
                    _nf->pc = PROTO_CODE_BASE(_p);
                    _nf->base_offset = (int) ((base + a + 1) - VM_STACK);
                    goto startfunc;
                }
            }
        }
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "only Array supports dynamic indexing");
    }
    XrArray *arr = XR_TO_ARRAY(obj_val);
    int64_t idx = XR_TO_INT(R(c));
    if ((uint64_t) idx < (uint64_t) arr->length) {
        R(a) = (arr->elem_type == XR_ELEM_ANY) ? ((XrValue *) arr->data)[idx]
                                               : xr_array_get_element(arr, (int32_t) idx);
    } else {
        R(a) = xr_null();
    }
    vmbreak;
}

vmcase(OP_ARRAY_GETC) {
    // OP_ARRAY_GETC: array/string constant index read
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue obj_val = R(b);
    // Struct inline fixed array constant index
    if (XR_IS_ARRAY_REF(obj_val)) {
        uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
        uint32_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        if ((unsigned) c < (unsigned) ecount) {
            uint8_t *base_ptr = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(xr_target_data_layout_host(), etype);
            uint8_t *ep = base_ptr + c * es;
            switch (etype) {
                case XR_NATIVE_I64:
                    R(a) = XR_FROM_INT(*(int64_t *) ep);
                    break;
                case XR_NATIVE_U64:
                    R(a) = XR_FROM_INT((int64_t) *(uint64_t *) ep);
                    break;
                case XR_NATIVE_ISIZE:
                    R(a) = XR_FROM_INT((int64_t) *(ptrdiff_t *) ep);
                    break;
                case XR_NATIVE_USIZE:
                    R(a) = XR_FROM_INT((int64_t) *(size_t *) ep);
                    break;
                case XR_NATIVE_F64:
                    R(a) = XR_FROM_FLOAT(*(double *) ep);
                    break;
                case XR_NATIVE_BOOL:
                    R(a) = *(uint8_t *) ep ? XR_TRUE_VAL : XR_FALSE_VAL;
                    break;
                case XR_NATIVE_VALUE:
                    R(a) = *(XrValue *) ep;
                    break;
                case XR_NATIVE_I32:
                    R(a) = XR_FROM_INT((int64_t) *(int32_t *) ep);
                    break;
                case XR_NATIVE_U32:
                    R(a) = XR_FROM_INT((int64_t) *(uint32_t *) ep);
                    break;
                case XR_NATIVE_I16:
                    R(a) = XR_FROM_INT((int64_t) *(int16_t *) ep);
                    break;
                case XR_NATIVE_U16:
                    R(a) = XR_FROM_INT((int64_t) *(uint16_t *) ep);
                    break;
                case XR_NATIVE_I8:
                    R(a) = XR_FROM_INT((int64_t) *(int8_t *) ep);
                    break;
                case XR_NATIVE_U8:
                    R(a) = XR_FROM_INT((int64_t) *(uint8_t *) ep);
                    break;
                case XR_NATIVE_F32:
                    R(a) = XR_FROM_FLOAT((double) *(float *) ep);
                    break;
                default:
                    R(a) = xr_null();
                    break;
            }
        } else {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "fixed array index out of range: %d (length %u)", c,
                             (unsigned) ecount);
        }
        vmbreak;
    }
    // Array indexing (includes slices — capacity==0 && source!=NULL)
    if (XR_IS_ARRAY(obj_val)) {
        XrArray *arr = XR_TO_ARRAY(obj_val);
        if (c < arr->length) {
            R(a) = (arr->elem_type == XR_ELEM_ANY) ? ((XrValue *) arr->data)[c]
                                                   : xr_array_get_element(arr, c);
        } else {
            R(a) = xr_null();
        }
        vmbreak;
    }
    // Range constant index
    {
        XrRange *rng = xr_value_get_range_body(isolate, obj_val);
        if (rng) {
            bool ok = false;
            int64_t value = xr_range_core_index(xr_range_core_view(rng), c, &ok);
            R(a) = ok ? xr_int(value) : xr_null();
            vmbreak;
        }
    }
    // Operator overload: operator[]
    if (xr_value_is_instance(obj_val)) {
        XrObjectInstance *_inst = xr_value_to_instance(obj_val);
        XrClass *_cls = xr_instance_get_class(_inst);
        if (XCLASS_HAS_OP(_cls, XR_OP_INDEX_FLAG)) {
            XrMethod *_m = xr_class_lookup_method(_cls, SYMBOL_OP_INDEX);
            if (_m && _m->type == XMETHOD_OPERATOR && _m->as.closure) {
                XrClosure *_cl = _m->as.closure;
                XrProto *_p = _cl->proto;
                VM_STACK_CHECK(a + 1 + _p->maxstacksize);
                R(a + 1) = obj_val;
                R(a + 2) = xr_int(c);
                savepc();
                int _fi = VM_FRAME_COUNT;
                VM_INC_FRAME_COUNT;
                XrBcCallFrame *_nf = &VM_FRAMES[_fi];
                _nf->closure = _cl;
                _nf->pc = PROTO_CODE_BASE(_p);
                _nf->base_offset = (int) ((base + a + 1) - VM_STACK);
                goto startfunc;
            }
        }
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX,
                     "only Array, Range, and operator[] support constant indexing");
}

vmcase(OP_ARRAY_SET) {
    // OP_ARRAY_SET: array dynamic index write
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue obj_val = R(a);
    // Struct inline fixed array dynamic index write
    if (XR_IS_ARRAY_REF(obj_val)) {
        uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
        uint32_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        int64_t idx = XR_TO_INT(R(b));
        if ((uint64_t) idx < (uint64_t) ecount) {
            uint8_t *bp = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(xr_target_data_layout_host(), etype);
            uint8_t *ep = bp + idx * es;
            XrValue _av = R(c);
            switch (etype) {
                case XR_NATIVE_I64:
                    *(int64_t *) ep = XR_TO_INT(_av);
                    break;
                case XR_NATIVE_U64:
                    *(uint64_t *) ep = (uint64_t) XR_TO_INT(_av);
                    break;
                case XR_NATIVE_ISIZE:
                    *(ptrdiff_t *) ep = (ptrdiff_t) XR_TO_INT(_av);
                    break;
                case XR_NATIVE_USIZE:
                    *(size_t *) ep = (size_t) XR_TO_INT(_av);
                    break;
                case XR_NATIVE_F64:
                    *(double *) ep = XR_TO_FLOAT(_av);
                    break;
                case XR_NATIVE_BOOL:
                    *(uint8_t *) ep = (uint8_t) _av.i;
                    break;
                case XR_NATIVE_VALUE:
                    *(XrValue *) ep = _av;
                    break;
                case XR_NATIVE_I32:
                    *(int32_t *) ep = (int32_t) XR_TO_INT(_av);
                    break;
                case XR_NATIVE_U32:
                    *(uint32_t *) ep = (uint32_t) XR_TO_INT(_av);
                    break;
                case XR_NATIVE_I16:
                    *(int16_t *) ep = (int16_t) XR_TO_INT(_av);
                    break;
                case XR_NATIVE_U16:
                    *(uint16_t *) ep = (uint16_t) XR_TO_INT(_av);
                    break;
                case XR_NATIVE_I8:
                    *(int8_t *) ep = (int8_t) XR_TO_INT(_av);
                    break;
                case XR_NATIVE_U8:
                    *(uint8_t *) ep = (uint8_t) XR_TO_INT(_av);
                    break;
                case XR_NATIVE_F32:
                    *(float *) ep = (float) XR_TO_FLOAT(_av);
                    break;
                default:
                    break;
            }
        } else {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "fixed array index out of range: %lld (length %u)", (long long) idx,
                             (unsigned) ecount);
        }
        vmbreak;
    }
    if (XR_IS_ARRAY(obj_val)) {
        XrArray *arr = XR_TO_ARRAY(obj_val);
        int64_t idx = XR_TO_INT(R(b));
        XrValue _av = R(c);
        /* Strict bounds: idx must be in [0, length). No append-at-length, no
         * negative wraparound (that is slice-only). */
        if ((uint64_t) idx >= (uint64_t) arr->length) {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "array index out of range: %lld (length %d)", (long long) idx,
                             (int) arr->length);
        }
        if (arr->elem_type == XR_ELEM_ANY) {
            ((XrValue *) arr->data)[idx] = _av;
            XR_ARRAY_MARK_REFS(arr, _av);
        } else {
            VM_ARRAY_CHECK_STORABLE(arr, _av);
            xr_array_set_element(arr, (int32_t) idx, _av);
        }
        vmbreak;
    }
    if (xr_value_is_instance(obj_val)) {
        XrObjectInstance *_inst = xr_value_to_instance(obj_val);
        XrClass *_cls = xr_instance_get_class(_inst);
        if (XCLASS_HAS_OP(_cls, XR_OP_INDEX_SET_FLAG)) {
            XrMethod *_m = xr_class_lookup_method(_cls, SYMBOL_OP_INDEX_SET);
            if (_m && _m->type == XMETHOD_OPERATOR && _m->as.closure) {
                XrClosure *_cl = _m->as.closure;
                XrProto *_p = _cl->proto;
                VM_STACK_CHECK(a + 2 + _p->maxstacksize);
                XrValue _key = R(b), _val = R(c);
                R(a + 2) = obj_val;
                R(a + 3) = _key;
                R(a + 4) = _val;
                savepc();
                int _fi = VM_FRAME_COUNT;
                VM_INC_FRAME_COUNT;
                XrBcCallFrame *_nf = &VM_FRAMES[_fi];
                _nf->closure = _cl;
                _nf->pc = PROTO_CODE_BASE(_p);
                _nf->base_offset = (int) ((base + a + 2) - VM_STACK);
                goto startfunc;
            }
        }
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "only Array support dynamic index assignment");
}

vmcase(OP_ARRAY_SETC) {
    // OP_ARRAY_SETC: array constant index write
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue obj_val = R(a);
    // Struct inline fixed array constant index write
    if (XR_IS_ARRAY_REF(obj_val)) {
        uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
        uint32_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        if ((unsigned) b < (unsigned) ecount) {
            uint8_t *bp = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(xr_target_data_layout_host(), etype);
            uint8_t *ep = bp + b * es;
            XrValue _acv = R(c);
            switch (etype) {
                case XR_NATIVE_I64:
                    *(int64_t *) ep = XR_TO_INT(_acv);
                    break;
                case XR_NATIVE_U64:
                    *(uint64_t *) ep = (uint64_t) XR_TO_INT(_acv);
                    break;
                case XR_NATIVE_ISIZE:
                    *(ptrdiff_t *) ep = (ptrdiff_t) XR_TO_INT(_acv);
                    break;
                case XR_NATIVE_USIZE:
                    *(size_t *) ep = (size_t) XR_TO_INT(_acv);
                    break;
                case XR_NATIVE_F64:
                    *(double *) ep = XR_TO_FLOAT(_acv);
                    break;
                case XR_NATIVE_BOOL:
                    *(uint8_t *) ep = (uint8_t) _acv.i;
                    break;
                case XR_NATIVE_VALUE:
                    *(XrValue *) ep = _acv;
                    break;
                case XR_NATIVE_I32:
                    *(int32_t *) ep = (int32_t) XR_TO_INT(_acv);
                    break;
                case XR_NATIVE_U32:
                    *(uint32_t *) ep = (uint32_t) XR_TO_INT(_acv);
                    break;
                case XR_NATIVE_I16:
                    *(int16_t *) ep = (int16_t) XR_TO_INT(_acv);
                    break;
                case XR_NATIVE_U16:
                    *(uint16_t *) ep = (uint16_t) XR_TO_INT(_acv);
                    break;
                case XR_NATIVE_I8:
                    *(int8_t *) ep = (int8_t) XR_TO_INT(_acv);
                    break;
                case XR_NATIVE_U8:
                    *(uint8_t *) ep = (uint8_t) XR_TO_INT(_acv);
                    break;
                case XR_NATIVE_F32:
                    *(float *) ep = (float) XR_TO_FLOAT(_acv);
                    break;
                default:
                    break;
            }
        } else {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "fixed array index out of range: %d (length %u)", b,
                             (unsigned) ecount);
        }
        vmbreak;
    }
    if (XR_IS_ARRAY(obj_val)) {
        XrArray *arr = XR_TO_ARRAY(obj_val);
        XrValue _acv = R(c);
        /* Strict bounds: the constant index must be in [0, length). */
        if (b >= arr->length) {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "array index out of range: %d (length %d)",
                             b, (int) arr->length);
        }
        if (arr->elem_type == XR_ELEM_ANY) {
            ((XrValue *) arr->data)[b] = _acv;
            XR_ARRAY_MARK_REFS(arr, _acv);
        } else {
            VM_ARRAY_CHECK_STORABLE(arr, _acv);
            xr_array_set_element(arr, b, _acv);
        }
        vmbreak;
    }
    if (xr_value_is_instance(obj_val)) {
        XrObjectInstance *_inst = xr_value_to_instance(obj_val);
        XrClass *_cls = xr_instance_get_class(_inst);
        if (XCLASS_HAS_OP(_cls, XR_OP_INDEX_SET_FLAG)) {
            XrMethod *_m = xr_class_lookup_method(_cls, SYMBOL_OP_INDEX_SET);
            if (_m && _m->type == XMETHOD_OPERATOR && _m->as.closure) {
                XrClosure *_cl = _m->as.closure;
                XrProto *_p = _cl->proto;
                VM_STACK_CHECK(a + 2 + _p->maxstacksize);
                XrValue _val = R(c);
                R(a + 2) = obj_val;
                R(a + 3) = xr_int(b);
                R(a + 4) = _val;
                savepc();
                int _fi = VM_FRAME_COUNT;
                VM_INC_FRAME_COUNT;
                XrBcCallFrame *_nf = &VM_FRAMES[_fi];
                _nf->closure = _cl;
                _nf->pc = PROTO_CODE_BASE(_p);
                _nf->base_offset = (int) ((base + a + 2) - VM_STACK);
                goto startfunc;
            }
        }
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX,
                     "only Array, typed array support constant index assignment");
}

vmcase(OP_ARRAY_PUSH) {
    // OP_ARRAY_PUSH: array push
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrArrayPushStatus status = xr_array_push_owned_checked(R(a), R(b));
    if (XR_UNLIKELY(status == XR_ARRAY_PUSH_SLICE))
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_ARRAY_SLICE_PUSH_MSG);
    if (XR_UNLIKELY(status == XR_ARRAY_PUSH_TYPE_MISMATCH))
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "value is not storable in typed array");
    if (XR_UNLIKELY(status == XR_ARRAY_PUSH_ALLOCATION_FAILED))
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "array push allocation failed");
    if (XR_UNLIKELY(status != XR_ARRAY_PUSH_OK))
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "array push receiver must be a valid Array");
    vmbreak;
}

vmcase(OP_ARRAY_EXTEND) {
    // OP_ARRAY_EXTEND: splice every element of R[B]:Array into R[A]:Array.
    // Elements are borrowed from the source, so each is retained as it is
    // copied (the source array keeps its own references). Mirrors the
    // retain-per-element contract of Array.concat.
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    if (!XR_IS_ARRAY(R(b))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "array spread source must be an array");
    }
    XrArray *dst = XR_TO_ARRAY(R(a));
    XrArray *src = XR_TO_ARRAY(R(b));
    int32_t n = src->length;
    for (int32_t j = 0; j < n; j++) {
        XrValue elem = xr_array_get_element(src, j);
        VM_ARRAY_CHECK_STORABLE(dst, elem);
        xr_rc_retain_value(elem);
        xr_array_push(dst, elem);
    }
    vmbreak;
}

vmcase(OP_ARRAY_LEN) {
    // OP_ARRAY_LEN: array length
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    if (XR_IS_SLICE_REF(R(b))) {
        XrSliceView *span = XR_TO_SLICE_REF(R(b));
        R(a) = xr_int((xr_Integer) (span ? span->length : 0));
        vmbreak;
    }
    XrArray *arr = XR_TO_ARRAY(R(b));
    R(a) = xr_int((xr_Integer) arr->length);
    vmbreak;
}

vmcase(OP_LEN) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue value = R(b);
    if (XR_IS_ARRAY(value)) {
        R(a) = xr_int((xr_Integer) XR_TO_ARRAY(value)->length);
        vmbreak;
    }
    if (XR_IS_SLICE_REF(value)) {
        XrSliceView *span = XR_TO_SLICE_REF(value);
        R(a) = xr_int((xr_Integer) (span ? span->length : 0));
        vmbreak;
    }
    if (XR_IS_MAP(value)) {
        R(a) = xr_int((xr_Integer) xr_map_size(XR_TO_MAP(value)));
        vmbreak;
    }
    if (XR_IS_SET(value)) {
        R(a) = xr_int((xr_Integer) xr_set_size(XR_TO_SET(value)));
        vmbreak;
    }
    if (XR_IS_STRING(value)) {
        /* Code points, not bytes -- the kernel names which count a string's
         * length is, so the two backends cannot drift to different answers. */
        XrString *string = XR_TO_STRING(value);
        R(a) =
            xr_int((xr_Integer) (xr_length_source_counts_runes_core(XR_LENGTH_SOURCE_STRING_RUNES)
                                     ? (string ? string->rune_length : 0)
                                     : (string ? string->length : 0)));
        vmbreak;
    }
    if (xr_value_is_channel(value)) {
        R(a) = xr_int((xr_Integer) xr_value_to_channel(value)->buf_count);
        vmbreak;
    }
    if (xr_value_is_work_queue(value)) {
        R(a) = xr_int((xr_Integer) xr_work_queue_length(xr_value_to_work_queue(value)));
        vmbreak;
    }
    if (xr_is_stringbuilder(value)) {
        R(a) = xr_int((xr_Integer) xr_stringbuilder_length(xr_to_stringbuilder(value)));
        vmbreak;
    }
    XrRange *range = xr_value_get_range_body(isolate, value);
    if (range) {
        R(a) = xr_int((xr_Integer) xr_range_length(range));
        vmbreak;
    }
    int64_t buffer_length = xr_buffer_length(value);
    if (buffer_length >= 0) {
        R(a) = xr_int((xr_Integer) buffer_length);
        vmbreak;
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "value does not implement Lengthable");
}

vmcase(OP_ARRAY_DATA_PTR) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    const void *data = NULL;
    XrDataPointerLifetime lifetime = XR_DATA_POINTER_OWNER_BORROW;
    if (XR_IS_STRING(R(b))) {
        XrString *str = XR_TO_STRING(R(b));
        data = str ? str->data : NULL;
        lifetime = XR_DATA_POINTER_STATIC;
    } else if (XR_IS_ARRAY_REF(R(b))) {
        data = R(b).ptr;
    } else if (XR_IS_SLICE_REF(R(b))) {
        XrSliceView *span = XR_TO_SLICE_REF(R(b));
        data = span ? span->data : NULL;
    } else if (XR_IS_ARRAY(R(b))) {
        XrArray *arr = XR_TO_ARRAY(R(b));
        data = arr->data;
    } else {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "ptr() expects static bytes, an array, fixed array, or slice receiver");
    }
    XrDataPointerProjection projection = XR_DATA_POINTER_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_DATA_POINTER_HI, XR_SEM_OWNER_ID_SHARED_DATA_POINTER_LO,
        XR_SEM_CONSUMER_VM, data, lifetime);
    R(a) = xr_int((xr_Integer) (intptr_t) projection.address);
    vmbreak;
}

vmcase(OP_STRING_BYTES_SLICE) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    if (!XR_IS_STRING(R(b)) || !XR_IS_INT(R(c))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "string.bytes() expects a string receiver");
    }
    XrString *str = XR_TO_STRING(R(b));
    XrSliceView *span = VM_SLICE_SLOT(R(c));
    span->data = str ? (void *) str->data : NULL;
    span->length = str ? str->length : 0;
    R(a) = xr_span_ref_typed(span, XR_ELEM_U8, 1, 0, 0, XR_SLICE_VIEW_READONLY);
    vmbreak;
}

#define VM_SLICE_VIEW(value, out_data, out_length, out_elem_type, out_elem_size, out_elem_tid,     \
                      out_contains_refs, out_reserved, out_guard, message)                         \
    do {                                                                                           \
        XrValue _span_value = (value);                                                             \
        if (XR_IS_SLICE_REF(_span_value)) {                                                        \
            XrSliceView *_span = XR_TO_SLICE_REF(_span_value);                                     \
            if (!_span) {                                                                          \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, (message));                                 \
            }                                                                                      \
            (out_data) = _span->data;                                                              \
            (out_length) = _span->length;                                                          \
            (out_elem_type) = XR_SLICE_REF_ELEM_TYPE(_span_value);                                 \
            (out_elem_size) = XR_SLICE_REF_ELEM_SIZE(_span_value);                                 \
            (out_elem_tid) = XR_SLICE_REF_ELEM_TID(_span_value);                                   \
            (out_contains_refs) = XR_SLICE_REF_CONTAINS_REFS(_span_value);                         \
            (out_reserved) = XR_SLICE_REF_IS_READONLY(_span_value) ? XR_SLICE_VIEW_READONLY : 0;   \
            (out_guard) = NULL;                                                                    \
        } else if (XR_IS_ARRAY(_span_value)) {                                                     \
            XrArray *_arr = XR_TO_ARRAY(_span_value);                                              \
            if (!_arr) {                                                                           \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, (message));                                 \
            }                                                                                      \
            (out_data) = _arr->data;                                                               \
            (out_length) = _arr->length;                                                           \
            (out_elem_type) = _arr->elem_type;                                                     \
            (out_elem_size) = _arr->elem_size;                                                     \
            (out_elem_tid) = _arr->elem_tid;                                                       \
            (out_contains_refs) = _arr->contains_refs;                                             \
            (out_reserved) = 0;                                                                    \
            (out_guard) = _arr;                                                                    \
        } else {                                                                                   \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, (message));                                     \
        }                                                                                          \
    } while (0)

vmcase(OP_SLICE_AS_BYTES) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    if (!XR_IS_INT(R(c))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.asArray<u8>() missing Slice frame slot");
    }
    void *data = NULL;
    int64_t length = 0;
    uint8_t elem_type = XR_ELEM_ANY;
    uint16_t elem_size = 0;
    uint8_t elem_tid = 0;
    uint8_t contains_refs = 0;
    uint32_t reserved = 0;
    void *guard = NULL;
    VM_SLICE_VIEW(R(b), data, length, elem_type, elem_size, elem_tid, contains_refs, reserved,
                  guard, "Slice.asArray<u8>() expects Slice");
    (void) elem_tid;
    (void) contains_refs;
    uint16_t layout_id = XR_IS_SLICE_REF(R(b)) ? XR_SLICE_REF_LAYOUT_ID(R(b)) : 0;
    XrPodSliceViewResult view_result = XR_POD_SLICE_VIEW_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_HI, XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_LO,
        XR_SEM_CONSUMER_VM,
        xr_pod_slice_view_core(XR_POD_SLICE_VIEW_AS_BYTES, data, length, elem_size,
                               elem_type < XR_ELEM_COUNT &&
                                   (elem_type != XR_ELEM_ANY || layout_id != 0),
                               0, 0, 0, false, false));
    if (view_result.status == XR_POD_SLICE_VIEW_INVALID_SOURCE_LAYOUT) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "Slice.asArray<u8>() requires POD Slice element type");
    }
    if (view_result.status != XR_POD_SLICE_VIEW_OK) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.asArray<u8>() byte length overflow");
    }
    XrSliceView *span = VM_SLICE_SLOT(R(c));
    span->data = view_result.data;
    span->length = view_result.length;
    R(a) =
        xr_span_ref_typed(span, XR_ELEM_U8, 1, 0, 0, (uint8_t) (reserved & XR_SLICE_VIEW_READONLY));
    vmbreak;
}

vmcase(OP_SLICE_COPY) {
    int a = GETARG_A(i);
    void *dst_data = NULL;
    void *src_data = NULL;
    int64_t dst_length = 0;
    int64_t src_length = 0;
    uint8_t dst_elem_type = XR_ELEM_ANY;
    uint8_t src_elem_type = XR_ELEM_ANY;
    uint16_t dst_elem_size = 0;
    uint16_t src_elem_size = 0;
    uint8_t dst_elem_tid = 0;
    uint8_t src_elem_tid = 0;
    uint8_t dst_contains_refs = 0;
    uint8_t src_contains_refs = 0;
    uint32_t dst_reserved = 0;
    uint32_t src_reserved = 0;
    void *dst_guard = NULL;
    void *src_guard = NULL;
    VM_SLICE_VIEW(R(a), dst_data, dst_length, dst_elem_type, dst_elem_size, dst_elem_tid,
                  dst_contains_refs, dst_reserved, dst_guard,
                  "Slice.copyFrom(src) receiver must be Slice");
    VM_SLICE_VIEW(R(a + 1), src_data, src_length, src_elem_type, src_elem_size, src_elem_tid,
                  src_contains_refs, src_reserved, src_guard,
                  "Slice.copyFrom(src) source must be Slice");
    (void) dst_elem_tid;
    (void) src_elem_tid;
    (void) dst_contains_refs;
    (void) src_contains_refs;
    (void) src_reserved;
    (void) dst_guard;
    (void) src_guard;
    if (dst_elem_type == XR_ELEM_ANY || dst_elem_type >= XR_ELEM_COUNT || dst_elem_size == 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.copyFrom(src) receiver must be POD Slice");
    }
    if (src_elem_type == XR_ELEM_ANY || src_elem_type >= XR_ELEM_COUNT || src_elem_size == 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.copyFrom(src) source must be POD Slice");
    }
    if (dst_elem_type != src_elem_type || dst_elem_size != src_elem_size) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.copyFrom(src) element type mismatch");
    }
    if (dst_reserved & XR_SLICE_VIEW_READONLY) {
        VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, "cannot write through readonly Slice");
    }
    XrPodSliceStatus copy_status =
        XR_POD_SLICE_COPY_OWNER_APPLY(XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_HI,
                                      XR_SEM_OWNER_ID_SHARED_POD_SLICE_COPY_LO, XR_SEM_CONSUMER_VM,
                                      xr_pod_slice_copy_core(dst_data, dst_length, dst_elem_size,
                                                             src_data, src_length, src_elem_size));
    if (copy_status == XR_POD_SLICE_BYTE_LENGTH_OVERFLOW) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.copyFrom(src) byte length overflow");
    }
    if (copy_status == XR_POD_SLICE_INVALID_LAYOUT) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.copyFrom(src) element type mismatch");
    }
    if (copy_status != XR_POD_SLICE_OK) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.copyFrom(src) range out of bounds");
    }
    vmbreak;
}

vmcase(OP_SLICE_FILL) {
    int a = GETARG_A(i);
    void *span_data = NULL;
    int64_t span_length = 0;
    uint8_t span_elem_type = XR_ELEM_ANY;
    uint16_t span_elem_size = 0;
    uint8_t span_elem_tid = 0;
    uint8_t span_contains_refs = 0;
    uint32_t span_reserved = 0;
    void *span_guard = NULL;
    XrValue fill_value = R(a + 1);
    VM_SLICE_VIEW(R(a), span_data, span_length, span_elem_type, span_elem_size, span_elem_tid,
                  span_contains_refs, span_reserved, span_guard,
                  "Slice.fill(value) receiver must be Slice");
    (void) span_elem_tid;
    (void) span_contains_refs;
    (void) span_guard;
    if (span_elem_type == XR_ELEM_ANY || span_elem_type >= XR_ELEM_COUNT || span_elem_size == 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.fill(value) receiver must be POD Slice");
    }
    if (span_reserved & XR_SLICE_VIEW_READONLY) {
        VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, "cannot write through readonly Slice");
    }
    XrPodSliceFillValue owner_value = {0};
    XrPodSliceFillKind owner_kind = (XrPodSliceFillKind) (span_elem_type - XR_ELEM_I8);
    if (span_elem_type >= XR_ELEM_I8 && span_elem_type <= XR_ELEM_U64) {
        owner_value.bits = (uint64_t) xr_value_to_int64_coerce(fill_value);
    } else if (span_elem_type == XR_ELEM_F32 || span_elem_type == XR_ELEM_F64) {
        owner_value.f64 = xr_value_to_f64_coerce(fill_value);
    } else if (span_elem_type == XR_ELEM_BOOL) {
        bool falsy = XR_IS_FALSE(fill_value) || XR_IS_NULL(fill_value) ||
                     (XR_IS_INT(fill_value) && XR_TO_INT(fill_value) == 0) ||
                     (XR_IS_FLOAT(fill_value) && XR_TO_FLOAT(fill_value) == 0.0);
        owner_value.bits = falsy ? 0 : 1;
    } else if (span_elem_type == XR_ELEM_RUNE) {
        if (!XR_IS_RUNE(fill_value)) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.fill(value) value type mismatch");
        }
        owner_value.bits = XR_TO_RUNE(fill_value);
    } else if (span_elem_type == XR_ELEM_RAWPTR) {
        owner_value.ptr = (void *) (intptr_t) xr_value_to_int64_coerce(fill_value);
    }
    XrPodSliceStatus fill_status = XR_POD_SLICE_FILL_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_HI, XR_SEM_OWNER_ID_SHARED_POD_SLICE_FILL_LO,
        XR_SEM_CONSUMER_VM,
        xr_pod_slice_fill_core(span_data, span_length, span_elem_size, owner_kind, owner_value));
    if (fill_status == XR_POD_SLICE_BYTE_LENGTH_OVERFLOW) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.fill(value) byte length overflow");
    }
    if (fill_status == XR_POD_SLICE_INVALID_LAYOUT) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.fill(value) element layout mismatch");
    }
    if (fill_status != XR_POD_SLICE_OK) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.fill(value) range out of bounds");
    }
    vmbreak;
}

vmcase(OP_SLICE_COMPARE) {
    int a = GETARG_A(i);
    void *left_data = NULL;
    void *right_data = NULL;
    int64_t left_length = 0;
    int64_t right_length = 0;
    uint8_t left_elem_type = XR_ELEM_ANY;
    uint8_t right_elem_type = XR_ELEM_ANY;
    uint16_t left_elem_size = 0;
    uint16_t right_elem_size = 0;
    uint8_t left_elem_tid = 0;
    uint8_t right_elem_tid = 0;
    uint8_t left_contains_refs = 0;
    uint8_t right_contains_refs = 0;
    uint32_t left_reserved = 0;
    uint32_t right_reserved = 0;
    void *left_guard = NULL;
    void *right_guard = NULL;
    VM_SLICE_VIEW(R(a), left_data, left_length, left_elem_type, left_elem_size, left_elem_tid,
                  left_contains_refs, left_reserved, left_guard,
                  "Slice.compare(other) receiver must be Slice");
    VM_SLICE_VIEW(R(a + 1), right_data, right_length, right_elem_type, right_elem_size,
                  right_elem_tid, right_contains_refs, right_reserved, right_guard,
                  "Slice.compare(other) operand must be Slice");
    (void) left_elem_tid;
    (void) right_elem_tid;
    (void) left_contains_refs;
    (void) right_contains_refs;
    (void) left_reserved;
    (void) right_reserved;
    (void) left_guard;
    (void) right_guard;
    if (left_elem_type == XR_ELEM_ANY || left_elem_type >= XR_ELEM_COUNT || left_elem_size == 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.compare(other) receiver must be POD Slice");
    }
    if (right_elem_type == XR_ELEM_ANY || right_elem_type >= XR_ELEM_COUNT ||
        right_elem_size == 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.compare(other) operand must be POD Slice");
    }
    if (left_elem_type != right_elem_type || left_elem_size != right_elem_size) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.compare(other) element type mismatch");
    }
    XrPodSliceCompareResult compare_result = XR_POD_SLICE_COMPARE_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_HI, XR_SEM_OWNER_ID_SHARED_POD_SLICE_COMPARE_LO,
        XR_SEM_CONSUMER_VM,
        xr_pod_slice_compare_core(left_data, left_length, left_elem_size, right_data, right_length,
                                  right_elem_size));
    if (compare_result.status == XR_POD_SLICE_BYTE_LENGTH_OVERFLOW) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice.compare(other) byte length overflow");
    }
    if (compare_result.status == XR_POD_SLICE_INVALID_LAYOUT) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.compare(other) element type mismatch");
    }
    if (compare_result.status != XR_POD_SLICE_OK) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice.compare(other) span has no data");
    }
    R(a) = xr_int(compare_result.ordering);
    vmbreak;
}

vmcase(OP_SLICE_REINTERPRET) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    if (!XR_IS_INT(R(c)) || !XR_IS_INT(R(c + 1)) || !XR_IS_INT(R(c + 2)) || !XR_IS_INT(R(c + 3)) ||
        !XR_IS_INT(R(c + 4)) || !XR_IS_INT(R(c + 5))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_MISSING_METADATA_MSG);
    }
    uint8_t target_elem_type = (uint8_t) XR_TO_INT(R(c + 1));
    uint16_t target_elem_size = (uint16_t) XR_TO_INT(R(c + 2));
    uint8_t target_elem_tid = (uint8_t) XR_TO_INT(R(c + 3));
    uint16_t target_alignment = (uint16_t) XR_TO_INT(R(c + 4));
    uint64_t target_layout_key = (uint64_t) XR_TO_INT(R(c + 5));
    XrAggregateLayout *target_layout =
        xr_struct_layout_lookup_by_stable_key(&isolate->vm, target_layout_key);
    bool target_is_aggregate = target_elem_type == XR_ELEM_ANY && target_layout != NULL;
    bool target_layout_valid = target_elem_type < XR_ELEM_COUNT &&
                               (target_elem_type != XR_ELEM_ANY || target_is_aggregate);
    uint16_t target_expected_elem_size =
        target_elem_type < XR_ELEM_COUNT && target_elem_type != XR_ELEM_ANY
            ? XR_ELEM_SIZES[target_elem_type]
            : target_elem_size;
    void *data = NULL;
    int64_t length = 0;
    uint8_t elem_type = XR_ELEM_ANY;
    uint16_t elem_size = 0;
    uint8_t elem_tid = 0;
    uint8_t contains_refs = 0;
    uint32_t reserved = 0;
    void *guard = NULL;
    VM_SLICE_VIEW(R(b), data, length, elem_type, elem_size, elem_tid, contains_refs, reserved,
                  guard, XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_EXPECTS_MSG);
    (void) elem_tid;
    (void) contains_refs;
    XrPodSliceViewResult view_result = XR_POD_SLICE_VIEW_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_HI, XR_SEM_OWNER_ID_SHARED_POD_SLICE_VIEW_LO,
        XR_SEM_CONSUMER_VM,
        xr_pod_slice_view_core(
            XR_POD_SLICE_VIEW_REINTERPRET, data, length, elem_type == XR_ELEM_U8 ? elem_size : 0,
            elem_type == XR_ELEM_U8 && elem_size == 1, target_elem_size, target_expected_elem_size,
            target_alignment, target_layout_valid, target_is_aggregate));
    if (view_result.status == XR_POD_SLICE_VIEW_INVALID_SOURCE_LAYOUT) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_RECEIVER_MSG);
    }
    if (view_result.status == XR_POD_SLICE_VIEW_INVALID_TARGET_LAYOUT) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_REQUIRES_POD_MSG);
    }
    if (view_result.status == XR_POD_SLICE_VIEW_TARGET_SIZE_MISMATCH) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_METADATA_MISMATCH_MSG);
    }
    if (view_result.status == XR_POD_SLICE_VIEW_BYTE_LENGTH_OVERFLOW) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                         XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_OVERFLOW_MSG);
    }
    if (view_result.status == XR_POD_SLICE_VIEW_LENGTH_NOT_DIVISIBLE) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                         XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_DIVISIBLE_MSG);
    }
    if (view_result.status != XR_POD_SLICE_VIEW_OK) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_REINTERPRET_MISALIGNED_MSG);
    }
    XrSliceView *span = VM_SLICE_SLOT(R(c));
    span->data = view_result.data;
    span->length = view_result.length;
    R(a) = xr_span_ref_typed(span, target_elem_type, target_elem_size, target_elem_tid,
                             target_layout ? target_layout->layout_id : 0,
                             (uint8_t) (reserved & XR_SLICE_VIEW_READONLY));
    vmbreak;
}

vmcase(OP_BYTE_ARRAY_APPEND_FROM) {
    int a = GETARG_A(i);
    if (!XR_IS_ARRAY(R(a))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_OPERANDS_MSG);
    }
    XrArray *dst = XR_TO_ARRAY(R(a));
    void *src_data = NULL;
    int64_t src_length = 0;
    uint8_t src_elem_type = XR_ELEM_ANY;
    uint16_t src_elem_size = 0;
    uint8_t src_elem_tid = 0;
    uint8_t src_contains_refs = 0;
    uint32_t src_reserved = 0;
    void *src_guard = NULL;
    VM_SLICE_VIEW(R(a + 1), src_data, src_length, src_elem_type, src_elem_size, src_elem_tid,
                  src_contains_refs, src_reserved, src_guard,
                  XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_EXPECTS_MSG);
    (void) src_elem_size;
    (void) src_elem_tid;
    (void) src_contains_refs;
    (void) src_reserved;
    if (dst->elem_type != XR_ELEM_U8 || src_elem_type != XR_ELEM_U8) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_OPERANDS_MSG);
    }
    XrByteArrayAppendResult append_result = XR_BYTE_ARRAY_APPEND_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_APPEND_HI, XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_APPEND_LO,
        XR_SEM_CONSUMER_VM,
        xr_byte_array_append_from_span_adapter(dst, src_data, src_length, src_elem_type,
                                               src_guard));
    if (append_result.status != XR_BYTE_ARRAY_APPEND_OK) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_ARRAY_APPEND_FROM_OOB_MSG);
    }
    vmbreak;
}

#undef VM_SLICE_VIEW

vmcase(OP_ARRAY_CLEAR) {
    int a = GETARG_A(i);
    if (!XR_IS_ARRAY(R(a))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Array.clear() expects an array receiver");
    }
    xr_array_clear(XR_TO_ARRAY(R(a)));
    vmbreak;
}

vmcase(OP_ARRAY_RESERVE) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    if (!XR_IS_ARRAY(R(a)) || !XR_IS_INT(R(b))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Array.reserve(capacity) expects an integer");
    }
    if (!xr_array_reserve(XR_TO_ARRAY(R(a)), (int32_t) XR_TO_INT(R(b)))) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "Array.reserve failed");
    }
    vmbreak;
}

vmcase(OP_ARRAY_RESIZE) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    if (!XR_IS_ARRAY(R(a)) || !XR_IS_INT(R(b))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Array.resize(length, fill) expects integer length");
    }
    XrArray *_resize_arr = XR_TO_ARRAY(R(a));
    VM_ARRAY_CHECK_STORABLE(_resize_arr, R(c));
    if (!xr_array_resize(_resize_arr, XR_TO_INT(R(b)), R(c))) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "Array.resize failed");
    }
    vmbreak;
}

#define VM_PARSE_ENDIAN_ARG(value, out_endian)                                                     \
    do {                                                                                           \
        XrValue _endian_value = (value);                                                           \
        if (XR_IS_INT(_endian_value)) {                                                            \
            (out_endian) = XR_TO_INT(_endian_value);                                               \
        } else if (xr_value_is_enum_aggregate(_endian_value)) {                                    \
            XrEnumAggregateValue *_endian_enum = xr_value_to_enum_aggregate(_endian_value);        \
            (out_endian) = (int64_t) _endian_enum->member_index;                                   \
        } else {                                                                                   \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_ENDIAN_EXPECTS_MSG);   \
        }                                                                                          \
        if ((out_endian) < XR_ENDIAN_NATIVE || (out_endian) > XR_ENDIAN_BE) {                      \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid Endian value");                        \
        }                                                                                          \
    } while (0)

/* OP_PTR_LOAD/STORE are emitted only after the analyzer has proven Endian.
 * Trust that bytecode contract instead of adding a recoverable type/range
 * error to an otherwise unchecked unsafe operation. */
#define VM_RAW_ENDIAN_ARG(value, out_endian)                                                       \
    do {                                                                                           \
        XrValue _endian_value = (value);                                                           \
        (out_endian) =                                                                             \
            XR_IS_INT(_endian_value)                                                               \
                ? XR_TO_INT(_endian_value)                                                         \
                : (int64_t) ((XrEnumAggregateValue *) XR_TO_PTR(_endian_value))->member_index;     \
    } while (0)

#define VM_BYTE_SLICE_VIEW(value, out_data, out_length, out_readonly, out_elem_type, message)      \
    do {                                                                                           \
        XrValue _span_value = (value);                                                             \
        if (XR_IS_SLICE_REF(_span_value)) {                                                        \
            XrSliceView *_span = XR_TO_SLICE_REF(_span_value);                                     \
            if (!_span || XR_SLICE_REF_ELEM_TYPE(_span_value) != XR_ELEM_U8) {                     \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, (message));                                 \
            }                                                                                      \
            (out_data) = _span->data;                                                              \
            (out_length) = _span->length;                                                          \
            (out_readonly) = XR_SLICE_REF_IS_READONLY(_span_value);                                \
            (out_elem_type) = XR_SLICE_REF_ELEM_TYPE(_span_value);                                 \
        } else if (XR_IS_ARRAY(_span_value)) {                                                     \
            XrArray *_arr = XR_TO_ARRAY(_span_value);                                              \
            if (!_arr || _arr->elem_type != XR_ELEM_U8) {                                          \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, (message));                                 \
            }                                                                                      \
            (out_data) = _arr->data;                                                               \
            (out_length) = _arr->length;                                                           \
            (out_readonly) = false;                                                                \
            (out_elem_type) = _arr->elem_type;                                                     \
        } else {                                                                                   \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, (message));                                     \
        }                                                                                          \
    } while (0)

#define VM_BYTE_SLICE_LOAD_CASE(opcode, load_fn, receiver_msg, oob_msg)                            \
    vmcase(opcode) {                                                                               \
        int a = GETARG_A(i);                                                                       \
        XrValue _recv = R(a + 1);                                                                  \
        XrValue _offset = R(a + 2);                                                                \
        if (!XR_IS_INT(_offset)) {                                                                 \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                                 \
                             XR_ERROR_CORE_BYTE_SLICE_LOAD_OFFSET_EXPECTS_MSG);                    \
        }                                                                                          \
        int64_t _endian = XR_ENDIAN_NATIVE;                                                        \
        VM_PARSE_ENDIAN_ARG(R(a + 3), _endian);                                                    \
        void *_data = NULL;                                                                        \
        int64_t _length = 0;                                                                       \
        bool _readonly = false;                                                                    \
        uint8_t _elem_type = XR_ELEM_ANY;                                                          \
        VM_BYTE_SLICE_VIEW(_recv, _data, _length, _readonly, _elem_type, receiver_msg);            \
        (void) _readonly;                                                                          \
        bool _ok = false;                                                                          \
        uint64_t _value = (uint64_t) XR_BYTE_SLICE_SCALAR_OWNER_APPLY(                             \
            XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_HI,                                           \
            XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_LO, XR_SEM_CONSUMER_VM,                       \
            load_fn(_data, _length, _elem_type, XR_TO_INT(_offset), _endian, &_ok));               \
        if (!_ok) {                                                                                \
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, oob_msg);                                 \
        }                                                                                          \
        R(a) = xr_int((xr_Integer) _value);                                                        \
        vmbreak;                                                                                   \
    }

#define VM_BYTE_SLICE_STORE_CASE(opcode, store_fn, value_type, receiver_msg, oob_msg)              \
    vmcase(opcode) {                                                                               \
        int a = GETARG_A(i);                                                                       \
        XrValue _recv = R(a + 1);                                                                  \
        XrValue _offset = R(a + 2);                                                                \
        XrValue _value = R(a + 3);                                                                 \
        if (!XR_IS_INT(_offset) || !XR_IS_INT(_value)) {                                           \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                                 \
                             XR_ERROR_CORE_BYTE_SLICE_STORE_VALUE_EXPECTS_MSG);                    \
        }                                                                                          \
        int64_t _endian = XR_ENDIAN_NATIVE;                                                        \
        VM_PARSE_ENDIAN_ARG(R(a + 4), _endian);                                                    \
        void *_data = NULL;                                                                        \
        int64_t _length = 0;                                                                       \
        bool _readonly = false;                                                                    \
        uint8_t _elem_type = XR_ELEM_ANY;                                                          \
        VM_BYTE_SLICE_VIEW(_recv, _data, _length, _readonly, _elem_type, receiver_msg);            \
        if (_readonly) {                                                                           \
            VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, XR_ERROR_CORE_BYTE_SLICE_READONLY_MSG);      \
        }                                                                                          \
        bool _ok = XR_BYTE_SLICE_SCALAR_OWNER_APPLY(                                               \
            XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_HI,                                           \
            XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_LO, XR_SEM_CONSUMER_VM,                       \
            store_fn(_data, _length, _elem_type, XR_TO_INT(_offset),                               \
                     (value_type) XR_TO_INT(_value), _endian));                                    \
        if (!_ok) {                                                                                \
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, oob_msg);                                 \
        }                                                                                          \
        R(a) = xr_null();                                                                          \
        vmbreak;                                                                                   \
    }

#define VM_BYTE_SLICE_LOAD_FLOAT_CASE(opcode, load_fn, receiver_msg, oob_msg)                      \
    vmcase(opcode) {                                                                               \
        int a = GETARG_A(i);                                                                       \
        XrValue _recv = R(a + 1);                                                                  \
        XrValue _offset = R(a + 2);                                                                \
        if (!XR_IS_INT(_offset)) {                                                                 \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                                 \
                             XR_ERROR_CORE_BYTE_SLICE_LOAD_OFFSET_EXPECTS_MSG);                    \
        }                                                                                          \
        int64_t _endian = XR_ENDIAN_NATIVE;                                                        \
        VM_PARSE_ENDIAN_ARG(R(a + 3), _endian);                                                    \
        void *_data = NULL;                                                                        \
        int64_t _length = 0;                                                                       \
        bool _readonly = false;                                                                    \
        uint8_t _elem_type = XR_ELEM_ANY;                                                          \
        VM_BYTE_SLICE_VIEW(_recv, _data, _length, _readonly, _elem_type, receiver_msg);            \
        (void) _readonly;                                                                          \
        bool _ok = false;                                                                          \
        double _value = (double) XR_BYTE_SLICE_SCALAR_OWNER_APPLY(                                 \
            XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_HI,                                           \
            XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_LO, XR_SEM_CONSUMER_VM,                       \
            load_fn(_data, _length, _elem_type, XR_TO_INT(_offset), _endian, &_ok));               \
        if (!_ok) {                                                                                \
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, oob_msg);                                 \
        }                                                                                          \
        R(a) = xr_float(_value);                                                                   \
        vmbreak;                                                                                   \
    }

#define VM_BYTE_SLICE_STORE_FLOAT_CASE(opcode, store_fn, value_type, receiver_msg, oob_msg)        \
    vmcase(opcode) {                                                                               \
        int a = GETARG_A(i);                                                                       \
        XrValue _recv = R(a + 1);                                                                  \
        XrValue _offset = R(a + 2);                                                                \
        XrValue _value = R(a + 3);                                                                 \
        if (!XR_IS_INT(_offset) || !XR_IS_FLOAT(_value)) {                                         \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                                 \
                             XR_ERROR_CORE_BYTE_SLICE_STORE_FLOAT_VALUE_EXPECTS_MSG);              \
        }                                                                                          \
        int64_t _endian = XR_ENDIAN_NATIVE;                                                        \
        VM_PARSE_ENDIAN_ARG(R(a + 4), _endian);                                                    \
        void *_data = NULL;                                                                        \
        int64_t _length = 0;                                                                       \
        bool _readonly = false;                                                                    \
        uint8_t _elem_type = XR_ELEM_ANY;                                                          \
        VM_BYTE_SLICE_VIEW(_recv, _data, _length, _readonly, _elem_type, receiver_msg);            \
        if (_readonly) {                                                                           \
            VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, XR_ERROR_CORE_BYTE_SLICE_READONLY_MSG);      \
        }                                                                                          \
        bool _ok = XR_BYTE_SLICE_SCALAR_OWNER_APPLY(                                               \
            XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_HI,                                           \
            XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_SCALAR_LO, XR_SEM_CONSUMER_VM,                       \
            store_fn(_data, _length, _elem_type, XR_TO_INT(_offset),                               \
                     (value_type) XR_TO_FLOAT(_value), _endian));                                  \
        if (!_ok) {                                                                                \
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, oob_msg);                                 \
        }                                                                                          \
        R(a) = xr_null();                                                                          \
        vmbreak;                                                                                   \
    }

VM_BYTE_SLICE_LOAD_CASE(OP_BYTE_SLICE_LOAD_U16, xr_array_core_bytes_load_u16,
                        XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_RECEIVER_MSG,
                        XR_ERROR_CORE_BYTE_SLICE_LOAD_U16_OOB_MSG)
VM_BYTE_SLICE_LOAD_CASE(OP_BYTE_SLICE_LOAD_U32, xr_array_core_bytes_load_u32,
                        XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_RECEIVER_MSG,
                        XR_ERROR_CORE_BYTE_SLICE_LOAD_U32_OOB_MSG)
VM_BYTE_SLICE_LOAD_CASE(OP_BYTE_SLICE_LOAD_U64, xr_array_core_bytes_load_u64,
                        XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_RECEIVER_MSG,
                        XR_ERROR_CORE_BYTE_SLICE_LOAD_U64_OOB_MSG)
VM_BYTE_SLICE_LOAD_FLOAT_CASE(OP_BYTE_SLICE_LOAD_F32, xr_array_core_bytes_load_f32,
                              XR_ERROR_CORE_BYTE_SLICE_LOAD_F32_RECEIVER_MSG,
                              XR_ERROR_CORE_BYTE_SLICE_LOAD_F32_OOB_MSG)
VM_BYTE_SLICE_LOAD_FLOAT_CASE(OP_BYTE_SLICE_LOAD_F64, xr_array_core_bytes_load_f64,
                              XR_ERROR_CORE_BYTE_SLICE_LOAD_F64_RECEIVER_MSG,
                              XR_ERROR_CORE_BYTE_SLICE_LOAD_F64_OOB_MSG)
VM_BYTE_SLICE_STORE_CASE(OP_BYTE_SLICE_STORE_U16, xr_array_core_bytes_store_u16, uint16_t,
                         XR_ERROR_CORE_BYTE_SLICE_STORE_U16_RECEIVER_MSG,
                         XR_ERROR_CORE_BYTE_SLICE_STORE_U16_OOB_MSG)
VM_BYTE_SLICE_STORE_CASE(OP_BYTE_SLICE_STORE_U32, xr_array_core_bytes_store_u32, uint32_t,
                         XR_ERROR_CORE_BYTE_SLICE_STORE_U32_RECEIVER_MSG,
                         XR_ERROR_CORE_BYTE_SLICE_STORE_U32_OOB_MSG)
VM_BYTE_SLICE_STORE_CASE(OP_BYTE_SLICE_STORE_U64, xr_array_core_bytes_store_u64, uint64_t,
                         XR_ERROR_CORE_BYTE_SLICE_STORE_U64_RECEIVER_MSG,
                         XR_ERROR_CORE_BYTE_SLICE_STORE_U64_OOB_MSG)
VM_BYTE_SLICE_STORE_FLOAT_CASE(OP_BYTE_SLICE_STORE_F32, xr_array_core_bytes_store_f32, float,
                               XR_ERROR_CORE_BYTE_SLICE_STORE_F32_RECEIVER_MSG,
                               XR_ERROR_CORE_BYTE_SLICE_STORE_F32_OOB_MSG)
VM_BYTE_SLICE_STORE_FLOAT_CASE(OP_BYTE_SLICE_STORE_F64, xr_array_core_bytes_store_f64, double,
                               XR_ERROR_CORE_BYTE_SLICE_STORE_F64_RECEIVER_MSG,
                               XR_ERROR_CORE_BYTE_SLICE_STORE_F64_OOB_MSG)

#undef VM_BYTE_SLICE_STORE_FLOAT_CASE
#undef VM_BYTE_SLICE_LOAD_FLOAT_CASE
#undef VM_BYTE_SLICE_STORE_CASE
#undef VM_BYTE_SLICE_LOAD_CASE

vmcase(OP_BYTE_SLICE_FILL) {
    int a = GETARG_A(i);
    void *dst_data = NULL;
    int64_t dst_length = 0;
    bool dst_readonly = false;
    uint8_t dst_elem_type = XR_ELEM_ANY;
    VM_BYTE_SLICE_VIEW(R(a), dst_data, dst_length, dst_readonly, dst_elem_type,
                       XR_ERROR_CORE_BYTE_SLICE_FILL_RECEIVER_MSG);
    if (dst_readonly) {
        VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, XR_ERROR_CORE_BYTE_SLICE_READONLY_MSG);
    }
    if (!XR_IS_INT(R(a + 1))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_FILL_VALUE_EXPECTS_MSG);
    }
    if (!XR_BYTE_SLICE_FILL_OWNER_APPLY(
            XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_FILL_HI, XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_FILL_LO,
            XR_SEM_CONSUMER_VM,
            xr_byte_slice_fill_core(dst_data, dst_length, dst_elem_type, XR_TO_INT(R(a + 1))))) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_FILL_OOB_MSG);
    }
    vmbreak;
}

vmcase(OP_BYTE_SLICE_COPY) {
    int a = GETARG_A(i);
    void *dst_data = NULL;
    void *src_data = NULL;
    int64_t dst_length = 0;
    int64_t src_length = 0;
    bool dst_readonly = false;
    bool src_readonly = false;
    uint8_t dst_elem_type = XR_ELEM_ANY;
    uint8_t src_elem_type = XR_ELEM_ANY;
    VM_BYTE_SLICE_VIEW(R(a), dst_data, dst_length, dst_readonly, dst_elem_type,
                       XR_ERROR_CORE_BYTE_SLICE_COPY_RECEIVER_MSG);
    VM_BYTE_SLICE_VIEW(R(a + 1), src_data, src_length, src_readonly, src_elem_type,
                       XR_ERROR_CORE_BYTE_SLICE_COPY_SOURCE_MSG);
    (void) src_readonly;
    if (dst_readonly) {
        VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, XR_ERROR_CORE_BYTE_SLICE_READONLY_MSG);
    }
    if (!XR_BYTE_SLICE_COPY_OWNER_APPLY(
            XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COPY_HI, XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COPY_LO,
            XR_SEM_CONSUMER_VM,
            xr_byte_slice_copy_core(dst_data, dst_length, dst_elem_type, src_data, src_length,
                                    src_elem_type))) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_COPY_OOB_MSG);
    }
    vmbreak;
}

vmcase(OP_BYTE_SLICE_COMPARE) {
    int a = GETARG_A(i);
    void *left_data = NULL;
    void *right_data = NULL;
    int64_t left_length = 0;
    int64_t right_length = 0;
    bool left_readonly = false;
    bool right_readonly = false;
    uint8_t left_elem_type = XR_ELEM_ANY;
    uint8_t right_elem_type = XR_ELEM_ANY;
    VM_BYTE_SLICE_VIEW(R(a), left_data, left_length, left_readonly, left_elem_type,
                       XR_ERROR_CORE_BYTE_SLICE_COMPARE_RECEIVER_MSG);
    VM_BYTE_SLICE_VIEW(R(a + 1), right_data, right_length, right_readonly, right_elem_type,
                       XR_ERROR_CORE_BYTE_SLICE_COMPARE_OPERAND_MSG);
    (void) left_readonly;
    (void) right_readonly;
    bool ok = false;
    int64_t ordering = XR_BYTE_SLICE_COMPARE_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_HI, XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMPARE_LO,
        XR_SEM_CONSUMER_VM,
        xr_byte_slice_compare_core(left_data, left_length, left_elem_type, right_data, right_length,
                                   right_elem_type, &ok));
    if (!ok) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_COMPARE_NO_DATA_MSG);
    }
    R(a) = xr_int(ordering);
    vmbreak;
}

vmcase(OP_BYTE_SLICE_COMMON_PREFIX) {
    int a = GETARG_A(i);
    void *left_data = NULL;
    void *right_data = NULL;
    int64_t left_length = 0;
    int64_t right_length = 0;
    bool left_readonly = false;
    bool right_readonly = false;
    uint8_t left_elem_type = XR_ELEM_ANY;
    uint8_t right_elem_type = XR_ELEM_ANY;
    VM_BYTE_SLICE_VIEW(R(a), left_data, left_length, left_readonly, left_elem_type,
                       XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_RECEIVER_MSG);
    VM_BYTE_SLICE_VIEW(R(a + 1), right_data, right_length, right_readonly, right_elem_type,
                       XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_OPERAND_MSG);
    (void) left_readonly;
    (void) right_readonly;
    bool ok = false;
    int64_t prefix = XR_BYTE_SLICE_COMMON_PREFIX_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_HI,
        XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_COMMON_PREFIX_LO, XR_SEM_CONSUMER_VM,
        xr_byte_slice_common_prefix_core(left_data, left_length, left_elem_type, right_data,
                                         right_length, right_elem_type, &ok));
    if (!ok) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_COMMON_PREFIX_NO_DATA_MSG);
    }
    R(a) = xr_int(prefix);
    vmbreak;
}

vmcase(OP_BYTE_SLICE_REPEAT) {
    int a = GETARG_A(i);
    void *data = NULL;
    int64_t length = 0;
    bool readonly = false;
    uint8_t elem_type = XR_ELEM_ANY;
    VM_BYTE_SLICE_VIEW(R(a), data, length, readonly, elem_type,
                       XR_ERROR_CORE_BYTE_SLICE_REPEAT_RECEIVER_MSG);
    if (readonly) {
        VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, XR_ERROR_CORE_BYTE_SLICE_READONLY_MSG);
    }
    if (!XR_IS_INT(R(a + 1)) || !XR_IS_INT(R(a + 2)) || !XR_IS_INT(R(a + 3))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_SLICE_REPEAT_INTS_EXPECTS_MSG);
    }
    if (!XR_BYTE_SLICE_REPEAT_OWNER_APPLY(
            XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_REPEAT_HI,
            XR_SEM_OWNER_ID_SHARED_BYTE_SLICE_REPEAT_LO, XR_SEM_CONSUMER_VM,
            xr_byte_slice_repeat_core(data, length, elem_type, XR_TO_INT(R(a + 1)),
                                      XR_TO_INT(R(a + 2)), XR_TO_INT(R(a + 3))))) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_SLICE_REPEAT_OOB_MSG);
    }
    vmbreak;
}

#undef VM_BYTE_SLICE_VIEW

/* Call-bound place descriptors use an absolute stack-slot index instead of a
 * raw pointer, so VM stack growth cannot invalidate an active ref/out borrow. */
vmcase(OP_LOCAL_ADDR) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    R(a) = (XrValue) {0};
    R(a).tag = XR_TAG_PLACE;
    R(a).i = (int64_t) ((base + b) - vm_ctx->stack);
    vmbreak;
}

vmcase(OP_PLACE_LOAD) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    if (!XR_IS_PLACE(R(b)) || R(b).i < 0 || R(b).i >= vm_ctx->stack_capacity) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid call-bound place load");
    }
    R(a) = vm_ctx->stack[R(b).i];
    vmbreak;
}

vmcase(OP_PLACE_STORE) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    if (!XR_IS_PLACE(R(a)) || R(a).i < 0 || R(a).i >= vm_ctx->stack_capacity) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid call-bound place store");
    }
    vm_ctx->stack[R(a).i] = R(b);
    vmbreak;
}

/* FFI raw-pointer access. A is a contiguous register window holding
 * result/address/endian for load and address/value/endian for store. B carries
 * the XrFFIType/flags byte. No bounds/null check (unsafe). */
vmcase(OP_PTR_LOAD) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int64_t endian = XR_ENDIAN_NATIVE;
    VM_RAW_ENDIAN_ARG(R(a + 2), endian);
    R(a) = xr_ffi_ptr_load((uintptr_t) (intptr_t) XR_TO_INT(R(a + 1)), (uint8_t) b, endian);
    vmbreak;
}

vmcase(OP_PTR_STORE) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int64_t endian = XR_ENDIAN_NATIVE;
    VM_RAW_ENDIAN_ARG(R(a + 2), endian);
    xr_ffi_ptr_store((uintptr_t) (intptr_t) XR_TO_INT(R(a)), (uint8_t) b, R(a + 1), endian);
    vmbreak;
}

#undef VM_PARSE_ENDIAN_ARG
#undef VM_RAW_ENDIAN_ARG

vmcase(OP_PTR_COPY_NONOVERLAP) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    (void) XR_RAW_MEMORY_COPY_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_HI, XR_SEM_OWNER_ID_SHARED_RAW_MEMORY_COPY_LO,
        XR_SEM_CONSUMER_VM, (void *) (uintptr_t) (intptr_t) XR_TO_INT(R(a)),
        (const void *) (uintptr_t) (intptr_t) XR_TO_INT(R(b)), XR_TO_INT(R(c)));
    vmbreak;
}

vmcase(OP_BYTE_ARRAY_COPY_WITHIN) {
    int a = GETARG_A(i);
    if (!XR_IS_ARRAY(R(a)) || !XR_IS_INT(R(a + 1)) || !XR_IS_INT(R(a + 2)) ||
        !XR_IS_INT(R(a + 3))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_EXPECTS_MSG);
    }
    XrArray *arr = XR_TO_ARRAY(R(a));
    XrByteArrayCopyResult copy_result = XR_BYTE_ARRAY_COPY_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_HI, XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_LO,
        XR_SEM_CONSUMER_VM,
        xr_byte_array_copy_core(XR_BYTE_ARRAY_COPY_WITHIN, arr->data, arr->length, arr->elem_type,
                                arr->data, arr->length, arr->elem_type, XR_TO_INT(R(a + 2)),
                                XR_TO_INT(R(a + 1)), XR_TO_INT(R(a + 3))));
    if (copy_result.status == XR_BYTE_ARRAY_COPY_WRONG_ELEMENT_TYPE)
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_RECEIVER_MSG);
    if (copy_result.status != XR_BYTE_ARRAY_COPY_OK) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_ARRAY_COPY_WITHIN_OOB_MSG);
    }
    if (copy_result.changed)
        XR_ARRAY_MARK_MUTATED(arr);
    vmbreak;
}

vmcase(OP_BYTE_ARRAY_COPY_FROM) {
    int a = GETARG_A(i);
    if (!XR_IS_ARRAY(R(a)) || !XR_IS_ARRAY(R(a + 1)) || !XR_IS_INT(R(a + 2)) ||
        !XR_IS_INT(R(a + 3)) || !XR_IS_INT(R(a + 4))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_COPY_FROM_EXPECTS_MSG);
    }
    XrArray *dst = XR_TO_ARRAY(R(a));
    XrArray *src = XR_TO_ARRAY(R(a + 1));
    XrByteArrayCopyResult copy_result = XR_BYTE_ARRAY_COPY_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_HI, XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_COPY_LO,
        XR_SEM_CONSUMER_VM,
        xr_byte_array_copy_core(XR_BYTE_ARRAY_COPY_FROM, dst->data, dst->length, dst->elem_type,
                                src->data, src->length, src->elem_type, XR_TO_INT(R(a + 2)),
                                XR_TO_INT(R(a + 3)), XR_TO_INT(R(a + 4))));
    if (copy_result.status == XR_BYTE_ARRAY_COPY_WRONG_ELEMENT_TYPE)
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_COPY_FROM_OPERANDS_MSG);
    if (copy_result.status != XR_BYTE_ARRAY_COPY_OK) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_ARRAY_COPY_FROM_OOB_MSG);
    }
    if (copy_result.changed)
        XR_ARRAY_MARK_MUTATED(dst);
    vmbreak;
}

vmcase(OP_BYTE_ARRAY_REPEAT_FROM) {
    int a = GETARG_A(i);
    if (!XR_IS_ARRAY(R(a)) || !XR_IS_INT(R(a + 1)) || !XR_IS_INT(R(a + 2))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_EXPECTS_MSG);
    }
    XrArray *arr = XR_TO_ARRAY(R(a));
    if (arr->elem_type != XR_ELEM_U8)
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_RECEIVER_MSG);
    XrByteArrayRepeatResult repeat_result = XR_BYTE_ARRAY_REPEAT_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_REPEAT_HI, XR_SEM_OWNER_ID_SHARED_BYTE_ARRAY_REPEAT_LO,
        XR_SEM_CONSUMER_VM,
        xr_byte_array_repeat_from_tail_adapter(arr, XR_TO_INT(R(a + 1)), XR_TO_INT(R(a + 2))));
    if (repeat_result.status != XR_BYTE_ARRAY_REPEAT_OK) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, XR_ERROR_CORE_BYTE_ARRAY_REPEAT_FROM_OOB_MSG);
    }
    vmbreak;
}

vmcase(OP_ARRAY_INIT) {
    /* OP_ARRAY_INIT: batch initialization
    ** A = array register, B = element count
    ** Elements are in R(A+1) .. R(A+B)
    ** Precondition: array already has capacity >= B (from OP_NEWARRAY)
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrArray *arr = XR_TO_ARRAY(R(a));
    if (arr->elem_type == XR_ELEM_ANY && b > 0) {
        // Fast path: bulk memcpy for ANY arrays
        memcpy(arr->data, &R(a + 1), (size_t) b * sizeof(XrValue));
        arr->length = b;
        // Scan for GC pointers in the batch
        XrValue *src = &R(a + 1);
        for (int j = 0; j < b; j++) {
            if (XR_VALUE_NEEDS_GC(src[j])) {
                arr->contains_refs = 1;
                break;
            }
        }
    } else {
        // Slow path: typed arrays need per-element unboxing
        for (int j = 1; j <= b; j++) {
            VM_ARRAY_CHECK_STORABLE(arr, R(a + j));
            xr_array_set(arr, j - 1, R(a + j));
        }
    }
    vmbreak;
}

/* ========================================================
** Map Operation Instructions
** ======================================================== */

vmcase(OP_MAP_GET) {
    // OP_MAP_GET: Map dynamic key access
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue map_val = R(b);
    XrValue key_val = R(c);

    if (XR_IS_MAP(map_val)) {
        XrMap *map = XR_TO_MAP(map_val);
        bool found;
        R(a) = xr_map_get(map, key_val, &found);
        if (!found)
            R(a) = xr_null();
        vmbreak;
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "Map.get requires Map type");
}

vmcase(OP_MAP_GETK) {
    // OP_MAP_GETK: Map constant key access
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue map_val = R(b);

    if (XR_IS_MAP(map_val)) {
        XrMap *map = XR_TO_MAP(map_val);
        XrValue key_val = k[c];
        XrString *key_str = XR_TO_STRING(key_val);
        bool found;
        XR_MAP_GET_STRING_FAST(map, key_str, R(a), found);
        if (!found)
            VM_RUNTIME_ERROR(XR_ERR_KEY_NOT_FOUND, "Map key not found");
        vmbreak;
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "index access requires Map type");
}

vmcase(OP_MAP_SET) {
    // OP_MAP_SET: Map dynamic key set
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue map_val = R(a);

    if (XR_IS_MAP(map_val)) {
        XrMap *map = XR_TO_MAP(map_val);
        xr_map_set(map, R(b), R(c));
        vmbreak;
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "Map.set requires Map type");
}

vmcase(OP_MAP_SETK) {
    // OP_MAP_SETK: Map constant key set
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue map_val = R(a);

    if (XR_IS_MAP(map_val)) {
        XrMap *map = XR_TO_MAP(map_val);
        XrValue key_val = k[b];
        XrString *key_str = XR_TO_STRING(key_val);
        XR_MAP_SET_STRING_FAST(map, key_str, key_val, R(c));
        vmbreak;
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "index assignment requires Map type");
}

vmcase(OP_MAP_INCREMENT) {
    /* OP_MAP_INCREMENT: Map counter pattern optimization
    ** R[A]:Map[R[B]]++ - if key doesn't exist set to 1, otherwise +1
    ** Replaces: if (map.containsKey(key)) { map[key] = map[key] + 1 } else { map[key] = 1 }
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue map_val = R(a);
    XrValue key = R(b);

    if (XR_IS_MAP(map_val)) {
        XrMap *map = XR_TO_MAP(map_val);
        bool found = false;
        XrValue old_val = xr_map_get(map, key, &found);

        if (found && XR_IS_INT(old_val)) {
            // Exists and is integer, +1
            xr_map_set(map, key, xr_int(XR_TO_INT(old_val) + 1));
        } else {
            // Doesn't exist or not integer, set to 1
            xr_map_set(map, key, xr_int(1));
        }
        vmbreak;
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "Map increment requires Map type");
}

vmcase(OP_SUBSTRING) {
    /* OP_SUBSTRING: inline string substring
    ** R[A] = R[B].substring(R[C], R[C+1])
    **
    ** Layout: B=string (any position), C=start, C+1=end
    **
    ** Optimization: string doesn't need MOVE to contiguous area
    ** Only start/end need to be contiguous, eliminates 1 MOVE
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    XrValue str_val = R(b);
    if (XR_IS_STRING(str_val)) {
        XrString *str = XR_TO_STRING(str_val);
        xr_Integer start = XR_TO_INT(R(c));
        xr_Integer end = XR_TO_INT(R(c + 1));
        XrString *result = xr_string_substring(isolate, str, start, end);
        R(a) = result ? xr_string_value(result) : xr_null();
        vmbreak;
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "substring requires String type");
}

vmcase(OP_STR_REPEAT) {
    /* OP_STR_REPEAT: string repeat
    ** R[A] = R[B] * R[C]
    **
    ** B = string, C = repeat count (integer)
    ** Example: "-" * 50 generates 50 "-"
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    XrValue str_val = R(b);
    XrValue count_val = R(c);

    if (XR_IS_STRING(str_val) && XR_IS_INT(count_val)) {
        XrString *str = XR_TO_STRING(str_val);
        xr_Integer count = XR_TO_INT(count_val);
        XrString *result = xr_string_repeat(isolate, str, count);
        R(a) = result ? xr_string_value(result) : xr_null();
        vmbreak;
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "string repeat requires String * Int");
}

/* ========================================================
** Generic Index Operations
** ======================================================== */

vmcase(OP_INDEX_GET) {
    // OP_INDEX_GET: generic index read
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue obj_val = R(b);
    XrValue key_val = R(c);

    // Fast path: struct inline fixed array ([T; N])
    if (XR_IS_ARRAY_REF(obj_val) && XR_IS_INT(key_val)) {
        uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
        uint32_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        int64_t idx = XR_TO_INT(key_val);
        if ((uint64_t) idx < (uint64_t) ecount) {
            uint8_t *base_ptr = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(xr_target_data_layout_host(), etype);
            uint8_t *ep = base_ptr + idx * es;
            switch (etype) {
                case XR_NATIVE_I64:
                    R(a) = XR_FROM_INT(*(int64_t *) ep);
                    break;
                case XR_NATIVE_U64:
                    R(a) = XR_FROM_INT((int64_t) *(uint64_t *) ep);
                    break;
                case XR_NATIVE_ISIZE:
                    R(a) = XR_FROM_INT((int64_t) *(ptrdiff_t *) ep);
                    break;
                case XR_NATIVE_USIZE:
                    R(a) = XR_FROM_INT((int64_t) *(size_t *) ep);
                    break;
                case XR_NATIVE_F64:
                    R(a) = XR_FROM_FLOAT(*(double *) ep);
                    break;
                case XR_NATIVE_BOOL:
                    R(a) = *(uint8_t *) ep ? XR_TRUE_VAL : XR_FALSE_VAL;
                    break;
                case XR_NATIVE_VALUE:
                    R(a) = *(XrValue *) ep;
                    break;
                case XR_NATIVE_I32:
                    R(a) = XR_FROM_INT((int64_t) *(int32_t *) ep);
                    break;
                case XR_NATIVE_U32:
                    R(a) = XR_FROM_INT((int64_t) *(uint32_t *) ep);
                    break;
                case XR_NATIVE_I16:
                    R(a) = XR_FROM_INT((int64_t) *(int16_t *) ep);
                    break;
                case XR_NATIVE_U16:
                    R(a) = XR_FROM_INT((int64_t) *(uint16_t *) ep);
                    break;
                case XR_NATIVE_I8:
                    R(a) = XR_FROM_INT((int64_t) *(int8_t *) ep);
                    break;
                case XR_NATIVE_U8:
                    R(a) = XR_FROM_INT((int64_t) *(uint8_t *) ep);
                    break;
                case XR_NATIVE_F32:
                    R(a) = XR_FROM_FLOAT((double) *(float *) ep);
                    break;
                default:
                    R(a) = xr_null();
                    break;
            }
        } else {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "fixed array index out of range: %lld (length %u)", (long long) idx,
                             (unsigned) ecount);
        }
        vmbreak;
    }
    // Fast path: frame-local Slice value
    if (XR_IS_SLICE_REF(obj_val) && XR_IS_INT(key_val)) {
        XrSliceView *span = XR_TO_SLICE_REF(obj_val);
        int64_t idx = XR_TO_INT(key_val);
        if (span && (uint64_t) idx < (uint64_t) span->length) {
            R(a) = VM_SLICE_GET_ELEMENT(obj_val, span, idx);
        } else {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "span index out of range: %lld (length %d)", (long long) idx,
                             (int) (span ? span->length : 0));
        }
        vmbreak;
    }
    // Fast path: Array (includes slices — capacity==0 && source!=NULL)
    if (XR_IS_ARRAY(obj_val)) {
        XrArray *arr = XR_TO_ARRAY(obj_val);
        int64_t idx = XR_TO_INT(key_val);
        /* Spec §3 (expr-index-access): an `int` index outside [0, length) — including
         * any negative index — throws E0430. No Python-style from-end wraparound. */
        if ((uint64_t) idx < (uint64_t) arr->length) {
            R(a) = (arr->elem_type == XR_ELEM_ANY) ? ((XrValue *) arr->data)[idx]
                                                   : xr_array_get_element(arr, (int32_t) idx);
        } else {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "array index out of range: %lld (length %d)", (long long) idx,
                             (int) arr->length);
        }
        vmbreak;
    }
    // Fast path: Range (lazy element access)
    if (XR_IS_INT(key_val)) {
        XrRange *rng = xr_value_get_range_body(isolate, obj_val);
        if (rng) {
            int64_t idx = XR_TO_INT(key_val);
            bool ok = false;
            int64_t value = xr_range_core_index(xr_range_core_view(rng), idx, &ok);
            R(a) = ok ? xr_int(value) : xr_null();
            vmbreak;
        }
    }
    // Fast path: Map
    if (XR_IS_MAP(obj_val)) {
        XrMap *map = XR_TO_MAP(obj_val);
        bool found;
        R(a) = xr_map_get(map, key_val, &found);
        if (!found)
            VM_RUNTIME_ERROR(XR_ERR_KEY_NOT_FOUND, "Map key not found");
        vmbreak;
    }
    // Set indexing: materialize values array, then index by position.
    // Enables for-in iteration over sets via INDEX_GET.
    if (XR_IS_SET(obj_val) && XR_IS_INT(key_val)) {
        XrSet *set = XR_TO_SET(obj_val);
        XrArray *vals = xr_set_values(VM_CURRENT_CORO, set);
        int64_t idx = XR_TO_INT(key_val);
        if (vals && (uint64_t) idx < (uint64_t) vals->length) {
            R(a) = (vals->elem_type == XR_ELEM_ANY) ? ((XrValue *) vals->data)[idx]
                                                    : xr_array_get_element(vals, (int32_t) idx);
        } else {
            R(a) = xr_null();
        }
        vmbreak;
    }
    // Operator overload
    if (xr_value_is_instance(obj_val)) {
        XrObjectInstance *_inst = xr_value_to_instance(obj_val);
        XrClass *_cls = xr_instance_get_class(_inst);
        if (XCLASS_HAS_OP(_cls, XR_OP_INDEX_FLAG)) {
            XrMethod *_m = xr_class_lookup_method(_cls, SYMBOL_OP_INDEX);
            if (_m && _m->type == XMETHOD_OPERATOR && _m->as.closure) {
                XrClosure *_cl = _m->as.closure;
                XrProto *_p = _cl->proto;
                VM_STACK_CHECK(a + 1 + _p->maxstacksize);
                R(a + 1) = obj_val;
                R(a + 2) = key_val;
                savepc();
                int _fi = VM_FRAME_COUNT;
                VM_INC_FRAME_COUNT;
                XrBcCallFrame *_nf = &VM_FRAMES[_fi];
                _nf->closure = _cl;
                _nf->pc = PROTO_CODE_BASE(_p);
                _nf->base_offset = (int) ((base + a + 1) - VM_STACK);
                goto startfunc;
            }
        }
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX,
                     "only Array, Map, Set, Range, typed array, and operator[] support indexing");
}

vmcase(OP_INDEX_SET) {
    // OP_INDEX_SET: generic index write
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue obj_val = R(a);
    XrValue key_val = R(b);
    XrValue val = R(c);

    // Fast path: struct inline fixed array ([T; N])
    if (XR_IS_ARRAY_REF(obj_val) && XR_IS_INT(key_val)) {
        uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
        uint32_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        int64_t idx = XR_TO_INT(key_val);
        if ((uint64_t) idx < (uint64_t) ecount) {
            uint8_t *base_ptr = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(xr_target_data_layout_host(), etype);
            uint8_t *ep = base_ptr + idx * es;
            switch (etype) {
                case XR_NATIVE_I64:
                    *(int64_t *) ep = XR_TO_INT(val);
                    break;
                case XR_NATIVE_U64:
                    *(uint64_t *) ep = (uint64_t) XR_TO_INT(val);
                    break;
                case XR_NATIVE_ISIZE:
                    *(ptrdiff_t *) ep = (ptrdiff_t) XR_TO_INT(val);
                    break;
                case XR_NATIVE_USIZE:
                    *(size_t *) ep = (size_t) XR_TO_INT(val);
                    break;
                case XR_NATIVE_F64:
                    *(double *) ep = XR_TO_FLOAT(val);
                    break;
                case XR_NATIVE_BOOL:
                    *(uint8_t *) ep = (uint8_t) val.i;
                    break;
                case XR_NATIVE_VALUE:
                    *(XrValue *) ep = val;
                    break;
                case XR_NATIVE_I32:
                    *(int32_t *) ep = (int32_t) XR_TO_INT(val);
                    break;
                case XR_NATIVE_U32:
                    *(uint32_t *) ep = (uint32_t) XR_TO_INT(val);
                    break;
                case XR_NATIVE_I16:
                    *(int16_t *) ep = (int16_t) XR_TO_INT(val);
                    break;
                case XR_NATIVE_U16:
                    *(uint16_t *) ep = (uint16_t) XR_TO_INT(val);
                    break;
                case XR_NATIVE_I8:
                    *(int8_t *) ep = (int8_t) XR_TO_INT(val);
                    break;
                case XR_NATIVE_U8:
                    *(uint8_t *) ep = (uint8_t) XR_TO_INT(val);
                    break;
                case XR_NATIVE_F32:
                    *(float *) ep = (float) XR_TO_FLOAT(val);
                    break;
                default:
                    break;
            }
        } else {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "fixed array index out of range: %lld (length %u)", (long long) idx,
                             (unsigned) ecount);
        }
        vmbreak;
    }
    // Fast path: frame-local Slice value
    if (XR_IS_SLICE_REF(obj_val) && XR_IS_INT(key_val)) {
        XrSliceView *span = XR_TO_SLICE_REF(obj_val);
        int64_t idx = XR_TO_INT(key_val);
        if (!span || (uint64_t) idx >= (uint64_t) span->length) {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "span index out of range: %lld (length %d)", (long long) idx,
                             (int) (span ? span->length : 0));
        }
        VM_SLICE_CHECK_STORABLE(obj_val, val);
        VM_SLICE_SET_ELEMENT(obj_val, span, idx, val);
        vmbreak;
    }
    // Fast path: Array (includes slices — capacity==0 && source!=NULL)
    if (XR_IS_ARRAY(obj_val)) {
        XrArray *arr = XR_TO_ARRAY(obj_val);
        int64_t idx = XR_TO_INT(key_val);
        /* Index assignment is strict: idx must be in [0, length). Assignment at
         * exactly length is out of bounds (push() is the append API) and
         * negative indexes never wrap from the end (that is slice-only). */
        if ((uint64_t) idx >= (uint64_t) arr->length) {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "array index out of range: %lld (length %d)", (long long) idx,
                             (int) arr->length);
        }
        if (arr->elem_type == XR_ELEM_ANY) {
            ((XrValue *) arr->data)[idx] = val;
            XR_ARRAY_MARK_REFS(arr, val);
        } else {
            VM_ARRAY_CHECK_STORABLE(arr, val);
            xr_array_set_element(arr, (int32_t) idx, val);
        }
        vmbreak;
    }
    // Fast path: Map
    if (XR_IS_MAP(obj_val)) {
        XrMap *map = XR_TO_MAP(obj_val);
        xr_map_set(map, key_val, val);
        vmbreak;
    }
    // Operator overload
    if (xr_value_is_instance(obj_val)) {
        XrObjectInstance *_inst = xr_value_to_instance(obj_val);
        XrClass *_cls = xr_instance_get_class(_inst);
        if (XCLASS_HAS_OP(_cls, XR_OP_INDEX_SET_FLAG)) {
            XrMethod *_m = xr_class_lookup_method(_cls, SYMBOL_OP_INDEX_SET);
            if (_m && _m->type == XMETHOD_OPERATOR && _m->as.closure) {
                XrClosure *_cl = _m->as.closure;
                XrProto *_p = _cl->proto;
                VM_STACK_CHECK(a + 2 + _p->maxstacksize);
                R(a + 2) = obj_val;
                R(a + 3) = key_val;
                R(a + 4) = val;
                savepc();
                int _fi = VM_FRAME_COUNT;
                VM_INC_FRAME_COUNT;
                XrBcCallFrame *_nf = &VM_FRAMES[_fi];
                _nf->closure = _cl;
                _nf->pc = PROTO_CODE_BASE(_p);
                _nf->base_offset = (int) ((base + a + 2) - VM_STACK);
                goto startfunc;
            }
        }
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX,
                     "only Array, Map, and typed array support index assignment");
}

vmcase(OP_SLICE) {
    /* OP_SLICE: slice operation
    ** R[A] = R[B][R[C]:R[C+1]]
    ** - R[B]: source object (Array/String/Array<u8>)
    ** - R[C]: start index (0 = from beginning)
    ** - R[C+1]: end index (large positive value = to end)
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue source = R(b);
    if (!XR_IS_INT(R(c)) || !XR_IS_INT(R(c + 1))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_SLICE_BOUNDS_EXPECTS_MSG);
    }
    int64_t start = XR_TO_INT(R(c));
    int64_t end = XR_TO_INT(R(c + 1));

    // Array slice: frame-local borrowed Slice value (R[C+2] is a slice slot).
    if (XR_IS_ARRAY(source)) {
        XrArray *arr = XR_TO_ARRAY(source);
        if (!XR_IS_INT(R(c + 2))) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "array slice missing Slice frame slot");
        }
        xr_array_normalize_slice(arr->length, &start, &end);
        XrSliceView *span = VM_SLICE_SLOT(R(c + 2));
        span->data = (arr->data && end > start)
                         ? (uint8_t *) arr->data + (size_t) start * arr->elem_size
                         : (uint8_t *) arr->data;
        span->length = end - start;
        R(a) = xr_span_ref_typed(span, arr->elem_type, arr->elem_size, arr->elem_tid, 0,
                                 arr->contains_refs ? XR_SLICE_VIEW_CONTAINS_REFS : 0);
        vmbreak;
    }

    // Frame-local fixed array slice: only target-typed Slice<T> is supported.
    if (XR_IS_ARRAY_REF(source)) {
        if (!XR_IS_INT(R(c + 2))) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                             "fixed array slices can only produce Slice views");
        }
        uint8_t native_type = XR_ARRAY_REF_ELEM_TYPE(source);
        uint32_t elem_count = XR_ARRAY_REF_ELEM_COUNT(source);
        xr_array_normalize_slice(elem_count, &start, &end);
        uint8_t elem_size = xr_native_type_size(xr_target_data_layout_host(), native_type);
        XrSliceView *span = VM_SLICE_SLOT(R(c + 2));
        span->data = (source.ptr && end > start)
                         ? (uint8_t *) source.ptr + (size_t) start * (size_t) elem_size
                         : (uint8_t *) source.ptr;
        span->length = end - start;
        uint8_t slice_elem_type = xr_native_type_to_elem_type(native_type);
        R(a) = xr_span_ref_typed(span, slice_elem_type,
                                 elem_size ? elem_size : (uint8_t) sizeof(XrValue), 0, 0,
                                 slice_elem_type == XR_ELEM_ANY ? XR_SLICE_VIEW_CONTAINS_REFS : 0);
        vmbreak;
    }

    // Slice slice-of-slice: pure pointer arithmetic into another frame-local Slice.
    if (XR_IS_SLICE_REF(source)) {
        XrSliceView *src_span = XR_TO_SLICE_REF(source);
        if (!src_span || !XR_IS_INT(R(c + 2))) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "slice-of-slice missing Slice frame slot");
        }
        xr_array_normalize_slice(src_span->length, &start, &end);
        uint16_t elem_size = XR_SLICE_REF_ELEM_SIZE(source);
        XrSliceView *span = VM_SLICE_SLOT(R(c + 2));
        span->data = (src_span->data && end > start)
                         ? (uint8_t *) src_span->data + (size_t) start * elem_size
                         : (uint8_t *) src_span->data;
        span->length = end - start;
        R(a) = xr_span_ref_typed(
            span, XR_SLICE_REF_ELEM_TYPE(source), elem_size, XR_SLICE_REF_ELEM_TID(source),
            XR_SLICE_REF_LAYOUT_ID(source),
            (uint8_t) (source.flags & (XR_SLICE_VIEW_READONLY | XR_SLICE_VIEW_CONTAINS_REFS)));
        vmbreak;
    }

    // String slice
    if (XR_IS_STRING(source)) {
        XrString *str = XR_TO_STRING(source);
        XrString *slice = xr_string_slice(isolate, str, start, end);
        R(a) = slice ? xr_string_value(slice) : xr_null();
        vmbreak;
    }

    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "this type does not support slicing");
}

vmcase(OP_SLICE_WINDOW) {
    /* Strict count-based borrowed view. Unlike slice syntax this operation
     * never clamps and never interprets negative values from the end. One
     * successful check proves every access inside [0, count), and the shared
     * owner states what that check is; this profile only publishes the
     * rejection as a VM runtime error. */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    if (!XR_IS_SLICE_REF(R(b)) || !XR_IS_INT(R(c)) || !XR_IS_INT(R(c + 1)) ||
        !XR_IS_INT(R(c + 2))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "Slice.window(start, count) expects a Slice and integer bounds");
    }
    XrSliceView *src = XR_TO_SLICE_REF(R(b));
    if (!src) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                         "Slice.window(start, count) is outside the source Slice");
    }
    int64_t start = XR_TO_INT(R(c));
    int64_t count = XR_TO_INT(R(c + 1));
    uint16_t elem_size = XR_SLICE_REF_ELEM_SIZE(R(b));
    XrSliceWindowPlan window = XR_SLICE_WINDOW_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_HI, XR_SEM_OWNER_ID_SHARED_SLICE_WINDOW_LO,
        XR_SEM_CONSUMER_VM, XR_SLICE_WINDOW_PROOF_NONE, src->length, start, count, src->data,
        elem_size);
    if (!window.admitted) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                         "Slice.window(start, count) is outside the source Slice");
    }
    XrSliceView *span = VM_SLICE_SLOT(R(c + 2));
    *span = *src;
    span->length = window.length;
    span->data = window.advances ? (uint8_t *) src->data + (size_t) window.byte_offset
                                 : (uint8_t *) src->data;
    R(a) = xr_span_ref_typed(
        span, XR_SLICE_REF_ELEM_TYPE(R(b)), elem_size, XR_SLICE_REF_ELEM_TID(R(b)),
        XR_SLICE_REF_LAYOUT_ID(R(b)),
        (uint8_t) (R(b).flags & (XR_SLICE_VIEW_READONLY | XR_SLICE_VIEW_CONTAINS_REFS)));
    vmbreak;
}

vmcase(OP_SLICE_FROM_PTR) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    if (!XR_IS_INT(R(b)) || !XR_IS_INT(R(c)) || !XR_IS_INT(R(c + 1)) || !XR_IS_INT(R(c + 2)) ||
        !XR_IS_INT(R(c + 3)) || !XR_IS_INT(R(c + 4)) || !XR_IS_INT(R(c + 5)) ||
        !XR_IS_INT(R(c + 6))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "mem.slice<T>() is missing pointer/count/layout evidence");
    }
    uintptr_t address = (uintptr_t) XR_TO_INT(R(b));
    int64_t count = XR_TO_INT(R(c));
    uint8_t elem_type = (uint8_t) XR_TO_INT(R(c + 2));
    uint16_t elem_size = (uint16_t) XR_TO_INT(R(c + 3));
    uint8_t elem_tid = (uint8_t) XR_TO_INT(R(c + 4));
    uint16_t alignment_flags = (uint16_t) XR_TO_INT(R(c + 5));
    bool writable = (alignment_flags & 0x8000u) != 0;
    uint16_t alignment = (uint16_t) (alignment_flags & 0x7fffu);
    uint64_t layout_key = (uint64_t) XR_TO_INT(R(c + 6));
    XrAggregateLayout *layout = xr_struct_layout_lookup_by_stable_key(&isolate->vm, layout_key);
    if (elem_size == 0 || alignment == 0 || elem_type >= XR_ELEM_COUNT)
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "mem.slice<T>() has invalid static layout evidence");
    /* Source-level mem.slice is an unsafe, caller-proven raw view. Keep only
     * compiler/bytecode layout integrity checks here; dynamic pointer, count,
     * alignment, and range validation would contradict that contract. */
    XrSliceView *slice = VM_SLICE_SLOT(R(c + 1));
    slice->data = (void *) address;
    slice->length = count;
    uint16_t layout_id = layout ? layout->layout_id : 0;
    if (layout_key != 0 && layout_id == 0)
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY,
                         "mem.slice<T>() could not resolve canonical aggregate layout");
    R(a) = xr_span_ref_typed(slice, elem_type, elem_size, elem_tid, layout_id,
                             writable ? 0 : XR_SLICE_VIEW_READONLY);
    vmbreak;
}

vmcase(OP_BUFFER_MATERIALIZE) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    if (!XR_IS_INT(R(c)) || !XR_IS_INT(R(c + 1)) || !XR_IS_INT(R(c + 2)) || !XR_IS_INT(R(c + 3)) ||
        !XR_IS_INT(R(c + 4))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "mem.assumeInitialized<T>() is missing compiler layout evidence");
    }
    int64_t raw_size = XR_TO_INT(R(c));
    int64_t raw_align = XR_TO_INT(R(c + 1));
    int64_t raw_code = XR_TO_INT(R(c + 2));
    int64_t raw_slot = XR_TO_INT(R(c + 3));
    uint64_t layout_key = (uint64_t) XR_TO_INT(R(c + 4));
    if (raw_size <= 0 || raw_size > UINT32_MAX || raw_align <= 0 || raw_align > UINT16_MAX ||
        raw_code < 0 || raw_code > UINT8_MAX || raw_slot < 0 || raw_slot > UINT16_MAX) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "mem.assumeInitialized<T>() has invalid compiler layout evidence");
    }
    uint32_t size = (uint32_t) raw_size;
    size_t align = (size_t) raw_align;
    uint8_t code = (uint8_t) raw_code;
    XrAggregateLayout *layout =
        layout_key ? xr_struct_layout_lookup_by_stable_key(&isolate->vm, layout_key) : NULL;
    if (code == UINT8_MAX) {
        if (!layout || layout->total_size != size || !xr_aggregate_layout_is_headerless(layout)) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                             "mem.assumeInitialized<T>() aggregate layout is unavailable");
        }
        uint16_t layout_id = xr_struct_layout_register(&isolate->vm, layout);
        uint8_t *dst = vm_ctx->struct_areas[VM_FRAME_COUNT - 1] + (uint16_t) raw_slot * 16u;
        if (!xr_buffer_materialize(R(b), dst, size, align, layout)) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                             "mem.assumeInitialized<T>() Buffer size/alignment evidence mismatch");
        }
        R(a) = xr_aggregate_ref(dst, layout_id);
    } else {
        /* The scalar materialization contract is capped at this 16-byte buffer.
         * Use the contract alignment directly because MSVC C11 does not expose
         * max_align_t even though it supports _Alignas. */
        _Alignas(16) uint8_t bytes[16] = {0};
        if (size > sizeof(bytes) || !xr_ffi_type_is_memory_scalar(code) ||
            !xr_buffer_materialize(R(b), bytes, size, align, NULL)) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                             "mem.assumeInitialized<T>() scalar layout evidence mismatch");
        }
        R(a) = xr_ffi_ptr_load((uintptr_t) bytes, code, XR_ENDIAN_NATIVE);
    }
    vmbreak;
}
