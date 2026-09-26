/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_emit_provider_typed.inc.c - Declared provider value projections
 */

static bool emit_provider_pack_value(CBuffer *buffer, const XrBackendIR *ir,
                                      uint16_t type_id, const char *value) {
    uint8_t token = type_id == XR_CORE_TYPE_VOID ? XR_PROVIDER_TYPE_UNIT :
                    type_id == XR_CORE_TYPE_BOOL ? XR_PROVIDER_TYPE_BOOL :
                    type_id == XR_CORE_TYPE_I64 ? XR_PROVIDER_TYPE_I64 :
                    type_id == XR_CORE_TYPE_STRING ? XR_PROVIDER_TYPE_BYTES : 0u;
    const XrValidatedType *type = xr_validated_program_type(ir->program, type_id);
    if (type) {
        token = type->kind == XR_CORE_IR_TYPE_PROVIDER_RESOURCE ? XR_PROVIDER_TYPE_RESOURCE :
                type->kind == XR_CORE_IR_TYPE_AGGREGATE ? XR_PROVIDER_TYPE_TUPLE :
                type->kind == XR_CORE_IR_TYPE_VARIANT ? XR_PROVIDER_TYPE_OPTIONAL : 0u;
    }
    if (!token || !append_format(buffer,
            "        { xr_node = &xr_arguments.nodes[xr_arguments.count++];\n"
            "          xr_node->token = UINT8_C(%u);\n", token))
        return false;
    if (token == XR_PROVIDER_TYPE_BOOL || token == XR_PROVIDER_TYPE_I64) {
        if (!append_format(buffer, "          xr_node->as.%s = %s;\n",
                           token == XR_PROVIDER_TYPE_BOOL ? "boolean" : "i64", value))
            return false;
    } else if (token == XR_PROVIDER_TYPE_BYTES) {
        if (!append_format(buffer, "          xr_node->as.bytes.data = (%s)->bytes;\n"
                                   "          xr_node->as.bytes.size = (%s)->size;\n", value, value))
            return false;
    } else if (token == XR_PROVIDER_TYPE_RESOURCE) {
        if (!append_text(buffer, "          xr_node->as.resource.id = (XrStableId){{"))
            return false;
        for (uint32_t i = 0u; i < XR_STABLE_ID_BYTES; ++i)
            if (!append_format(buffer, "%s%u", i ? "," : "", type->resource_id.bytes[i]))
                return false;
        if (!append_format(buffer, "}};\n          xr_node->as.resource.owner = (%s).owner;\n", value))
            return false;
    } else if (token == XR_PROVIDER_TYPE_OPTIONAL) {
        if (!append_format(buffer, "          xr_node->child_count = (uint8_t)(%s).tag;\n"
                                   "          if ((%s).tag == UINT32_C(1)) {\n", value, value))
            return false;
        char child[1024];
        int n = snprintf(child, sizeof(child), "(%s).payload.case_1.f0", value);
        if (n < 0 || (size_t)n >= sizeof(child) ||
            !emit_provider_pack_value(buffer, ir, type->variants[1].payload_types[0], child) ||
            !append_text(buffer, "          }\n"))
            return false;
    } else if (token == XR_PROVIDER_TYPE_TUPLE) {
        if (!append_format(buffer, "          xr_node->child_count = UINT8_C(%u);\n", type->field_count))
            return false;
        for (uint32_t i = 0u; i < type->field_count; ++i) {
            char child[1024];
            int n = snprintf(child, sizeof(child), "(%s).f%u", value, i);
            if (n < 0 || (size_t)n >= sizeof(child) ||
                !emit_provider_pack_value(buffer, ir, type->field_types[i], child))
                return false;
        }
    }
    return append_text(buffer, "        }\n");
}

