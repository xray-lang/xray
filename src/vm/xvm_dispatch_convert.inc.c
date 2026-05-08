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
    ** C: bit0=newline, bit1-2=slot_type hint (0=ANY, 1=I64, 2=F64)
    **
    ** If value is instance with toString() method, call it first
    */
    int a = GETARG_A(i);
    int add_space = GETARG_B(i);
    int c_field = GETARG_C(i);
    int newline = c_field & 1;
    int slot_hint = (c_field >> 1) & 3;

    // Reconstruct tagged value from raw slot if hint provided
    XrValue val;
    if (slot_hint == 1) {
        val = XR_FROM_INT(R(a).i);
    } else if (slot_hint == 2) {
        val = XR_FROM_FLOAT(R(a).f);
    } else {
        val = R(a);
    }

    if (add_space)
        printf(" ");

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
    printf("%s", print_str->data);
    if (newline)
        printf("\n");

    vmbreak;
}

vmcase(OP_TYPEOF) {
    // Returns int (XrTypeId) for fast comparison
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
    R(a) = XR_FROM_INT((int64_t) xr_value_typeid(val));
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
    const char *type_name = NULL;
    // For instances, return class name
    if (xr_value_is_instance(val)) {
        XrInstance *inst = xr_value_to_instance(val);
        XrClass *cls = xr_instance_get_class(inst);
        if (cls && cls->name)
            type_name = cls->name;
    }
    // For enum values, return enum name
    if (type_name == NULL && XR_IS_ENUM_VALUE(val)) {
        XrEnumValue *ev = (XrEnumValue *) XR_TO_PTR(val);
        if (ev->enum_name)
            type_name = ev->enum_name;
    }
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
    xr_value_dump(val, b);
    vmbreak;
}

vmcase(OP_TOINT) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue val = R(b);
    if (XR_IS_INT(val)) {
        R(a) = val;
    } else if (XR_IS_FLOAT(val)) {
        R(a) = xr_int((xr_Integer) XR_TO_FLOAT(val));
    } else if (XR_IS_STRING(val)) {
        XrString *str = XR_TO_STRING(val);
        const char *p = str->data;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        char *end;
        long long v = strtoll(p, &end, 10);
        R(a) = (end == p) ? xr_null() : xr_int((xr_Integer) v);
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
    XrValue val = R(b);
    if (XR_IS_FLOAT(val)) {
        R(a) = val;
    } else if (XR_IS_INT(val)) {
        R(a) = xr_float((xr_Number) XR_TO_INT(val));
    } else if (XR_IS_STRING(val)) {
        XrString *str = XR_TO_STRING(val);
        const char *p = str->data;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        char *end;
        double v = strtod(p, &end);
        R(a) = (end == p) ? xr_null() : xr_float(v);
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
    XrValue _src = R(b);
    // Fast path: flat-copyable struct (alloc + memcpy, no recursion)
    if (XR_IS_PTR(_src) && XR_HEAP_TYPE(_src) == XR_TINSTANCE) {
        XrInstance *_inst = (XrInstance *) XR_TO_PTR(_src);
        XrClass *_cls = _inst->klass;
        if ((_cls->flags & (XR_CLASS_VALUE_TYPE | XR_CLASS_FLAT_COPYABLE)) ==
            (XR_CLASS_VALUE_TYPE | XR_CLASS_FLAT_COPYABLE)) {
            uint32_t _fc = xr_class_instance_field_count(_cls);
            size_t _sz = sizeof(XrInstance) + sizeof(XrValue) * _fc;
            XrInstance *_new =
                (XrInstance *) xr_gc_alloc(xr_isolate_get_gc(isolate), _sz, XR_TINSTANCE);
            if (_new) {
                memcpy(_new->fields, _inst->fields, sizeof(XrValue) * _fc);
                _new->klass = _cls;
                _new->gc.extra = (_new->gc.extra & 0x01) | (_inst->gc.extra & ~0x01);
                R(a) = XR_FROM_PTR(_new);
                vmbreak;
            }
        }
    }
    R(a) = xr_deep_copy_to_coro(isolate, _src, (XrCoroutine *) vm_ctx->current_coro);
    vmbreak;
}

vmcase(OP_CHR) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue val = R(b);
    if (XR_IS_INT(val)) {
        xr_Integer cp = XR_TO_INT(val);
        if (cp >= 0 && cp <= XR_UNICODE_MAX) {
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
