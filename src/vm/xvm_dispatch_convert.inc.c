/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_convert.inc.c — print / typeof / type conversion / chr
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, R,
 * vmcase, vmbreak, VM_RUNTIME_ERROR, ...) provided by the
 * surrounding scope. CMake excludes *.inc.c from the VM_SRC
 * glob.
 *
 * Owns:
 *   I/O / debug:
 *     OP_PRINT     — println(R[A..A+B])
 *     OP_TYPEOF    — runtime type id
 *     OP_TYPENAME  — runtime type name string
 *     OP_DUMP      — diagnostic value dump
 *
 *   Type conversion:
 *     OP_TOINT / OP_TOFLOAT / OP_TOSTRING / OP_TOBOOL
 *     OP_COPY      — deep copy
 *
 *   Char-from-int:
 *     OP_CHR       — single Unicode character string
 */

vmcase(OP_PRINT) {
    /* OP_PRINT: print value with toString support
    ** A: value register
    ** B: 1=add space before (not first argument)
    ** C: bit0=newline, bit1-2=slot_type hint (0=ANY, 1=I64, 2=F64),
    **    bit3=skip_null (REPL auto-echo: print nothing if val is null)
    **
    ** If value is instance with toString() method, call it first
    */
    int a = GETARG_A(i);
    int add_space = GETARG_B(i);
    int c_field = GETARG_C(i);
    int newline = c_field & 1;
    int slot_hint = (c_field >> 1) & 3;
    int skip_null = (c_field >> 3) & 1;

    // Reconstruct tagged value from raw slot if hint provided.
    XrValue val = XR_NULL_VAL;
    if (slot_hint == 1) {
        val = XR_FROM_INT(R(a).i);
    } else if (slot_hint == 2) {
        val = XR_FROM_FLOAT(R(a).f);
    } else if (slot_hint != 3) {
        val = R(a);
    } else {
        /* slot_hint=3 is uint64: the register stores raw bits in i64 form,
         * but formatting must interpret them as unsigned. */
    }

    /* REPL auto-echo suppression: bare expressions that evaluate to
     * null are silently dropped so the prompt stays clean.  Explicit
     * `print(null)` does not set skip_null and still prints "null". */
    if (slot_hint != 3 && skip_null && XR_IS_NULL(val))
        vmbreak;

    FILE *print_stream = xr_isolate_stdout(isolate);
    if (add_space)
        fputc(' ', print_stream);

    if (slot_hint == 3) {
        fprintf(print_stream, "%llu", (unsigned long long) (uint64_t) R(a).i);
        if (newline)
            fputc('\n', print_stream);
        vmbreak;
    }

    // Check if instance has toString method
    if (xr_value_is_instance(val)) {
        XrInstance *inst = xr_value_to_instance(val);
        XrClass *cls = xr_instance_get_class(inst);
        if (cls) {
            XrMethod *method = xr_class_lookup_method(cls, SYMBOL_TOSTRING);
            if (method && method->type == XMETHOD_CLOSURE && method->as.closure) {
                // Call toString() method
                XrClosure *closure = method->as.closure;
                XrProto *proto = closure->proto;

                // Setup call: R[a+1] = this (instance)
                VM_STACK_CHECK(a + 1 + proto->maxstacksize);
                R(a + 1) = val;

                // Save current PC (continue after return)
                savepc();

                // Create new call frame
                int _fidx = VM_FRAME_COUNT;
                VM_INC_FRAME_COUNT;
                XrBcCallFrame *new_frame = &VM_FRAMES[_fidx];
                new_frame->closure = closure;
                new_frame->pc = PROTO_CODE_BASE(proto);
                new_frame->base_offset = (int) ((base + a + 1) - VM_STACK);

                // Mark as toString print call, return value needs printing
                // Use flags to mark (check on return)
                new_frame->flags = newline ? 0x02 : 0x01;  // 0x01=print, 0x02=print+newline

                goto startfunc;
            }
        }
    }

    // Default print: use unified xr_value_to_string
    XrString *print_str = xr_value_to_string(isolate, val);
    fputs(print_str->data, print_stream);
    if (newline)
        fputc('\n', print_stream);

    vmbreak;
}

