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
static bool c_array_element_supported(XrLirType element) {
    return element == XR_LIR_I64 || element == XR_LIR_STRING || element == XR_LIR_U8 || element == XR_LIR_U64;
}
static void c_array_admit_shapes(LirC *c) {
    XrLirArrayShape shape = {0};
    if (c->view.result == XR_LIR_ARRAY) {
        if (xr_lir_array_result_view(c->request->module, c->function, &shape) != XR_LIR_OK ||
            !c_array_element_supported(shape.element))
            c_error(c, XR_LIR_C_UNSUPPORTED, "Array result has no native element implementation");
    }
    for (uint32_t id = 1; id <= c->view.value_count && c->status == XR_LIR_C_OK; ++id) {
        XrLirValue value = {c->request->module, c->function.id, id};
        XrLirType type = c_value_type(c, id);
        if (type == XR_LIR_ARRAY || type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF) {
            if (xr_lir_array_shape_view(c->request->module, value, &shape) != XR_LIR_OK ||
                !c_array_element_supported(shape.element))
                c_error(c, XR_LIR_C_UNSUPPORTED, "Array value has no native element implementation");
        } else if (type == XR_LIR_STATE_REF) {
            XrLirStateReference reference = {0};
            if (xr_lir_state_reference_view(c->request->module, value, &reference) != XR_LIR_OK ||
                (reference.payload == XR_LIR_ARRAY && !c_array_element_supported(reference.array_element)))
                c_error(c, XR_LIR_C_UNSUPPORTED, "module reference has no native element implementation");
        }
    }
}
static bool c_array_point(LirC *c, const XrLirInstructionView *v) {
    XrLirArrayIntrinsic shape = {0};
    return c_array_intrinsic(c, v, &shape) || v->op == XR_LIR_ARRAY_RETAIN ||
        v->op == XR_LIR_ARRAY_BORROW || v->op == XR_LIR_ARRAY_REF_READ || v->op == XR_LIR_ARRAY_REF_WRITE ||
        (v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_ARRAY_CELL) ||
        (v->op == XR_LIR_CELL_READ && v->type == XR_LIR_ARRAY) ||
        (v->op == XR_LIR_CELL_WRITE && c_value_type(c, v->operands[0]) == XR_LIR_ARRAY_CELL);
}
static bool c_array_move_pair(LirC *c, const XrLirInstructionView *v,
    const XrLirInstructionView *next) {
    if (v->operand_count != 1 || next->operand_count != 1 || v->operands[0] != next->operands[0])
        return false;
    bool create = v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_ARRAY_CELL && next->op == XR_LIR_ARRAY_DROP;
    bool read = v->op == XR_LIR_CELL_READ && v->type == XR_LIR_ARRAY && next->op == XR_LIR_ARRAY_CELL_DROP;
    if (!create && !read) return false;
    if (!c_request_poll(c)) return true;
    if (c_located(&v->source)) ++c->location_cursor;
    /* The adjacent verified DROP consumes that actual owner. Transfer its one
     * canonical reference into or out of the Cell without creating a copy. */
    if (create) {
        c_format(c, "    XrtArray *xrl_cell%" PRIu32 " = ", v->result);
        c_value(c, v->operands[0]);
        c_text(c, ";\n    "); c_value(c, v->operands[0]);
        c_format(c, " = NULL;\n    v%" PRIu32 " = &xrl_cell%" PRIu32 ";\n", v->result, v->result);
    } else {
        c_format(c, "    v%" PRIu32 " = *", v->result); c_value(c, v->operands[0]);
        c_text(c, ";\n    *"); c_value(c, v->operands[0]); c_text(c, " = NULL;\n");
    }
    return true;
}
static void c_array_call(LirC *c, const XrLirInstructionView *v,
    const XrLirArrayIntrinsic *shape, uint32_t source) {
    switch (shape->operation) {
    case XR_LIR_ARRAY_OP_NEW: case XR_LIR_ARRAY_OP_LEN: case XR_LIR_ARRAY_OP_GET:
    case XR_LIR_ARRAY_OP_SET: case XR_LIR_ARRAY_OP_PUSH: case XR_LIR_ARRAY_OP_REPEAT: break;
    default:
        c_error(c, XR_LIR_C_UNSUPPORTED, "Array role has no native implementation");
        return;
    }
    bool mutator = shape->operation == XR_LIR_ARRAY_OP_SET || shape->operation == XR_LIR_ARRAY_OP_PUSH;
    XrLirType receiver = mutator ? c_value_type(c, v->operands[0]) : XR_LIR_TYPE_INVALID;
    if (mutator && receiver != XR_LIR_ARRAY_CELL && receiver != XR_LIR_ARRAY_REF) {
        c_error(c, XR_LIR_C_UNSUPPORTED, "Array mutator receiver has no native implementation");
        return;
    }
    XrLirType element = v->type == XR_LIR_ARRAY ? shape->result_element : shape->parameter_elements[0];
    const char *family;
    switch (element) {
    case XR_LIR_I64: family = "i64"; break;
    case XR_LIR_STRING: family = "string"; break;
    case XR_LIR_U8: family = "u8"; break;
    case XR_LIR_U64: family = "u64"; break;
    default: c_error(c, XR_LIR_C_UNSUPPORTED, "Array element has no native implementation"); return;
    }
    c_text(c, "    ");
    if (v->result) c_format(c, "v%" PRIu32 " = ", v->result);
    if (shape->operation == XR_LIR_ARRAY_OP_NEW) {
        c_format(c, "xrt_array_%s_new(", family);
        if (v->operand_count) {
            if (element == XR_LIR_STRING) c_text(c, "(XrtString *const[]){");
            else c_format(c, "(const %s[]){", c_type(element));
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
    else c_format(c, "xrt_array_%s%s_%s(", receiver == XR_LIR_ARRAY_REF ? "ref_" : "", family,
        shape->operation == XR_LIR_ARRAY_OP_GET ? "get" :
        shape->operation == XR_LIR_ARRAY_OP_SET ? "set" : shape->operation == XR_LIR_ARRAY_OP_PUSH ? "push" : "repeat");
    for (uint32_t p = 0; p < v->operand_count; ++p) {
        if (p) c_text(c, ", ");
        c_value(c, v->operands[p]);
    }
    c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
}
static void c_array_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    XrLirArrayIntrinsic shape = {0};
    if (c_array_intrinsic(c, v, &shape)) { c_array_call(c, v, &shape, source); return; }
    if (v->op == XR_LIR_ARRAY_BORROW) {
        XrLirArrayShape reference = {0};
        XrLirValue result = {c->request->module, c->function.id, v->result};
        xr_lir_array_shape_view(c->request->module, result, &reference);
        const char *kind = reference.element == XR_LIR_I64 ? "XRT_MODULE_ARRAY_I64" :
            reference.element == XR_LIR_STRING ? "XRT_MODULE_ARRAY_STRING" :
            reference.element == XR_LIR_U8 ? "XRT_MODULE_ARRAY_U8" : "XRT_MODULE_ARRAY_U64";
        c_format(c, "    v%" PRIu32 " = xrt_array_ref_%s(", v->result,
            c_value_type(c, v->operands[0]) == XR_LIR_STATE_REF ? "state" : "local");
        c_value(c, v->operands[0]);
        c_format(c, ", %s, UINT32_C(%" PRIu32 "));\n", kind, source);
    } else if (v->op == XR_LIR_ARRAY_REF_READ) {
        c_format(c, "    v%" PRIu32 " = xrt_array_ref_read(", v->result);
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
    } else if (v->op == XR_LIR_ARRAY_REF_WRITE) {
        c_text(c, "    xrt_array_ref_write("); c_value(c, v->operands[0]);
        c_text(c, ", "); c_value(c, v->operands[1]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
    } else if (v->op == XR_LIR_CELL_NEW) {
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
