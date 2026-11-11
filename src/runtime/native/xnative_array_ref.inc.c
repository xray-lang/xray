/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_array_ref.inc.c - Synchronous access to the current Array binding
 */
static XrtArrayElement array_ref_element(XrtModuleTypeKind kind, uint32_t source_id) {
    switch (kind) {
    case XRT_MODULE_ARRAY_I64: return XRT_ARRAY_I64;
    case XRT_MODULE_ARRAY_STRING: return XRT_ARRAY_STRING;
    case XRT_MODULE_ARRAY_U8: return XRT_ARRAY_U8;
    case XRT_MODULE_ARRAY_U64: return XRT_ARRAY_U64;
    default: xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    }
}

XRAY_API XrtArrayRef xrt_array_ref_local(XrtArray **cell, XrtModuleTypeKind kind, uint32_t source_id) {
    (void)array_ref_element(kind, source_id);
    if (!cell) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    return (XrtArrayRef){.cell=cell, .state={.kind=kind}};
}

XRAY_API XrtArrayRef xrt_array_ref_state(XrtModuleRef *state, XrtModuleTypeKind kind, uint32_t source_id) {
    (void)array_ref_element(kind, source_id);
    XrtArrayRef reference = {0};
    xrt_module_require(xrt_module_reference_cache(state, &reference.state), source_id);
    if (reference.state.kind != kind || !reference.state.is_mutable)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    return reference;
}

static XrtArray *array_ref_current(XrtArrayRef reference, uint32_t source_id) {
    if (reference.cell) {
        if (reference.state.reference) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
        return *reference.cell;
    }
    if (!reference.state.reference || !reference.state.flags || !reference.state.payload ||
        !reference.state.is_mutable || reference.state.size != sizeof(XrtArray *))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
    if ((*reference.state.flags & reference.state.required_flags) != reference.state.required_flags)
        xrt_module_uninitialized(source_id);
    XrtArray *value = NULL;
    memcpy(&value, reference.state.payload, sizeof(value));
    return value;
}

static void array_ref_check(const XrtArray *value, XrtArrayElement element, uint32_t source_id) {
    XrtValueRoot *root = xrt_value_root_current(source_id);
    XrXirValueAdmission admission = array_admission(root, source_id);
    (void)array_argument(root, value, &admission, element, source_id);
}

XRAY_API XrtArray *xrt_array_ref_read(XrtArrayRef reference, uint32_t source_id) {
    XrtArrayElement element = array_ref_element(reference.state.kind, source_id);
    XrtArray *value = array_ref_current(reference, source_id);
    array_ref_check(value, element, source_id);
    return xrt_array_retain(value, source_id);
}

XRAY_API void xrt_array_ref_write(XrtArrayRef reference, const XrtArray *value, uint32_t source_id) {
    XrtArrayElement element = array_ref_element(reference.state.kind, source_id);
    XrtArray *previous = array_ref_current(reference, source_id);
    array_ref_check(previous, element, source_id);
    array_ref_check(value, element, source_id);
    if (reference.cell) xrt_array_assign(reference.cell, value, source_id);
    else xrt_module_require(xrt_module_replace(reference.state.reference, &value), source_id);
}