vmcase(OP_TYPEOF) {
    // Returns int (XrTypeId) for fast comparison
    XR_TYPE_IDENTITY_CORE_OWNER_GUARD(XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI,
                                      XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO);
    XR_TYPE_IDENTITY_CORE_CONSUMER_GUARD(XR_SEM_CONSUMER_VM);
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int slot_hint = GETARG_C(i);

    XrValue val;
    if (slot_hint == 1) {
        val = XR_FROM_INT(R(b).i);
    } else if (slot_hint == 2) {
        val = XR_FROM_FLOAT(R(b).f);
    } else {
        val = R(b);
    }
    R(a) = XR_FROM_INT((int64_t) xr_value_typeid_vm(val));
    vmbreak;
}

vmcase(OP_TYPENAME) {
    // Returns type name as string
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int slot_hint = GETARG_C(i);

    XrValue val;
    if (slot_hint == 1) {
        val = XR_FROM_INT(R(b).i);
    } else if (slot_hint == 2) {
        val = XR_FROM_FLOAT(R(b).f);
    } else {
        val = R(b);
    }
    /* Slice and fixed-array spellings are decided once, in the shared lowering:
     * xi_lower_expr folds typeName() for those static types into a constant, so
     * a ref value cannot arrive here. The VM used to spell them a second time
     * and disagree with that fold -- "[byte;4]" against "[byte; 4]" -- which is
     * a divergence waiting for the first value whose static type is lost. */
    XR_DCHECK(!XR_IS_SLICE_REF(val) && !XR_IS_ARRAY_REF(val),
              "typename: slice and array refs are folded during lowering");
    const char *type_name = NULL;
    // For instances, return class name
    if (xr_value_is_instance(val)) {
        XrInstance *inst = xr_value_to_instance(val);
        XrClass *cls = xr_instance_get_class(inst);
        if (cls && cls->builtin_kind == XR_BK_STRUCT_OBJECT)
            type_name = TYPE_NAME_OBJECT;
        else if (cls && cls->name)
            type_name = cls->name;
    }
    /* A struct ref keeps its class pointer in the first word of the struct
     * area. Array and slice refs share the aggregate tag but start with view
     * state instead, so they must not be read this way. */
    if (type_name == NULL && val.tag == XR_TAG_AGG_REF && !XR_IS_ARRAY_REF(val) &&
        !XR_IS_SLICE_REF(val) && val.ptr) {
        XrClass *cls = *(XrClass **) val.ptr;
        if (cls && cls->name)
            type_name = cls->name;
    }
    /* Everything else, a payload variant constructor included, answers from the
     * public type id table. A constructor is a callable, so it reports what
     * every other callable reports rather than the enum it belongs to. */
    if (type_name == NULL) {
        type_name = xr_typeid_name(xr_value_typeid(val));
    }
    size_t len = strlen(type_name);
    XrString *str = xr_string_intern(isolate, type_name, len, 0);
    R(a) = xr_string_value(str);
    vmbreak;
}

vmcase(OP_DUMP) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue val = R(a);
    xr_value_dump_with_isolate(isolate, val, b);
    vmbreak;
}

