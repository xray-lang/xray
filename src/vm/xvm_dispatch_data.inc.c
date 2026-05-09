/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_data.inc.c — typed-storage box/unbox + StringBuilder
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, R, vmcase,
 * vmbreak, ...) provided by the surrounding scope. CMake
 * excludes *.inc.c from the VM_SRC glob.
 *
 * Owns the typed-payload boundary helpers and the inline
 * StringBuilder fast path that sit between the arithmetic and
 * bitwise blocks in source order:
 *
 *   OP_BOX_I64 / OP_BOX_F64           — raw payload -> tagged
 *   OP_UNBOX_I64 / OP_UNBOX_F64       — tagged -> raw payload
 *   OP_ARRAY_GET_NOCHECK              — checked-out array fetch
 *   OP_STRBUF_NEW / APPEND / FINISH   — StringBuilder hot path
 */

// Box/Unbox: typed storage (TypedArray/TypedField) ↔ tagged boundary
// BOX creates tagged value from raw payload
// UNBOX extracts raw payload with type check
vmcase(OP_BOX_I64) {
    R(GETARG_A(i)) = XR_FROM_INT(R(GETARG_B(i)).i);
    vmbreak;
}
vmcase(OP_BOX_F64) {
    R(GETARG_A(i)) = XR_FROM_FLOAT(R(GETARG_B(i)).f);
    vmbreak;
}
vmcase(OP_UNBOX_I64) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue src = R(b);
    if (XR_IS_INT(src)) {
        XR_SET_INT(R(a), XR_TO_INT(src));
    } else if (XR_IS_FLOAT(src)) {
        VM_RUNTIME_ERROR(
            XR_ERR_TYPE_MISMATCH,
            "cannot implicitly convert float to int (use int() for explicit conversion)");
    } else {
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "cannot assign non-int value to int variable");
    }
    vmbreak;
}
vmcase(OP_UNBOX_F64) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue src = R(b);
    if (XR_IS_FLOAT(src)) {
        R(a).f = XR_TO_FLOAT(src);
        R(a).tag = XR_TAG_F64;
    } else if (XR_IS_INT(src)) {
        // int → float promotion (allowed)
        R(a).f = (double) XR_TO_INT(src);
        R(a).tag = XR_TAG_F64;
    } else {
        // non-numeric → float is not allowed (second gate: runtime)
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH,
                         "cannot implicitly convert non-numeric value to float");
    }
    vmbreak;
}

// Narrow: truncate tagged int/float to sub-width, re-extend back.
// The value stays tagged — only the numeric range changes.
vmcase(OP_NARROW_I8) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_INT((int64_t)(int8_t)XR_TO_INT(R(b)));
    vmbreak;
}
vmcase(OP_NARROW_U8) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_INT((int64_t)(uint8_t)XR_TO_INT(R(b)));
    vmbreak;
}
vmcase(OP_NARROW_I16) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_INT((int64_t)(int16_t)XR_TO_INT(R(b)));
    vmbreak;
}
vmcase(OP_NARROW_U16) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_INT((int64_t)(uint16_t)XR_TO_INT(R(b)));
    vmbreak;
}
vmcase(OP_NARROW_I32) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_INT((int64_t)(int32_t)XR_TO_INT(R(b)));
    vmbreak;
}
vmcase(OP_NARROW_U32) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_INT((int64_t)(uint32_t)XR_TO_INT(R(b)));
    vmbreak;
}
vmcase(OP_NARROW_F32) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_FLOAT((double)(float)XR_TO_FLOAT(R(b)));
    vmbreak;
}

// Widen: sign/zero extend sub-width value back to full width.
// Semantically identical to NARROW for these bit widths, but placed
// at load points to make the extension direction explicit.
vmcase(OP_WIDEN_I8) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_INT((int64_t)(int8_t)XR_TO_INT(R(b)));
    vmbreak;
}
vmcase(OP_WIDEN_U8) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_INT((int64_t)(uint8_t)XR_TO_INT(R(b)));
    vmbreak;
}
vmcase(OP_WIDEN_I16) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_INT((int64_t)(int16_t)XR_TO_INT(R(b)));
    vmbreak;
}
vmcase(OP_WIDEN_U16) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_INT((int64_t)(uint16_t)XR_TO_INT(R(b)));
    vmbreak;
}
vmcase(OP_WIDEN_I32) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_INT((int64_t)(int32_t)XR_TO_INT(R(b)));
    vmbreak;
}
vmcase(OP_WIDEN_U32) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_INT((int64_t)(uint32_t)XR_TO_INT(R(b)));
    vmbreak;
}
vmcase(OP_WIDEN_F32) {
    int a = GETARG_A(i), b = GETARG_B(i);
    R(a) = XR_FROM_FLOAT((double)(float)XR_TO_FLOAT(R(b)));
    vmbreak;
}

vmcase(OP_ARRAY_GET_NOCHECK) {
    // Array access without bounds check (compiler proved index is valid)
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrArray *arr = XR_TO_ARRAY(R(b));
    int idx = (int) XR_TO_INT(R(c));
    R(a) = (arr->elem_type == XR_ELEM_ANY) ? ((XrValue *) arr->data)[idx]
                                           : xr_array_get_element(arr, idx);
    vmbreak;
}

/* === StringBuilder Instructions === */

vmcase(OP_STRBUF_NEW) {
    int a = GETARG_A(i);

    XrStringBuilder *sb = xr_stringbuilder_new(VM_CURRENT_CORO);
    R(a) = xr_stringbuilder_value(sb);
    checkGC(base + a + 1);
    vmbreak;
}

vmcase(OP_STRBUF_APPEND) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);

    XrStringBuilder *sb = xr_to_stringbuilder(R(a));

    XrValue vb = R(b);
    if (XR_IS_STRING(vb)) {
        xr_stringbuilder_append_cstr(sb, xr_value_str_data(&vb), xr_value_str_len(&vb));
    } else if (XR_IS_INT(vb)) {
        xr_stringbuilder_append_int(sb, XR_TO_INT(vb));
    } else if (XR_IS_FLOAT(vb)) {
        xr_stringbuilder_append_float(sb, XR_TO_FLOAT(vb));
    } else {
        XrString *str = xr_value_to_string(isolate, vb);
        xr_stringbuilder_append_str(sb, str);
    }
    vmbreak;
}

vmcase(OP_STRBUF_FINISH) {
    int a = GETARG_A(i);

    XrStringBuilder *sb = xr_to_stringbuilder(R(a));
    XrString *result = xr_stringbuilder_to_string(sb);

    R(a) = xr_string_value(result);
    checkGC(base + a + 1);
    vmbreak;
}
