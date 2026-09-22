/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_emit_copy.inc.c - Typed recursive ownership materialization
 *
 * KEY CONCEPT:
 *   One helper per logical type preserves independent owning captures without
 *   recursively expanding a callable's potentially cyclic target-type graph.
 */

static bool needs_copy_helper(const XrValidatedType *type) {
    return type && type->copy_contract == XR_CORE_IR_COPY_EXPLICIT;
}

static bool emit_copy_field(CBuffer *buffer, const XrBackendIR *ir, uint16_t type_id,
                            const char *field, const char *failure) {
    const XrValidatedType *type = xr_validated_program_type(ir->program, type_id);
    if (type_id == XR_CORE_TYPE_STRING)
        return append_format(buffer,
                             "    copy.%s = xr_aot_string_from_bytes(xr_ctx, source->%s->bytes, "
                             "source->%s->size);\n"
                             "    if (!copy.%s) goto %s;\n",
                             field, field, field, field, failure);
    if (!needs_copy_helper(type))
        return true;
    return append_format(buffer,
                         "    if (!xr_aot_copy_%u(xr_ctx, &source->%s, &copy.%s)) goto %s;\n",
                         type_id, field, field, failure);
}

static bool copy_field_can_fail(const XrBackendIR *ir, uint16_t type_id) {
    return type_id == XR_CORE_TYPE_STRING ||
           needs_copy_helper(xr_validated_program_type(ir->program, type_id));
}

/* Each failure label releases only the completed prefix. A child helper owns
 * its own partial copy, and the failed output never aliases a published owner. */
static bool emit_copy_fields(CBuffer *buffer, const XrBackendIR *ir, const uint16_t *types,
                             uint32_t count, const char *prefix, uint32_t group) {
    uint32_t fallible = 0u;
    for (uint32_t index = 0u; index < count; ++index) {
        char field[64], failure[64];
        (void) snprintf(field, sizeof(field), "%sf%u", prefix, index);
        (void) snprintf(failure, sizeof(failure), "xr_copy_fail_%u_%u", group, index);
        if (!emit_copy_field(buffer, ir, types[index], field, failure))
            return false;
        fallible += copy_field_can_fail(ir, types[index]);
    }
    if (!fallible)
        return true;
    if (!append_format(buffer, "    goto xr_copy_done_%u;\n", group))
        return false;
    bool failed_suffix = false;
    for (uint32_t index = count; index != 0u; --index) {
        uint32_t field = index - 1u;
        if (!copy_field_can_fail(ir, types[field]))
            continue;
        if (failed_suffix) {
            char value[80];
            (void) snprintf(value, sizeof(value), "copy.%sf%u", prefix, field);
            if (!emit_owned_value_drop(buffer, ir, types[field], value, "UINT32_MAX"))
                return false;
        }
        if (!append_format(buffer, "xr_copy_fail_%u_%u:\n", group, field))
            return false;
        failed_suffix = true;
    }
    return append_format(buffer, "    return 0;\nxr_copy_done_%u:\n    ;\n", group);
}

static bool emit_copy_callable(CBuffer *buffer, const XrBackendIR *ir,
                               const XrValidatedType *type) {
    if (!append_text(buffer, "    switch (source->function_id) {\n"))
        return false;
    for (uint32_t target_id = 0u; target_id < ir->program->function_count; ++target_id) {
        if (!callable_type_can_target(ir, type->type_id, target_id))
            continue;
        const XrValidatedFunction *target = &ir->program->functions[target_id];
        if (!append_format(buffer, "        case UINT32_C(%u):\n", target_id))
            return false;
        if (!target->has_receiver) {
            if (!append_text(buffer, "            copy.capture = NULL;\n"
                                     "            break;\n"))
                return false;
            continue;
        }
        uint16_t capture_id = target->parameter_types[0];
        const XrValidatedType *capture = xr_validated_program_type(ir->program, capture_id);
        char storage[32];
        const char *name = type_c_name(capture_id, storage);
        if (!name || !needs_copy_helper(capture) ||
            !append_text(buffer, "        {\n"
                                 "            if (!source->capture) return 0;\n") ||
            !emit_allocation_alignment(buffer, name, "            ") ||
            !append_format(buffer,
                           "            %s *capture = (%s *)xr_aot_alloc(xr_ctx, sizeof(%s));\n"
                           "            if (!capture) return 0;\n"
                           "            if (!xr_aot_copy_%u(xr_ctx, (const %s *)source->capture, "
                           "capture)) { xr_aot_free(xr_ctx, capture); return 0; }\n"
                           "            copy.capture = (void *)capture;\n"
                           "            break;\n"
                           "        }\n",
                           name, name, name, capture_id, name))
            return false;
    }
    return append_text(buffer, "        default: return 0;\n"
                               "    }\n");
}

