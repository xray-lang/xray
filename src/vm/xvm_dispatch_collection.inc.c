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
 * VM_CURRENT_CORO, checkGC, startfunc label, ...) provided by
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

#define VM_SPAN_CHECK_STORABLE(span, val)                                                          \
    do {                                                                                           \
        if (!xr_typed_value_is_storable((val), (span)->elem_type)) {                               \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, (span)->elem_type == XR_ELEM_RUNE               \
                                                       ? "Span<char> element must be char"         \
                                                       : "typed span element must be numeric");    \
        }                                                                                          \
    } while (0)

#define VM_SPAN_SLOT(slot_value)                                                                   \
    ((XrSpanView *) (vm_ctx->struct_areas[VM_FRAME_COUNT - 1] +                                    \
                     (uint16_t) (XR_TO_INT(slot_value)) * 16u))

#define VM_SPAN_GET_ELEMENT(span, idx)                                                             \
    xr_typed_get((span)->data, (int32_t) (idx), (span)->elem_type)

#define VM_SPAN_SET_ELEMENT(span, idx, val)                                                        \
    do {                                                                                           \
        if (((span)->reserved & XR_SPAN_VIEW_READONLY) != 0) {                                     \
            VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, "cannot write through readonly Span");       \
        }                                                                                          \
        if (xr_typed_set((span)->data, (int32_t) (idx), (val), (span)->elem_type) &&               \
            (span)->contains_refs && (span)->guard) {                                              \
            XrArray *_span_owner = (XrArray *) (span)->guard;                                      \
            XR_ARRAY_MARK_REFS(_span_owner, (val));                                                \
        }                                                                                          \
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
    int storage_mode = c_field & 0x03;
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
    if (storage_mode == 0)
        checkGC(base + a + 1);
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

    int storage_mode = c_field & 0x03;
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
    if (storage_mode == 0)
        checkGC(base + a + 1);
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

    int storage_mode = c_field & 0x03;
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
    if (storage_mode == 0)
        checkGC(base + a + 1);
    vmbreak;
}

vmcase(OP_NEWTUPLE) {
    /* OP_NEWTUPLE: build a new tuple from B consecutive elements
    ** A = destination register
    ** B = arity (== element count)
    ** C = unused
    **
    ** Elements are read from R[A+1..A+B] (same convention as the
    ** untyped path of OP_NEWARRAY).  Tuples are immutable from user
    ** code, so the construction-then-publish pattern is the only
    ** writer of element slots.
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrTuple *tup = xr_tuple_new(VM_CURRENT_CORO, (uint16_t) b);
    if (tup) {
        for (int j = 0; j < b; j++)
            xr_tuple_set(tup, (uint16_t) j, R(a + 1 + j));
    }
    R(a) = xr_value_from_tuple(tup);
    checkGC(base + a + 1);
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
    **     flags bits0-1: storage mode (0=normal, 1=shared, 2=owned), bit2: weak
    **     value_tid: bits 3-7 (5 bits, XrTypeId 0-31)
    **     key_kind:  bits 8-9 (2 bits: 0=any, 1=string, 2=int)
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    int storage_mode = c & 0x03;
    int is_weak = c & 0x04;
    uint8_t value_tid = (uint8_t) ((c >> 3) & 0x1F);
    int key_kind = (c >> 8) & 0x03;
    uint8_t key_tid = (key_kind == 1) ? XR_TID_STRING : (key_kind == 2) ? XR_TID_INT : 0;

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
        if (is_weak)
            map->flags |= XR_MAP_FLAG_WEAK;
        map->key_tid = key_tid;
        map->value_tid = value_tid;
    }

    R(a) = xr_value_from_map(map);
    if (storage_mode == 0)
        checkGC(base + a + 1);
    vmbreak;
}

vmcase(OP_NEWSET) {
    /* OP_NEWSET: create Set
    ** A = destination register
    ** B = (elem_tid << 3) | flags
    **     flags bits0-1: storage mode (0=normal, 1=shared, 2=owned), bit2: weak
    **     elem_tid: bits 3-7 (XrTypeId, 0=any)
    ** C = init mode (0=empty, 1=from array in R[A+1])
    */
    int a = GETARG_A(i);
    int b_arg = GETARG_B(i);
    int init_mode = GETARG_C(i);
    int storage_mode = b_arg & 0x03;
    int is_weak = b_arg & 0x04;
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
        if (is_weak)
            set->flags |= XR_SET_FLAG_WEAK;
        set->elem_tid = elem_tid;
    }

    R(a) = xr_value_from_set(set);
    if (storage_mode == 0)
        checkGC(base + a + 1);
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
    R(a) = xr_range_new(isolate, start_val, end_val, false);
    checkGC(base + a + 1);
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
    R(a) = xr_range_new(isolate, start_val, end_val, true);
    checkGC(base + a + 1);
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
    int storage_mode = GETARG_B(i);

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
    if (storage_mode == 0)
        checkGC(base + a + 1);
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
        uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        int64_t idx = XR_TO_INT(R(c));
        if ((uint64_t) idx < (uint64_t) ecount) {
            uint8_t *bp = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(etype);
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
            XrInstance *_inst = xr_value_to_instance(obj_val);
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
        uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        if ((unsigned) c < (unsigned) ecount) {
            uint8_t *base_ptr = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(etype);
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
    // String indexing support
    if (XR_IS_STRING(obj_val)) {
        XrString *str = XR_TO_STRING(obj_val);
        uint32_t cp = 0;
        R(a) = (c >= 0 && xr_utf8_rune_at(str->data, str->length, (size_t) c, &cp, NULL) &&
                xr_unicode_is_scalar(cp))
                   ? xr_rune(cp)
                   : xr_null();
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
            int64_t len = xr_range_length(rng);
            if (c >= 0 && c < len) {
                R(a) = xr_int(rng->start + (int64_t) c * rng->step);
            } else {
                R(a) = xr_null();
            }
            vmbreak;
        }
    }
    // Operator overload: operator[]
    if (xr_value_is_instance(obj_val)) {
        XrInstance *_inst = xr_value_to_instance(obj_val);
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
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "only Array, String support constant indexing");
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
        uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        int64_t idx = XR_TO_INT(R(b));
        if ((uint64_t) idx < (uint64_t) ecount) {
            uint8_t *bp = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(etype);
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
        XrInstance *_inst = xr_value_to_instance(obj_val);
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
        uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        if ((unsigned) b < (unsigned) ecount) {
            uint8_t *bp = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(etype);
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
        XrInstance *_inst = xr_value_to_instance(obj_val);
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
    XrArray *arr = XR_TO_ARRAY(R(a));
    XrValue val = R(b);
    if (arr->length >= arr->capacity)
        xr_array_grow(arr);
    if (arr->elem_type == XR_ELEM_ANY) {
        ((XrValue *) arr->data)[arr->length++] = val;
        XR_ARRAY_MARK_REFS(arr, val);
    } else {
        VM_ARRAY_CHECK_STORABLE(arr, val);
        xr_array_set_element(arr, arr->length++, val);
    }
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
    if (XR_IS_SPAN_REF(R(b))) {
        XrSpanView *span = XR_TO_SPAN_REF(R(b));
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
    bool json_dynamic = GETARG_C(i) != 0;
    XrValue value = R(b);
    if (XR_IS_ARRAY(value)) {
        R(a) = xr_int((xr_Integer) XR_TO_ARRAY(value)->length);
        vmbreak;
    }
    if (XR_IS_SPAN_REF(value)) {
        XrSpanView *span = XR_TO_SPAN_REF(value);
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
        XrString *string = XR_TO_STRING(value);
        R(a) = xr_int((xr_Integer) (string ? string->rune_length : 0));
        vmbreak;
    }
    if (xr_value_is_json(value)) {
        XrJson *json = xr_value_to_json(value);
        R(a) = xr_int((xr_Integer) xr_json_field_count(isolate, json));
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
    int64_t buffer_length = xr_mem_buffer_length(value);
    if (buffer_length >= 0) {
        R(a) = xr_int((xr_Integer) buffer_length);
        vmbreak;
    }
    if (json_dynamic) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "len(Json) requires an object, array, or string variant");
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "value does not implement Lengthable");
}

vmcase(OP_ARRAY_DATA_PTR) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    if (XR_IS_SPAN_REF(R(b))) {
        XrSpanView *span = XR_TO_SPAN_REF(R(b));
        R(a) = xr_int((xr_Integer) (intptr_t) (span ? span->data : NULL));
        vmbreak;
    }
    if (!XR_IS_ARRAY(R(b))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Array.ptr() expects an array or span receiver");
    }
    XrArray *arr = XR_TO_ARRAY(R(b));
    R(a) = xr_int((xr_Integer) (intptr_t) arr->data);
    vmbreak;
}

vmcase(OP_STRING_BYTES_SPAN) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    if (!XR_IS_STRING(R(b)) || !XR_IS_INT(R(c))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "string.bytes() expects a string receiver");
    }
    XrString *str = XR_TO_STRING(R(b));
    XrSpanView *span = VM_SPAN_SLOT(R(c));
    span->data = str ? (void *) str->data : NULL;
    span->length = str ? str->length : 0;
    span->elem_type = XR_ELEM_U8;
    span->elem_size = 1;
    span->elem_tid = 0;
    span->contains_refs = 0;
    span->reserved = XR_SPAN_VIEW_READONLY;
    span->guard = str;
    R(a) = xr_span_ref(span);
    vmbreak;
}

