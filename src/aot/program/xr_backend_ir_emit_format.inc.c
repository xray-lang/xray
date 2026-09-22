/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_emit_format.inc.c - Native layout adapter for shared formatting
 */

static bool has_boundary_error(const XrBackendIR *ir) {
    for (uint32_t function = 0u; function < ir->program->function_count; ++function)
        if (first_boundary_error_type(ir, function))
            return true;
    return false;
}

static bool has_boundary_panic(const XrBackendIR *ir) {
    for (uint32_t function = 0u; function < ir->program->function_count; ++function)
        if (ir->program->functions[function].panic_type_id != XR_CORE_TYPE_VOID)
            return true;
    return false;
}

static bool emit_format_name(CBuffer *buffer, const char *name) {
    if (!name)
        return append_text(buffer, "NULL");
    if (!append_text(buffer, "\""))
        return false;
    /* Fixed-width octal escapes preserve UTF-8 without C escape adjacency or
     * dependence on the host compiler's execution character set. */
    for (const unsigned char *byte = (const unsigned char *) name; *byte; ++byte)
        if (!append_format(buffer, "\\%03o", (unsigned) *byte))
            return false;
    return append_text(buffer, "\"");
}

static bool emit_format_reader(CBuffer *buffer, const XrBackendIR *ir) {
    if (!has_boundary_error(ir) && !has_boundary_panic(ir))
        return true;
    if (!append_text(buffer, (const char *) xr_value_format_source))
        return false;
    if (!has_boundary_error(ir))
        return true;
    if (!append_text(buffer,
            "\nstatic int xr_aot_format_read(const void *context, XrValueFormatNode node, "
            "XrValueFormatView *view) {\n"
            "    (void)context; if (!node.value) return 0;\n"
            "    switch (node.type) {\n"))
        return false;
    const uint16_t integers[] = {XR_CORE_TYPE_I8, XR_CORE_TYPE_U8, XR_CORE_TYPE_I16,
        XR_CORE_TYPE_U16, XR_CORE_TYPE_I32, XR_CORE_TYPE_U32, XR_CORE_TYPE_I64, XR_CORE_TYPE_U64};
    for (uint32_t index = 0u; index < XR_COUNTOF(integers); ++index) {
        const XrCoreIntegerType *integer = xr_core_spec_integer_type(integers[index]);
        char storage[32];
        if (!append_format(buffer,
            "    case %u: view->kind = XR_VALUE_FORMAT_%s; view->%s_value = "
            "*(const %s *)node.value; return 1;\n", integers[index],
            integer->is_signed ? "SIGNED" : "UNSIGNED",
            integer->is_signed ? "signed" : "unsigned", type_c_name(integers[index], storage)))
            return false;
    }
    if (!append_format(buffer,
        "    case %u: view->kind = XR_VALUE_FORMAT_BOOL; "
        "view->unsigned_value = *(const uint8_t *)node.value; return 1;\n"
        "    case %u: view->kind = XR_VALUE_FORMAT_BYTES; "
        "view->size = xr_text_encode_rune(*(const uint32_t *)node.value, view->scalar_bytes); "
        "view->bytes = view->scalar_bytes; view->quote = '\\''; return view->size != 0;\n",
        XR_CORE_TYPE_BOOL, XR_CORE_TYPE_RUNE))
        return false;
    if (has_string_values(ir) && !append_format(buffer,
        "    case %u: { const XrAotString *value = *(XrAotString * const *)node.value; "
        "if (!value) return 0; view->kind = XR_VALUE_FORMAT_BYTES; "
        "view->bytes = value->bytes; view->size = value->size; view->quote = '\"'; return 1; }\n",
        XR_CORE_TYPE_STRING))
        return false;
    for (uint32_t index = 0u; index < ir->program->type_count; ++index) {
        const XrValidatedType *type = &ir->program->types[index];
        if (type->kind != XR_CORE_IR_TYPE_VARIANT)
            continue;
        if (!append_format(buffer,
            "    case %u: { const XrAotType%u *value = node.value; "
            "view->kind = XR_VALUE_FORMAT_ENUM; view->name = ", type->type_id, type->type_id) ||
            !emit_format_name(buffer, type->display_name) ||
            !append_text(buffer, "; switch (value->tag) {\n"))
            return false;
        for (uint32_t variant = 0u; variant < type->variant_count; ++variant) {
            if (!append_format(buffer, "    case %u: view->member = ", variant) ||
                !emit_format_name(buffer, type->variants[variant].display_name) ||
                !append_format(buffer, "; view->children = %u; return 1;\n",
                    type->variants[variant].payload_count))
                return false;
        }
        if (!append_text(buffer, "    default: return 0; } }\n"))
            return false;
    }
    if (!append_text(buffer, "    default: return 0; }\n}\n"
        "static int xr_aot_format_child(const void *context, XrValueFormatNode node, "
        "uint32_t index, XrValueFormatNode *child) {\n"
        "    (void)context; (void)index; (void)child; if (!node.value) return 0;\n"
        "    switch (node.type) {\n"))
        return false;
    for (uint32_t index = 0u; index < ir->program->type_count; ++index) {
        const XrValidatedType *type = &ir->program->types[index];
        if (type->kind != XR_CORE_IR_TYPE_VARIANT)
            continue;
        if (!append_format(buffer,
            "    case %u: { const XrAotType%u *value = node.value; switch (value->tag) {\n",
            type->type_id, type->type_id))
            return false;
        for (uint32_t variant = 0u; variant < type->variant_count; ++variant) {
            if (!append_format(buffer, "    case %u: switch (index) {\n", variant))
                return false;
            for (uint32_t field = 0u; field < type->variants[variant].payload_count; ++field)
                if (!append_format(buffer,
                    "    case %u: child->type = %u; child->value = &value->payload.case_%u.f%u; "
                    "return 1;\n", field, type->variants[variant].payload_types[field], variant, field))
                    return false;
            if (!append_text(buffer, "    default: return 0; }\n"))
                return false;
        }
        if (!append_text(buffer, "    default: return 0; } }\n"))
            return false;
    }
    return append_text(buffer, "    default: return 0; }\n}\n");
}
