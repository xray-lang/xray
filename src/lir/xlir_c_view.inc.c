/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_view.inc.c - Native calls from owned integer view intrinsic roles
 */
static uint32_t c_view_operation(LirC *c, const XrLirInstructionView *v) {
    XrLirIntrinsic entry = {0};
    if (v->op != XR_LIR_CALL_INTRINSIC ||
        xr_lir_intrinsic_view(c->request->module, v->intrinsic, &entry) != XR_LIR_OK) return 0;
    return entry.operation < XR_LIR_VIEW_OP_COUNT ? entry.operation : 0;
}
static bool c_view_point(LirC *c, const XrLirInstructionView *v) {
    return v->op == XR_LIR_VIEW_FRAME_ENTER || v->op == XR_LIR_VIEW_FRAME_LEAVE || c_view_operation(c, v);
}
static void c_view_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    uint32_t role = c_view_operation(c, v);
    const char *name = v->op == XR_LIR_VIEW_FRAME_ENTER ? "xrt_view_frame_enter" :
        v->op == XR_LIR_VIEW_FRAME_LEAVE ? "xrt_view_frame_leave" :
        xr_lir_view_native_symbol((XrLirViewOperation)role);
    XR_CHECK(name, "native view calls consume the validated closed role catalog");
    c_text(c, "    ");
    if (v->result) c_format(c, "v%" PRIu32 " = ", v->result);
    c_format(c, "%s(", name);
    if (v->op == XR_LIR_VIEW_FRAME_ENTER) {
        c_format(c, "UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 "), ",
            (uint32_t)v->bits, (uint32_t)(v->bits >> 32));
    } else if (role == XR_LIR_VIEW_OP_NEW || role == XR_LIR_VIEW_OP_REBORROW) {
        for (uint32_t n = 0; n < 3; ++n) { c_value(c, v->operands[n]); c_text(c, ", "); }
        c_text(c, "(XrtViewRange){");
        for (uint32_t n = 3; n < 6; ++n) {
            if (n != 3) c_text(c, ", ");
            c_value(c, v->operands[n]);
        }
        c_text(c, "}, ");
    } else {
        for (uint32_t n = 0; n < v->operand_count; ++n) {
            c_value(c, v->operands[n]);
            c_text(c, ", ");
        }
    }
    c_format(c, "UINT32_C(%" PRIu32 "));\n", source);
    if (v->result) c_format(c, "    (void)v%" PRIu32 ";\n", v->result);
    if (role == XR_LIR_VIEW_OP_CELL_CONSTRUCT) c_state_reference_cache(c, v->result, source);
}
