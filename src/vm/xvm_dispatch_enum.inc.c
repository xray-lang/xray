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
        XrEnumAggregateValue *value = xr_enum_zero_payload_value(
            xr_isolate_get_runtime_core(isolate), enum_type, (uint32_t) member_index);
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

vmcase(OP_ENUM_VARIANT_AT) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrEnumMetadataResult result = XR_ENUM_METADATA_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_HI,
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_LO, XR_SEM_CONSUMER_VM,
        xr_enum_metadata_variant_at_core(XR_TO_INT(R(b)), XR_TO_INT(R(c))));
    if (result.status != XR_ENUM_METADATA_OK)
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "%s",
                         XR_ERROR_CORE_ENUM_VARIANT_INDEX_OOB_MSG);
    R(a) = XR_FROM_INT(result.value);
    vmbreak;
}

vmcase(OP_ENUM_PAYLOAD_AT) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrEnumMetadataResult result = XR_ENUM_METADATA_ACCESS_OWNER_APPLY(
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_HI,
        XR_SEM_OWNER_ID_SHARED_ENUM_METADATA_ACCESS_LO, XR_SEM_CONSUMER_VM,
        xr_enum_metadata_payload_at_core((uint64_t) XR_TO_INT(R(b)), XR_TO_INT(R(c))));
    if (result.status != XR_ENUM_METADATA_OK)
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "%s",
                         XR_ERROR_CORE_ENUM_PAYLOAD_INDEX_OOB_MSG);
    R(a) = XR_FROM_INT(result.value);
    vmbreak;
}

vmcase(OP_ENUM_VARIANT_NAME) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrEnumType *enum_type = (XrEnumType *) XR_TO_PTR(R(b));
    int64_t ordinal = XR_TO_INT(R(c));
    const char *name = ordinal >= 0 && (uint64_t) ordinal < enum_type->member_count
                           ? xr_enum_type_member_name(enum_type, (uint32_t) ordinal)
                           : NULL;
    if (!name)
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "enum variant ordinal out of bounds");
    size_t len = strlen(name);
    R(a) = xr_string_value(xr_string_intern(isolate, name, len, xr_string_hash(name, len)));
    vmbreak;
}

vmcase(OP_ENUM_VARIANT_PAYLOAD_COUNT) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrEnumType *enum_type = (XrEnumType *) XR_TO_PTR(R(b));
    int64_t ordinal = XR_TO_INT(R(c));
    if (ordinal < 0 || (uint64_t) ordinal >= enum_type->member_count)
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "enum variant ordinal out of bounds");
    R(a) = XR_FROM_INT(xr_enum_type_payload_count(enum_type, (uint32_t) ordinal));
    vmbreak;
}

vmcase(OP_ENUM_PAYLOAD_FIELD_NAME) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrEnumType *enum_type = (XrEnumType *) XR_TO_PTR(R(b));
    uint64_t packed = (uint64_t) XR_TO_INT(R(c));
    uint32_t ordinal = (uint32_t) (packed >> 32);
    uint32_t field = (uint32_t) packed;
    const XrEnumVariantLayout *variant = xr_enum_layout_variant(enum_type->layout, ordinal);
    if (!variant || field >= variant->payload_count)
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "enum payload field index out of bounds");
    const char *name = variant->payload_names && variant->payload_names[field]
                           ? variant->payload_names[field]
                           : "";
    size_t len = strlen(name);
    R(a) = xr_string_value(xr_string_intern(isolate, name, len, xr_string_hash(name, len)));
    vmbreak;
}

vmcase(OP_ENUM_PAYLOAD_FIELD_TYPE) {
    int a = GETARG_A(i);
    int b = GETARG_B(i);
    int c = GETARG_C(i);
    XrEnumType *enum_type = (XrEnumType *) XR_TO_PTR(R(b));
    uint64_t packed = (uint64_t) XR_TO_INT(R(c));
    uint32_t ordinal = (uint32_t) (packed >> 32);
    uint32_t field = (uint32_t) packed;
    const XrEnumVariantLayout *variant = xr_enum_layout_variant(enum_type->layout, ordinal);
    if (!variant || field >= variant->payload_count)
        VM_RUNTIME_ERROR(XR_ERR_INDEX_OUT_OF_BOUNDS, "enum payload field index out of bounds");
    R(a) = XR_FROM_INT(variant->payload_type_ids ? variant->payload_type_ids[field] : 0);
    vmbreak;
}
