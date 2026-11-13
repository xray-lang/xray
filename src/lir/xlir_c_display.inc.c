/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_display.inc.c - Immutable physical catalogs and borrowed value spans
 */
static bool c_display_point(LirC *c, const XrLirInstructionView *v) {
    return xr_lir_display_point(c->request->module, v);
}
static void c_display_catalog(LirC *c) {
    if (!c->displays || !c->pod_count) return;
    c_text(c, "static const XrPodType xrl_display_types[] = {\n");
    for (uint32_t n = 0; n < c->constructed_count && c_request_poll(c); ++n) {
        if (c->constructed_types[n].pod_index == UINT32_MAX) continue;
        XrPodType type = {0};
        if (xr_lir_pod_type(c->request->module, XR_LIR_TYPE_COUNT + n, &type) != XR_LIR_OK) {
            c_error(c, XR_LIR_C_INVALID_LIR, "display lost its exact physical type");
            return;
        }
        if (type.kind == XR_POD_NULLABLE && type.element >= XR_POD_SCALAR_COUNT)
            type.element = XR_POD_SCALAR_COUNT + c->constructed_types[type.element - XR_POD_SCALAR_COUNT].pod_index;
        c_format(c, "    { %s, UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32
            "), {UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 ")} },\n",
            type.kind == XR_POD_NULLABLE ? "XR_POD_NULLABLE" : "XR_POD_FIXED_U8",
            type.count, type.element, type.physical.size, type.physical.alignment, type.physical.scalar_count);
    }
    c_text(c, "};\n\n");
}
static void c_display_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    XrLirType type = c_value_type(c, v->operands[0]);
    uint32_t encoded = type >= XR_LIR_TYPE_COUNT ?
        XR_POD_SCALAR_COUNT + c->constructed_types[type - XR_LIR_TYPE_COUNT].pod_index : lir_pod_scalar(type);
    c_format(c, "    v%" PRIu32 " = xrt_pod_display(%s, UINT32_C(%" PRIu32
        "), UINT32_C(%" PRIu32 "), &", v->result,
        c->pod_count ? "xrl_display_types" : "NULL", c->pod_count, encoded);
    c_value(c, v->operands[0]);
    c_text(c, ", sizeof(");
    c_value(c, v->operands[0]);
    c_format(c, "), UINT32_C(%" PRIu32 "));\n", source);
}
