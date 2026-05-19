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
 * VM_INC_FRAME_COUNT, VM_BARRIER_BACK, VM_BARRIER_VAL,
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

vmcase(OP_NEWARRAY) {
    /* OP_NEWARRAY: create array
    ** A = destination register
    ** B = capacity/initial element count
    ** C = (elem_tid << 2) | storage_mode
    **     storage_mode: bits 0-1 (0=normal, 1=shared)
    **     elem_tid:     bits 2-6 (XrTypeId, 0=any)
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c_field = GETARG_C(i);
    int storage_mode = c_field & 0x03;
    uint8_t elem_tid = (uint8_t) ((c_field >> 2) & 0x1F);
    uint8_t elem_type = xr_tid_to_elem_type(elem_tid);

    XrArray *array;
    if (storage_mode != 0 && isolate->sys_heap) {
        // shared: allocate on system heap
        array = (XrArray *) xr_sysheap_alloc_shared(isolate->sys_heap, sizeof(XrArray), XR_TARRAY);
        if (array) {
            xr_array_init_inplace(array, b > 0 ? b : 4, elem_type);
            // Set storage mode
            XR_GC_SET_STORAGE(&array->gc, storage_mode);
            if (storage_mode == XR_GC_STORAGE_SHARED) {
                xr_shared_set_refc(&array->gc, 1);
            }
        }
    } else {
        // normal: allocate on coroutine heap
        if (elem_type != XR_ELEM_ANY) {
            array = xr_array_with_capacity_typed(VM_CURRENT_CORO, b > 0 ? b : 4, elem_type);
        } else {
            array = (b > 0) ? xr_array_with_capacity(VM_CURRENT_CORO, b)
                            : xr_array_new(VM_CURRENT_CORO);
        }
    }

    if (array) {
        array->elem_tid = elem_tid;
        if (elem_type != XR_ELEM_ANY) {
            /* Typed arrays: set length directly (data is uninitialized).
             * Caller populates via OP_INDEX_SET / OP_ARRAY_INIT afterward.
             * Pushing from registers would crash on non-numeric garbage. */
            array->length = b;
        }
        /* ANY arrays: b is capacity hint only. length stays 0.
         * lower_array_literal emits OP_INDEX_SET arr[i]=v for i=0..n-1,
         * which append-grows via the idx == length branch of OP_INDEX_SET.
         * This matches the JIT path (xr_jit_index_set in xm_jit_runtime.c)
         * and avoids reading uninitialized register slots that would only
         * be immediately overwritten by the subsequent OP_INDEX_SET. */
    }
    R(a) = xr_value_from_array(array);
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
    ** C = (key_kind << 7) | (value_tid << 2) | flags
    **     flags bit0: shared, bit1: weak
    **     value_tid: bits 2-6 (5 bits, XrTypeId 0-31)
    **     key_kind:  bits 7-8 (2 bits: 0=any, 1=string, 2=int)
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    int storage_mode = c & 0x01;
    int is_weak = c & 0x02;
    uint8_t value_tid = (uint8_t) ((c >> 2) & 0x1F);
    int key_kind = (c >> 7) & 0x03;
    uint8_t key_tid = (key_kind == 1) ? XR_TID_STRING : (key_kind == 2) ? XR_TID_INT : 0;

    XrMap *map;
    if (storage_mode != 0 && isolate->sys_heap) {
        // shared: allocate on system heap
        map = (XrMap *) xr_sysheap_alloc_shared(isolate->sys_heap, sizeof(XrMap), XR_TMAP);
        if (map) {
            xr_map_init_inplace(map, b > 0 ? b : 8);
            // Set storage mode
            XR_GC_SET_STORAGE(&map->gc, storage_mode);
            if (storage_mode == XR_GC_STORAGE_SHARED) {
                xr_shared_set_refc(&map->gc, 1);
            }
        }
    } else {
        // normal: allocate on coroutine heap
        map = (b > 0) ? xr_map_with_capacity(VM_CURRENT_CORO, b) : xr_map_new(VM_CURRENT_CORO);
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
    ** B = (elem_tid << 2) | flags
    **     flags bit0: shared, bit1: weak
    **     elem_tid: bits 2-6 (XrTypeId, 0=any)
    ** C = init mode (0=empty, 1=from array in R[A+1])
    */
    int a = GETARG_A(i);
    int b_arg = GETARG_B(i);
    int init_mode = GETARG_C(i);
    int storage_mode = b_arg & 0x01;
    int is_weak = b_arg & 0x02;
    uint8_t elem_tid = (uint8_t) ((b_arg >> 2) & 0x1F);

    XrSet *set;
    if (init_mode == 1 && XR_IS_ARRAY(R(a + 1)) && storage_mode != 0 && isolate->sys_heap) {
        // Initialize from array on system heap (shared)
        set = (XrSet *) xr_sysheap_alloc_shared(isolate->sys_heap, sizeof(XrSet), XR_TSET);
        if (set) {
            xr_set_init_inplace(set);
            XR_GC_SET_STORAGE(&set->gc, storage_mode);
            xr_shared_set_refc(&set->gc, 1);
            XrArray *arr = XR_TO_ARRAY(R(a + 1));
            int32_t len = arr->length;
            XrValue *elems = (XrValue *) arr->data;
            for (int32_t j = 0; j < len; j++)
                xr_set_add(set, elems[j]);
        }
    } else if (init_mode == 1 && XR_IS_ARRAY(R(a + 1))) {
        // Initialize from array on coroutine heap
        XrArray *arr = XR_TO_ARRAY(R(a + 1));
        set = xr_set_from_array(xr_current_coro(isolate), arr);
    } else if (storage_mode != 0 && isolate->sys_heap) {
        // shared: allocate on system heap
        set = (XrSet *) xr_sysheap_alloc_shared(isolate->sys_heap, sizeof(XrSet), XR_TSET);
        if (set) {
            xr_set_init_inplace(set);
            XR_GC_SET_STORAGE(&set->gc, storage_mode);
            if (storage_mode == XR_GC_STORAGE_SHARED) {
                xr_shared_set_refc(&set->gc, 1);
            }
        }
    } else {
        // normal: allocate on coroutine heap
        set = xr_set_new(VM_CURRENT_CORO);
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
    R(a) = xr_range_new(isolate, start_val, end_val);
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
    ** B = storage mode (0=normal, 1=shared)
    */
    int a = GETARG_A(i);
    int storage_mode = GETARG_B(i);

    XrStringBuilder *sb;
    if (storage_mode != 0 && isolate->sys_heap) {
        // shared: allocate on system heap
        sb = (XrStringBuilder *) xr_sysheap_alloc_shared(isolate->sys_heap, sizeof(XrStringBuilder),
                                                         XR_TINSTANCE);
        if (sb) {
            sb->klass = isolate->core->stringBuilderClass;
            xr_stringbuilder_init_inplace(sb);
            XR_GC_SET_STORAGE(&sb->gc, storage_mode);
            if (storage_mode == XR_GC_STORAGE_SHARED) {
                xr_shared_set_refc(&sb->gc, 1);
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
        int idx = (int) XR_TO_INT(R(c));
        if ((unsigned) idx < (unsigned) ecount) {
            uint8_t *bp = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(etype);
            uint8_t *ep = bp + idx * es;
            switch (etype) {
                case XR_NATIVE_I64:
                    R(a) = XR_FROM_INT(*(int64_t *) ep);
                    break;
                case XR_NATIVE_F64:
                    R(a) = XR_FROM_FLOAT(*(double *) ep);
                    break;
                case XR_NATIVE_BOOL:
                    R(a) = *(uint8_t *) ep ? XR_TRUE_VAL : XR_FALSE_VAL;
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
                             "fixed array index out of range: %d (length %u)", idx,
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
    int idx = (int) XR_TO_INT(R(c));
    if ((unsigned) idx < (unsigned) arr->length) {
        R(a) = (arr->elem_type == XR_ELEM_ANY) ? ((XrValue *) arr->data)[idx]
                                               : xr_array_get_element(arr, idx);
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
                case XR_NATIVE_F64:
                    R(a) = XR_FROM_FLOAT(*(double *) ep);
                    break;
                case XR_NATIVE_BOOL:
                    R(a) = *(uint8_t *) ep ? XR_TRUE_VAL : XR_FALSE_VAL;
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
        XrString *ch = xr_string_char_at_unicode(isolate, str, (size_t) c);
        R(a) = ch ? xr_string_value(ch) : xr_null();
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
        int idx = (int) XR_TO_INT(R(b));
        if ((unsigned) idx < (unsigned) ecount) {
            uint8_t *bp = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(etype);
            uint8_t *ep = bp + idx * es;
            XrValue _av = R(c);
            switch (etype) {
                case XR_NATIVE_I64:
                    *(int64_t *) ep = XR_TO_INT(_av);
                    break;
                case XR_NATIVE_F64:
                    *(double *) ep = XR_TO_FLOAT(_av);
                    break;
                case XR_NATIVE_BOOL:
                    *(uint8_t *) ep = (uint8_t) _av.i;
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
                             "fixed array index out of range: %d (length %u)", idx,
                             (unsigned) ecount);
        }
        vmbreak;
    }
    if (XR_IS_ARRAY(obj_val)) {
        XrArray *arr = XR_TO_ARRAY(obj_val);
        int idx = (int) XR_TO_INT(R(b));
        XrValue _av = R(c);
        if ((unsigned) idx < (unsigned) arr->length) {
            if (arr->elem_type == XR_ELEM_ANY) {
                ((XrValue *) arr->data)[idx] = _av;
                XR_ARRAY_MARK_GC_PTRS(arr, _av);
                VM_BARRIER_VAL(arr, _av);
            } else {
                xr_array_set_element(arr, idx, _av);
            }
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
                case XR_NATIVE_F64:
                    *(double *) ep = XR_TO_FLOAT(_acv);
                    break;
                case XR_NATIVE_BOOL:
                    *(uint8_t *) ep = (uint8_t) _acv.i;
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
        if (b < arr->length) {
            if (arr->elem_type == XR_ELEM_ANY) {
                ((XrValue *) arr->data)[b] = _acv;
                XR_ARRAY_MARK_GC_PTRS(arr, _acv);
                VM_BARRIER_VAL(arr, _acv);
            } else {
                xr_array_set_element(arr, b, _acv);
            }
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
                     "only Array, Bytes, typed array support constant index assignment");
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
        XR_ARRAY_MARK_GC_PTRS(arr, val);
        XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), arr);
    } else {
        xr_array_set_element(arr, arr->length++, val);
    }
    vmbreak;
}

vmcase(OP_ARRAY_LEN) {
    // OP_ARRAY_LEN: array length
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrArray *arr = XR_TO_ARRAY(R(b));
    R(a) = xr_int((xr_Integer) arr->length);
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
                arr->has_gc_ptrs = 1;
                break;
            }
        }
        if (arr->has_gc_ptrs)
            XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), arr);
    } else {
        // Slow path: typed arrays need per-element unboxing
        for (int j = 1; j <= b; j++) {
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
        VM_BARRIER_BACK(map);
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
        VM_BARRIER_BACK(map);
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
        VM_BARRIER_BACK(json);
        vmbreak;
    }
    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "index assignment requires Map or Json type");
}

