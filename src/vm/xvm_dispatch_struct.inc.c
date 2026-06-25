/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_struct.inc.c — typed-array, typed-field, struct dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, vm_ctx, R,
 * vmcase, vmbreak, VM_RUNTIME_ERROR,
 * VM_FRAME_COUNT, TRACE_EXECUTION, ...) provided by the
 * surrounding scope. CMake excludes *.inc.c from the VM_SRC
 * glob.
 *
 * Owns:
 *   - OP_TARRAY_GET / GETC / SET / PUSH  — typed compact array R/W
 *   - OP_TFIELD_GET / SET                — typed json field R/W
 *   - OP_NEW_STRUCT / STRUCT_GET / SET / COPY  — stack struct lifecycle
 *
 * Placeholder OP_NOP is kept inline in xvm.c next to the
 * computed-goto fallback / loop close.
 */

vmcase(OP_TARRAY_GET) {
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrArray *arr = XR_TO_ARRAY(R(b));
    int32_t idx = (int32_t) R(c).i;
    if (XR_UNLIKELY(idx < 0 || idx >= arr->length)) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "typed array index %d out of bounds [0, %d)",
                         idx, arr->length);
    }
    R(a) = xr_typed_get(arr->data, idx, arr->elem_type);
    vmbreak;
}

vmcase(OP_TARRAY_GETC) {
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrArray *arr = XR_TO_ARRAY(R(b));
    if (XR_UNLIKELY(c < 0 || c >= arr->length)) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "typed array index %d out of bounds [0, %d)",
                         c, arr->length);
    }
    R(a) = xr_typed_get(arr->data, (int32_t) c, arr->elem_type);
    vmbreak;
}

vmcase(OP_TARRAY_SET) {
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrArray *arr = XR_TO_ARRAY(R(a));
    int32_t idx = (int32_t) R(b).i;
    if (XR_UNLIKELY(idx < 0 || idx >= arr->length)) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "typed array index %d out of bounds [0, %d)",
                         idx, arr->length);
    }
    if (xr_typed_set(arr->data, idx, R(c), arr->elem_type)) {
        XR_ARRAY_MARK_REFS(arr, R(c));
    }
    vmbreak;
}

vmcase(OP_TARRAY_PUSH) {
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrArray *arr = XR_TO_ARRAY(R(a));
    if (XR_UNLIKELY(xr_array_is_slice(arr))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, XR_ERROR_CORE_ARRAY_SLICE_PUSH_MSG);
    }
    if (arr->length >= arr->capacity) {
        xr_array_grow(arr);
    }
    int32_t idx = arr->length++;
    if (xr_typed_set(arr->data, idx, R(b), arr->elem_type)) {
        XR_ARRAY_MARK_REFS(arr, R(b));
    }
    vmbreak;
}

vmcase(OP_TFIELD_GET) {
    TRACE_EXECUTION();
    XrJson *json = (XrJson *) XR_TO_PTR(R(GETARG_B(i)));
    R(GETARG_A(i)) = json->fields[GETARG_C(i)];
    vmbreak;
}

vmcase(OP_TFIELD_SET) {
    TRACE_EXECUTION();
    XrJson *json = (XrJson *) XR_TO_PTR(R(GETARG_A(i)));
    XrValue _tfv = R(GETARG_C(i));
    json->fields[GETARG_B(i)] = _tfv;
    vmbreak;
}

/* === Struct Native Storage === */