static bool emit_copy_array(CBuffer *buffer, const XrBackendIR *ir, const XrValidatedType *type) {
    char storage[32];
    const char *element = type_c_name(type->array_element_type, storage);
    bool fallible = copy_field_can_fail(ir, type->array_element_type);
    if (!element || !emit_allocation_alignment(buffer, element, "    ") ||
        !append_format(buffer,
            "    copy.storage = (struct XrAotArrayStorage%u *)xr_aot_alloc(xr_ctx, sizeof(*copy.storage));\n"
            "    if (!copy.storage) return 0;\n"
            "    copy.storage->owners = 1u;\n"
            "    copy.storage->length = source->storage->length;\n"
            "    copy.storage->data = NULL;\n"
            "    if (source->storage->length > SIZE_MAX / sizeof(%s)) { xr_aot_free(xr_ctx, copy.storage); return 0; }\n"
            "    if (source->storage->length) {\n"
            "        copy.storage->data = (%s *)xr_aot_alloc(xr_ctx, source->storage->length * sizeof(%s));\n"
            "        if (!copy.storage->data) { xr_aot_free(xr_ctx, copy.storage); return 0; }\n"
            "    }\n"
            "    size_t copied = 0u;\n"
            "    for (; copied < source->storage->length; ++copied) {\n"
            "        copy.storage->data[copied] = source->storage->data[copied];\n",
            type->type_id, element, element, element) ||
        !emit_copy_field(buffer, ir, type->array_element_type, "storage->data[copied]", "xr_array_copy_fail") ||
        !append_text(buffer, "    }\n"))
        return false;
    if (!fallible)
        return true;
    return append_text(buffer,
               "    goto xr_array_copy_done;\nxr_array_copy_fail:\n"
               "    while (copied) {\n        --copied;\n") &&
           emit_owned_value_drop(buffer, ir, type->array_element_type, "copy.storage->data[copied]", "UINT32_MAX") &&
           append_text(buffer, "    }\n    xr_aot_free(xr_ctx, copy.storage->data);\n    xr_aot_free(xr_ctx, copy.storage);\n    return 0;\nxr_array_copy_done:\n    ;\n");
}

