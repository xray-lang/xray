/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_map.inc.c - Recognize closed Map operations for fixed home consumers
 *
 * KEY CONCEPT:
 *   Existing logical role queries identify exact operations without
 *   constructing a retired runtime carrier or a second ownership directory.
 */
static uint32_t rbc_lir_map_role(const XrLirModule *module, const XrLirInstructionView *i) {
    XrLirIntrinsic entry = {0};
    return i->op == XR_LIR_CALL_INTRINSIC &&
        xr_lir_intrinsic_view(module, i->intrinsic, &entry) == XR_LIR_OK &&
        xr_lir_map_intrinsic_valid(module, &entry) ? entry.operation : 0;
}

static bool rbc_lir_map_point(RbcLirCompiler *c, const XrLirInstructionView *i) {
    if (rbc_lir_captured_cell_point(c->input, c->function, i)) return false;
    if (rbc_lir_map_role(c->input, i)) return true;
    if (i->op == XR_LIR_STATE_READ) return i->type == XR_LIR_MAP_STRING_I64;
    if (i->op == XR_LIR_STATE_WRITE) {
        XrLirType type = XR_LIR_TYPE_INVALID;
        return xr_lir_value_type(c->input, (XrLirValue){c->input, c->function.id, i->operands[1]}, &type) == XR_LIR_OK &&
            type == XR_LIR_MAP_STRING_I64;
    }
    if (i->op >= XR_LIR_MAP_RETAIN && i->op <= XR_LIR_MAP_REF_WRITE) return true;
    if (i->op == XR_LIR_CELL_NEW) return i->type == XR_LIR_MAP_STRING_I64_CELL;
    if (i->op == XR_LIR_CELL_READ) return i->type == XR_LIR_MAP_STRING_I64;
    XrLirType type = XR_LIR_TYPE_INVALID;
    return i->op == XR_LIR_CELL_WRITE && xr_lir_value_type(c->input,
        (XrLirValue){c->input, c->function.id, i->operands[0]}, &type) == XR_LIR_OK &&
        type == XR_LIR_MAP_STRING_I64_CELL;
}
