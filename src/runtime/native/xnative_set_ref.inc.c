/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_set_ref.inc.c - Synchronous access to current Set bindings
 *
 * KEY CONCEPT:
 *   Every operation resolves the actual local or module payload. State
 *   readiness and mutability are checked at the operation boundary.
 */
XRAY_API XrtSetRef xrt_set_ref_local(XrtSet **cell, uint32_t source) {
    if (!cell) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return (XrtSetRef){.cell=cell, .state={.kind=XRT_MODULE_SET_I64}};
}
XRAY_API XrtSetRef xrt_set_ref_state(XrtModuleRef *state, uint32_t source) {
    XrtSetRef reference = {0};
    xrt_module_require(xrt_module_reference_cache(state, &reference.state), source);
    if (reference.state.kind != XRT_MODULE_SET_I64 || !reference.state.is_mutable)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return reference;
}
static XrtSet **set_ref_binding(XrtSetRef reference, uint32_t source) {
    if (reference.state.kind != XRT_MODULE_SET_I64)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if (reference.cell) {
        if (reference.state.reference) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
        return reference.cell;
    }
    if (!reference.state.reference || !reference.state.flags || !reference.state.payload ||
        !reference.state.is_mutable || reference.state.size != sizeof(XrtSet *))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if ((*reference.state.flags & reference.state.required_flags) != reference.state.required_flags)
        xrt_module_uninitialized(source);
    return reference.state.payload;
}
XRAY_API XrtSet *xrt_set_ref_read(XrtSetRef reference, uint32_t source) {
    return xrt_set_retain(*set_ref_binding(reference, source), source);
}
XRAY_API void xrt_set_ref_write(XrtSetRef reference, const XrtSet *value, uint32_t source) {
    XrtSet **binding = set_ref_binding(reference, source);
    if (reference.cell) xrt_set_assign(binding, value, source);
    else xrt_module_require(xrt_module_replace(reference.state.reference, &value), source);
}
XRAY_API void xrt_set_ref_i64_add(XrtSetRef reference, int64_t value, uint32_t source) {
    (void)set_mutate(set_ref_binding(reference, source), reference.cell != NULL, value, SET_ADD, source);
}
XRAY_API bool xrt_set_ref_i64_remove(XrtSetRef reference, int64_t value, uint32_t source) {
    return set_mutate(set_ref_binding(reference, source), reference.cell != NULL, value, SET_REMOVE, source);
}
XRAY_API void xrt_set_ref_clear(XrtSetRef reference, uint32_t source) {
    (void)set_mutate(set_ref_binding(reference, source), reference.cell != NULL, 0, SET_CLEAR, source);
}
