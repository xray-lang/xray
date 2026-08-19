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

// Box/Unbox: typed storage (typed array / compact field) to tagged boundary
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

vmcase(OP_ENUM_DESCRIPTOR_BOX) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    int64_t token = XR_TO_INT(R(c));
    uint32_t layout_id = (uint32_t) ((uint64_t) token >> 8);
    uint8_t metadata_kind = (uint8_t) (token & 0xff);
    R(a) = xr_enum_descriptor_box_new((XrCoroutine *) VM_CURRENT_CORO, layout_id, metadata_kind,
                                      R(b).i);
    if (XR_IS_NULL(R(a)))
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "cannot allocate erased enum descriptor");
    vmbreak;
}

vmcase(OP_ENUM_DESCRIPTOR_UNBOX) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int64_t scalar = 0;
    if (!xr_enum_descriptor_scalar(R(b), &scalar))
        VM_RUNTIME_ERROR(XR_ERR_TYPE_MISMATCH, "value is not an erased enum descriptor");
    XR_SET_INT(R(a), scalar);
    vmbreak;
}

/* Narrow/widen tagged width handlers are generated from xisa/xi/lowering.def. */
#include "xvm_template_width_gen.inc.c"

vmcase(OP_ARRAY_GET_NOCHECK) {
    // Array access without bounds check (compiler proved index is valid)
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrArray *arr = XR_TO_ARRAY(R(b));
    int64_t idx = XR_TO_INT(R(c));
    R(a) = (arr->elem_type == XR_ELEM_ANY) ? ((XrValue *) arr->data)[idx]
                                           : xr_array_get_element(arr, (int32_t) idx);
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

/* Concatenation in the two passes the shared kernel states: measure every
 * part, allocate once, copy. Non-string parts render first, since a rendered
 * length is what the measurement needs; their text is owned until copied. */
vmcase(OP_STR_CONCAT_N) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int count = GETARG_C(i);

    XrStringConcatPartCore cores[XR_STR_CONCAT_INLINE_PARTS];
    XrString *rendered[XR_STR_CONCAT_INLINE_PARTS];
    for (int k = 0; k < count; k++) {
        XrValue part = R(b + k);
        if (XR_IS_STRING(part)) {
            rendered[k] = NULL;
            cores[k].a = xr_value_str_data(&part);
            cores[k].alen = xr_value_str_len(&part);
        } else {
            XrString *text = xr_value_to_string(isolate, part);
            rendered[k] = text;
            cores[k].a = text ? text->data : "";
            cores[k].alen = text ? (size_t) text->length : 0;
        }
        cores[k].b = NULL;
        cores[k].blen = 0;
        cores[k].joins_with_dot = 0;
    }

    XR_STRING_CONCAT_OWNER_GUARD(XR_SEM_OWNER_ID_SHARED_STRING_CONCAT_HI,
                                 XR_SEM_OWNER_ID_SHARED_STRING_CONCAT_LO);
    XR_STRING_CONCAT_CONSUMER_GUARD(XR_SEM_CONSUMER_VM);
    size_t total = xr_string_concat_total_core(cores, (size_t) count);
    if (total == SIZE_MAX) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "string concatenation length overflow");
    }

    char stack_buf[XR_STR_CONCAT_INLINE_BYTES];
    char *buf = total < sizeof(stack_buf) ? stack_buf : (char *) xr_malloc(total + 1u);
    if (!buf) {
        VM_RUNTIME_ERROR(XR_ERR_OUT_OF_MEMORY, "out of memory concatenating strings");
    }
    char *dst = buf;
    for (int k = 0; k < count; k++)
        dst = xr_string_concat_copy_core(dst, &cores[k]);
    *dst = 0;

    /* Interned, not coroutine-local: a concatenation can be a task's result,
     * and publishing one across a task boundary requires a shared or
     * transferable domain. xr_string_new produces an execution-local string,
     * which the publish check rejects. The builder path this instruction
     * replaced interned its result for the same reason. */
    XrString *result = xr_string_intern(isolate, buf, total, 0);
    if (buf != stack_buf)
        xr_free(buf);

    R(a) = xr_string_value(result);
    checkGC(base + a + 1);
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
