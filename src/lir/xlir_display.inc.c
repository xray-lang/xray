/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_display.inc.c - Shared qualification and physical catalog projection
 */
XR_FUNC bool xr_lir_display_type(const XrLirModule *m, XrLirType type) {
    if (type == XR_LIR_I1) return true;
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
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    XrLirIntrinsic entries[XR_LIR_VIEW_OP_COUNT] = {{0}};
    uint32_t count = 0;
    for (uint32_t role = 1; views && role < XR_LIR_VIEW_OP_COUNT; ++role) {
        const LirViewSignature *shape = &lir_view_signatures[role];
        entries[count++] = (XrLirIntrinsic){XR_LIR_ARRAY_CATALOG_COUNT + role,
            shape->parameters, shape->count, shape->result, true, role};
    }
    entries[count++] = (XrLirIntrinsic){XR_LIR_ARRAY_CATALOG_COUNT + XR_LIR_VIEW_OP_COUNT,
        NULL, 0, XR_LIR_STRING, true, XR_LIR_INTRINSIC_DISPLAY};
    XrLirRegistry registry = {entries, count};
    status = arrays || views ? lir_array_catalog(m, &registry) : xr_lir_set_registry(m, &registry);
    return status == XR_LIR_OK ? status : lir_fail(m, status);
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
