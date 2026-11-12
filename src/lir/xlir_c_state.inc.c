/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_state.inc.c - Typed module storage under the native execution owner
 *
 * KEY CONCEPT:
 *   Cache immutable reference metadata at its producer. Readiness is tested
 *   only at the actual read or write, inside its original control-flow path.
 */
static bool c_state_point(const XrLirInstructionView *v) {
    return v->op >= XR_LIR_MODULE_BEGIN && v->op <= XR_LIR_STATE_WRITE;
}

static uint32_t c_state_payload(LirC *c, XrLirType type, XrLirType element) {
    if (type >= XR_LIR_TYPE_COUNT && type - XR_LIR_TYPE_COUNT < c->constructed_count)
        return c->state_type_base + type - XR_LIR_TYPE_COUNT;
    XrtModuleTypeKind kind = lir_state_payload_kind(type, element);
    if (kind) return (uint32_t)kind - XRT_MODULE_UNIT;
    c_error(c, XR_LIR_C_UNSUPPORTED, "module payload has no exact runtime type");
    return 0;
}

static void c_state_layout(LirC *c) {
    static const char *const names[] = {
        "XRT_MODULE_UNIT", "XRT_MODULE_BOOL", "XRT_MODULE_RUNE",
        "XRT_MODULE_I8", "XRT_MODULE_I16", "XRT_MODULE_I32", "XRT_MODULE_I64",
        "XRT_MODULE_U8", "XRT_MODULE_U16", "XRT_MODULE_U32", "XRT_MODULE_U64",
        "XRT_MODULE_F32", "XRT_MODULE_F64", "XRT_MODULE_STRING",
        "XRT_MODULE_ARRAY_I64", "XRT_MODULE_ARRAY_STRING",
        "XRT_MODULE_ARRAY_U8", "XRT_MODULE_ARRAY_U64"
    };
    _Static_assert(sizeof(names) / sizeof(*names) == LIR_STATE_CONSTRUCTED_BASE,
        "Every builtin module type has a symbolic C name");
    const XrLirStateLayout *layout = &c->state_layout;
    c_text(c, "static const XrtModuleTypeSpec xrl_state_types[] = {\n");
    for (uint32_t kind = XRT_MODULE_UNIT; kind < XRT_MODULE_FIXED_U8; ++kind)
        c_format(c, "    {%s, 0},\n", names[kind - XRT_MODULE_UNIT]);
    for (uint32_t n = 0; n < c->constructed_count && c_request_poll(c); ++n) {
        XrLirTypeView shape = c->constructed_types[n].shape;
        uint32_t argument = shape.count;
        if (shape.kind == XR_LIR_TYPE_NULLABLE) argument = shape.element >= XR_LIR_TYPE_COUNT ?
            c->state_type_base + shape.element - XR_LIR_TYPE_COUNT :
            (uint32_t)xr_pod_module_kind(lir_pod_scalar(shape.element)) - XRT_MODULE_UNIT;
        c_format(c, "    {%s, UINT32_C(%" PRIu32 ")},\n",
            shape.kind == XR_LIR_TYPE_NULLABLE ? "XRT_MODULE_NULLABLE" : "XRT_MODULE_FIXED_U8", argument);
    }
    c_text(c, "};\n");
    if (layout->module_count) c_text(c, "static const XrtModuleSpec xrl_state_modules[] = {\n");
    for (uint32_t i = 0; i < layout->module_count && c_request_poll(c); ++i)
        c_format(c, "    {UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 ")},\n",
            layout->modules[i].first_dependency, layout->modules[i].dependency_count);
    if (layout->module_count) c_text(c, "};\n");
    if (layout->dependency_count) c_text(c, "static const uint32_t xrl_state_dependencies[] = {");
    for (uint32_t i = 0; i < layout->dependency_count && c_request_poll(c); ++i)
        c_format(c, "UINT32_C(%" PRIu32 "),", layout->dependencies[i]);
    if (layout->dependency_count) c_text(c, "};\n");
    if (layout->slot_count) c_text(c, "static const XrtModuleSlotSpec xrl_state_slots[] = {\n");
    for (uint32_t i = 0; i < layout->slot_count && c_request_poll(c); ++i) {
        XrLirStateSlot slot = layout->slots[i];
        uint32_t type = c_state_payload(c, slot.payload, slot.array_element);
        c_format(c, "    {UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 "), %s,\n",
            slot.module, type, slot.mutable ? "XRT_MODULE_SLOT_MUTABLE" : "0");
        c_format(c, "     UINT64_C(%" PRIu64 "), UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 ")},\n",
            slot.offset, slot.size, slot.alignment);
    }
    if (layout->slot_count) c_text(c, "};\n");
    c_text(c, "static const XrtModuleLayoutSpec xrl_state_spec = {\n");
    c_format(c, "    xrl_state_types, %u, %s, UINT32_C(%" PRIu32 "),\n",
        c->state_type_base + c->constructed_count, layout->module_count ? "xrl_state_modules" : "NULL", layout->module_count);
    c_format(c, "    %s, UINT32_C(%" PRIu32 "), %s, UINT32_C(%" PRIu32 "),\n",
        layout->dependency_count ? "xrl_state_dependencies" : "NULL", layout->dependency_count,
        layout->slot_count ? "xrl_state_slots" : "NULL", layout->slot_count);
    c_format(c, "    UINT64_C(%" PRIu64 "), UINT64_MAX, UINT32_C(%" PRIu32 ")\n};\n\n",
        layout->payload_size, layout->payload_alignment);
}