vmcase(OP_NEW_STRUCT) {
    /* A = dest reg, B = class reg, C = struct_area slot offset
     * Allocate struct in per-frame struct_area (zero heap allocation). */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue class_val = R(b);
    XrClass *cls = xr_value_to_class(class_val);
    XrStructLayout *layout = cls->struct_layout;
    XR_DCHECK(layout != NULL, "OP_NEW_STRUCT requires struct_layout");
    uint16_t layout_id = xr_vm_struct_layout_register(&isolate->vm, layout);
    if (XR_UNLIKELY(layout_id == 0 && xr_struct_layout_is_headerless(layout))) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "failed to register @repr(C) struct layout");
    }
    XR_DCHECK(vm_ctx->struct_areas && vm_ctx->struct_areas[VM_FRAME_COUNT - 1],
              "OP_NEW_STRUCT requires allocated struct_area");

    uint8_t *struct_ptr = vm_ctx->struct_areas[VM_FRAME_COUNT - 1] + c * 16;
    uint16_t header_size = xr_struct_layout_header_size(layout);
    if (header_size)
        *(XrClass **) struct_ptr = cls;
    uint8_t *payload = struct_ptr + header_size;
    memset(payload, 0, layout->total_size);

    // Apply field default values from class descriptor
    if (cls->field_default_values) {
        int fc = layout->field_count < cls->field_count ? layout->field_count : cls->field_count;
        for (int fi = 0; fi < fc; fi++) {
            XrValue dv = cls->field_default_values[fi];
            if (dv.tag == XR_TAG_NULL)
                continue;
            XrStructFieldLayout *fl = &layout->fields[fi];
            uint8_t *fp = payload + fl->offset;
            switch (fl->native_type) {
                case XR_NATIVE_I64:
                    *(int64_t *) fp = XR_TO_INT(dv);
                    break;
                case XR_NATIVE_U64:
                    *(uint64_t *) fp = (uint64_t) XR_TO_INT(dv);
                    break;
                case XR_NATIVE_F64:
                    *(double *) fp = XR_TO_FLOAT(dv);
                    break;
                case XR_NATIVE_BOOL:
                    *(uint8_t *) fp = (uint8_t) dv.i;
                    break;
                case XR_NATIVE_I32:
                    *(int32_t *) fp = (int32_t) XR_TO_INT(dv);
                    break;
                case XR_NATIVE_U32:
                    *(uint32_t *) fp = (uint32_t) XR_TO_INT(dv);
                    break;
                case XR_NATIVE_I16:
                    *(int16_t *) fp = (int16_t) XR_TO_INT(dv);
                    break;
                case XR_NATIVE_U16:
                    *(uint16_t *) fp = (uint16_t) XR_TO_INT(dv);
                    break;
                case XR_NATIVE_I8:
                    *(int8_t *) fp = (int8_t) XR_TO_INT(dv);
                    break;
                case XR_NATIVE_U8:
                    *(uint8_t *) fp = (uint8_t) XR_TO_INT(dv);
                    break;
                case XR_NATIVE_F32:
                    *(float *) fp = (float) XR_TO_FLOAT(dv);
                    break;
                case XR_NATIVE_STRING:
                    *(XrString **) fp = (XrString *) dv.ptr;
                    break;
                case XR_NATIVE_ARRAY_REF:
                case XR_NATIVE_MAP_REF:
                case XR_NATIVE_SET_REF:
                    *(XrValue *) fp = dv;
                    break;
                default:
                    break;
            }
        }
    }

    R(a) = xr_struct_ref(struct_ptr, layout_id);
    vmbreak;
}

vmcase(OP_STRUCT_GET) {
    /* R[A] = struct(R[B]).field[C]
     * Read native field from stack-allocated struct, box to XrValue */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    XrStructLayout *layout = NULL;
    uint8_t *payload = xr_vm_struct_ref_payload(isolate, R(b), &layout);
    if (XR_UNLIKELY(!payload || !layout || c < 0 || c >= layout->field_count)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct field read");
    }
    XrStructFieldLayout *field = &layout->fields[c];
    uint8_t *fp = payload + field->offset;

    switch (field->native_type) {
        case XR_NATIVE_I64:
            R(a) = XR_FROM_INT(*(int64_t *) fp);
            break;
        case XR_NATIVE_U64:
            R(a) = XR_FROM_INT((int64_t) *(uint64_t *) fp);
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
        case XR_NATIVE_U32:
            R(a) = XR_FROM_INT((int64_t) *(uint32_t *) fp);
            break;
        case XR_NATIVE_I16:
            R(a) = XR_FROM_INT((int64_t) *(int16_t *) fp);
            break;
        case XR_NATIVE_U16:
            R(a) = XR_FROM_INT((int64_t) *(uint16_t *) fp);
            break;
        case XR_NATIVE_I8:
            R(a) = XR_FROM_INT((int64_t) *(int8_t *) fp);
            break;
        case XR_NATIVE_U8:
            R(a) = XR_FROM_INT((int64_t) *(uint8_t *) fp);
            break;
        case XR_NATIVE_F32:
            R(a) = XR_FROM_FLOAT((double) *(float *) fp);
            break;
        case XR_NATIVE_STRING: {
            XrString *s = *(XrString **) fp;
            R(a) = s ? XR_FROM_STR(s) : xr_null();
            break;
        }
        case XR_NATIVE_STRUCT:
            if (field->sub_layout && field->sub_layout_id == 0)
                field->sub_layout_id =
                    xr_vm_struct_layout_register(&isolate->vm, field->sub_layout);
            R(a) = xr_struct_ref(fp, field->sub_layout_id);
            break;
        case XR_NATIVE_ARRAY:
            R(a) = xr_array_ref(fp, field->elem_native_type, field->elem_count);
            break;
        case XR_NATIVE_ARRAY_REF:
        case XR_NATIVE_MAP_REF:
        case XR_NATIVE_SET_REF:
            R(a) = *(XrValue *) fp;
            break;
        default:
            R(a) = xr_null();
            break;
    }
    vmbreak;
}