#define VM_SPAN_VIEW(value, out_data, out_length, out_elem_type, out_elem_size, out_elem_tid,      \
                     out_contains_refs, out_reserved, out_guard, message)                          \
    do {                                                                                           \
        XrValue _span_value = (value);                                                             \
        if (XR_IS_SPAN_REF(_span_value)) {                                                         \
            XrSpanView *_span = XR_TO_SPAN_REF(_span_value);                                       \
            if (!_span) {                                                                          \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, (message));                                 \
            }                                                                                      \
            (out_data) = _span->data;                                                              \
            (out_length) = _span->length;                                                          \
            (out_elem_type) = _span->elem_type;                                                    \
            (out_elem_size) = _span->elem_size;                                                    \
            (out_elem_tid) = _span->elem_tid;                                                      \
            (out_contains_refs) = _span->contains_refs;                                            \
            (out_reserved) = _span->reserved;                                                      \
            (out_guard) = _span->guard;                                                            \
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

vmcase(OP_SPAN_AS_BYTES) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    if (!XR_IS_INT(R(c))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Span.asArray<byte>() missing Span frame slot");
    }
    void *data = NULL;
    int64_t length = 0;
    uint8_t elem_type = XR_ELEM_ANY;
    uint8_t elem_size = 0;
    uint8_t elem_tid = 0;
    uint8_t contains_refs = 0;
    uint32_t reserved = 0;
    void *guard = NULL;
    VM_SPAN_VIEW(R(b), data, length, elem_type, elem_size, elem_tid, contains_refs, reserved, guard,
                 "Span.asArray<byte>() expects Span");
    (void) elem_tid;
    (void) contains_refs;
    if (elem_type == XR_ELEM_ANY || elem_type >= XR_ELEM_COUNT || elem_size == 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "Span.asArray<byte>() requires POD Span element type");
    }
    if (length < 0 || (elem_size > 0 && length > INT64_MAX / (int64_t) elem_size)) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Span.asArray<byte>() byte length overflow");
    }
    XrSpanView *span = VM_SPAN_SLOT(R(c));
    span->data = data;
    span->length = length * (int64_t) elem_size;
    span->elem_type = XR_ELEM_U8;
    span->elem_size = 1;
    span->elem_tid = 0;
    span->contains_refs = 0;
    span->reserved = reserved & XR_SPAN_VIEW_READONLY;
    span->guard = guard;
    R(a) = xr_span_ref(span);
    vmbreak;
}

