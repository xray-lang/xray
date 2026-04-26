/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_dispatch_enum.inc.c — enum opcode dispatch
 *
 * NOT a standalone translation unit. Included from inside the
 * dispatch switch in xvm.c; relies on locals (i, isolate, R, vmcase,
 * vmbreak, VM_RUNTIME_ERROR, ...) provided by the surrounding scope.
 * CMake excludes *.inc.c from the VM_SRC glob.
 *
 * Owns:
 *   - OP_ENUM_ACCESS  : enum_type[index]   -> XrEnumValue
 *   - OP_ENUM_CONVERT : XrEnumValue from underlying value
 *   - OP_ENUM_NAME    : member_name interned as string
 */

vmcase(OP_ENUM_ACCESS) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrEnumType *enum_type = (XrEnumType *) XR_TO_PTR(R(b));
    int member_index = (int) XR_TO_INT(R(c));
    if (member_index < 0 || (uint32_t) member_index >= enum_type->member_count) {
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "enum member index out of bounds");
    }
    R(a) = XR_FROM_PTR(enum_type->members[member_index].instance);
    vmbreak;
}

vmcase(OP_ENUM_CONVERT) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrEnumType *enum_type = (XrEnumType *) XR_TO_PTR(R(b));
    XrEnumValue *result = xr_enum_from_value(enum_type, R(c));
    R(a) = result ? XR_FROM_PTR(result) : xr_null();
    vmbreak;
}

vmcase(OP_ENUM_NAME) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    XrValue enum_val = R(b);
    if (!XR_IS_PTR(enum_val)) {
        R(a) = xr_null();
        vmbreak;
    }
    XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(enum_val);
    if (XR_GC_GET_TYPE(gc) != XR_TENUM_VALUE) {
        R(a) = xr_null();
        vmbreak;
    }
    XrEnumValue *ev = (XrEnumValue *) gc;
    size_t len = strlen(ev->member_name);
    uint32_t hash = xr_string_hash(ev->member_name, len);
    XrString *name_str = xr_string_intern(isolate, ev->member_name, len, hash);
    R(a) = xr_string_value(name_str);
    vmbreak;
}
