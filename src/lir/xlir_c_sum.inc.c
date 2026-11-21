/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_sum.inc.c - Tagged sums as ordinary inline C values
 *
 * KEY CONCEPT:
 *   The common receiving plan owns the tag width, the shared payload offset
 *   and each variant's field range. Construction and projection touch only
 *   those exact bytes; P1 payloads are owner-free, so no owner transaction is
 *   involved.
 */
static bool c_sum_point(const XrLirInstructionView *v) {
    return v->op == XR_LIR_SUM_NEW || v->op == XR_LIR_SUM_TAG || v->op == XR_LIR_SUM_GET;
}
static const XrtValuePlanSpec *c_sum_spec(LirC *c, uint32_t value) {
    if (!c->homes || !c->homes->values || !value || value > c->homes->value_count ||
        !c->homes->values[value].type || !c->value_types) return NULL;
    uint32_t type = c->homes->values[value].type;
    if (type > c->value_types->count) return NULL;
    const XrtValuePlanSpec *spec = &c->value_types->rows[type - 1].spec;
    return spec->kind == XRT_VALUE_SUM ? spec : NULL;
}
static void c_sum_instruction(LirC *c, const XrLirInstructionView *v) {
    uint32_t subject = v->op == XR_LIR_SUM_NEW ? v->result : v->operands[0];
    const XrtValuePlanSpec *spec = c_sum_spec(c, subject);
    if (!spec) {
        c_error(c, XR_LIR_C_INVALID_LIR, "tagged sum operation lost its receiving plan");
        return;
    }
    if (v->op == XR_LIR_SUM_TAG) {
        if (!spec->sum.tag_size) c_format(c, "    v%" PRIu32 " = INT64_C(0);\n", v->result);
        else {
            c_format(c, "    v%" PRIu32 " = (int64_t)", v->result);
            c_value(c, v->operands[0]);
            c_text(c, ".tag;\n");
        }
        return;
    }
    if (v->op == XR_LIR_SUM_GET) {
        uint64_t at = v->bits;
        for (size_t variant = 0; variant < spec->sum.variant_count; ++variant) {
            XrtValueSumVariantSpec row = spec->sum.variants[variant];
            if (at < row.field_begin || at >= row.field_begin + row.field_count) continue;
            c_format(c, "    v%" PRIu32 " = ", v->result);
            c_value(c, v->operands[0]);
            c_format(c, ".payload.variant%zu.f%" PRIu64 ";\n", variant, at);
            return;
        }
        c_error(c, XR_LIR_C_INVALID_LIR, "tagged sum projection lost its variant field");
        return;
    }
    if (v->bits >= spec->sum.variant_count) {
        c_error(c, XR_LIR_C_INVALID_LIR, "tagged sum construction lost its variant");
        return;
    }
    XrtValueSumVariantSpec variant = spec->sum.variants[v->bits];
    if (variant.field_count != v->operand_count) {
        c_error(c, XR_LIR_C_INVALID_LIR, "tagged sum construction lost its payload fields");
        return;
    }
    c_format(c, "    { const %s input = {", c_type(c, v->type));
    bool wrote = false;
    if (spec->sum.tag_size) {
        c_format(c, ".tag = UINT%u_C(%" PRIu64 ")", (unsigned)(spec->sum.tag_size * 8), v->bits);
        wrote = true;
    }
    if (variant.field_count) {
        if (wrote) c_text(c, ", ");
        c_format(c, ".payload = {.variant%" PRIu64 " = {", v->bits);
        for (uint32_t f = 0; f < v->operand_count && c_request_poll(c); ++f) {
            if (f) c_text(c, ", ");
            c_value(c, v->operands[f]);
        }
        c_text(c, "}}");
        wrote = true;
    }
    if (!wrote) c_text(c, "0");
    c_text(c, "};\n");
    c_format(c, "      v%" PRIu32 " = input;\n    }\n", v->result);
}
