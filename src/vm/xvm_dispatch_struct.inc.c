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
 *   - OP_AGG_NEW / AGG_GET / SET / COPY  — stack struct lifecycle
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
    VM_ARRAY_CHECK_STORABLE(arr, R(c));
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
    VM_ARRAY_CHECK_STORABLE(arr, R(b));
    if (xr_typed_set(arr->data, idx, R(b), arr->elem_type)) {
        XR_ARRAY_MARK_REFS(arr, R(b));
    }
    vmbreak;
}

vmcase(OP_TFIELD_GET) {
    TRACE_EXECUTION();
    XrObjectInstance *json = (XrObjectInstance *) XR_TO_PTR(R(GETARG_B(i)));
    R(GETARG_A(i)) = json->fields[GETARG_C(i)];
    vmbreak;
}

vmcase(OP_TFIELD_SET) {
    TRACE_EXECUTION();
    XrObjectInstance *json = (XrObjectInstance *) XR_TO_PTR(R(GETARG_A(i)));
    XrValue _tfv = R(GETARG_C(i));
    json->fields[GETARG_B(i)] = _tfv;
    vmbreak;
}

/* === Struct Native Storage === */

vmcase(OP_AGG_NEW) {
    /* A = dest reg, B = class reg, C = struct_area slot offset
     * Allocate an aggregate in per-frame struct_area (zero heap allocation). */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrValue class_val = R(b);
    XrClass *cls = xr_value_to_class(class_val);
    XrAggregateLayout *layout = cls->struct_layout;
    XR_DCHECK(layout != NULL, "OP_AGG_NEW requires struct_layout");
    uint16_t layout_id = xr_vm_struct_layout_register(&isolate->vm, layout);
    if (layout->nominal_name &&
        XR_UNLIKELY(!xr_vm_struct_layout_bind_class(&isolate->vm, layout, cls))) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "value-struct layout/class identity collision");
    }
    if (XR_UNLIKELY(layout_id == 0 && xr_aggregate_layout_is_headerless(layout))) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "failed to register fixed-layout aggregate");
    }
    XR_DCHECK(vm_ctx->struct_areas && vm_ctx->struct_areas[VM_FRAME_COUNT - 1],
              "OP_AGG_NEW requires allocated struct_area");

    uint8_t *struct_ptr = vm_ctx->struct_areas[VM_FRAME_COUNT - 1] + c * 16;
    uint16_t header_size = xr_aggregate_layout_header_size(layout);
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
            XrAggregateFieldLayout *fl = &layout->fields[fi];
            uint8_t *fp = payload + fl->offset;
            (void) xr_vm_struct_write_field_value(isolate, fp, fl, dv);
        }
    }

    R(a) = xr_aggregate_ref(struct_ptr, layout_id);
    vmbreak;
}

vmcase(OP_AGG_GET) {
    /* R[A] = struct(R[B]).field[C]
     * Read native field from stack-allocated struct, box to XrValue */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    XrAggregateLayout *layout = NULL;
    uint8_t *payload = xr_vm_struct_ref_payload(isolate, R(b), &layout);
    if (XR_UNLIKELY(!payload || !layout || c < 0 || c >= layout->field_count)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct field read");
    }
    XrAggregateFieldLayout *field = &layout->fields[c];
    uint8_t *fp = payload + field->offset;
    if (!xr_vm_struct_read_field_value(isolate, fp, field, &R(a))) {
        R(a) = xr_null();
    }
    vmbreak;
}

vmcase(OP_AGG_SET) {
    /* struct(R[A]).field[B] = R[C]
     * Unbox XrValue and write native field to stack-allocated struct */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    XrAggregateLayout *layout = NULL;
    uint8_t *payload = xr_vm_struct_ref_payload(isolate, R(a), &layout);
    if (XR_UNLIKELY(!payload || !layout || b < 0 || b >= layout->field_count)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct field write");
    }
    XrAggregateFieldLayout *field = &layout->fields[b];
    uint8_t *fp = payload + field->offset;
    XrValue src = R(c);
    if (!xr_vm_struct_write_field_value(isolate, fp, field, src)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid value for struct field write");
    }
    vmbreak;
}

