/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_slice.inc.c - One typed catalog for natural readonly Array windows
 */
enum { LIR_SLICE_OPERATION_BASE = 65536, LIR_SLICE_ID_BASE = 1024, LIR_SLICE_CATALOG_COUNT = 16 };
XR_FUNC bool xr_lir_internal_slice_type(const XrLirModule *m, XrLirType type) {
    return m && type >= XR_LIR_TYPE_COUNT && type - XR_LIR_TYPE_COUNT < m->type_count &&
        m->types[type - XR_LIR_TYPE_COUNT].shape.kind == XR_LIR_TYPE_SLICE;
}
static bool lir_slice_decode(uint32_t operation, XrLirSliceIntrinsic *output) {
    if (operation < LIR_SLICE_OPERATION_BASE) return false;
    uint32_t encoded = operation - LIR_SLICE_OPERATION_BASE;
    uint32_t role = encoded % XR_LIR_SLICE_OP_COUNT, family = encoded / XR_LIR_SLICE_OP_COUNT;
    if (!role || family >= 4 || ((family & 1u) && role != XR_LIR_SLICE_OP_NEW)) return false;
    *output = (XrLirSliceIntrinsic){.operation=(XrLirSliceOperation)role,
        .element=family < 2 ? XR_LIR_I64 : XR_LIR_U8,
        .binding_type=role == XR_LIR_SLICE_OP_NEW ? (family & 1u) ? XR_LIR_ARRAY_REF : XR_LIR_ARRAY_CELL : 0};
    return true;
}
static XrLirType lir_slice_find_type(const XrLirModule *m, XrLirType element) {
    for (uint32_t n = 0; m && n < m->type_count; ++n)
        if (m->types[n].shape.kind == XR_LIR_TYPE_SLICE && m->types[n].shape.element == element)
            return XR_LIR_TYPE_COUNT + n;
    return XR_LIR_TYPE_INVALID;
}
static uint32_t lir_slice_signature(XrLirSliceIntrinsic shape, XrLirType slice,
    XrLirType parameters[4], XrLirType *result) {
    parameters[0] = slice; *result = slice;
    switch (shape.operation) {
    case XR_LIR_SLICE_OP_NEW:
        parameters[0] = shape.binding_type; parameters[1] = parameters[2] = XR_LIR_I64; return 3;
    case XR_LIR_SLICE_OP_REBORROW:
        parameters[1] = parameters[2] = XR_LIR_I64; parameters[3] = XR_LIR_I1; return 4;
    case XR_LIR_SLICE_OP_BIND: return 1;
    case XR_LIR_SLICE_OP_LEN: *result = XR_LIR_I64; return 1;
    case XR_LIR_SLICE_OP_GET: *result = shape.element; parameters[1] = XR_LIR_I64; return 2;
    case XR_LIR_SLICE_OP_COPY: *result = XR_LIR_ARRAY; return 1;
    case XR_LIR_SLICE_OP_END: *result = XR_LIR_UNIT; return 1;
    default: return 0;
    }
}
XR_FUNC bool xr_lir_internal_slice_intrinsic_valid(const XrLirModule *m, const XrLirIntrinsic *entry) {
    XrLirSliceIntrinsic shape; XrLirType parameters[4] = {0}, result;
    if (!entry || !entry->scalar_sync || !lir_slice_decode(entry->operation, &shape)) return false;
    XrLirType type = lir_slice_find_type(m, shape.element);
    uint32_t count = lir_slice_signature(shape, type, parameters, &result);
    if (!type || !count || !entry->parameters || entry->parameter_count != count || entry->result != result) return false;
    for (uint32_t p = 0; p < count; ++p) if (entry->parameters[p] != parameters[p]) return false;
    return true;
}
XR_FUNC const XrLirIntrinsic *xr_lir_internal_slice_intrinsic(const XrLirModule *m, uint32_t id) {
    for (uint32_t n = 0; m && m->failure == XR_LIR_OK && n < m->intrinsic_count; ++n)
        if (m->intrinsics[n].id == id && xr_lir_internal_slice_intrinsic_valid(m, &m->intrinsics[n]))
            return &m->intrinsics[n];
    return NULL;
}
XR_FUNC XrLirStatus xr_lir_slice_intrinsic_view(const XrLirModule *m, uint32_t id, XrLirSliceIntrinsic *output) {
    if (!m || !output || m->failure != XR_LIR_OK) return XR_LIR_INVALID_ARGUMENT;
    const XrLirIntrinsic *entry = xr_lir_internal_slice_intrinsic(m, id);
    XrLirSliceIntrinsic shape;
    if (!entry || !lir_slice_decode(entry->operation, &shape)) return XR_LIR_UNSUPPORTED;
    shape.id = id; *output = shape; return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_slice_operation(const XrLirModule *m, XrLirSliceOperation operation,
    XrLirType element, XrLirType binding, uint32_t *output) {
    if (!m || !output || m->failure != XR_LIR_OK) return XR_LIR_INVALID_ARGUMENT;
    for (uint32_t n = 0; n < m->intrinsic_count; ++n) {
        XrLirSliceIntrinsic shape;
        if (xr_lir_slice_intrinsic_view(m, m->intrinsics[n].id, &shape) == XR_LIR_OK &&
            shape.operation == operation && shape.element == element && shape.binding_type == binding) {
            *output = shape.id; return XR_LIR_OK;
        }
    }
    return XR_LIR_UNSUPPORTED;
}
XR_FUNC XrLirStatus xr_lir_slice_library_catalog_install(XrLirModule *m, uint32_t flags) {
    if (flags & ~(XR_LIR_SLICE_LIBRARY_DISPLAY | XR_LIR_SLICE_LIBRARY_STRING |
            XR_LIR_SLICE_LIBRARY_RANGE | XR_LIR_SLICE_LIBRARY_MAP)) return XR_LIR_INVALID_ARGUMENT;
    uint32_t catalog = LIR_LIBRARY_SLICE | LIR_LIBRARY_ARRAY;
    if (flags & XR_LIR_SLICE_LIBRARY_DISPLAY) catalog |= LIR_LIBRARY_DISPLAY;
    if (flags & XR_LIR_SLICE_LIBRARY_STRING) catalog |= LIR_LIBRARY_STRING;
    if (flags & XR_LIR_SLICE_LIBRARY_RANGE) catalog |= LIR_LIBRARY_RANGE;
    if (flags & XR_LIR_SLICE_LIBRARY_MAP) catalog |= LIR_LIBRARY_MAP;
    return lir_library_catalog(m, catalog);
}
XR_FUNC XrLirStatus xr_lir_slice_catalog_install(XrLirModule *m) {
    return xr_lir_slice_library_catalog_install(m, 0);
}
