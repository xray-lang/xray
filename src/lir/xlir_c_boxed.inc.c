/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_boxed.inc.c - Ordinary C calls into the canonical value kernel
 */
static bool c_boxed_point(LirC *c, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_VALUE_RETAIN || v->op == XR_LIR_VALUE_DROP ||
        v->op == XR_LIR_RECORD_NEW || v->op == XR_LIR_RECORD_FIELD) return true;
    if (v->op < XR_LIR_NULLABLE_NONE || v->op > XR_LIR_NULLABLE_UNWRAP) return false;
    XrLirType type = v->op == XR_LIR_NULLABLE_NONE || v->op == XR_LIR_NULLABLE_SOME ?
        v->type : c_value_type(c, v->operands[0]);
    return xr_lir_boxed_type(c->request->module, type);
}
static void c_boxed_instruction(LirC *c, const XrLirInstructionView *v) {
    uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
    if (v->op == XR_LIR_VALUE_RETAIN || v->op == XR_LIR_VALUE_DROP) {
        if (v->result) c_format(c, "    v%" PRIu32 " = ", v->result);
        else c_text(c, "    ");
        c_text(c, v->result ? "xrt_boxed_retain(" : "xrt_boxed_drop(");
        c_value(c, v->operands[0]);
        c_format(c, ", %" PRIu32 ");\n", source);
    } else if (v->op == XR_LIR_RECORD_NEW || v->op == XR_LIR_NULLABLE_NONE || v->op == XR_LIR_NULLABLE_SOME) {
        c_text(c, "    {\n");
        if (v->operand_count) {
            c_format(c, "    uint64_t xrl_fields[%" PRIu32 "] = {0};\n", v->operand_count);
            for (uint32_t n = 0; n < v->operand_count; ++n) {
                c_format(c, "    { %s xrl_field = ", c_type(c, c_value_type(c, v->operands[n])));
                c_value(c, v->operands[n]);
                c_format(c, "; memcpy(&xrl_fields[%" PRIu32 "], &xrl_field, sizeof(xrl_field)); }\n", n);
            }
        }
        c_format(c, "    v%" PRIu32 " = xrt_boxed_new(&xrl_boxed_image, %" PRIu32 ", %s, %" PRIu32 ", %" PRIu32 ");\n    }\n",
            v->result, v->type - XR_LIR_TYPE_COUNT, v->operand_count ? "xrl_fields" : "NULL", v->operand_count, source);
    } else if (v->op == XR_LIR_NULLABLE_IS_SOME) {
        c_format(c, "    v%" PRIu32 " = xrt_boxed_present(", v->result);
        c_value(c, v->operands[0]);
        c_format(c, ", %" PRIu32 ");\n", source);
    } else {
        c_text(c, "    { uint64_t xrl_field = xrt_boxed_field(");
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu64 "), %" PRIu32 ");\n", v->bits, source);
        if (v->type != XR_LIR_UNIT)
            c_format(c, "    memcpy(&v%" PRIu32 ", &xrl_field, sizeof(v%" PRIu32 "));\n", v->result, v->result);
        else c_text(c, "    (void)xrl_field;\n");
        c_text(c, "    }\n");
    }
}