vmcase(OP_AGG_COPY) {
    /* R[A] = deep copy of aggregate R[B], placed at struct_area slot C
     * memcpy entire struct (class ptr + field data) */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    XrValue src = R(b);
    uint8_t *src_ptr = (uint8_t *) xr_to_struct_ptr(src);
    XrAggregateLayout *layout = xr_vm_struct_ref_layout(isolate, src);
    if (XR_UNLIKELY(!src_ptr || !layout)) {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "invalid struct copy");
    }
    uint16_t layout_id = xr_aggregate_layout_id(src);
    if (layout_id == 0)
        layout_id = xr_vm_struct_layout_register(&isolate->vm, layout);

    uint8_t *dst_ptr = vm_ctx->struct_areas[VM_FRAME_COUNT - 1] + c * 16;

    memcpy(dst_ptr, src_ptr, xr_aggregate_layout_storage_size(layout));
    R(a) = xr_aggregate_ref(dst_ptr, layout_id);
    vmbreak;
}

vmcase(OP_FIXED_ARRAY_NEW) {
    /* A = dest reg, B = struct_area slot offset, C = const metadata
     * metadata packs (elem_count << 8) | elem_native_type. */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);

    if (XR_UNLIKELY(c < 0 || c >= PROTO_CONST_COUNT(cl->proto) || !XR_IS_INT(K(c)))) {
        VM_RUNTIME_ERROR(XR_ERR_RUNTIME, "invalid fixed array metadata");
    }
    uint32_t meta = (uint32_t) XR_TO_INT(K(c));
    uint8_t elem_native_type = (uint8_t) (meta & 0xFFu);
    uint16_t elem_count = (uint16_t) (meta >> 8);
    if (XR_UNLIKELY(elem_count == 0)) {
        VM_RUNTIME_ERROR(XR_ERR_RUNTIME, "invalid fixed array length");
    }
    XR_DCHECK(vm_ctx->struct_areas && vm_ctx->struct_areas[VM_FRAME_COUNT - 1],
              "OP_FIXED_ARRAY_NEW requires allocated struct_area");

    uint8_t *array_ptr = vm_ctx->struct_areas[VM_FRAME_COUNT - 1] + (uint16_t) b * 16u;
    uint32_t bytes = (uint32_t) elem_count *
                     (uint32_t) xr_native_type_size(xr_target_data_layout_host(), elem_native_type);
    memset(array_ptr, 0, bytes);
    R(a) = xr_array_ref(array_ptr, elem_native_type, elem_count);
    vmbreak;
}

vmcase(OP_FIXED_BYTES_CONST) {
    /* A = string payload in / fixed-array ref out, Bx = 16-byte storage slot. */
    TRACE_EXECUTION();
    int a = GETARG_A(i);
    int bx = GETARG_Bx(i);
    if (XR_UNLIKELY(!XR_IS_STRING(R(a)))) {
        VM_RUNTIME_ERROR(XR_ERR_RUNTIME, "invalid fixed byte payload");
    }
    XrString *payload = XR_TO_STRING(R(a));
    if (XR_UNLIKELY(!payload || payload->length > XR_ARRAY_REF_MAX_COUNT)) {
        VM_RUNTIME_ERROR(XR_ERR_RUNTIME, "fixed byte payload is too large");
    }
    XR_DCHECK(vm_ctx->struct_areas && vm_ctx->struct_areas[VM_FRAME_COUNT - 1],
              "OP_FIXED_BYTES_CONST requires allocated struct_area");
    uint8_t *array_ptr = vm_ctx->struct_areas[VM_FRAME_COUNT - 1] + (uint32_t) bx * 16u;
    if (payload->length > 0)
        memcpy(array_ptr, payload->data, payload->length);
    else
        array_ptr[0] = 0;
    R(a) = xr_array_ref(array_ptr, XR_NATIVE_U8, (uint32_t) payload->length);
    vmbreak;
}
