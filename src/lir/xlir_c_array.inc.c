/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_array.inc.c - Consume owned Array roles through the common runtime
 */
static bool c_array_intrinsic(LirC *c, const XrLirInstructionView *v, XrLirArrayIntrinsic *shape) {
    return v->op == XR_LIR_CALL_INTRINSIC &&
        xr_lir_array_intrinsic_view(c->request->module, v->intrinsic, shape) == XR_LIR_OK && shape->id;
}
static bool c_array_point(LirC *c, const XrLirInstructionView *v) {
    XrLirArrayIntrinsic shape = {0};
    return c_array_intrinsic(c, v, &shape) || v->op == XR_LIR_ARRAY_RETAIN ||
        (v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_ARRAY_CELL) ||
        (v->op == XR_LIR_CELL_READ && v->type == XR_LIR_ARRAY) ||
        (v->op == XR_LIR_CELL_WRITE && c_value_type(c, v->operands[0]) == XR_LIR_ARRAY_CELL);
}
static void c_array_call(LirC *c, const XrLirInstructionView *v,
    const XrLirArrayIntrinsic *shape, uint32_t source) {
    XrLirType element = shape->operation == XR_LIR_ARRAY_OP_NEW ? shape->result_element : shape->parameter_elements[0];
    const char *family = element == XR_LIR_I64 ? "i64" : "string";
    c_text(c, "    ");
    if (v->result) c_format(c, "v%" PRIu32 " = ", v->result);
    if (shape->operation == XR_LIR_ARRAY_OP_NEW) {
        c_format(c, "xrt_array_%s_new(", family);
        if (v->operand_count) {
            c_text(c, element == XR_LIR_I64 ? "(const int64_t[]){" : "(XrtString *const[]){");
            for (uint32_t p = 0; p < v->operand_count; ++p) {
                if (p) c_text(c, ", ");
                c_value(c, v->operands[p]);
            }
            c_text(c, "}");
        } else c_text(c, "NULL");
        c_format(c, ", UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 "));\n", v->operand_count, source);
        return;
    }
    if (shape->operation == XR_LIR_ARRAY_OP_LEN) c_text(c, "xrt_array_length(");
    else c_format(c, "xrt_array_%s_%s(", family, shape->operation == XR_LIR_ARRAY_OP_GET ? "get" :
        shape->operation == XR_LIR_ARRAY_OP_SET ? "set" : "push");
    for (uint32_t p = 0; p < v->operand_count; ++p) {
        if (p) c_text(c, ", ");
        c_value(c, v->operands[p]);
    }
    c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
}
static void c_array_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    XrLirArrayIntrinsic shape = {0};
    if (c_array_intrinsic(c, v, &shape)) { c_array_call(c, v, &shape, source); return; }
    if (v->op == XR_LIR_CELL_NEW) {
        c_format(c, "    XrtArray *xrl_cell%" PRIu32 " = xrt_array_retain(", v->result);
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n    v%" PRIu32 " = &xrl_cell%" PRIu32 ";\n",
            source, v->result, v->result);
    } else if (v->op == XR_LIR_CELL_WRITE) {
        c_text(c, "    xrt_array_assign("); c_value(c, v->operands[0]);
        c_text(c, ", "); c_value(c, v->operands[1]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
    } else if (v->op == XR_LIR_ARRAY_DROP || v->op == XR_LIR_ARRAY_CELL_DROP) {
        c_text(c, "    xrt_array_drop(");
        if (v->op == XR_LIR_ARRAY_DROP) c_text(c, "&");
        c_value(c, v->operands[0]); c_text(c, ");\n");
    } else {
        c_format(c, "    v%" PRIu32 " = xrt_array_retain(", v->result);
        if (v->op == XR_LIR_CELL_READ) c_text(c, "*");
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
    }
}
