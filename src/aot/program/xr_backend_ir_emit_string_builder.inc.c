/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_emit_string_builder.inc.c - Owned mutable text realization
 */

static bool emit_string_builder_runtime(CBuffer *buffer, bool snapshot) {
    if (!append_text(buffer,
        "XR_TEXT_KERNEL_FUNCTION void *xr_aot_builder_allocate(void *context, size_t size) {\n"
        "    return xr_aot_alloc((XrAotContext *)context, size);\n"
        "}\n"
        "XR_TEXT_KERNEL_FUNCTION void xr_aot_builder_release(void *context, void *pointer) {\n"
        "    xr_aot_free((XrAotContext *)context, pointer);\n"
        "}\n"
        "XR_TEXT_KERNEL_FUNCTION XrStringBuilderStorage *xr_aot_builder_new(XrAotContext *context) {\n"
        "    XrStringBuilderStorage *value = (XrStringBuilderStorage *)xr_aot_alloc(context, sizeof(*value));\n"
        "    if (value) *value = (XrStringBuilderStorage){0};\n"
        "    return value;\n"
        "}\n"
        "XR_TEXT_KERNEL_FUNCTION void xr_aot_builder_drop(XrAotContext *context, XrStringBuilderStorage *value) {\n"
        "    if (!value) return;\n"
        "    XrStringBuilderAllocator allocator = {context, xr_aot_builder_allocate, xr_aot_builder_release};\n"
        "    xr_string_builder_dispose(value, &allocator);\n"
        "    xr_aot_free(context, value);\n"
        "}\n"
        "XR_TEXT_KERNEL_FUNCTION int xr_aot_builder_append(XrStringBuilderStorage *builder, const XrTextDisplayOperand *value) {\n"
        "    XrAotContext *owner = (((XrAotAllocation *)builder) - 1)->link.owner;\n"
        "    XrStringBuilderAllocator allocator = {owner, xr_aot_builder_allocate, xr_aot_builder_release};\n"
        "    size_t maximum = SIZE_MAX < (uint64_t)INT64_MAX ? SIZE_MAX : (size_t)INT64_MAX;\n"
        "    return xr_string_builder_append_value(builder, value, maximum, &allocator) == XR_STRING_BUILDER_OK;\n"
        "}\n\n"))
        return false;
    if (!snapshot)
        return true;
    return append_text(buffer,
        "typedef struct XrAotBuilderSnapshot { XrAotContext *context; XrAotString *value; } XrAotBuilderSnapshot;\n"
        "XR_TEXT_KERNEL_FUNCTION void *xr_aot_builder_snapshot_allocate(void *opaque, size_t size) {\n"
        "    XrAotBuilderSnapshot *snapshot = (XrAotBuilderSnapshot *)opaque;\n"
        "    snapshot->value = xr_aot_string_new(snapshot->context, size);\n"
        "    return snapshot->value ? snapshot->value->bytes : NULL;\n"
        "}\n"
        "XR_TEXT_KERNEL_FUNCTION void xr_aot_builder_snapshot_release(void *opaque, void *pointer) {\n"
        "    XrAotBuilderSnapshot *snapshot = (XrAotBuilderSnapshot *)opaque;\n"
        "    (void)pointer; xr_aot_free(snapshot->context, snapshot->value); snapshot->value = NULL;\n"
        "}\n"
        "XR_TEXT_KERNEL_FUNCTION XrAotString *xr_aot_builder_snapshot(XrAotContext *context, const XrStringBuilderStorage *builder) {\n"
        "    XrAotBuilderSnapshot snapshot = {context, NULL}; uint8_t *bytes = NULL;\n"
        "    XrStringBuilderAllocator allocator = {&snapshot, xr_aot_builder_snapshot_allocate, xr_aot_builder_snapshot_release};\n"
        "    if (xr_string_builder_snapshot(builder, SIZE_MAX, &allocator, &bytes) != XR_STRING_BUILDER_OK) return NULL;\n"
        "    snapshot.value->size = builder->size; snapshot.value->scalar_count = builder->scalar_count;\n"
        "    return snapshot.value;\n"
        "}\n\n");
}

static bool emit_string_builder_operation(CBuffer *buffer, const XrBackendIR *ir,
                                           const XrValidatedFunction *function,
                                           const XrValidatedInstruction *instruction) {
    switch (instruction->operation_id) {
        case XR_CORE_OP_CORE_STRING_BUILDER_CONSTRUCT:
            return append_format(buffer,
                "        v%u = xr_aot_builder_new(xr_ctx);\n"
                "        if (!v%u) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n",
                instruction->result_id, instruction->result_id);
        case XR_CORE_OP_CORE_STRING_BUILDER_APPEND: {
            if (!emit_module_place_check(buffer, ir, instruction->operands[0])) return false;
            uint16_t type = function->value_types[instruction->operands[1]];
            if (type == XR_CORE_TYPE_VOID)
                return append_format(buffer,
                    "        if (!xr_aot_builder_append(*v%u, NULL)) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n",
                    instruction->operands[0]);
            const char *kind = display_operand_kind(type);
            if (!kind || !append_format(buffer,
                "        { XrTextDisplayOperand value = {0}; value.kind = %s;\n", kind)) return false;
            uint32_t operand = instruction->operands[1];
            if (type == XR_CORE_TYPE_STRING) {
                if (!append_format(buffer, "          value.bytes = v%u->bytes; value.size = v%u->size;\n",
                                   operand, operand)) return false;
            } else {
                const char *field = type == XR_CORE_TYPE_I64 ? "i64" : type == XR_CORE_TYPE_BOOL ? "boolean" :
                                    type == XR_CORE_TYPE_F64 ? "f64" : "rune";
                if (!append_format(buffer, "          value.%s = v%u;\n", field, operand)) return false;
            }
            return append_format(buffer,
                "          if (!xr_aot_builder_append(*v%u, &value)) XR_AOT_FAIL(xr_aot_make(4, 0, 0)); }\n",
                instruction->operands[0]);
        }
        case XR_CORE_OP_CORE_STRING_BUILDER_CLEAR:
            return emit_module_place_check(buffer, ir, instruction->operands[0]) && append_format(buffer,
                "        if (!xr_string_builder_clear(*v%u)) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n",
                instruction->operands[0]);
        case XR_CORE_OP_CORE_STRING_BUILDER_LENGTH:
            return append_format(buffer, "        v%u = (int64_t)v%u->scalar_count;\n",
                                  instruction->result_id, instruction->operands[0]);
        case XR_CORE_OP_CORE_STRING_BUILDER_SNAPSHOT:
            return append_format(buffer,
                "        v%u = xr_aot_builder_snapshot(xr_ctx, v%u);\n"
                "        if (!v%u) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n",
                instruction->result_id, instruction->operands[0], instruction->result_id);
        default:
            return false;
    }
}
