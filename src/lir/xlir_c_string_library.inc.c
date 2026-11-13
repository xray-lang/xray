/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_string_library.inc.c - String operations on actual native bindings
 */
static void c_string_library(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    uint32_t role = xr_lir_string_role(c->request->module, v);
    c_text(c, "    ");
    if (v->result) c_format(c, "v%" PRIu32 " = ", v->result);
    c_format(c, "%s(", xr_lir_string_native_symbol((XrLirStringOperation)role));
    if (role == XR_LIR_STRING_OP_APPEND_STATE)
        c_format(c, "xrt_string_ref_state(xrl_access%" PRIu32 ", UINT32_C(%" PRIu32 "))",
            v->operands[0], source);
    else if (role == XR_LIR_STRING_OP_BORROW_STATE)
        c_format(c, "xrl_access%" PRIu32, v->operands[0]);
    else if (role == XR_LIR_STRING_OP_APPEND_OWNED) {
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(
            &c->request->module->functions[c->function.id - 1], v->operands[0]);
        bool literal = definition && definition->op == XR_LIR_STRING_LITERAL;
        if (literal) c_text(c, "xrt_string_retain(");
        c_value(c, v->operands[0]);
        if (literal) c_text(c, ", UINT32_C(0))");
    } else c_value(c, v->operands[0]);
    if (v->operand_count == 2) {
        c_text(c, ", ");
        c_value(c, v->operands[1]);
    }
    c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
}