vmcase(OP_TOINT) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    uint16_t conversion = (uint16_t) GETARG_C(i);
    XrValue val = R(b);
    if (xr_conversion_bytecode_is_numeric(conversion)) {
        uint8_t source_rep = xr_conversion_bytecode_source_rep(conversion);
        uint8_t target_rep = xr_conversion_bytecode_target_rep(conversion);
        uint8_t pointer_bits = xr_conversion_bytecode_pointer_bits(conversion);
        if (source_rep == XR_NATIVE_F32 || source_rep == XR_NATIVE_F64) {
            int64_t converted = 0;
            if (!XR_IS_FLOAT(val) ||
                !xr_numeric_float_to_int(XR_TO_FLOAT(val), target_rep, pointer_bits, &converted))
                VM_RUNTIME_ERROR(XR_ERR_OVERFLOW, XR_ERROR_CORE_NUMERIC_CONVERSION_RANGE_MSG);
            R(a) = xr_int(converted);
        } else {
            if (!XR_IS_INT(val))
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "numeric conversion requires integer");
            R(a) = xr_int(
                xr_numeric_int_convert_i64(XR_TO_INT(val), source_rep, target_rep, pointer_bits));
        }
        vmbreak;
    }
    if (XR_IS_INT(val)) {
        R(a) = val;
    } else if (XR_IS_FLOAT(val)) {
        R(a) = xr_int((xr_Integer) XR_TO_FLOAT(val));
    } else if (XR_IS_RUNE(val)) {
        /* int(char) yields the Unicode codepoint. */
        R(a) = xr_int((xr_Integer) XR_TO_RUNE(val));
    } else if (XR_IS_STRING(val)) {
        XrString *str = XR_TO_STRING(val);
        XrStringCoreParseIntResult parsed = xr_string_core_parse_int64(str->data, str->length);
        R(a) = parsed.ok ? xr_int((xr_Integer) parsed.value) : xr_null();
    } else if (XR_IS_BOOL(val)) {
        R(a) = xr_int(XR_TO_BOOL(val) ? 1 : 0);
    } else {
        R(a) = xr_null();
    }
    vmbreak;
}

vmcase(OP_TOFLOAT) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    uint16_t conversion = (uint16_t) GETARG_C(i);
    XrValue val = R(b);
    if (xr_conversion_bytecode_is_numeric(conversion)) {
        uint8_t source_rep = xr_conversion_bytecode_source_rep(conversion);
        uint8_t target_rep = xr_conversion_bytecode_target_rep(conversion);
        uint8_t pointer_bits = xr_conversion_bytecode_pointer_bits(conversion);
        if (source_rep == XR_NATIVE_F32 || source_rep == XR_NATIVE_F64) {
            if (!XR_IS_FLOAT(val))
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "numeric conversion requires float");
            R(a) = xr_float(xr_numeric_float_convert(XR_TO_FLOAT(val), target_rep));
        } else {
            if (!XR_IS_INT(val))
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "numeric conversion requires integer");
            R(a) = xr_float(
                xr_numeric_int_to_float(XR_TO_INT(val), source_rep, target_rep, pointer_bits));
        }
        vmbreak;
    }
    if (XR_IS_FLOAT(val)) {
        R(a) = val;
    } else if (XR_IS_INT(val)) {
        R(a) = xr_float((xr_Number) XR_TO_INT(val));
    } else if (XR_IS_STRING(val)) {
        XrString *str = XR_TO_STRING(val);
        XrStringCoreParseFloatResult parsed = xr_string_core_parse_float64(str->data, str->length);
        R(a) = parsed.ok ? xr_float(parsed.value) : xr_null();
    } else if (XR_IS_BOOL(val)) {
        R(a) = xr_float(XR_TO_BOOL(val) ? 1.0 : 0.0);
    } else {
        R(a) = xr_null();
    }
    vmbreak;
}

vmcase(OP_TOSTRING) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int slot_hint = GETARG_C(i);

    XrValue val;
    if (slot_hint == 1) {
        // Raw I64: format directly
        int64_t raw = R(b).i;
        char buf[24];
        int len = snprintf(buf, sizeof(buf), "%" PRId64, raw);
        R(a) = xr_string_value(xr_string_intern(isolate, buf, len, 0));
        vmbreak;
    } else if (slot_hint == 2) {
        // Raw F64: format directly
        val = XR_FROM_FLOAT(R(b).f);
        R(a) = xr_string_value(xr_value_to_string(isolate, val));
        vmbreak;
    } else if (slot_hint == 3) {
        // Raw U64: same bits as I64, formatted through the static uint view.
        R(a) = xr_string_value(xr_string_from_uint64(isolate, (uint64_t) R(b).i));
        vmbreak;
    }

    val = R(b);
    if (XR_IS_STRING(val)) {
        R(a) = val;
    } else {
        /* All types including null: explicit string() conversion always
         * produces a string (null → "null", not null propagation). */
        R(a) = xr_string_value(xr_value_to_string(isolate, val));
    }
    vmbreak;
}