vmcase(OP_SPAN_COPY) {
    int a = GETARG_A(i);
    void *dst_data = NULL;
    void *src_data = NULL;
    int64_t dst_length = 0;
    int64_t src_length = 0;
    uint8_t dst_elem_type = XR_ELEM_ANY;
    uint8_t src_elem_type = XR_ELEM_ANY;
    uint8_t dst_elem_size = 0;
    uint8_t src_elem_size = 0;
    uint8_t dst_elem_tid = 0;
    uint8_t src_elem_tid = 0;
    uint8_t dst_contains_refs = 0;
    uint8_t src_contains_refs = 0;
    uint32_t dst_reserved = 0;
    uint32_t src_reserved = 0;
    void *dst_guard = NULL;
    void *src_guard = NULL;
    VM_SPAN_VIEW(R(a), dst_data, dst_length, dst_elem_type, dst_elem_size, dst_elem_tid,
                 dst_contains_refs, dst_reserved, dst_guard,
                 "Span.copyFrom(src) receiver must be Span");
    VM_SPAN_VIEW(R(a + 1), src_data, src_length, src_elem_type, src_elem_size, src_elem_tid,
                 src_contains_refs, src_reserved, src_guard,
                 "Span.copyFrom(src) source must be Span");
    (void) dst_elem_tid;
    (void) src_elem_tid;
    (void) dst_contains_refs;
    (void) src_contains_refs;
    (void) src_reserved;
    (void) dst_guard;
    (void) src_guard;
    if (dst_elem_type == XR_ELEM_ANY || dst_elem_type >= XR_ELEM_COUNT || dst_elem_size == 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Span.copyFrom(src) receiver must be POD Span");
    }
    if (src_elem_type == XR_ELEM_ANY || src_elem_type >= XR_ELEM_COUNT || src_elem_size == 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Span.copyFrom(src) source must be POD Span");
    }
    if (dst_elem_type != src_elem_type || dst_elem_size != src_elem_size) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Span.copyFrom(src) element type mismatch");
    }
    if (dst_reserved & XR_SPAN_VIEW_READONLY) {
        VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, "cannot write through readonly Span");
    }
    if (dst_length < 0 || src_length < 0 || dst_length > INT64_MAX / (int64_t) dst_elem_size ||
        src_length > INT64_MAX / (int64_t) src_elem_size) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Span.copyFrom(src) byte length overflow");
    }
    int64_t dst_bytes = dst_length * (int64_t) dst_elem_size;
    int64_t src_bytes = src_length * (int64_t) src_elem_size;
    if (src_bytes > dst_bytes) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Span.copyFrom(src) range out of bounds");
    }
    if (src_bytes > 0) {
        if (!dst_data || !src_data) {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Span.copyFrom(src) range out of bounds");
        }
        memmove(dst_data, src_data, (size_t) src_bytes);
    }
    vmbreak;
}

vmcase(OP_SPAN_FILL) {
    int a = GETARG_A(i);
    void *span_data = NULL;
    int64_t span_length = 0;
    uint8_t span_elem_type = XR_ELEM_ANY;
    uint8_t span_elem_size = 0;
    uint8_t span_elem_tid = 0;
    uint8_t span_contains_refs = 0;
    uint32_t span_reserved = 0;
    void *span_guard = NULL;
    XrValue fill_value = R(a + 1);
    VM_SPAN_VIEW(R(a), span_data, span_length, span_elem_type, span_elem_size, span_elem_tid,
                 span_contains_refs, span_reserved, span_guard,
                 "Span.fill(value) receiver must be Span");
    (void) span_elem_tid;
    (void) span_contains_refs;
    (void) span_guard;
    if (span_elem_type == XR_ELEM_ANY || span_elem_type >= XR_ELEM_COUNT || span_elem_size == 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Span.fill(value) receiver must be POD Span");
    }
    if (span_reserved & XR_SPAN_VIEW_READONLY) {
        VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, "cannot write through readonly Span");
    }
    if (span_length < 0 || span_length > INT64_MAX / (int64_t) span_elem_size) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Span.fill(value) byte length overflow");
    }
    if (span_length > 0 && !span_data) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Span.fill(value) range out of bounds");
    }
    if (!xr_typed_fill(span_data, span_length, fill_value, span_elem_type)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Span.fill(value) value type mismatch");
    }
    vmbreak;
}

vmcase(OP_SPAN_COMPARE) {
    int a = GETARG_A(i);
    void *left_data = NULL;
    void *right_data = NULL;
    int64_t left_length = 0;
    int64_t right_length = 0;
    uint8_t left_elem_type = XR_ELEM_ANY;
    uint8_t right_elem_type = XR_ELEM_ANY;
    uint8_t left_elem_size = 0;
    uint8_t right_elem_size = 0;
    uint8_t left_elem_tid = 0;
    uint8_t right_elem_tid = 0;
    uint8_t left_contains_refs = 0;
    uint8_t right_contains_refs = 0;
    uint32_t left_reserved = 0;
    uint32_t right_reserved = 0;
    void *left_guard = NULL;
    void *right_guard = NULL;
    VM_SPAN_VIEW(R(a), left_data, left_length, left_elem_type, left_elem_size, left_elem_tid,
                 left_contains_refs, left_reserved, left_guard,
                 "Span.compare(other) receiver must be Span");
    VM_SPAN_VIEW(R(a + 1), right_data, right_length, right_elem_type, right_elem_size,
                 right_elem_tid, right_contains_refs, right_reserved, right_guard,
                 "Span.compare(other) operand must be Span");
    (void) left_elem_tid;
    (void) right_elem_tid;
    (void) left_contains_refs;
    (void) right_contains_refs;
    (void) left_reserved;
    (void) right_reserved;
    (void) left_guard;
    (void) right_guard;
    if (left_elem_type == XR_ELEM_ANY || left_elem_type >= XR_ELEM_COUNT || left_elem_size == 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Span.compare(other) receiver must be POD Span");
    }
    if (right_elem_type == XR_ELEM_ANY || right_elem_type >= XR_ELEM_COUNT ||
        right_elem_size == 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Span.compare(other) operand must be POD Span");
    }
    if (left_elem_type != right_elem_type || left_elem_size != right_elem_size) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Span.compare(other) element type mismatch");
    }
    if (left_length < 0 || right_length < 0 || left_length > INT64_MAX / (int64_t) left_elem_size ||
        right_length > INT64_MAX / (int64_t) right_elem_size) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Span.compare(other) byte length overflow");
    }
    int64_t left_bytes = left_length * (int64_t) left_elem_size;
    int64_t right_bytes = right_length * (int64_t) right_elem_size;
    int64_t n = left_bytes < right_bytes ? left_bytes : right_bytes;
    int cmp = 0;
    if (n > 0) {
        if (!left_data || !right_data) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Span.compare(other) span has no data");
        }
        cmp = memcmp(left_data, right_data, (size_t) n);
    }
    if (cmp == 0) {
        if (left_length < right_length)
            cmp = -1;
        else if (left_length > right_length)
            cmp = 1;
    } else {
        cmp = cmp < 0 ? -1 : 1;
    }
    R(a) = xr_int(cmp);
    vmbreak;
}

