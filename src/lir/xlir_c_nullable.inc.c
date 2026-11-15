/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_nullable.inc.c - Inline typed presence and payload operations
 */
static bool c_nullable_managed_point(LirC *c, const XrLirInstructionView *v) {
    return (v->op == XR_LIR_NULLABLE_NONE || v->op == XR_LIR_NULLABLE_SOME) &&
        xr_lir_boxed_type(c->request->module, v->type);
}
static void c_nullable_instruction(LirC *c, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_NULLABLE_NONE || v->op == XR_LIR_NULLABLE_SOME) {
        uint32_t source = c_nullable_managed_point(c, v) && c_located(&v->source) ? ++c->location_cursor : 0;
        if (c->homes && c->homes->values[v->result].owner_count) {
            if (v->op == XR_LIR_NULLABLE_NONE) {
                c_values_replace_begin(c, v->result, source); c_text(c, "NULL"); c_values_replace_end(c, source);
            } else {
                c_format(c, "    { const %s input = {.tag=1, .payload=", c_type(c, v->type));
                c_value(c, v->operands[0]); c_text(c, "};\n");
                c_values_replace_begin(c, v->result, source); c_text(c, "&input"); c_values_replace_end(c, source);
                c_text(c, "    }\n");
            }
            return;
        }
        c_format(c, "    v%" PRIu32 " = (%s){0};\n", v->result, c_type(c, v->type));
        if (v->op == XR_LIR_NULLABLE_SOME) {
            if (v->operand_count) {
                c_format(c, "    v%" PRIu32 ".payload = ", v->result);
                c_value(c, v->operands[0]);
                c_text(c, ";\n");
            }
            c_format(c, "    v%" PRIu32 ".tag = 1;\n", v->result);
        }
        return;
    }
    if (v->op == XR_LIR_NULLABLE_IS_SOME) {
        c_format(c, "    v%" PRIu32 " = ", v->result);
        c_value(c, v->operands[0]);
        c_text(c, ".tag != 0;\n");
        return;
    }
    uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
    c_text(c, "    { uint8_t xrl_tag = ");
    c_value(c, v->operands[0]);
    c_format(c, ".tag;\n    if (!xrl_tag) { volatile uint8_t xrl_rejected = xrl_tag;\n"
        "        if (!xrl_rejected) xrt_null_unwrap(UINT32_C(%" PRIu32 "));\n    }\n", source);
    if (v->type != XR_LIR_UNIT) {
        bool owned = c->homes && c->homes->values[v->result].owner_count;
        if (owned) { c_values_replace_begin(c, v->result, source); c_text(c, "&"); }
        else c_format(c, "    v%" PRIu32 " = ", v->result);
        c_value(c, v->operands[0]); c_text(c, ".payload");
        if (owned) c_values_replace_end(c, source); else c_text(c, ";\n");
    }
    c_text(c, "    }\n");
}
