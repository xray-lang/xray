/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_display.inc.c - Exact POD spans received by the fixed String core
 */
static bool c_display_point(LirC *c, const XrLirInstructionView *v) {
    return xr_lir_display_point(c->request->module, v);
}
static void c_display_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    XrLirType type = c_value_type(c, v->operands[0]);
    if (xr_lir_tuple_display_type(c->request->module, type)) {
        const XrLirValueHome *input = &c->homes->values[v->operands[0]];
        const XrLirValueImage *owner = c->value_image;
        const XrtValueImage *image = xr_lir_value_image_view(owner);
        XR_CHECK(image && input->type && input->type <= image->type_count &&
                 (image->types[input->type - 1].flags & XRT_VALUE_INLINE_DISPLAY),
            "a verified Tuple display uses its exact common physical type");
        c_values_ensure(c, source);
        c_format(c, "    xrt_native_tuple_display_create(xrl_frame, %zu, &xrl_value_image, UINT32_C(%" PRIu32 "), &",
            c->homes->values[v->result].temporary_first_owner, input->type);
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
        c_values_publish_leaf(c, v->result, source);
        return;
    }
    XrLirTypeLayout physical = {0};
    XR_CHECK(xr_lir_display_type(c->request->module, type) &&
        xr_lir_type_layout(c->request->module, type, &physical) == XR_LIR_OK,
        "an admitted display carries its exact primitive or Nullable POD span");
    uint32_t depth = 0;
    while (type >= XR_LIR_TYPE_COUNT) {
        const XrLirTypeView *shape = &c->constructed_types[type - XR_LIR_TYPE_COUNT].shape;
        XR_CHECK(shape->kind == XR_LIR_TYPE_NULLABLE && shape->element < type,
            "every admitted Nullable display reaches one primitive without a cycle");
        type = shape->element; ++depth;
    }
    c_values_ensure(c, source);
    c_format(c, "    xrt_native_pod_display_create(xrl_frame, %zu, &(XrtPodDisplay){(XrPodScalar)%u, UINT32_C(%" PRIu32 "),\n",
        c->homes->values[v->result].temporary_first_owner, (unsigned)lir_pod_scalar(type), depth);
    c_format(c, "        {UINT32_C(%" PRIu64 "), UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 ")}}, &",
        (uint64_t)physical.size, physical.alignment, physical.scalar_count);
    c_value(c, v->operands[0]);
    c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
    c_values_publish_leaf(c, v->result, source);
}