vmcase(OP_SPAN_REINTERPRET) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    if (!XR_IS_INT(R(c)) || !XR_IS_INT(R(c + 1)) || !XR_IS_INT(R(c + 2)) || !XR_IS_INT(R(c + 3))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice<byte>.reinterpret<T>() missing metadata");
    }
    uint8_t target_elem_type = (uint8_t) XR_TO_INT(R(c + 1));
    uint8_t target_elem_size = (uint8_t) XR_TO_INT(R(c + 2));
    uint8_t target_elem_tid = (uint8_t) XR_TO_INT(R(c + 3));
    if (target_elem_type == XR_ELEM_ANY || target_elem_type >= XR_ELEM_COUNT ||
        target_elem_size == 0) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "Slice<byte>.reinterpret<T>() requires POD target type");
    }
    if (XR_ELEM_SIZES[target_elem_type] != target_elem_size) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "Slice<byte>.reinterpret<T>() target metadata mismatch");
    }
    void *data = NULL;
    int64_t length = 0;
    uint8_t elem_type = XR_ELEM_ANY;
    uint8_t elem_size = 0;
    uint8_t elem_tid = 0;
    uint8_t contains_refs = 0;
    uint32_t reserved = 0;
    void *guard = NULL;
    VM_SPAN_VIEW(R(b), data, length, elem_type, elem_size, elem_tid, contains_refs, reserved, guard,
                 "Slice<byte>.reinterpret<T>() expects Slice<byte>");
    (void) elem_tid;
    (void) contains_refs;
    if (elem_type != XR_ELEM_U8 || elem_size != 1) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "Slice<byte>.reinterpret<T>() expects Slice<byte> receiver");
    }
    if (length < 0) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                         "Slice<byte>.reinterpret<T>() byte length overflow");
    }
    if (length % (int64_t) target_elem_size != 0) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                         "Slice<byte>.reinterpret<T>() length is not divisible by target size");
    }
    XrSpanView *span = VM_SPAN_SLOT(R(c));
    span->data = data;
    span->length = length / (int64_t) target_elem_size;
    span->elem_type = target_elem_type;
    span->elem_size = target_elem_size;
    span->elem_tid = target_elem_tid;
    span->contains_refs = 0;
    span->reserved = reserved & XR_SPAN_VIEW_READONLY;
    span->guard = guard;
    R(a) = xr_span_ref(span);
    vmbreak;
}

#undef VM_SPAN_VIEW

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
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice<byte> load/store expects Endian");       \
        }                                                                                          \
        if ((out_endian) < XR_ENDIAN_NATIVE || (out_endian) > XR_ENDIAN_BE) {                      \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid Endian value");                        \
        }                                                                                          \
    } while (0)

#define VM_BYTES_LOAD_CASE(opcode, load_fn, width_name)                                            \
    vmcase(opcode) {                                                                               \
        int a = GETARG_A(i);                                                                       \
        XrValue _recv = R(a + 1);                                                                  \
        XrValue _offset = R(a + 2);                                                                \
        if (!XR_IS_INT(_offset)) {                                                                 \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                                 \
                             "Slice<byte>.load<T>() expects integer offset");                      \
        }                                                                                          \
        int64_t _endian = XR_ENDIAN_NATIVE;                                                        \
        VM_PARSE_ENDIAN_ARG(R(a + 3), _endian);                                                    \
        void *_data = NULL;                                                                        \
        int64_t _length = 0;                                                                       \
        uint8_t _elem_type = XR_ELEM_ANY;                                                          \
        if (XR_IS_SPAN_REF(_recv)) {                                                               \
            XrSpanView *_span = XR_TO_SPAN_REF(_recv);                                             \
            if (!_span || _span->elem_type != XR_ELEM_U8) {                                        \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                             \
                                 "Slice<byte>.load<" width_name ">() expects Slice<byte>");        \
            }                                                                                      \
            _data = _span->data;                                                                   \
            _length = _span->length;                                                               \
            _elem_type = _span->elem_type;                                                         \
        } else if (XR_IS_ARRAY(_recv)) {                                                           \
            XrArray *_arr = XR_TO_ARRAY(_recv);                                                    \
            if (!_arr || _arr->elem_type != XR_ELEM_U8) {                                          \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                             \
                                 "Slice<byte>.load<" width_name ">() expects Slice<byte>");        \
            }                                                                                      \
            _data = _arr->data;                                                                    \
            _length = _arr->length;                                                                \
            _elem_type = _arr->elem_type;                                                          \
        } else {                                                                                   \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                                 \
                             "Slice<byte>.load<" width_name ">() expects Slice<byte>");            \
        }                                                                                          \
        bool _ok = false;                                                                          \
        uint64_t _value =                                                                          \
            (uint64_t) load_fn(_data, _length, _elem_type, XR_TO_INT(_offset), _endian, &_ok);     \
        if (!_ok) {                                                                                \
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,                                           \
                             "Slice<byte>.load<" width_name ">() offset out of bounds");           \
        }                                                                                          \
        R(a) = xr_int((xr_Integer) _value);                                                        \
        vmbreak;                                                                                   \
    }

#define VM_BYTES_STORE_CASE(opcode, store_fn, value_type, width_name)                              \
    vmcase(opcode) {                                                                               \
        int a = GETARG_A(i);                                                                       \
        XrValue _recv = R(a + 1);                                                                  \
        XrValue _offset = R(a + 2);                                                                \
        XrValue _value = R(a + 3);                                                                 \
        if (!XR_IS_INT(_offset) || !XR_IS_INT(_value)) {                                           \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                                 \
                             "Slice<byte>.store<T>() expects integer offset and value");           \
        }                                                                                          \
        int64_t _endian = XR_ENDIAN_NATIVE;                                                        \
        VM_PARSE_ENDIAN_ARG(R(a + 4), _endian);                                                    \
        void *_data = NULL;                                                                        \
        int64_t _length = 0;                                                                       \
        uint8_t _elem_type = XR_ELEM_ANY;                                                          \
        if (XR_IS_SPAN_REF(_recv)) {                                                               \
            XrSpanView *_span = XR_TO_SPAN_REF(_recv);                                             \
            if (!_span || _span->elem_type != XR_ELEM_U8) {                                        \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                             \
                                 "Slice<byte>.store<" width_name ">() expects Slice<byte>");       \
            }                                                                                      \
            if ((_span->reserved & XR_SPAN_VIEW_READONLY) != 0) {                                  \
                VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, "cannot write through readonly Span");   \
            }                                                                                      \
            _data = _span->data;                                                                   \
            _length = _span->length;                                                               \
            _elem_type = _span->elem_type;                                                         \
        } else if (XR_IS_ARRAY(_recv)) {                                                           \
            XrArray *_arr = XR_TO_ARRAY(_recv);                                                    \
            if (!_arr || _arr->elem_type != XR_ELEM_U8) {                                          \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                             \
                                 "Slice<byte>.store<" width_name ">() expects Slice<byte>");       \
            }                                                                                      \
            _data = _arr->data;                                                                    \
            _length = _arr->length;                                                                \
            _elem_type = _arr->elem_type;                                                          \
        } else {                                                                                   \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                                 \
                             "Slice<byte>.store<" width_name ">() expects Slice<byte>");           \
        }                                                                                          \
        bool _ok = store_fn(_data, _length, _elem_type, XR_TO_INT(_offset),                        \
                            (value_type) XR_TO_INT(_value), _endian);                              \
        if (!_ok) {                                                                                \
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,                                           \
                             "Slice<byte>.store<" width_name ">() offset out of bounds");          \
        }                                                                                          \
        R(a) = xr_null();                                                                          \
        vmbreak;                                                                                   \
    }