static void c_state_require_end(LirC *c, uint32_t source) {
    c_format(c, ", UINT32_C(%" PRIu32 "));\n", source);
}

static void c_state_reference_cache(LirC *c, uint32_t value, uint32_t source) {
    c_format(c, "    xrt_module_require(xrt_module_reference_cache(v%" PRIu32
        ", &xrl_access%" PRIu32 ")", value, value);
    c_state_require_end(c, source);
}

static void c_state_check_access(LirC *c, uint32_t reference, uint32_t source) {
    c_format(c, "    if ((*xrl_access%" PRIu32 ".flags & xrl_access%" PRIu32
        ".required_flags) != xrl_access%" PRIu32 ".required_flags)\n", reference, reference, reference);
    c_format(c, "        xrt_module_uninitialized(UINT32_C(%" PRIu32 "));\n", source);
}

static void c_state_access(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    uint32_t reference = v->operands[0];
    XrLirStateReference shape = {0};
    if (xr_lir_state_reference_view(c->request->module,
        (XrLirValue){c->request->module, c->function.id, reference}, &shape) != XR_LIR_OK) {
        c_error(c, XR_LIR_C_INVALID_LIR, "module reference lost its owned shape");
        return;
    }
    (void)c_state_payload(c, shape.payload, shape.array_element);
    if (v->op == XR_LIR_STATE_WRITE && !shape.mutable) {
        c_error(c, XR_LIR_C_INVALID_LIR, "module reference is immutable");
        return;
    }
    c_state_check_access(c, reference, source);
    if (v->op == XR_LIR_STATE_READ) {
        if (shape.payload == XR_LIR_UNIT) return;
        if (shape.payload >= XR_LIR_TYPE_COUNT) {
            c_format(c, "    memcpy(&v%" PRIu32 ", xrl_access%" PRIu32 ".payload, sizeof(v%" PRIu32 "));\n",
                v->result, reference, v->result);
            return;
        }
        c_format(c, "    v%" PRIu32 " = ", v->result);
        if (shape.payload == XR_LIR_STRING || shape.payload == XR_LIR_ARRAY)
            c_text(c, shape.payload == XR_LIR_ARRAY ? "xrt_array_retain(" : "xrt_string_retain(");
        c_format(c, "*(%s const *)xrl_access%" PRIu32 ".payload", c_type(c, shape.payload), reference);
        if (shape.payload == XR_LIR_STRING || shape.payload == XR_LIR_ARRAY)
            c_format(c, ", UINT32_C(%" PRIu32 "))", source);
        c_text(c, ";\n");
    } else if (shape.payload == XR_LIR_STRING || shape.payload == XR_LIR_ARRAY || shape.payload == XR_LIR_UNIT ||
        shape.payload >= XR_LIR_TYPE_COUNT) {
        c_text(c, "    xrt_module_require(xrt_module_replace(");
        c_value(c, reference);
        c_text(c, ", ");
        if (shape.payload == XR_LIR_UNIT) c_text(c, "NULL");
        else { c_text(c, "&"); c_value(c, v->operands[1]); }
        c_text(c, ")");
        c_state_require_end(c, source);
    } else {
        c_format(c, "    *(%s *)xrl_access%" PRIu32 ".payload = ", c_type(c, shape.payload), reference);
        c_value(c, v->operands[1]);
        c_text(c, ";\n");
    }
}