vmcase(OP_STRUCT_SET) {
    /* struct(R[A]).field[B] = R[C]
     * Unbox XrValue and write native field to stack-allocated struct */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    XrStructLayout *layout = NULL;
    uint8_t *payload = xr_vm_struct_ref_payload(isolate, R(a), &layout);
    if (XR_UNLIKELY(!payload || !layout || b < 0 || b >= layout->field_count)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct field write");
    }
    XrStructFieldLayout *field = &layout->fields[b];
    uint8_t *fp = payload + field->offset;
    XrValue src = R(c);

    switch (field->native_type) {
        case XR_NATIVE_I64:
            *(int64_t *) fp = XR_TO_INT(src);
            break;
        case XR_NATIVE_F64:
            *(double *) fp = XR_TO_FLOAT(src);
            break;
        case XR_NATIVE_BOOL:
            *(uint8_t *) fp = (uint8_t) src.i;
            break;
        case XR_NATIVE_I32:
            *(int32_t *) fp = (int32_t) XR_TO_INT(src);
            break;
        case XR_NATIVE_U32:
            *(uint32_t *) fp = (uint32_t) XR_TO_INT(src);
            break;
        case XR_NATIVE_I16:
            *(int16_t *) fp = (int16_t) XR_TO_INT(src);
            break;
        case XR_NATIVE_U16:
            *(uint16_t *) fp = (uint16_t) XR_TO_INT(src);
            break;
        case XR_NATIVE_I8:
            *(int8_t *) fp = (int8_t) XR_TO_INT(src);
            break;
        case XR_NATIVE_U8:
            *(uint8_t *) fp = (uint8_t) XR_TO_INT(src);
            break;
        case XR_NATIVE_F32:
            *(float *) fp = (float) XR_TO_FLOAT(src);
            break;
        case XR_NATIVE_STRING: {
            *(XrString **) fp = (XrString *) src.ptr;
            break;
        }
        case XR_NATIVE_STRUCT: {
            if (!vm_struct_write_field_bytes(isolate, fp, field, src)) {
                VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                                 "cannot assign non-struct value to nested struct field");
            }
            break;
        }
        case XR_NATIVE_ARRAY: {
            // Copy from heap Array into inline storage
            if (XR_IS_ARRAY(src)) {
                XrArray *arr = (XrArray *) src.ptr;
                int count = arr->length < field->elem_count ? arr->length : field->elem_count;
                uint8_t es = xr_native_type_size(field->elem_native_type);
                for (int idx = 0; idx < count; idx++) {
                    XrValue elem = xr_array_get(arr, idx);
                    uint8_t *ep = fp + idx * es;
                    switch (field->elem_native_type) {
                        case XR_NATIVE_I64:
                            *(int64_t *) ep = XR_TO_INT(elem);
                            break;
                        case XR_NATIVE_F64:
                            *(double *) ep = XR_TO_FLOAT(elem);
                            break;
                        case XR_NATIVE_BOOL:
                            *(uint8_t *) ep = (uint8_t) elem.i;
                            break;
                        case XR_NATIVE_I32:
                            *(int32_t *) ep = (int32_t) XR_TO_INT(elem);
                            break;
                        case XR_NATIVE_U32:
                            *(uint32_t *) ep = (uint32_t) XR_TO_INT(elem);
                            break;
                        case XR_NATIVE_I16:
                            *(int16_t *) ep = (int16_t) XR_TO_INT(elem);
                            break;
                        case XR_NATIVE_U16:
                            *(uint16_t *) ep = (uint16_t) XR_TO_INT(elem);
                            break;
                        case XR_NATIVE_I8:
                            *(int8_t *) ep = (int8_t) XR_TO_INT(elem);
                            break;
                        case XR_NATIVE_U8:
                            *(uint8_t *) ep = (uint8_t) XR_TO_INT(elem);
                            break;
                        case XR_NATIVE_F32:
                            *(float *) ep = (float) XR_TO_FLOAT(elem);
                            break;
                        default:
                            break;
                    }
                }
                // Zero remaining elements if array shorter than field
                if (count < field->elem_count) {
                    memset(fp + count * es, 0, (field->elem_count - count) * es);
                }
            } else if (XR_IS_ARRAY_REF(src)) {
                // Copy from another struct's array_ref
                memcpy(fp, src.ptr, field->size);
            }
            break;
        }
        case XR_NATIVE_ARRAY_REF:
        case XR_NATIVE_MAP_REF:
        case XR_NATIVE_SET_REF:
            *(XrValue *) fp = src;
            break;
        default:
            break;
    }
    vmbreak;
}

vmcase(OP_STRUCT_COPY) {
    /* R[A] = deep copy of struct R[B], placed at struct_area slot C
     * memcpy entire struct (class ptr + field data) */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    XrValue src = R(b);
    uint8_t *src_ptr = (uint8_t *) xr_to_struct_ptr(src);
    XrStructLayout *layout = xr_vm_struct_ref_layout(isolate, src);
    if (XR_UNLIKELY(!src_ptr || !layout)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct copy");
    }
    uint16_t layout_id = xr_struct_layout_id(src);
    if (layout_id == 0)
        layout_id = xr_vm_struct_layout_register(&isolate->vm, layout);

    uint8_t *dst_ptr = vm_ctx->struct_areas[VM_FRAME_COUNT - 1] + c * 16;

    memcpy(dst_ptr, src_ptr, xr_struct_layout_storage_size(layout));
    R(a) = xr_struct_ref(dst_ptr, layout_id);
    vmbreak;
}