#define VM_BYTES_LOAD_FLOAT_CASE(opcode, load_fn, width_name)                                      \
    vmcase(opcode) {                                                                               \
        int a = GETARG_A(i);                                                                       \
        XrValue _recv = R(a + 1);                                                                  \
        XrValue _offset = R(a + 2);                                                                \
        if (!XR_IS_INT(_offset)) {                                                                 \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                                 \
                             "Slice<byte>.load<T>() expects integer offset");                      \
        }                                                                                          \
        int64_t _endian = XR_ENDIAN_NATIVE;                                                        \
        VM_PARSE_ENDIAN_ARG(R(a + 3), _endian);                                                    \
        void *_data = NULL;                                                                        \
        int64_t _length = 0;                                                                       \
        uint8_t _elem_type = XR_ELEM_ANY;                                                          \
        if (XR_IS_SPAN_REF(_recv)) {                                                               \
            XrSpanView *_span = XR_TO_SPAN_REF(_recv);                                             \
            if (!_span || _span->elem_type != XR_ELEM_U8) {                                        \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                             \
                                 "Slice<byte>.load<" width_name ">() expects Slice<byte>");        \
            }                                                                                      \
            _data = _span->data;                                                                   \
            _length = _span->length;                                                               \
            _elem_type = _span->elem_type;                                                         \
        } else if (XR_IS_ARRAY(_recv)) {                                                           \
            XrArray *_arr = XR_TO_ARRAY(_recv);                                                    \
            if (!_arr || _arr->elem_type != XR_ELEM_U8) {                                          \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                             \
                                 "Slice<byte>.load<" width_name ">() expects Slice<byte>");        \
            }                                                                                      \
            _data = _arr->data;                                                                    \
            _length = _arr->length;                                                                \
            _elem_type = _arr->elem_type;                                                          \
        } else {                                                                                   \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                                 \
                             "Slice<byte>.load<" width_name ">() expects Slice<byte>");            \
        }                                                                                          \
        bool _ok = false;                                                                          \
        double _value =                                                                            \
            (double) load_fn(_data, _length, _elem_type, XR_TO_INT(_offset), _endian, &_ok);       \
        if (!_ok) {                                                                                \
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,                                           \
                             "Slice<byte>.load<" width_name ">() offset out of bounds");           \
        }                                                                                          \
        R(a) = xr_float(_value);                                                                   \
        vmbreak;                                                                                   \
    }

#define VM_BYTES_STORE_FLOAT_CASE(opcode, store_fn, value_type, width_name)                        \
    vmcase(opcode) {                                                                               \
        int a = GETARG_A(i);                                                                       \
        XrValue _recv = R(a + 1);                                                                  \
        XrValue _offset = R(a + 2);                                                                \
        XrValue _value = R(a + 3);                                                                 \
        if (!XR_IS_INT(_offset) || !XR_IS_FLOAT(_value)) {                                         \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                                 \
                             "Slice<byte>.store<T>() expects integer offset and float value");     \
        }                                                                                          \
        int64_t _endian = XR_ENDIAN_NATIVE;                                                        \
        VM_PARSE_ENDIAN_ARG(R(a + 4), _endian);                                                    \
        void *_data = NULL;                                                                        \
        int64_t _length = 0;                                                                       \
        uint8_t _elem_type = XR_ELEM_ANY;                                                          \
        if (XR_IS_SPAN_REF(_recv)) {                                                               \
            XrSpanView *_span = XR_TO_SPAN_REF(_recv);                                             \
            if (!_span || _span->elem_type != XR_ELEM_U8) {                                        \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                             \
                                 "Slice<byte>.store<" width_name ">() expects Slice<byte>");       \
            }                                                                                      \
            if ((_span->reserved & XR_SPAN_VIEW_READONLY) != 0) {                                  \
                VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, "cannot write through readonly Span");   \
            }                                                                                      \
            _data = _span->data;                                                                   \
            _length = _span->length;                                                               \
            _elem_type = _span->elem_type;                                                         \
        } else if (XR_IS_ARRAY(_recv)) {                                                           \
            XrArray *_arr = XR_TO_ARRAY(_recv);                                                    \
            if (!_arr || _arr->elem_type != XR_ELEM_U8) {                                          \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                             \
                                 "Slice<byte>.store<" width_name ">() expects Slice<byte>");       \
            }                                                                                      \
            _data = _arr->data;                                                                    \
            _length = _arr->length;                                                                \
            _elem_type = _arr->elem_type;                                                          \
        } else {                                                                                   \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,                                                 \
                             "Slice<byte>.store<" width_name ">() expects Slice<byte>");           \
        }                                                                                          \
        bool _ok = store_fn(_data, _length, _elem_type, XR_TO_INT(_offset),                        \
                            (value_type) XR_TO_FLOAT(_value), _endian);                            \
        if (!_ok) {                                                                                \
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,                                           \
                             "Slice<byte>.store<" width_name ">() offset out of bounds");          \
        }                                                                                          \
        R(a) = xr_null();                                                                          \
        vmbreak;                                                                                   \
    }