vmcase(OP_MAP_INCREMENT) {
    /* OP_MAP_INCREMENT: Map counter pattern optimization
    ** R[A]:Map[R[B]]++ - if key doesn't exist set to 1, otherwise +1
    ** Replaces: if (map.has(key)) { map[key] = map[key] + 1 } else { map[key] = 1 }
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
        VM_BARRIER_BACK(map);
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

    // Fast path: struct inline fixed array ([N]T)
    if (XR_IS_ARRAY_REF(obj_val) && XR_IS_INT(key_val)) {
        uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
        uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        int idx = (int) XR_TO_INT(key_val);
        if ((unsigned) idx < (unsigned) ecount) {
            uint8_t *base_ptr = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(etype);
            uint8_t *ep = base_ptr + idx * es;
            switch (etype) {
                case XR_NATIVE_I64:
                    R(a) = XR_FROM_INT(*(int64_t *) ep);
                    break;
                case XR_NATIVE_F64:
                    R(a) = XR_FROM_FLOAT(*(double *) ep);
                    break;
                case XR_NATIVE_BOOL:
                    R(a) = *(uint8_t *) ep ? XR_TRUE_VAL : XR_FALSE_VAL;
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
                             "fixed array index out of range: %d (length %u)", idx,
                             (unsigned) ecount);
        }
        vmbreak;
    }
    // Fast path: String (Unicode character index)
    if (XR_IS_STRING(obj_val) && XR_IS_INT(key_val)) {
        XrString *str = XR_TO_STRING(obj_val);
        size_t idx = (size_t) XR_TO_INT(key_val);
        XrString *ch = xr_string_char_at_unicode(isolate, str, idx);
        R(a) = ch ? xr_string_value(ch) : xr_null();
        vmbreak;
    }
    // Fast path: Array (includes slices — capacity==0 && source!=NULL)
    if (XR_IS_ARRAY(obj_val)) {
        XrArray *arr = XR_TO_ARRAY(obj_val);
        int idx = (int) XR_TO_INT(key_val);
        if ((unsigned) idx < (unsigned) arr->length) {
            R(a) = (arr->elem_type == XR_ELEM_ANY) ? ((XrValue *) arr->data)[idx]
                                                   : xr_array_get_element(arr, idx);
        } else {
            R(a) = xr_null();
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
        int idx = (int) XR_TO_INT(key_val);
        if (vals && (unsigned) idx < (unsigned) vals->length) {
            R(a) = (vals->elem_type == XR_ELEM_ANY) ? ((XrValue *) vals->data)[idx]
                                                    : xr_array_get_element(vals, idx);
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
                     "only Array, Map, Json, String, Bytes, typed array support indexing");
}

vmcase(OP_INDEX_SET) {
    // OP_INDEX_SET: generic index write
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue obj_val = R(a);
    XrValue key_val = R(b);
    XrValue val = R(c);

    // Fast path: struct inline fixed array ([N]T)
    if (XR_IS_ARRAY_REF(obj_val) && XR_IS_INT(key_val)) {
        uint8_t etype = XR_ARRAY_REF_ELEM_TYPE(obj_val);
        uint16_t ecount = XR_ARRAY_REF_ELEM_COUNT(obj_val);
        int idx = (int) XR_TO_INT(key_val);
        if ((unsigned) idx < (unsigned) ecount) {
            uint8_t *base_ptr = (uint8_t *) obj_val.ptr;
            uint8_t es = xr_native_type_size(etype);
            uint8_t *ep = base_ptr + idx * es;
            switch (etype) {
                case XR_NATIVE_I64:
                    *(int64_t *) ep = XR_TO_INT(val);
                    break;
                case XR_NATIVE_F64:
                    *(double *) ep = XR_TO_FLOAT(val);
                    break;
                case XR_NATIVE_BOOL:
                    *(uint8_t *) ep = (uint8_t) val.i;
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
                             "fixed array index out of range: %d (length %u)", idx,
                             (unsigned) ecount);
        }
        vmbreak;
    }
    // Fast path: Array (includes slices — capacity==0 && source!=NULL)
    if (XR_IS_ARRAY(obj_val)) {
        XrArray *arr = XR_TO_ARRAY(obj_val);
        int idx = (int) XR_TO_INT(key_val);
        if ((unsigned) idx < (unsigned) arr->length) {
            if (arr->elem_type == XR_ELEM_ANY) {
                ((XrValue *) arr->data)[idx] = val;
                XR_ARRAY_MARK_GC_PTRS(arr, val);
                VM_BARRIER_VAL(arr, val);
            } else {
                xr_array_set_element(arr, idx, val);
            }
        } else if (idx == arr->length && arr->elem_type == XR_ELEM_ANY && !xr_array_is_slice(arr)) {
            /* Append: matches JIT semantics. Used by lower_array_literal
             * which emits OP_NEWARRAY (length=0) followed by OP_INDEX_SET
             * arr[i]=v for i=0..n-1. Only valid for ANY arrays; typed
             * and slice arrays use explicit OP_ARRAY_PUSH or grow. */
            xr_array_push(arr, val);
        }
        vmbreak;
    }
    // Fast path: Map
    if (XR_IS_MAP(obj_val)) {
        XrMap *map = XR_TO_MAP(obj_val);
        xr_map_set(map, key_val, val);
        VM_BARRIER_BACK(map);
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
            VM_BARRIER_BACK(json);
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
                     "only Array, Map, Json, Bytes, typed array support index assignment");
}

vmcase(OP_SLICE) {
    /* OP_SLICE: slice operation
    ** R[A] = R[B][R[C]:R[C+1]]
    ** - R[B]: source object (Array/String/Bytes)
    ** - R[C]: start index (0 = from beginning)
    ** - R[C+1]: end index (-1 = to end)
    */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue source = R(b);
    int64_t start = XR_TO_INT(R(c));
    int64_t end = XR_TO_INT(R(c + 1));

    // Array/slice: zero-copy, shared data
    if (XR_IS_ARRAY(source)) {
        XrArray *arr = XR_TO_ARRAY(source);

        // Use slice function
        XrArray *slice = xr_array_slice(VM_CURRENT_CORO, arr, (int32_t) start, (int32_t) end);
        R(a) = slice ? XR_FROM_PTR(slice) : xr_null();
        vmbreak;
    }

    VM_RUNTIME_ERROR(XR_ERR_TYPE_NO_INDEX, "this type does not support slicing");
}
