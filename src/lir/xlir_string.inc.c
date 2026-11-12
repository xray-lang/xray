/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_string.inc.c - Signatures shared by admission and both execution backends
 */
typedef struct LirStringSignature {
    const char *symbol;
    XrLirType result, parameters[2];
    uint32_t count;
} LirStringSignature;
static const LirStringSignature lir_string_signatures[] = {
    {"xrt_string_append_owned", XR_LIR_STRING, {XR_LIR_STRING, XR_LIR_STRING}, 2},
    {"xrt_string_cell_append", XR_LIR_UNIT, {XR_LIR_STRING_CELL, XR_LIR_STRING}, 2},
    {"xrt_string_ref_append", XR_LIR_UNIT, {XR_LIR_STATE_REF, XR_LIR_STRING}, 2},
    {"xrt_string_ref_local", XR_LIR_STRING_REF, {XR_LIR_STRING_CELL}, 1},
    {"xrt_string_ref_state", XR_LIR_STRING_REF, {XR_LIR_STATE_REF}, 1},
    {"xrt_string_ref_read", XR_LIR_STRING, {XR_LIR_STRING_REF}, 1},
    {"xrt_string_ref_write", XR_LIR_UNIT, {XR_LIR_STRING_REF, XR_LIR_STRING}, 2},
    {"xrt_string_ref_append", XR_LIR_UNIT, {XR_LIR_STRING_REF, XR_LIR_STRING}, 2}
};
XR_FUNC bool xr_lir_string_intrinsic_valid(const XrLirIntrinsic *entry) {
    if (!entry || entry->operation < XR_LIR_STRING_OP_APPEND_OWNED || entry->operation >= XR_LIR_STRING_OP_END ||
        !entry->scalar_sync || !entry->parameters) return false;
    const LirStringSignature *shape = &lir_string_signatures[entry->operation - XR_LIR_STRING_OP_APPEND_OWNED];
    return entry->result == shape->result && entry->parameter_count == shape->count &&
        entry->parameters[0] == shape->parameters[0] &&
        (shape->count == 1 || entry->parameters[1] == shape->parameters[1]);
}
XR_FUNC uint32_t xr_lir_string_role(const XrLirModule *m, const XrLirInstructionView *v) {
    if (!m || !v || v->op != XR_LIR_CALL_INTRINSIC) return 0;
    for (uint32_t n = 0; n < m->intrinsic_count; ++n)
        if (m->intrinsics[n].id == v->intrinsic)
            return xr_lir_string_intrinsic_valid(&m->intrinsics[n]) ? m->intrinsics[n].operation : 0;
    return 0;
}
XR_FUNC XrLirStatus xr_lir_string_operation(const XrLirModule *m, XrLirStringOperation role, uint32_t *output) {
    if (!m || !output || m->failure != XR_LIR_OK || role < XR_LIR_STRING_OP_APPEND_OWNED ||
        role >= XR_LIR_STRING_OP_END) return XR_LIR_INVALID_ARGUMENT;
    for (uint32_t n = 0; n < m->intrinsic_count; ++n)
        if (m->intrinsics[n].operation == (uint32_t)role) {
            *output = m->intrinsics[n].id;
            return XR_LIR_OK;
        }
    return XR_LIR_UNSUPPORTED;
}
XR_FUNC const char *xr_lir_string_native_symbol(XrLirStringOperation role) {
    return role >= XR_LIR_STRING_OP_APPEND_OWNED && role < XR_LIR_STRING_OP_END ?
        lir_string_signatures[role - XR_LIR_STRING_OP_APPEND_OWNED].symbol : NULL;
}
XR_FUNC XrLirStatus xr_lir_string_catalog_install(XrLirModule *m, bool views, bool arrays, bool display) {
    return lir_library_catalog(m, (views ? LIR_LIBRARY_VIEW : 0) | (arrays ? LIR_LIBRARY_ARRAY : 0) |
        (display ? LIR_LIBRARY_DISPLAY : 0) | LIR_LIBRARY_STRING);
}