VM_BYTES_LOAD_CASE(OP_BYTES_LOAD_U16, xr_array_core_bytes_load_u16, "uint16")
VM_BYTES_LOAD_CASE(OP_BYTES_LOAD_U32, xr_array_core_bytes_load_u32, "uint32")
VM_BYTES_LOAD_CASE(OP_BYTES_LOAD_U64, xr_array_core_bytes_load_u64, "uint64")
VM_BYTES_LOAD_FLOAT_CASE(OP_BYTES_LOAD_F32, xr_array_core_bytes_load_f32, "float32")
VM_BYTES_LOAD_FLOAT_CASE(OP_BYTES_LOAD_F64, xr_array_core_bytes_load_f64, "float64")
VM_BYTES_STORE_CASE(OP_BYTES_STORE_U16, xr_array_core_bytes_store_u16, uint16_t, "uint16")
VM_BYTES_STORE_CASE(OP_BYTES_STORE_U32, xr_array_core_bytes_store_u32, uint32_t, "uint32")
VM_BYTES_STORE_CASE(OP_BYTES_STORE_U64, xr_array_core_bytes_store_u64, uint64_t, "uint64")
VM_BYTES_STORE_FLOAT_CASE(OP_BYTES_STORE_F32, xr_array_core_bytes_store_f32, float, "float32")
VM_BYTES_STORE_FLOAT_CASE(OP_BYTES_STORE_F64, xr_array_core_bytes_store_f64, double, "float64")

#undef VM_BYTES_STORE_FLOAT_CASE
#undef VM_BYTES_LOAD_FLOAT_CASE
#undef VM_BYTES_STORE_CASE
#undef VM_BYTES_LOAD_CASE
#undef VM_PARSE_ENDIAN_ARG

#define VM_BYTESPAN_VIEW(value, out_data, out_length, out_readonly, message)                       \
    do {                                                                                           \
        XrValue _span_value = (value);                                                             \
        if (XR_IS_SPAN_REF(_span_value)) {                                                         \
            XrSpanView *_span = XR_TO_SPAN_REF(_span_value);                                       \
            if (!_span || _span->elem_type != XR_ELEM_U8) {                                        \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, (message));                                 \
            }                                                                                      \
            (out_data) = _span->data;                                                              \
            (out_length) = _span->length;                                                          \
            (out_readonly) = ((_span->reserved & XR_SPAN_VIEW_READONLY) != 0);                     \
        } else if (XR_IS_ARRAY(_span_value)) {                                                     \
            XrArray *_arr = XR_TO_ARRAY(_span_value);                                              \
            if (!_arr || _arr->elem_type != XR_ELEM_U8) {                                          \
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, (message));                                 \
            }                                                                                      \
            (out_data) = _arr->data;                                                               \
            (out_length) = _arr->length;                                                           \
            (out_readonly) = false;                                                                \
        } else {                                                                                   \
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, (message));                                     \
        }                                                                                          \
    } while (0)

vmcase(OP_BYTES_SPAN_FILL) {
    int a = GETARG_A(i);
    void *dst_data = NULL;
    int64_t dst_length = 0;
    bool dst_readonly = false;
    VM_BYTESPAN_VIEW(R(a), dst_data, dst_length, dst_readonly,
                     "Slice<byte>.fill(value) expects Slice<byte>");
    if (dst_readonly) {
        VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, "cannot write through readonly Span");
    }
    if (!XR_IS_INT(R(a + 1))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "Slice<byte>.fill(value) expects integer byte value");
    }
    if (!xr_array_core_bytes_fill_value(dst_data, dst_length, R(a + 1))) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice<byte>.fill(value) range out of bounds");
    }
    vmbreak;
}

vmcase(OP_BYTES_SPAN_COPY) {
    int a = GETARG_A(i);
    void *dst_data = NULL;
    void *src_data = NULL;
    int64_t dst_length = 0;
    int64_t src_length = 0;
    bool dst_readonly = false;
    bool src_readonly = false;
    VM_BYTESPAN_VIEW(R(a), dst_data, dst_length, dst_readonly,
                     "Slice<byte>.copyFrom(src) receiver must be Slice<byte>");
    VM_BYTESPAN_VIEW(R(a + 1), src_data, src_length, src_readonly,
                     "Slice<byte>.copyFrom(src) source must be Slice<byte>");
    (void) src_readonly;
    if (dst_readonly) {
        VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, "cannot write through readonly Span");
    }
    if (!xr_array_core_bytes_copy_from(dst_data, dst_length, XR_ELEM_U8, src_data, src_length,
                                       XR_ELEM_U8, 0, 0, src_length, false)) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                         "Slice<byte>.copyFrom(src) range out of bounds");
    }
    vmbreak;
}

vmcase(OP_BYTES_SPAN_COMPARE) {
    int a = GETARG_A(i);
    void *left_data = NULL;
    void *right_data = NULL;
    int64_t left_length = 0;
    int64_t right_length = 0;
    bool left_readonly = false;
    bool right_readonly = false;
    VM_BYTESPAN_VIEW(R(a), left_data, left_length, left_readonly,
                     "Slice<byte>.compare(other) receiver must be Slice<byte>");
    VM_BYTESPAN_VIEW(R(a + 1), right_data, right_length, right_readonly,
                     "Slice<byte>.compare(other) operand must be Slice<byte>");
    (void) left_readonly;
    (void) right_readonly;
    int64_t n = left_length < right_length ? left_length : right_length;
    int cmp = 0;
    if (n > 0) {
        if (!left_data || !right_data) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice<byte>.compare(other) span has no data");
        }
        cmp = memcmp(left_data, right_data, (size_t) n);
    }
    if (cmp == 0) {
        if (left_length < right_length)
            cmp = -1;
        else if (left_length > right_length)
            cmp = 1;
    } else {
        cmp = cmp < 0 ? -1 : 1;
    }
    R(a) = xr_int(cmp);
    vmbreak;
}

vmcase(OP_BYTES_SPAN_COMMON_PREFIX) {
    int a = GETARG_A(i);
    void *left_data = NULL;
    void *right_data = NULL;
    int64_t left_length = 0;
    int64_t right_length = 0;
    bool left_readonly = false;
    bool right_readonly = false;
    VM_BYTESPAN_VIEW(R(a), left_data, left_length, left_readonly,
                     "Slice<byte>.commonPrefix(other) receiver must be Slice<byte>");
    VM_BYTESPAN_VIEW(R(a + 1), right_data, right_length, right_readonly,
                     "Slice<byte>.commonPrefix(other) operand must be Slice<byte>");
    (void) left_readonly;
    (void) right_readonly;
    bool ok = false;
    int64_t prefix = xr_array_core_bytes_common_prefix(left_data, left_length, XR_ELEM_U8,
                                                       right_data, right_length, XR_ELEM_U8, &ok);
    if (!ok) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice<byte>.commonPrefix(other) span has no data");
    }
    R(a) = xr_int(prefix);
    vmbreak;
}

