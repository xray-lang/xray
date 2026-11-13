/* Copyright (c) 2026 Xinglei Xu. MIT License. */
XR_FUNC bool xr_lir_range_intrinsic_valid(const XrLirIntrinsic *entry) {
    return entry && entry->operation == XR_LIR_RANGE_OP_CHECK && entry->scalar_sync &&
        entry->result == XR_LIR_UNIT && entry->parameter_count == 3 && entry->parameters &&
        entry->parameters[0] == XR_LIR_I64 && entry->parameters[1] == XR_LIR_I64 &&
        entry->parameters[2] == XR_LIR_I64;
}
XR_FUNC bool xr_lir_range_point(const XrLirModule *m, const XrLirInstructionView *v) {
    if (!m || !v || v->op != XR_LIR_CALL_INTRINSIC) return false;
    for (uint32_t n = 0; n < m->intrinsic_count; ++n)
        if (m->intrinsics[n].id == v->intrinsic)
            return xr_lir_range_intrinsic_valid(&m->intrinsics[n]);
    return false;
}
XR_FUNC XrLirStatus xr_lir_range_operation(const XrLirModule *m, uint32_t *output) {
    if (!m || !output || m->failure != XR_LIR_OK) return XR_LIR_INVALID_ARGUMENT;
    for (uint32_t n = 0; n < m->intrinsic_count; ++n)
        if (xr_lir_range_intrinsic_valid(&m->intrinsics[n])) {
            *output = m->intrinsics[n].id;
            return XR_LIR_OK;
        }
    return XR_LIR_UNSUPPORTED;
}
XR_FUNC XrLirStatus xr_lir_range_catalog_install(XrLirModule *m, bool views,
    bool arrays, bool display, bool strings) {
    return lir_library_catalog(m, (views ? LIR_LIBRARY_VIEW : 0) | (arrays ? LIR_LIBRARY_ARRAY : 0) |
        (display ? LIR_LIBRARY_DISPLAY : 0) | (strings ? LIR_LIBRARY_STRING : 0) | LIR_LIBRARY_RANGE);
}