static bool emit_provider_take_value(CBuffer *buffer, const XrBackendIR *ir,
                                      uint16_t type_id, const char *value) {
    uint8_t token = type_id == XR_CORE_TYPE_VOID ? XR_PROVIDER_TYPE_UNIT :
                    type_id == XR_CORE_TYPE_BOOL ? XR_PROVIDER_TYPE_BOOL :
                    type_id == XR_CORE_TYPE_I64 ? XR_PROVIDER_TYPE_I64 : 0u;
    const XrValidatedType *type = xr_validated_program_type(ir->program, type_id);
    if (type) {
        token = type->kind == XR_CORE_IR_TYPE_PROVIDER_RESOURCE ? XR_PROVIDER_TYPE_RESOURCE :
                type->kind == XR_CORE_IR_TYPE_AGGREGATE ? XR_PROVIDER_TYPE_TUPLE :
                type->kind == XR_CORE_IR_TYPE_VARIANT ? XR_PROVIDER_TYPE_OPTIONAL : 0u;
    }
    if (!token || !append_format(buffer,
            "        if (xr_ok && xr_offset < xr_result.count) {\n"
            "          xr_node = &xr_result.nodes[xr_offset++];\n"
            "          if (xr_node->token != UINT8_C(%u)) xr_ok = 0;\n", token))
        return false;
    if (token == XR_PROVIDER_TYPE_BOOL || token == XR_PROVIDER_TYPE_I64) {
        if (!append_format(buffer, "          if (xr_ok) %s = xr_node->as.%s;\n", value,
                           token == XR_PROVIDER_TYPE_BOOL ? "boolean" : "i64"))
            return false;
    } else if (token == XR_PROVIDER_TYPE_RESOURCE) {
        if (!append_format(buffer,
                "          if (!xr_node->as.resource.owner || !xr_ctx->resource_free) xr_ok = 0;\n"
                "          if (xr_ok) { (%s).owner = xr_node->as.resource.owner;\n"
                "            (%s).release = xr_ctx->resource_free; xr_node->as.resource.owner = NULL; }\n",
                value, value))
            return false;
    } else if (token == XR_PROVIDER_TYPE_OPTIONAL) {
        if (!append_format(buffer, "          if (xr_node->child_count > UINT8_C(1)) xr_ok = 0;\n"
                                   "          if (xr_ok) (%s).tag = xr_node->child_count;\n"
                                   "          if (xr_ok && (%s).tag == UINT32_C(1)) {\n", value, value))
            return false;
        char child[1024];
        int n = snprintf(child, sizeof(child), "(%s).payload.case_1.f0", value);
        if (n < 0 || (size_t)n >= sizeof(child) ||
            !emit_provider_take_value(buffer, ir, type->variants[1].payload_types[0], child) ||
            !append_text(buffer, "          }\n"))
            return false;
    } else if (token == XR_PROVIDER_TYPE_TUPLE) {
        if (!append_format(buffer, "          if (xr_node->child_count != UINT8_C(%u)) xr_ok = 0;\n", type->field_count))
            return false;
        for (uint32_t i = 0u; i < type->field_count; ++i) {
            char child[1024];
            int n = snprintf(child, sizeof(child), "(%s).f%u", value, i);
            if (n < 0 || (size_t)n >= sizeof(child) ||
                !emit_provider_take_value(buffer, ir, type->field_types[i], child))
                return false;
        }
    }
    return append_text(buffer, "        } else xr_ok = 0;\n");
}

static bool emit_provider_borrow_cleanup(CBuffer *buffer, uint32_t count) {
    return count == 0u || append_format(buffer,
        "        for (uint32_t xr_i = 0; xr_i < UINT32_C(%u); ++xr_i) xr_aot_free(xr_ctx, xr_borrow[xr_i]);\n",
        count);
}

static bool emit_provider_byte_borrow(CBuffer *buffer, const XrValidatedFunction *function,
                                      const XrValidatedInstruction *instruction,
                                      uint32_t parameter, uint32_t operands, uint32_t function_id) {
    uint32_t value = instruction->operands[parameter];
    if (!append_format(buffer,
            "        if (!v%u || !v%u->storage || !v%u->storage->owners ||\n"
            "            (v%u->storage->length && !v%u->storage->data)) {\n",
            value, value, value, value, value) ||
        !emit_provider_borrow_cleanup(buffer, operands) ||
        !emit_provider_failure(buffer, function, instruction, operands, function_id) ||
        !append_format(buffer,
            "        }\n"
            "        xr_borrow_size[%u] = v%u->storage->length;\n"
            "        xr_borrow_dest[%u] = v%u->storage->data;\n"
            "        if (xr_borrow_size[%u]) {\n"
            "          xr_borrow[%u] = (uint8_t *)xr_aot_alloc(xr_ctx, xr_borrow_size[%u]);\n"
            "          if (!xr_borrow[%u]) {\n",
            parameter, value, parameter, value, parameter, parameter, parameter, parameter) ||
        !emit_provider_borrow_cleanup(buffer, operands) ||
        !append_format(buffer,
            "            XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n          }\n"
            "          memcpy(xr_borrow[%u], xr_borrow_dest[%u], xr_borrow_size[%u]);\n"
            "        }\n"
            "        xr_node = &xr_arguments.nodes[xr_arguments.count++];\n"
            "        xr_node->token = UINT8_C(%u);\n"
            "        xr_node->as.u8_array.data = xr_borrow[%u];\n"
            "        xr_node->as.u8_array.size = xr_borrow_size[%u];\n",
            parameter, parameter, parameter, XR_PROVIDER_TYPE_U8_ARRAY, parameter, parameter))
        return false;
    return true;
}