vmcase(OP_BYTES_SPAN_REPEAT) {
    int a = GETARG_A(i);
    void *data = NULL;
    int64_t length = 0;
    bool readonly = false;
    VM_BYTESPAN_VIEW(R(a), data, length, readonly,
                     "Slice<byte>.repeatFrom(dstOffset, distance, count) expects Slice<byte>");
    if (readonly) {
        VM_RUNTIME_ERROR(XR_ERR_CMP_CONST_ASSIGN, "cannot write through readonly Span");
    }
    if (!XR_IS_INT(R(a + 1)) || !XR_IS_INT(R(a + 2)) || !XR_IS_INT(R(a + 3))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "Slice<byte>.repeatFrom(dstOffset, distance, count) expects integers");
    }
    if (!xr_array_core_bytes_repeat_from(data, length, XR_ELEM_U8, XR_TO_INT(R(a + 1)),
                                         XR_TO_INT(R(a + 2)), XR_TO_INT(R(a + 3)))) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                         "Slice<byte>.repeatFrom(dstOffset, distance, count) range out of bounds");
    }
    vmbreak;
}

#undef VM_BYTESPAN_VIEW

/* FFI raw-pointer access. B (load) / A (store) holds an address-width int;
 * C is the XrFFIType width of the pointee. No bounds/null check (unsafe). */
vmcase(OP_PTR_LOAD) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    R(a) = xr_ffi_ptr_load((uintptr_t) (intptr_t) XR_TO_INT(R(b)), (uint8_t) c);
    vmbreak;
}

vmcase(OP_PTR_STORE) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    xr_ffi_ptr_store((uintptr_t) (intptr_t) XR_TO_INT(R(a)), (uint8_t) c, R(b));
    vmbreak;
}

vmcase(OP_PTR_COPY_NONOVERLAP) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    memcpy((void *) (uintptr_t) (intptr_t) XR_TO_INT(R(a)),
           (const void *) (uintptr_t) (intptr_t) XR_TO_INT(R(b)), (size_t) XR_TO_INT(R(c)));
    vmbreak;
}

vmcase(OP_BYTES_COPY_WITHIN) {
    int a = GETARG_A(i);
    if (!XR_IS_ARRAY(R(a)) || !XR_IS_INT(R(a + 1)) || !XR_IS_INT(R(a + 2)) ||
        !XR_IS_INT(R(a + 3))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "Slice<byte>.copyFrom expects integer offsets and count");
    }
    XrArray *arr = XR_TO_ARRAY(R(a));
    if (arr->elem_type != XR_ELEM_U8)
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice<byte>.copyFrom receiver must be Array<byte>");
    if (!xr_array_bytes_copy_within(arr, (int32_t) XR_TO_INT(R(a + 1)),
                                    (int32_t) XR_TO_INT(R(a + 2)), (int32_t) XR_TO_INT(R(a + 3)))) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice<byte>.copyFrom range out of bounds");
    }
    vmbreak;
}

vmcase(OP_BYTES_COPY_FROM) {
    int a = GETARG_A(i);
    if (!XR_IS_ARRAY(R(a)) || !XR_IS_ARRAY(R(a + 1)) || !XR_IS_INT(R(a + 2)) ||
        !XR_IS_INT(R(a + 3)) || !XR_IS_INT(R(a + 4))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "Slice<byte>.copyFrom expects Slice<byte> and integer ranges");
    }
    XrArray *dst = XR_TO_ARRAY(R(a));
    XrArray *src = XR_TO_ARRAY(R(a + 1));
    if (dst->elem_type != XR_ELEM_U8 || src->elem_type != XR_ELEM_U8)
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "Slice<byte>.copyFrom operands must be Slice<byte>");
    if (!xr_array_bytes_copy_from(dst, src, (int32_t) XR_TO_INT(R(a + 2)),
                                  (int32_t) XR_TO_INT(R(a + 3)), (int32_t) XR_TO_INT(R(a + 4)))) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Slice<byte>.copyFrom range out of bounds");
    }
    vmbreak;
}

