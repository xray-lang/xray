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
 *   - OP_ENUM_ACCESS  : enum_type[index]   -> enum aggregate or payload constructor metadata
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
    if (xr_enum_type_payload_count(enum_type, (uint32_t) member_index) == 0) {
        XrEnumAggregateValue *value =
            xr_enum_zero_payload_value(isolate, enum_type, (uint32_t) member_index);
        R(a) = value ? XR_FROM_PTR(value) : xr_null();
    } else {
        R(a) = XR_FROM_PTR(enum_type->members[member_index].ctor);
    }
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
    const char *member_name = NULL;
    if (XR_IS_ENUM_CTOR(enum_val)) {
        XrEnumCtor *ev = (XrEnumCtor *) XR_TO_PTR(enum_val);
        member_name = ev->member_name;
    } else if (xr_value_is_enum_aggregate(enum_val)) {
        XrEnumAggregateValue *ev = xr_value_to_enum_aggregate(enum_val);
        member_name = xr_enum_aggregate_member_name(ev);
    }
    if (!member_name) {
        R(a) = xr_null();
        vmbreak;
    }
    size_t len = strlen(member_name);
    uint32_t hash = xr_string_hash(member_name, len);
    XrString *name_str = xr_string_intern(isolate, member_name, len, hash);
    R(a) = xr_string_value(name_str);
    vmbreak;
}
