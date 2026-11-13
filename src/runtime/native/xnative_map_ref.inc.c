/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_map_ref.inc.c - Synchronous access to current Map bindings
 *
 * KEY CONCEPT:
 *   Every operation resolves the actual local or module payload. State
 *   readiness and mutability are checked at the operation boundary.
 */
XRAY_API XrtMapRef xrt_map_ref_local(XrtMap **cell, uint32_t source) {
    if (!cell) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return (XrtMapRef){.cell=cell, .state={.kind=XRT_MODULE_MAP_STRING_I64}};
}
XRAY_API XrtMapRef xrt_map_ref_state(XrtModuleRef *state, uint32_t source) {
    XrtMapRef reference = {0};
    xrt_module_require(xrt_module_reference_cache(state, &reference.state), source);
    if (reference.state.kind != XRT_MODULE_MAP_STRING_I64 || !reference.state.is_mutable)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    return reference;
}
static XrtMap **map_ref_binding(XrtMapRef reference, uint32_t source) {
    if (reference.state.kind != XRT_MODULE_MAP_STRING_I64)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if (reference.cell) {
        if (reference.state.reference) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
        return reference.cell;
    }
    if (!reference.state.reference || !reference.state.flags || !reference.state.payload ||
        !reference.state.is_mutable || reference.state.size != sizeof(XrtMap *))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if ((*reference.state.flags & reference.state.required_flags) != reference.state.required_flags)
        xrt_module_uninitialized(source);
    return reference.state.payload;
}
XRAY_API XrtMap *xrt_map_ref_read(XrtMapRef reference, uint32_t source) {
    return xrt_map_retain(*map_ref_binding(reference, source), source);
}
XRAY_API void xrt_map_ref_write(XrtMapRef reference, const XrtMap *value, uint32_t source) {
    XrtMap **binding = map_ref_binding(reference, source);
    if (reference.cell) xrt_map_assign(binding, value, source);
    else xrt_module_require(xrt_module_replace(reference.state.reference, &value), source);
}
XRAY_API void xrt_map_ref_string_i64_set(XrtMapRef reference, const XrtString *key,
    int64_t value, uint32_t source) {
    (void)map_mutate(map_ref_binding(reference, source), reference.cell != NULL,
        key, value, MAP_SET, source);
}
XRAY_API bool xrt_map_ref_string_i64_remove(XrtMapRef reference, const XrtString *key, uint32_t source) {
    return map_mutate(map_ref_binding(reference, source), reference.cell != NULL, key, 0, MAP_REMOVE, source);
}
XRAY_API void xrt_map_ref_clear(XrtMapRef reference, uint32_t source) {
    (void)map_mutate(map_ref_binding(reference, source), reference.cell != NULL, NULL, 0, MAP_CLEAR, source);
}