static void c_state_instruction(LirC *c, const XrLirInstructionView *v, uint32_t source) {
    if (v->op == XR_LIR_STATE_READ || v->op == XR_LIR_STATE_WRITE) {
        c_state_access(c, v, source);
        return;
    }
    if (v->op == XR_LIR_STATE_REFERENCE) {
        c_text(c, "    xrt_module_require(xrt_module_cache(");
        c_value(c, v->operands[0]);
        c_format(c, ", UINT32_C(%" PRIu64 "), &xrl_access%" PRIu32 ")", v->bits, v->result);
        c_state_require_end(c, source);
        c_format(c, "    v%" PRIu32 " = xrl_access%" PRIu32 ".reference;\n", v->result, v->result);
        return;
    }
    const char *operation = NULL;
    switch (v->op) {
    case XR_LIR_MODULE_BEGIN: operation = "xrt_module_begin"; break;
    case XR_LIR_MODULE_READY: operation = "xrt_module_ready"; break;
    case XR_LIR_STATE_CONSTRUCT: operation = "xrt_module_construct"; break;
    case XR_LIR_STATE_PUBLISH: operation = "xrt_module_publish"; break;
    case XR_LIR_STATE_INITIALIZE: operation = "xrt_module_initialize"; break;
    default: c_error(c, XR_LIR_C_INVALID_LIR, "invalid module state operation"); return;
    }
    if (v->op == XR_LIR_STATE_CONSTRUCT || v->op == XR_LIR_STATE_REFERENCE)
        c_format(c, "    v%" PRIu32 " = NULL;\n", v->result);
    c_format(c, "    xrt_module_require(%s(", operation);
    c_value(c, v->operands[0]);
    if (v->op != XR_LIR_STATE_PUBLISH) c_format(c, ", UINT32_C(%" PRIu64 ")", v->bits);
    if (v->op == XR_LIR_STATE_CONSTRUCT || v->op == XR_LIR_STATE_INITIALIZE) {
        c_text(c, ", ");
        if (v->operand_count == 1) c_text(c, "NULL");
        else { c_text(c, "&"); c_value(c, v->operands[1]); }
    }
    if (v->op == XR_LIR_STATE_CONSTRUCT || v->op == XR_LIR_STATE_REFERENCE)
        c_format(c, ", &v%" PRIu32, v->result);
    c_text(c, ")");
    c_state_require_end(c, source);
    if (v->op == XR_LIR_STATE_CONSTRUCT || v->op == XR_LIR_STATE_REFERENCE)
        c_state_reference_cache(c, v->result, source);
}

static void c_state_root(LirC *c, XrLirType result) {
    const char *type = c_type(c, result);
    c_text(c, "static void xrl_invoke(void *context, XrtModuleState *state) {\n");
    if (result == XR_LIR_UNIT) c_text(c, "    (void)context;\n    ");
    else if (result == XR_LIR_STRING || result == XR_LIR_ARRAY) c_format(c, "    %s owned = ", type);
    else c_format(c, "    *(%s *)context = ", type);
    c_function_name(c, c->request->entry.id);
    c_text(c, "(state);\n");
    if (result == XR_LIR_STRING) c_text(c, "    *(XrtString **)context = xrt_string_root_take(owned);\n");
    if (result == XR_LIR_ARRAY) c_text(c, "    *(XrtArray **)context = xrt_array_root_take(owned);\n");
    c_format(c, "}\n\n%s xray_lir_entry(void) {\n", type);
    if (result != XR_LIR_UNIT) c_format(c, "    %s result = {0};\n", type);
    c_format(c, "    xrt_run_standalone_state(&xrt_standalone_source_map, xrl_invoke, %s, &xrl_state_spec);\n",
        result == XR_LIR_UNIT ? "NULL" : "&result");
    if (result != XR_LIR_UNIT) c_text(c, "    return result;\n");
    c_text(c, "}\n\n");
}
