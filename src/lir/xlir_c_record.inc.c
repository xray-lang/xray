/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_record.inc.c - Natural Record values over stable inline owner homes
 *
 * KEY CONCEPT:
 *   Typed local snapshots borrow their operand owners only through the Core
 *   call. All owned results publish through their real Frame leaf slots.
 */
static bool c_record_point(const XrLirInstructionView *v) {
    return v->op == XR_LIR_RECORD_NEW || v->op == XR_LIR_RECORD_FIELD ||
        v->op == XR_LIR_VALUE_RETAIN || v->op == XR_LIR_VALUE_DROP;
}
static void c_record_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    uint32_t destination = v->op == XR_LIR_VALUE_DROP ? v->operands[0] : v->result;
    bool owned = c->homes && c->homes->values[destination].owner_count;
    if (v->op == XR_LIR_VALUE_DROP) {
        if (owned) { c_values_replace_begin(c, destination, source); c_text(c, "NULL"); c_values_replace_end(c, source); }
        return;
    }
    if (v->op == XR_LIR_RECORD_NEW) {
        c_format(c, "    { const %s input = {", c_type(c, v->type));
        if (!v->operand_count) c_text(c, "0");
        for (uint32_t a = 0; a < v->operand_count && c_request_poll(c); ++a) {
            if (a) c_text(c, ", ");
            c_value(c, v->operands[a]);
        }
        c_text(c, "};\n");
        if (owned) { c_values_replace_begin(c, destination, source); c_text(c, "&input"); c_values_replace_end(c, source); }
        else c_format(c, "    v%" PRIu32 " = input;\n", destination);
        c_text(c, "    }\n"); return;
    }
    if (owned) { c_values_replace_begin(c, destination, source); c_text(c, "&"); }
    else c_format(c, "    v%" PRIu32 " = ", destination);
    c_value(c, v->operands[0]);
    if (v->op == XR_LIR_RECORD_FIELD) c_format(c, ".f%" PRIu64, v->bits);
    if (owned) c_values_replace_end(c, source); else c_text(c, ";\n");
}
