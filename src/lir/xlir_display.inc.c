/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_display.inc.c - Shared qualification and physical catalog projection
 */
XR_FUNC bool xr_lir_display_type(const XrLirModule *m, XrLirType type) {
    if (type == XR_LIR_I1 || type == XR_LIR_U8 || type == XR_LIR_U64) return true;
    bool nullable = false;
    while (type >= XR_LIR_TYPE_COUNT) {
        XrLirTypeView shape = {0};
        if (xr_lir_type_view(m, type, &shape) != XR_LIR_OK ||
            shape.kind != XR_LIR_TYPE_NULLABLE || shape.element >= type) return false;
        nullable = true;
        type = shape.element;
    }
    return nullable && lir_pod_scalar(type) < XR_POD_SCALAR_COUNT;
}
XR_FUNC bool xr_lir_display_intrinsic_valid(const XrLirIntrinsic *entry) {
    return entry && entry->operation == XR_LIR_INTRINSIC_DISPLAY && entry->scalar_sync &&
        entry->result == XR_LIR_STRING && !entry->parameter_count && !entry->parameters;
}
XR_FUNC bool xr_lir_display_point(const XrLirModule *m, const XrLirInstructionView *v) {
    if (!m || !v || v->op != XR_LIR_CALL_INTRINSIC) return false;
    for (uint32_t n = 0; n < m->intrinsic_count; ++n)
        if (m->intrinsics[n].id == v->intrinsic)
            return xr_lir_display_intrinsic_valid(&m->intrinsics[n]);
    return false;
}
XR_FUNC XrLirStatus xr_lir_display_operation(const XrLirModule *m, uint32_t *output) {
    if (!m || !output || m->failure != XR_LIR_OK) return XR_LIR_INVALID_ARGUMENT;
    for (uint32_t n = 0; n < m->intrinsic_count; ++n)
        if (xr_lir_display_intrinsic_valid(&m->intrinsics[n])) {
            *output = m->intrinsics[n].id;
            return XR_LIR_OK;
        }
    return XR_LIR_UNSUPPORTED;
}
XR_FUNC XrLirStatus xr_lir_display_catalog_install(XrLirModule *m, bool views, bool arrays) {
    return lir_library_catalog(m, (views ? LIR_LIBRARY_VIEW : 0) | (arrays ? LIR_LIBRARY_ARRAY : 0) |
        LIR_LIBRARY_DISPLAY);
}
XR_FUNC XrLirStatus xr_lir_pod_type(const XrLirModule *m, XrLirType type, XrPodType *output) {
    XrLirTypeView shape = {0};
    XrLirTypeLayout layout = {0};
    if (!output || type < XR_LIR_TYPE_COUNT || xr_lir_type_view(m, type, &shape) != XR_LIR_OK ||
        xr_lir_type_layout(m, type, &layout) != XR_LIR_OK || layout.managed || layout.size > UINT32_MAX ||
        (shape.kind != XR_LIR_TYPE_FIXED_ARRAY && shape.kind != XR_LIR_TYPE_NULLABLE))
        return XR_LIR_UNSUPPORTED;
    uint32_t element = shape.kind == XR_LIR_TYPE_FIXED_ARRAY ? 0 : shape.element >= XR_LIR_TYPE_COUNT ?
        XR_POD_SCALAR_COUNT + shape.element - XR_LIR_TYPE_COUNT : (uint32_t)lir_pod_scalar(shape.element);
    *output = (XrPodType){shape.kind == XR_LIR_TYPE_FIXED_ARRAY ? XR_POD_FIXED_U8 : XR_POD_NULLABLE,
        shape.count, element, {(uint32_t)layout.size, layout.alignment, layout.scalar_count}};
    return XR_LIR_OK;
}