static bool emit_copy_type(CBuffer *buffer, const XrBackendIR *ir, const XrValidatedType *type) {
    if (!append_format(buffer,
                       "static inline int xr_aot_copy_%u(XrAotContext *xr_ctx, "
                       "const XrAotType%u *source, XrAotType%u *result) {\n"
                       "    (void)xr_ctx;\n"
                       "    XrAotType%u copy = *source;\n",
                       type->type_id, type->type_id, type->type_id, type->type_id))
        return false;
    if (type->kind == XR_CORE_IR_TYPE_ATOMIC) {
        if (!append_text(buffer, "    if (!source->storage || !xr_atomic_storage_retain_core(source->storage)) return 0;\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_ARRAY) {
        if (!emit_copy_array(buffer, ir, type))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
        if (!emit_copy_callable(buffer, ir, type))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
        if (!emit_copy_fields(buffer, ir, type->field_types, type->field_count, "", 0u))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_VARIANT) {
        if (!append_text(buffer, "    switch (source->tag) {\n"))
            return false;
        for (uint32_t index = 0u; index < type->variant_count; ++index) {
            const XrValidatedVariant *variant = &type->variants[index];
            if (!append_format(buffer, "        case UINT32_C(%u):\n", index))
                return false;
            char prefix[48];
            (void) snprintf(prefix, sizeof(prefix), "payload.case_%u.", index);
            if (!emit_copy_fields(buffer, ir, variant->payload_types, variant->payload_count,
                                  prefix, index))
                return false;
            if (!append_text(buffer, "            break;\n"))
                return false;
        }
        if (!append_text(buffer, "        default: return 0;\n"
                                 "    }\n"))
            return false;
    } else {
        return false;
    }
    return append_text(buffer, "    *result = copy;\n"
                               "    return 1;\n"
                               "}\n\n");
}

static void require_copy_type(const XrBackendIR *ir, uint16_t type_id, uint8_t *required,
                              uint32_t *queue, uint32_t *count) {
    const XrValidatedType *type = xr_validated_program_type(ir->program, type_id);
    if (!needs_copy_helper(type))
        return;
    uint32_t index = type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
    if (!required[index]) {
        required[index] = 1u;
        queue[(*count)++] = index;
    }
}

static void collect_copy_types(const XrBackendIR *ir, uint8_t *required, uint32_t *queue) {
    uint32_t count = 0u;
    for (uint32_t f = 0u; f < ir->program->function_count; ++f) {
        const XrValidatedFunction *function = &ir->program->functions[f];
        for (uint32_t b = 0u; b < function->block_count; ++b) {
            const XrValidatedBlock *block = &function->blocks[b];
            for (uint32_t i = 0u; i < block->instruction_count; ++i) {
                const XrValidatedInstruction *instruction = &block->instructions[i];
                if (instruction->operation_id == XR_CORE_OP_CORE_OWNER_COPY)
                    require_copy_type(ir, instruction->result_type_id, required, queue, &count);
            }
        }
    }
    for (uint32_t cursor = 0u; cursor < count; ++cursor) {
        const XrValidatedType *type = &ir->program->types[queue[cursor]];
        if (type->kind == XR_CORE_IR_TYPE_ARRAY)
            require_copy_type(ir, type->array_element_type, required, queue, &count);
        for (uint32_t field = 0u; field < type->field_count; ++field)
            require_copy_type(ir, type->field_types[field], required, queue, &count);
        for (uint32_t index = 0u; index < type->variant_count; ++index) {
            const XrValidatedVariant *variant = &type->variants[index];
            for (uint32_t field = 0u; field < variant->payload_count; ++field)
                require_copy_type(ir, variant->payload_types[field], required, queue, &count);
        }
        if (type->kind != XR_CORE_IR_TYPE_CALLABLE)
            continue;
        for (uint32_t f = 0u; f < ir->program->function_count; ++f) {
            const XrValidatedFunction *target = &ir->program->functions[f];
            if (target->has_receiver && callable_type_can_target(ir, type->type_id, f))
                require_copy_type(ir, target->parameter_types[0], required, queue, &count);
        }
    }
}

static bool emit_required_copy_helpers(CBuffer *buffer, const XrBackendIR *ir,
                                       const uint8_t *required) {
    /* Callable target graphs may refer back through another capture. Declare
     * the complete typed namespace before emitting any recursive body. */
    for (uint32_t index = 0u; index < ir->program->type_count; ++index) {
        const XrValidatedType *type = &ir->program->types[index];
        if (required[index] &&
            !append_format(buffer,
                           "static inline int xr_aot_copy_%u(XrAotContext *xr_ctx, "
                           "const XrAotType%u *source, XrAotType%u *result);\n",
                           type->type_id, type->type_id, type->type_id))
            return false;
    }
    for (uint32_t index = 0u; index < ir->program->type_count; ++index) {
        const XrValidatedType *type = &ir->program->types[index];
        if (required[index] && !emit_copy_type(buffer, ir, type))
            return false;
    }
    return true;
}

static bool emit_copy_helpers(CBuffer *buffer, const XrBackendIR *ir) {
    uint32_t count = ir->program->type_count;
    if (count == 0u)
        return true;
    uint8_t *required = xr_calloc(count, sizeof(*required));
    uint32_t *queue = xr_calloc(count, sizeof(*queue));
    if (!required || !queue) {
        xr_free(required);
        xr_free(queue);
        buffer->failed = true;
        return false;
    }
    collect_copy_types(ir, required, queue);
    bool emitted = emit_required_copy_helpers(buffer, ir, required);
    xr_free(queue);
    xr_free(required);
    return emitted;
}

static bool emit_owner_copy(CBuffer *buffer, const XrBackendIR *ir,
                            const XrValidatedInstruction *instruction) {
    const XrValidatedType *type =
        xr_validated_program_type(ir->program, instruction->result_type_id);
    if (instruction->result_type_id == XR_CORE_TYPE_STRING)
        return append_format(buffer,
                             "        v%u = xr_aot_string_from_bytes(xr_ctx, v%u->bytes, "
                             "v%u->size);\n"
                             "        if (!v%u) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n",
                             instruction->result_id, instruction->operands[0],
                             instruction->operands[0], instruction->result_id);
    if (!needs_copy_helper(type))
        return append_format(buffer, "        v%u = v%u;\n", instruction->result_id,
                             instruction->operands[0]);
    return append_format(buffer,
                         "        if (!xr_aot_copy_%u(xr_ctx, &v%u, &v%u)) "
                         "XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n",
                         type->type_id, instruction->operands[0], instruction->result_id);
}