static bool emit_typed_provider_call(CBuffer *buffer, const XrBackendIR *ir,
                                      const XrValidatedFunction *function,
                                      const XrValidatedInstruction *instruction,
                                      uint32_t operands, uint32_t function_id) {
    const XrProviderLogicalContract *logical = &ir->program->provider_requirements[
        instruction->immediate.provider_operation.requirement_index].operations[
        instruction->immediate.provider_operation.operation_index].logical_contract;
    if (operands > XR_PROVIDER_LOGICAL_MAX_PARAMETERS || operands != logical->parameter_count)
        return false;
    uint32_t borrows = 0u;
    for (uint32_t i = 0u; i < operands; ++i) {
        if (logical->parameter_modes[i] == XR_PROVIDER_MODE_REF) {
            const XrValidatedType *type = xr_validated_program_type(
                ir->program, function->value_types[instruction->operands[i]]);
            if (!type || type->kind != XR_CORE_IR_TYPE_ARRAY || type->array_element_type != XR_CORE_TYPE_U8 ||
                function->value_categories[instruction->operands[i]] != XR_CORE_IR_PLACE)
                return false;
            borrows = operands;
        }
    }
    if (!append_text(buffer, "    {\n        XrProviderValuePack xr_arguments = {0}, xr_result = {0};\n"
                             "        XrProviderValueNode *xr_node;\n"
                             "        uint32_t xr_offset = 0;\n"
                             "        int xr_ok = 1;\n"))
        return false;
    bool result = instruction->result_type_id != XR_CORE_TYPE_VOID;
    if (borrows && !append_format(buffer,
            "        uint8_t *xr_borrow[%u] = {0}, *xr_borrow_dest[%u] = {0};\n"
            "        size_t xr_borrow_size[%u] = {0};\n", borrows, borrows, borrows))
        return false;
    if (result) {
        char storage[32];
        const char *name = type_c_name(instruction->result_type_id, storage);
        if (!name || !append_format(buffer, "        %s xr_value = {0};\n", name))
            return false;
    }
    if (!append_text(buffer, "        if (!xr_ctx->provider_call_typed || !xr_ctx->provider_dispose_typed) ") ||
        !emit_provider_failure(buffer, function, instruction, operands, function_id))
        return false;
    for (uint32_t i = 0u; i < operands; ++i) {
        if (logical->parameter_modes[i] == XR_PROVIDER_MODE_REF) {
            if (!emit_provider_byte_borrow(buffer, function, instruction, i, operands, function_id))
                return false;
            continue;
        }
        char value[32];
        snprintf(value, sizeof(value), "v%u", instruction->operands[i]);
        if (!emit_provider_pack_value(buffer, ir, function->value_types[instruction->operands[i]], value))
            return false;
    }
    if (!append_format(buffer,
            "        int xr_status = xr_ctx->provider_call_typed(xr_ctx->provider_context, UINT32_C(%u), UINT32_C(%u), &xr_arguments, &xr_result);\n"
            "        if (xr_status != 0) {\n",
            instruction->immediate.provider_operation.requirement_index,
            instruction->immediate.provider_operation.operation_index) ||
        !emit_provider_borrow_cleanup(buffer, borrows) ||
        !append_format(buffer,
            "        xr_ctx->provider_dispose_typed(&xr_result);\n"
            "        if (xr_status == %u) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
            "        ",
            XR_BACKEND_NATIVE_PROVIDER_RESOURCE_LIMIT) ||
        !emit_provider_failure(buffer, function, instruction, operands, function_id) ||
        !append_text(buffer, "        }\n        if (xr_result.count > XR_PROVIDER_VALUE_MAX_NODES) xr_ok = 0;\n") ||
        !emit_provider_take_value(buffer, ir, instruction->result_type_id, "xr_value") ||
        !append_text(buffer, "        xr_ok = xr_ok && xr_offset == xr_result.count;\n"
                             "        xr_ctx->provider_dispose_typed(&xr_result);\n"
                             "        if (!xr_ok) {\n"))
        return false;
    if (result && !emit_owned_value_drop(buffer, ir, instruction->result_type_id, "xr_value", "UINT32_C(4)"))
        return false;
    if (!emit_provider_borrow_cleanup(buffer, borrows) ||
        !emit_provider_failure(buffer, function, instruction, operands, function_id) ||
        !append_text(buffer, "        }\n"))
        return false;
    if (borrows && (!append_format(buffer,
            "        for (uint32_t xr_i = 0; xr_i < UINT32_C(%u); ++xr_i) {\n"
            "          if (xr_borrow_size[xr_i]) memcpy(xr_borrow_dest[xr_i], xr_borrow[xr_i], xr_borrow_size[xr_i]);\n"
            "        }\n", borrows) || !emit_provider_borrow_cleanup(buffer, borrows)))
        return false;
    if (result && !append_format(buffer, "        v%u = xr_value;\n", instruction->result_id))
        return false;
    return append_text(buffer, "    }\n");
}
