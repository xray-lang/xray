/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_view.inc.c - Receive typed library roles without changing the hot loop
 */
static uint32_t rbc_lir_view_role(const XrLirModule *m, const XrLirInstructionView *i) {
    if (i->op == XR_LIR_VIEW_FRAME_ENTER) return XR_RBC_VIEW_FRAME_ENTER;
    if (i->op == XR_LIR_VIEW_FRAME_LEAVE) return XR_RBC_VIEW_FRAME_LEAVE;
    if (i->op != XR_LIR_CALL_INTRINSIC) return 0;
    XrLirIntrinsic entry = {0};
    return xr_lir_intrinsic_view(m, i->intrinsic, &entry) == XR_LIR_OK &&
        entry.operation < XR_LIR_VIEW_OP_COUNT ? entry.operation : 0;
}
static bool rbc_lir_view_emit(RbcLirCompiler *c, const XrLirInstructionView *i) {
    if (c->view_access_count > UINT16_MAX)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "view library table exceeds bytecode indices");
    void *storage = c->view_accesses;
    if (!rbc_lir_grow(c, &storage, &c->view_access_capacity,
        c->view_access_count + 1, sizeof(*c->view_accesses))) return false;
    c->view_accesses = storage;
    XrRbcViewAccess row = {.operation=(XrRbcViewOperation)rbc_lir_view_role(c->input, i),
        .count=i->operand_count, .bits=i->bits};
    if (row.operation == XR_RBC_VIEW_CELL_CONSTRUCT) {
        if (!c->view_constants || c->view_constants[i->operands[3]] == UINT32_MAX)
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "view module slot has no exact constant producer");
        row.bits = c->view_constants[i->operands[3]];
    }
    if (i->result) row.result = rbc_lir_array_location(c, i->result);
    for (uint32_t p = 0; p < row.count; ++p) row.operands[p] = rbc_lir_array_location(c, i->operands[p]);
    c->view_accesses[c->view_access_count] = row;
    return rbc_lir_emit(c, XR_RBC_VIEW_ACCESS, 0, 0, 0, (int64_t)c->view_access_count++);
}
