/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_array.inc.c - Query closed Array shapes and ordinary physical locations
 */
static uint32_t rbc_lir_array_family(XrLirType element) {
    switch (element) {
    case XR_LIR_I64: return 0;
    case XR_LIR_STRING: return 1;
    case XR_LIR_U8: return 2;
    case XR_LIR_U64: return 3;
    default: return XR_RBC_ARRAY_FAMILY_COUNT;
    }
}
static uint64_t rbc_lir_array_temporaries(const RbcLirCompiler *c, uint32_t families) {
    uint64_t count = 0;
    for (uint32_t i = 0; i < families; ++i) count += c->temporary_arrays[i];
    return count;
}
static bool rbc_lir_array_point(RbcLirCompiler *c, const XrLirInstructionView *i) {
    if (i->op == XR_LIR_CALL_INTRINSIC) {
        XrLirArrayIntrinsic shape = {0};
        return xr_lir_array_intrinsic_view(c->input, i->intrinsic, &shape) == XR_LIR_OK && shape.id;
    }
    if (i->op == XR_LIR_ARRAY_RETAIN || i->op == XR_LIR_ARRAY_DROP || i->op == XR_LIR_ARRAY_CELL_DROP ||
        i->op == XR_LIR_ARRAY_BORROW || i->op == XR_LIR_ARRAY_REF_READ || i->op == XR_LIR_ARRAY_REF_WRITE ||
        ((i->op == XR_LIR_CELL_NEW || i->op == XR_LIR_CELL_READ) &&
         (i->type == XR_LIR_ARRAY || i->type == XR_LIR_ARRAY_CELL))) return true;
    XrLirType type = XR_LIR_TYPE_INVALID;
    return i->op == XR_LIR_CELL_WRITE &&
        xr_lir_value_type(c->input, (XrLirValue){c->input,c->function.id,i->operands[0]}, &type) == XR_LIR_OK &&
        type == XR_LIR_ARRAY_CELL;
}
static XrRbcArrayLocation rbc_lir_array_location(RbcLirCompiler *c, uint32_t value) {
    bool spill = c->slots[value] == UINT32_MAX;
    return (XrRbcArrayLocation){spill ? c->spill_home[value] : c->slots[value], spill};
}
static bool rbc_lir_array_move(RbcLirCompiler *c, XrRbcRegisterKind kind, uint32_t destination, uint32_t source) {
    if (kind == XR_RBC_STRING || kind == XR_RBC_MAP_STRING_I64 ||
        xr_rbc_array_family(kind) < XR_RBC_ARRAY_FAMILY_COUNT)
        return rbc_lir_fail(c,XR_RBC_COMPILE_UNSUPPORTED,"managed transfer requires common stable homes");
    if (destination == source) return true;
    return rbc_lir_emit(c, XR_RBC_MOV, destination, source, 0, 0);
}