vmcase(OP_BYTES_REPEAT_FROM) {
    int a = GETARG_A(i);
    if (!XR_IS_ARRAY(R(a)) || !XR_IS_INT(R(a + 1)) || !XR_IS_INT(R(a + 2)) ||
        !XR_IS_INT(R(a + 3))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "Array<byte>.repeatFrom expects integer offsets and count");
    }
    XrArray *arr = XR_TO_ARRAY(R(a));
    if (arr->elem_type != XR_ELEM_U8)
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "Array<byte>.repeatFrom receiver must be Array<byte>");
    if (!xr_array_bytes_repeat_from(arr, (int32_t) XR_TO_INT(R(a + 1)),
                                    (int32_t) XR_TO_INT(R(a + 2)), (int32_t) XR_TO_INT(R(a + 3)))) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "Array<byte>.repeatFrom range out of bounds");
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
    // OP_MAP_GETK: Map/Json constant key access
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
        (void) found;
        vmbreak;
    }
    // Json object support
    if (xr_value_is_json(map_val)) {
        XrJson *json = xr_value_to_json(map_val);
        XrValue key_val = k[c];
        XrString *key_str = XR_TO_STRING(key_val);
        R(a) = xr_json_get_by_key(isolate, json, key_str->data);
        vmbreak;
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "index access requires Map or Json type");
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
    // OP_MAP_SETK: Map/Json constant key set
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
    // Json object support
    if (xr_value_is_json(map_val)) {
        XrJson *json = xr_value_to_json(map_val);
        XrValue key_val = k[b];
        XrString *key_str = XR_TO_STRING(key_val);
        if (!xr_json_set_by_key(isolate, json, key_str->data, R(c))) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY, "cannot add property to sealed Json object");
        }
        vmbreak;
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "index assignment requires Map or Json type");
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
        uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        int64_t idx = XR_TO_INT(key_val);
        if ((uint64_t) idx < (uint64_t) ecount) {
            uint8_t *base_ptr = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(etype);
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
    // Fast path: frame-local Span value
    if (XR_IS_SPAN_REF(obj_val) && XR_IS_INT(key_val)) {
        XrSpanView *span = XR_TO_SPAN_REF(obj_val);
        int64_t idx = XR_TO_INT(key_val);
        if (span && (uint64_t) idx < (uint64_t) span->length) {
            R(a) = VM_SPAN_GET_ELEMENT(span, idx);
        } else {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "span index out of range: %lld (length %d)", (long long) idx,
                             (int) (span ? span->length : 0));
        }
        vmbreak;
    }
    // Fast path: String (Unicode character index)
    if (XR_IS_STRING(obj_val) && XR_IS_INT(key_val)) {
        XrString *str = XR_TO_STRING(obj_val);
        int64_t idx = XR_TO_INT(key_val);
        uint32_t cp = 0;
        R(a) = (idx >= 0 && xr_utf8_rune_at(str->data, str->length, (size_t) idx, &cp, NULL) &&
                xr_unicode_is_scalar(cp))
                   ? xr_rune(cp)
                   : xr_null();
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
            int64_t len = xr_range_length(rng);
            if (idx >= 0 && idx < len) {
                R(a) = xr_int(rng->start + idx * rng->step);
            } else {
                R(a) = xr_null();
            }
            vmbreak;
        }
    }
    // Fast path: Map
    if (XR_IS_MAP(obj_val)) {
        XrMap *map = XR_TO_MAP(obj_val);
        bool found;
        R(a) = xr_map_get(map, key_val, &found);
        if (!found)
            R(a) = xr_null();
        vmbreak;
    }
    // Fast path: Json object (string keys only)
    if (xr_value_is_json(obj_val)) {
        if (XR_IS_STRING(key_val)) {
            XrJson *json = xr_value_to_json(obj_val);
            XrString *key_str = XR_TO_STRING(key_val);
            R(a) = xr_json_get_by_key(isolate, json, key_str->data);
            vmbreak;
        }
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "Json object only supports string keys");
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
        XrInstance *_inst = xr_value_to_instance(obj_val);
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
                     "only Array, Map, Json, String, typed array support indexing");
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
        uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        int64_t idx = XR_TO_INT(key_val);
        if ((uint64_t) idx < (uint64_t) ecount) {
            uint8_t *base_ptr = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(etype);
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
    // Fast path: frame-local Span value
    if (XR_IS_SPAN_REF(obj_val) && XR_IS_INT(key_val)) {
        XrSpanView *span = XR_TO_SPAN_REF(obj_val);
        int64_t idx = XR_TO_INT(key_val);
        if (!span || (uint64_t) idx >= (uint64_t) span->length) {
            VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS,
                             "span index out of range: %lld (length %d)", (long long) idx,
                             (int) (span ? span->length : 0));
        }
        VM_SPAN_CHECK_STORABLE(span, val);
        VM_SPAN_SET_ELEMENT(span, idx, val);
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
    // Fast path: Json object (string keys only)
    if (xr_value_is_json(obj_val)) {
        if (XR_IS_STRING(key_val)) {
            XrJson *json = xr_value_to_json(obj_val);
            XrString *key_str = XR_TO_STRING(key_val);
            if (!xr_json_set_by_key(isolate, json, key_str->data, val)) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_PROPERTY,
                                 "cannot add property to sealed Json object");
            }
            vmbreak;
        }
        VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "Json object only supports string keys");
    }
    // Operator overload
    if (xr_value_is_instance(obj_val)) {
        XrInstance *_inst = xr_value_to_instance(obj_val);
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
                     "only Array, Map, Json, typed array support index assignment");
}

vmcase(OP_SLICE) {
    /* OP_SLICE: slice operation
    ** R[A] = R[B][R[C]:R[C+1]]
    ** - R[B]: source object (Array/String/Array<byte>)
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

    // Array slice: either frame-local borrowed Span value (R[C+2] is a span
    // slot), or storage-backed heap View (R[C+2] is null).
    if (XR_IS_ARRAY(source)) {
        XrArray *arr = XR_TO_ARRAY(source);
        if (!XR_IS_INT(R(c + 2))) {
            XrArray *slice = xr_array_slice(VM_CURRENT_CORO, arr, start, end);
            if (!slice) {
                VM_RUNTIME_ERROR(XR_ERR_RUNTIME, "array view slice allocation failed");
            }
            R(a) = XR_FROM_PTR(slice);
            vmbreak;
        }
        xr_array_normalize_slice(arr->length, &start, &end);
        XrSpanView *span = VM_SPAN_SLOT(R(c + 2));
        span->data = (arr->data && end > start)
                         ? (uint8_t *) arr->data + (size_t) start * arr->elem_size
                         : (uint8_t *) arr->data;
        span->length = end - start;
        span->elem_type = arr->elem_type;
        span->elem_size = arr->elem_size;
        span->elem_tid = arr->elem_tid;
        span->contains_refs = arr->contains_refs;
        span->reserved = 0;
        span->guard = arr;
        R(a) = xr_span_ref(span);
        vmbreak;
    }

    // Frame-local fixed array slice: only target-typed Span<T> is supported.
    if (XR_IS_ARRAY_REF(source)) {
        if (!XR_IS_INT(R(c + 2))) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                             "fixed array slices can only produce Span views");
        }
        uint8_t native_type = XR_ARRAY_REF_ELEM_TYPE(source);
        uint16_t elem_count = XR_ARRAY_REF_ELEM_COUNT(source);
        xr_array_normalize_slice(elem_count, &start, &end);
        uint8_t elem_size = xr_native_type_size(native_type);
        XrSpanView *span = VM_SPAN_SLOT(R(c + 2));
        span->data = (source.ptr && end > start)
                         ? (uint8_t *) source.ptr + (size_t) start * (size_t) elem_size
                         : (uint8_t *) source.ptr;
        span->length = end - start;
        span->elem_type = xr_native_type_to_elem_type(native_type);
        span->elem_size = elem_size ? elem_size : (uint8_t) sizeof(XrValue);
        span->elem_tid = 0;
        span->contains_refs = span->elem_type == XR_ELEM_ANY;
        span->reserved = 0;
        span->guard = NULL;
        R(a) = xr_span_ref(span);
        vmbreak;
    }

    // Span slice-of-slice: pure pointer arithmetic into another frame-local Span.
    if (XR_IS_SPAN_REF(source)) {
        XrSpanView *src_span = XR_TO_SPAN_REF(source);
        if (!src_span || !XR_IS_INT(R(c + 2))) {
            VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "span slice missing Span frame slot");
        }
        xr_array_normalize_slice(src_span->length, &start, &end);
        XrSpanView *span = VM_SPAN_SLOT(R(c + 2));
        span->data = (src_span->data && end > start)
                         ? (uint8_t *) src_span->data + (size_t) start * src_span->elem_size
                         : (uint8_t *) src_span->data;
        span->length = end - start;
        span->elem_type = src_span->elem_type;
        span->elem_size = src_span->elem_size;
        span->elem_tid = src_span->elem_tid;
        span->contains_refs = src_span->contains_refs;
        span->reserved = src_span->reserved;
        span->guard = src_span->guard;
        R(a) = xr_span_ref(span);
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
