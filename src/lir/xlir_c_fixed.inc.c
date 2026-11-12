/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_fixed.inc.c - Exact byte aggregates with ordinary C value transport
 *
 * KEY CONCEPT:
 *   Each physical type has one struct definition. Calls, returns and selected
 *   edges copy that value. State storage has byte alignment and is read with
 *   memcpy, while extraction checks bounds before indexing the actual value.
 */
static const char *c_type(const LirC *c, XrLirType type) {
    if (type < XR_LIR_TYPE_COUNT) return c_builtin_type(type);
    uint32_t index = type - XR_LIR_TYPE_COUNT;
    return index < c->fixed_count ? c->fixed_types[index].name : NULL;
}

static void c_prepare_types(LirC *c) {
    const XrLirModule *module = c->request->module;
    if (!xr_lir_module_is_readable(module)) {
        c_error(c, XR_LIR_C_INVALID_LIR, "C emission requires a finished physical type pool");
        return;
    }
    c->state_type_base = 5;
    for (uint32_t s = 0; s < c->state_layout.slot_count; ++s)
        if (c->state_layout.slots[s].payload == XR_LIR_ARRAY) c->state_type_base = 9;
    uint32_t count = xr_lir_constructed_type_count(module);
    if (!count) return;
    if (sizeof(*c->fixed_types) > SIZE_MAX / count) {
        c_error(c, XR_LIR_C_OVERFLOW, "C physical type inventory exceeds machine capacity");
        return;
    }
    void *memory = NULL;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources,
        count, sizeof(*c->fixed_types), &memory))) return;
    c->fixed_types = memory;
    c->fixed_count = count;
    for (uint32_t n = 0; n < count && c_request_poll(c); ++n) {
        XrLirType type = XR_LIR_TYPE_COUNT + n;
        LirCFixedType *owned = &c->fixed_types[n];
        if (xr_lir_type_view(module, type, &owned->shape) != XR_LIR_OK ||
            xr_lir_type_layout(module, type, &owned->layout) != XR_LIR_OK ||
            owned->shape.kind != XR_LIR_TYPE_FIXED_ARRAY || owned->shape.element != XR_LIR_U8 ||
            !owned->shape.count || owned->shape.count > 65535 || owned->layout.size != owned->shape.count ||
            owned->layout.alignment != 1 || owned->layout.scalar_count != owned->shape.count ||
            owned->layout.managed) {
            c_error(c, XR_LIR_C_UNSUPPORTED, "constructed physical types have no native implementation");
            return;
        }
        snprintf(owned->name, sizeof(owned->name), "XrlFixed_%" PRIu32, type);
    }
}

static void c_fixed_types(LirC *c) {
    for (uint32_t n = 0; n < c->fixed_count && c_request_poll(c); ++n) {
        const LirCFixedType *type = &c->fixed_types[n];
        c_format(c, "typedef struct %s { uint8_t bytes[%" PRIu32 "]; } %s;\n",
            type->name, type->shape.count, type->name);
        c_format(c, "_Static_assert(sizeof(%s) == UINT64_C(%" PRIu64 "), \"fixed byte layout\");\n",
            type->name, type->layout.size);
    }
}

static void c_fixed_instruction(LirC *c, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_FIXED_ARRAY_NEW) {
        c_format(c, "    v%" PRIu32 " = (%s){{", v->result, c_type(c, v->type));
        for (uint32_t n = 0; n < v->operand_count && c_request_poll(c); ++n) {
            if (n) c_text(c, ", ");
            c_value(c, v->operands[n]);
        }
        c_text(c, "}};\n");
        return;
    }
    XrLirType type = c_value_type(c, v->operands[0]);
    const LirCFixedType *fixed = &c->fixed_types[type - XR_LIR_TYPE_COUNT];
    uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
    c_text(c, "    {\n    int64_t xrl_index = ");
    c_value(c, v->operands[1]);
    /* Keep valid loads ordinary C. Only the rejected branch captures through
     * volatile, so host propagation cannot diagnose subsequent guest code as
     * unreachable after a statically known panic. Both checks use one value. */
    c_format(c, ";\n    if ((uint64_t)xrl_index >= UINT64_C(%" PRIu32 ")) {\n"
        "        volatile int64_t xrl_guard = xrl_index;\n        xrl_index = xrl_guard;\n"
        "        if ((uint64_t)xrl_index >= UINT64_C(%" PRIu32 "))\n"
        "            xrt_fixed_array_bounds(UINT32_C(%" PRIu32 "));\n    }\n",
        fixed->shape.count,
        fixed->shape.count, source);
    c_format(c, "    v%" PRIu32 " = ", v->result);
    c_value(c, v->operands[0]);
    c_text(c, ".bytes[xrl_index];\n    }\n");
}
