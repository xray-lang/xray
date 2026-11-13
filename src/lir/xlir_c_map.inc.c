/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_map.inc.c - Typed Map expressions and synchronous native references
 *
 * KEY CONCEPT:
 *   Generated calls use canonical runtime owners. Borrowed references carry
 *   the actual local or State binding without a retained payload snapshot.
 */
static uint32_t c_map_role(LirC *c, const XrLirInstructionView *v) {
    XrLirIntrinsic entry;
    return v->op == XR_LIR_CALL_INTRINSIC &&
        xr_lir_intrinsic_view(c->request->module, v->intrinsic, &entry) == XR_LIR_OK &&
        xr_lir_map_intrinsic_valid(c->request->module, &entry) ? entry.operation : 0;
}
static bool c_map_point(LirC *c, const XrLirInstructionView *v) {
    return c_map_role(c, v) || v->op == XR_LIR_MAP_RETAIN || v->op == XR_LIR_MAP_BORROW ||
        v->op == XR_LIR_MAP_REF_READ || v->op == XR_LIR_MAP_REF_WRITE ||
        (v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_MAP_STRING_I64_CELL) ||
        (v->op == XR_LIR_CELL_READ && v->type == XR_LIR_MAP_STRING_I64) ||
        (v->op == XR_LIR_CELL_WRITE && c_value_type(c, v->operands[0]) == XR_LIR_MAP_STRING_I64_CELL);
}
static void c_map_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    uint32_t role = c_map_role(c, v);
    if (role == XR_LIR_MAP_OP_NEW) {
        c_format(c, "    v%" PRIu32 " = xrt_map_string_i64_new(", v->result);
        for (uint32_t field = 0; field < 2; ++field) {
            if (!v->operand_count) c_text(c, "NULL");
            else {
                c_text(c, field ? "(const int64_t[]){" : "(XrtString *const[]){");
                for (uint32_t n = field; n < v->operand_count; n += 2) {
                    if (n > field) c_text(c, ", ");
                    c_value(c, v->operands[n]);
                }
                c_text(c, "}");
            }
            c_text(c, ", ");
        }
        c_format(c, "(size_t)%" PRIu32 ", UINT32_C(%" PRIu32 "));\n", v->operand_count / 2, source);
        return;
    }
    if (role == XR_LIR_MAP_OP_GET) {
        c_format(c, "    v%" PRIu32 " = (%s){0};\n    v%" PRIu32 ".tag = xrt_map_string_i64_get(",
            v->result, c_type(c, v->type), v->result);
        c_value(c, v->operands[0]); c_text(c, ", "); c_value(c, v->operands[1]);
        c_format(c, ", &v%" PRIu32 ".payload, UINT32_C(%" PRIu32 "));\n", v->result, source);
        return;
    }
    if (role) {
        const char *operation = role == XR_LIR_MAP_OP_LEN ? "length" : role == XR_LIR_MAP_OP_CONTAINS ?
            "string_i64_contains" : role == XR_LIR_MAP_OP_SET ? "string_i64_set" :
            role == XR_LIR_MAP_OP_REMOVE ? "string_i64_remove" : "clear";
        c_text(c, "    ");
        if (v->type != XR_LIR_UNIT) c_format(c, "v%" PRIu32 " = ", v->result);
        c_format(c, "xrt_map_%s%s(", c_value_type(c, v->operands[0]) == XR_LIR_MAP_STRING_I64_REF ? "ref_" : "", operation);
        for (uint32_t n = 0; n < v->operand_count; ++n) {
            if (n) c_text(c, ", ");
            c_value(c, v->operands[n]);
        }
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
    } else if (v->op == XR_LIR_MAP_BORROW) {
        c_format(c, "    v%" PRIu32 " = xrt_map_ref_%s(", v->result,
            c_value_type(c, v->operands[0]) == XR_LIR_STATE_REF ? "state" : "local");
        c_value(c, v->operands[0]); c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
    } else if (v->op == XR_LIR_MAP_REF_READ) {
        c_format(c, "    v%" PRIu32 " = xrt_map_ref_read(", v->result);
        c_value(c, v->operands[0]); c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
    } else if (v->op == XR_LIR_MAP_REF_WRITE || v->op == XR_LIR_CELL_WRITE) {
        c_text(c, v->op == XR_LIR_CELL_WRITE ? "    xrt_map_assign(" : "    xrt_map_ref_write(");
        c_value(c, v->operands[0]); c_text(c, ", "); c_value(c, v->operands[1]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
    } else if (v->op == XR_LIR_CELL_NEW) {
        c_format(c, "    XrtMap *xrl_cell%" PRIu32 " = xrt_map_retain(", v->result);
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n    v%" PRIu32 " = &xrl_cell%" PRIu32 ";\n", source, v->result, v->result);
    } else if (v->op == XR_LIR_MAP_DROP || v->op == XR_LIR_MAP_CELL_DROP) {
        c_text(c, "    xrt_map_drop(");
        if (v->op == XR_LIR_MAP_DROP) c_text(c, "&");
        c_value(c, v->operands[0]); c_text(c, ");\n");
    } else {
        c_format(c, "    v%" PRIu32 " = xrt_map_retain(", v->result);
        if (v->op == XR_LIR_CELL_READ) c_text(c, "*");
        c_value(c, v->operands[0]); c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
    }
}
