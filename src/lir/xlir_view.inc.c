/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_view.inc.c - One signature catalog for integer view library calls
 */
typedef struct LirViewSignature {
    const char *symbol;
    XrLirType result, parameters[6];
    uint32_t count;
} LirViewSignature;
static const LirViewSignature lir_view_signatures[] = {
    {0},
#define XR_LIR_VIEW_OP(name, symbol, result, count, a, b, c, d, e, f) {symbol, result, {a, b, c, d, e, f}, count},
#include "xlir_view_ops.def"
#undef XR_LIR_VIEW_OP
};
_Static_assert(sizeof(lir_view_signatures) / sizeof(*lir_view_signatures) == XR_LIR_VIEW_OP_COUNT,
    "Every view role has one complete closed signature");
XR_FUNC bool xr_lir_internal_view_intrinsic_valid(const XrLirIntrinsic *entry) {
    if (!entry || !entry->view_operation || entry->view_operation >= XR_LIR_VIEW_OP_COUNT ||
        !entry->scalar_sync) return false;
    const LirViewSignature *shape = &lir_view_signatures[entry->view_operation];
    if (entry->result != shape->result || entry->parameter_count != shape->count || !entry->parameters) return false;
    for (uint32_t p = 0; p < shape->count; ++p)
        if (entry->parameters[p] != shape->parameters[p]) return false;
    return true;
}
XR_FUNC const XrLirIntrinsic *xr_lir_internal_view_intrinsic(const XrLirModule *m, uint32_t id) {
    for (uint32_t i = 0; m && i < m->intrinsic_count; ++i)
        if (m->intrinsics[i].id == id && m->intrinsics[i].view_operation) return &m->intrinsics[i];
    return NULL;
}
XR_FUNC const char *xr_lir_view_native_symbol(XrLirViewOperation operation) {
    return operation > XR_LIR_VIEW_OP_NONE && operation < XR_LIR_VIEW_OP_COUNT ?
        lir_view_signatures[operation].symbol : NULL;
}
XR_FUNC bool xr_lir_internal_view_construct_slot(const XrLirModule *m,
    const XrLirFunctionData *f, const XrLirInstructionView *v, uint32_t *slot) {
    const XrLirIntrinsic *entry = v && v->op == XR_LIR_CALL_INTRINSIC ?
        xr_lir_internal_view_intrinsic(m, v->intrinsic) : NULL;
    if (!entry || entry->view_operation != XR_LIR_VIEW_OP_CELL_CONSTRUCT ||
        v->operand_count != 4 || !slot) return false;
    const XrLirInstructionView *constant = xr_lir_internal_value_definition(f, v->operands[3]);
    if (!constant || constant->op != XR_LIR_CONST || constant->type != XR_LIR_U32 ||
        constant->bits >= m->state_layout.slot_count) return false;
    *slot = (uint32_t)constant->bits;
    return true;
}
XR_FUNC XrLirStatus xr_lir_view_catalog_install(XrLirModule *m) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    XrLirIntrinsic entries[XR_LIR_VIEW_OP_COUNT - 1];
    for (uint32_t role = 1; role < XR_LIR_VIEW_OP_COUNT; ++role) {
        const LirViewSignature *shape = &lir_view_signatures[role];
        entries[role - 1] = (XrLirIntrinsic){.id=XR_LIR_ARRAY_CATALOG_COUNT + role,
            .parameters=shape->parameters, .parameter_count=shape->count,
            .result=shape->result, .scalar_sync=true, .view_operation=role};
    }
    XrLirRegistry registry = {entries, XR_LIR_VIEW_OP_COUNT - 1};
    status = lir_array_catalog(m, &registry);
    return status == XR_LIR_OK ? status : lir_fail(m, status);
}
XR_FUNC XrLirStatus xr_lir_view_operation(const XrLirModule *m,
    XrLirViewOperation operation, uint32_t *output) {
    if (!m || !output || m->failure != XR_LIR_OK ||
        operation <= XR_LIR_VIEW_OP_NONE || operation >= XR_LIR_VIEW_OP_COUNT) return XR_LIR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < m->intrinsic_count; ++i) {
        if (m->intrinsics[i].view_operation == (uint32_t)operation) {
            *output = m->intrinsics[i].id;
            return XR_LIR_OK;
        }
    }
    return XR_LIR_UNSUPPORTED;
}