vmcase(OP_TOBOOL) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue val = R(b);
    bool result = true;
    if (XR_IS_BOOL(val)) {
        result = XR_TO_BOOL(val);
    } else if (XR_IS_NULL(val)) {
        result = false;
    } else if (XR_IS_INT(val)) {
        result = XR_TO_INT(val) != 0;
    } else if (XR_IS_FLOAT(val)) {
        result = XR_TO_FLOAT(val) != 0.0;
    } else if (XR_IS_STRING(val)) {
        XrString *str = XR_TO_STRING(val);
        result = str->length > 0;
    } else if (XR_IS_ARRAY(val)) {
        XrArray *arr = XR_TO_ARRAY(val);
        result = arr->length > 0;
    } else if (XR_IS_MAP(val)) {
        XrMap *map = XR_TO_MAP(val);
        result = xr_map_size(map) > 0;
    } else if (XR_IS_SET(val)) {
        XrSet *set = XR_TO_SET(val);
        result = xr_set_size(set) > 0;
    }
    R(a) = xr_bool(result);
    vmbreak;
}

vmcase(OP_COPY) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int storage_mode = vm_resolve_allocation_storage_mode(base, (uint8_t) GETARG_C(i));
    XrValue _src = R(b);
    if (XR_IS_AGG_REF(_src) && !XR_IS_ARRAY_REF(_src) && !XR_IS_SLICE_REF(_src)) {
        R(a) = xr_struct_materialize_instance(isolate, _src);
        if (XR_IS_NULL(R(a)))
            VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "failed to materialize value-struct copy");
        vmbreak;
    }
    if (XR_IS_SLICE_REF(_src)) {
        XrSliceView *span = XR_TO_SLICE_REF(_src);
        uint8_t elem_type = XR_SLICE_REF_ELEM_TYPE(_src);
        uint16_t elem_size = XR_SLICE_REF_ELEM_SIZE(_src);
        if (!span || span->length < 0 || span->length > INT32_MAX || elem_type >= XR_ELEM_COUNT ||
            elem_size == 0 || (span->length > 0 && !span->data)) {
            VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "Slice copy length exceeds VM array limit");
        }
        XrArray *arr = NULL;
        if (storage_mode == XR_OBJ_STORAGE_SHARED || storage_mode == XR_OBJ_STORAGE_TRANSFER) {
            arr = (XrArray *) xr_sysheap_alloc_storage(xr_isolate_get_sys_heap(isolate),
                                                       sizeof(XrArray), XR_TARRAY,
                                                       (uint8_t) storage_mode);
            if (arr) {
                xr_array_init_inplace(arr, span->length > 0 ? (int) span->length : 0, elem_type);
                XR_OBJ_SET_STORAGE(&arr->hdr, storage_mode);
                if (storage_mode == XR_OBJ_STORAGE_SHARED)
                    xr_shared_set_refc(&arr->hdr, 1);
            }
        } else {
            arr = xr_array_with_capacity_typed(VM_CURRENT_CORO, (int32_t) span->length,
                                               (XrArrayElemType) elem_type);
        }
        if (!arr)
            VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "Slice copy failed");
        arr->elem_tid = XR_SLICE_REF_ELEM_TID(_src);
        if (arr->elem_type == XR_ELEM_ANY) {
            XrValue *items = (XrValue *) span->data;
            for (int64_t idx = 0; idx < span->length; idx++)
                xr_array_push(arr,
                              xr_deep_copy_explicit_to_coro(isolate, items[idx], VM_CURRENT_CORO));
            arr->contains_refs = XR_SLICE_REF_CONTAINS_REFS(_src);
        } else {
            if (span->length > 0) {
                if (!arr->data)
                    VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "Slice copy failed");
                memcpy(arr->data, span->data, (size_t) span->length * elem_size);
            }
            arr->length = span->length;
        }
        R(a) = xr_value_from_array(arr);
        vmbreak;
    }
    // Fast path: flat-copyable struct (alloc + memcpy, no recursion)
    if (XR_IS_PTR(_src) && XR_HEAP_TYPE(_src) == XR_TINSTANCE) {
        XrInstance *_inst = (XrInstance *) XR_TO_PTR(_src);
        XrClass *_cls = _inst->klass;
        if (storage_mode == XR_OBJ_STORAGE_NORMAL &&
            (_cls->flags & (XR_CLASS_VALUE_TYPE | XR_CLASS_FLAT_COPYABLE)) ==
                (XR_CLASS_VALUE_TYPE | XR_CLASS_FLAT_COPYABLE)) {
            XrInstance *_new = xr_instance_clone(isolate, _inst);
            if (_new) {
                R(a) = XR_FROM_PTR(_new);
                vmbreak;
            }
        }
    }
    if (storage_mode == XR_OBJ_STORAGE_SHARED || storage_mode == XR_OBJ_STORAGE_TRANSFER)
        R(a) = xr_deep_copy_explicit_to_storage(isolate, _src, (uint8_t) storage_mode);
    else
        R(a) = xr_deep_copy_explicit_to_coro(isolate, _src, (XrCoroutine *) vm_ctx->current_coro);
    /* Deep copy fails whole (returns null for a non-null source) when the
     * graph exceeds XR_DEEP_COPY_MAX_DEPTH or allocation fails (R2-6);
     * surface it instead of silently materializing null. */
    if (XR_IS_NULL(R(a)) && !XR_IS_NULL(_src))
        VM_RUNTIME_ERROR(XR_ERR_STACK_OVERFLOW,
                         "copy() failed: object graph exceeds max deep-copy depth (1000) or out "
                         "of memory");
    vmbreak;
}

vmcase(OP_CHR) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue val = R(b);
    if (XR_IS_INT(val)) {
        xr_Integer cp = XR_TO_INT(val);
        if (cp >= 0 && xr_unicode_is_scalar((uint32_t) cp)) {
            char buf[4];
            int len = 0;
            if (cp < 0x80) {
                buf[0] = (char) cp;
                len = 1;
            } else if (cp < 0x800) {
                buf[0] = (char) (0xC0 | (cp >> 6));
                buf[1] = (char) (0x80 | (cp & 0x3F));
                len = 2;
            } else if (cp < 0x10000) {
                buf[0] = (char) (0xE0 | (cp >> 12));
                buf[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
                buf[2] = (char) (0x80 | (cp & 0x3F));
                len = 3;
            } else {
                buf[0] = (char) (0xF0 | (cp >> 18));
                buf[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
                buf[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
                buf[3] = (char) (0x80 | (cp & 0x3F));
                len = 4;
            }
            R(a) = xr_string_value(xr_string_intern(isolate, buf, len, 0));
        } else {
            R(a) = xr_null();
        }
    } else {
        R(a) = xr_null();
    }
    vmbreak;
}

vmcase(OP_TORUNE) {
    /* char(x): construct a Unicode scalar char.
     *   - char(char) is identity.
     *   - char(int) validates the scalar range (excluding surrogates).
     *   - invalid codepoint / non-int yields null (caller may guard). */
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue val = R(b);
    if (XR_IS_RUNE(val)) {
        R(a) = val;
    } else if (XR_IS_INT(val)) {
        xr_Integer cp = XR_TO_INT(val);
        if (cp >= 0 && xr_unicode_is_scalar((uint32_t) cp))
            R(a) = xr_rune((uint32_t) cp);
        else
            R(a) = xr_null();
    } else {
        R(a) = xr_null();
    }
    vmbreak;
}
