/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_emit_class.inc.c - Private generated-C class-reference lowering
 *
 * KEY CONCEPT: A Program class value is an opaque handle.  The generated
 * object, owner count, identity, lifecycle observer, and reclamation path are
 * private AOT details; no VM value or source-level by-value class layout leaks
 * into generated C.
 */

typedef struct XrGeneratedClassEvent {
    uint32_t kind;
    uint32_t origin;
    uint16_t type_id;
    uint32_t field_ordinal;
    const char *identity;
    const char *related_identity;
    const char *old_i64;
    const char *replacement_i64;
} XrGeneratedClassEvent;

static bool emit_class_lifecycle_abi(CBuffer *buffer) {
    return append_text(
        buffer,
        "typedef struct XrAotLifecycleEvent {\n"
        "    uint32_t kind;\n"
        "    uint32_t origin;\n"
        "    uint16_t type_id;\n"
        "    uint16_t reserved16;\n"
        "    uint32_t field_ordinal;\n"
        "    uint64_t identity;\n"
        "    uint64_t related_identity;\n"
        "    uint8_t has_i64_exchange;\n"
        "    int64_t old_i64;\n"
        "    int64_t replacement_i64;\n"
        "} XrAotLifecycleEvent;\n"
        "typedef void (*XrAotLifecycleEventHandler)(\n"
        "    void *context, const XrAotLifecycleEvent *event);\n\n");
}

static bool emit_generated_class_event(CBuffer *buffer,
                                       const XrGeneratedClassEvent *event) {
    const char *identity = event->identity ? event->identity : "UINT64_MAX";
    const char *related =
        event->related_identity ? event->related_identity : "UINT64_MAX";
    const char *old_i64 = event->old_i64 ? event->old_i64 : "INT64_C(0)";
    const char *replacement_i64 =
        event->replacement_i64 ? event->replacement_i64 : "INT64_C(0)";
    return append_format(
        buffer,
        "        xr_aot_lifecycle_emit(xr_ctx, (XrAotLifecycleEvent){"
        ".kind = UINT32_C(%u), .origin = UINT32_C(%u), .type_id = UINT16_C(%u), "
        ".field_ordinal = UINT32_C(%u), .identity = %s, .related_identity = %s, "
        ".has_i64_exchange = UINT8_C(%u), .old_i64 = %s, .replacement_i64 = %s});\n",
        event->kind, event->origin, event->type_id, event->field_ordinal, identity, related,
        event->old_i64 ? 1u : 0u, old_i64, replacement_i64);
}

static bool type_needs_owned_drop(const XrBackendIR *ir, uint16_t type_id) {
    const XrValidatedType *type = xr_validated_program_type(ir->program, type_id);
    /* Panic messages reuse the immutable string allocation owner. */
    if (!type)
        return type_id == XR_CORE_TYPE_STRING_BUILDER || type_id == XR_CORE_TYPE_STRING ||
               (type_id == XR_CORE_TYPE_PANIC_INFO && has_panic_messages(ir));
    if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL &&
        type->interface_use_kind == XR_CORE_IR_INTERFACE_EXISTENTIAL_REF)
        return false;
    return xr_validated_program_type_ownership(ir->program, type_id) ==
           XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
}

static bool has_affine_owner_drops(const XrBackendIR *ir) {
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        for (uint32_t slot = 0u; slot < ir->program->modules[module].slot_count; ++slot)
            if (type_needs_owned_drop(ir, ir->program->modules[module].slots[slot].type_id))
                return true;
    for (uint32_t function = 0u; function < ir->program->function_count; ++function) {
        const XrValidatedFunction *fn = &ir->program->functions[function];
        for (uint32_t value = 0u; value < fn->value_count; ++value)
            if (fn->value_ownerships[value] == XR_CORE_IR_OWNER &&
                type_needs_owned_drop(ir, fn->value_types[value]))
                return true;
    }
    return false;
}

static bool emit_owned_value_drop(CBuffer *buffer, const XrBackendIR *ir, uint16_t type_id,
                                  const char *value, const char *origin) {
    if (!type_needs_owned_drop(ir, type_id))
        return append_format(buffer, "    (void)(%s);\n", value);
    if (type_id == XR_CORE_TYPE_PANIC_INFO)
        return append_format(buffer, "    xr_aot_free(xr_ctx, (%s).message);\n", value);
    if (type_id == XR_CORE_TYPE_STRING_BUILDER)
        return append_format(buffer, "    xr_aot_builder_drop(xr_ctx, %s);\n", value);
    if (type_id == XR_CORE_TYPE_STRING)
        return append_format(buffer, "    xr_aot_free(xr_ctx, %s);\n", value);
    return append_format(buffer, "    xr_aot_%sdrop_%u(xr_ctx, %s, %s);\n",
                         type_is_reference_record(ir, type_id) ? "class_" : "", type_id, value,
                         origin);
}

static void require_drop_type(const XrBackendIR *ir, uint16_t type_id, bool *required,
                              uint32_t *queue, uint32_t *count) {
    const XrValidatedType *type = xr_validated_program_type(ir->program, type_id);
    if (!type || !type_needs_owned_drop(ir, type_id))
        return;
    uint32_t index = type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
    if (!required[index]) {
        required[index] = true;
        queue[(*count)++] = index;
    }
}

static void collect_owned_types(const XrBackendIR *ir, bool *required, uint32_t *queue,
                                bool instruction_drops) {
    uint32_t count = 0u;
    require_drop_type(ir, ir->program->functions[ir->program->entry_function].error_type_id,
                      required, queue, &count);
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        require_drop_type(ir, ir->program->functions[ir->program->modules[module].initializer]
                                 .error_type_id, required, queue, &count);
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        for (uint32_t slot = 0u; slot < ir->program->modules[module].slot_count; ++slot)
            require_drop_type(ir, ir->program->modules[module].slots[slot].type_id, required, queue,
                              &count);
    for (uint32_t f = 0u; f < ir->program->function_count; ++f) {
        const XrValidatedFunction *function = &ir->program->functions[f];
        if (instruction_drops)
            for (uint32_t value = 0u; value < function->value_count; ++value)
                if (function->value_ownerships[value] == XR_CORE_IR_OWNER)
                    require_drop_type(ir, function->value_types[value], required, queue, &count);
        for (uint32_t b = 0u; b < function->block_count; ++b) {
            const XrValidatedBlock *block = &function->blocks[b];
            for (uint32_t i = 0u; i < block->instruction_count; ++i) {
                const XrValidatedInstruction *op = &block->instructions[i];
                if (instruction_drops && op->operation_id == XR_CORE_OP_CORE_OWNER_DROP)
                    require_drop_type(ir, function->value_types[op->operands[0]], required, queue,
                                      &count);
                if (!instruction_drops && (op->operation_id == XR_CORE_OP_CORE_PLACE_INITIALIZE ||
                                           op->operation_id == XR_CORE_OP_CORE_PLACE_STORE ||
                                           op->operation_id == XR_CORE_OP_CORE_PLACE_EXCHANGE))
                    require_drop_type(ir, function->value_types[op->operands[1]], required, queue,
                                      &count);
            }
        }
    }
    for (uint32_t cursor = 0u; cursor < count; ++cursor) {
        const XrValidatedType *type = &ir->program->types[queue[cursor]];
        if (type->kind == XR_CORE_IR_TYPE_ARRAY)
            require_drop_type(ir, type->array_element_type, required, queue, &count);
        for (uint32_t field = 0u; field < type->field_count; ++field)
            require_drop_type(ir, type->field_types[field], required, queue, &count);
        for (uint32_t variant = 0u; variant < type->variant_count; ++variant) {
            const XrValidatedVariant *row = &type->variants[variant];
            for (uint32_t field = 0u; field < row->payload_count; ++field)
                require_drop_type(ir, row->payload_types[field], required, queue, &count);
        }
        if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
            for (uint32_t target = 0u; target < ir->program->function_count; ++target) {
                const XrValidatedFunction *function = &ir->program->functions[target];
                if (function->has_receiver && callable_type_can_target(ir, type->type_id, target))
                    require_drop_type(ir, function->parameter_types[0], required, queue, &count);
            }
        } else if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL) {
            for (uint32_t index = 0u; index < ir->program->conformance_count; ++index) {
                const XrValidatedConformance *row = &ir->program->conformances[index];
                if (row->interface_id == type->interface_id)
                    require_drop_type(ir, row->implementor_type_id, required, queue, &count);
            }
        }
    }
}

static bool emit_owned_payload_drop(CBuffer *buffer, const XrBackendIR *ir, uint16_t type_id,
                                    const char *payload) {
    char storage[32], value[96];
    const char *type = type_c_name(type_id, storage);
    if (!type)
        return false;
    (void) snprintf(value, sizeof(value), "*(%s *)value.%s", type, payload);
    return emit_owned_value_drop(buffer, ir, type_id, value, "origin");
}

static bool emit_compound_drop_helper(CBuffer *buffer, const XrBackendIR *ir,
                                      const XrValidatedType *type) {
    if (!append_format(buffer,
                       "static inline void xr_aot_drop_%u(XrAotContext *xr_ctx, "
                       "XrAotType%u value, uint32_t origin) {\n"
                       "    (void)xr_ctx; (void)origin; (void)value;\n",
                       type->type_id, type->type_id))
        return false;
    if (type->kind == XR_CORE_IR_TYPE_PROVIDER_RESOURCE) {
        if (!append_text(buffer, "    if (value.owner && value.release) value.release(&value.owner);\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_CHANNEL) {
        if (!append_text(buffer,
                "    if (value.storage && xr_channel_storage_release(value.storage) == XR_CHANNEL_STORAGE_LAST_OWNER) {\n"
                "        free(value.storage->slots); free(value.storage);\n"
                "    }\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_ATOMIC) {
        if (!append_text(buffer, "    if (value.storage && xr_atomic_storage_release_core(value.storage) == XR_ATOMIC_STORAGE_RELEASE_LAST) free(value.storage);\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
        for (uint32_t field = type->field_count; field != 0u; --field) {
            char value[32];
            (void) snprintf(value, sizeof(value), "value.f%u", field - 1u);
            if (!emit_owned_value_drop(buffer, ir, type->field_types[field - 1u], value, "origin"))
                return false;
        }
    } else if (type->kind == XR_CORE_IR_TYPE_ARRAY) {
        if (!append_text(buffer, "    if (!value.storage || --value.storage->owners != 0u) return;\n"
                                 "    for (size_t i = value.storage->length; i != 0u; --i) {\n") ||
            !emit_owned_value_drop(buffer, ir, type->array_element_type, "value.storage->data[i - 1u]", "origin") ||
            !append_text(buffer, "    }\n    xr_aot_free(xr_ctx, value.storage->data);\n"
                                   "    xr_aot_free(xr_ctx, value.storage);\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_VARIANT) {
        if (!append_text(buffer, "    switch (value.tag) {\n"))
            return false;
        for (uint32_t variant = 0u; variant < type->variant_count; ++variant) {
            const XrValidatedVariant *row = &type->variants[variant];
            if (!append_format(buffer, "    case UINT32_C(%u):\n", variant))
                return false;
            for (uint32_t field = row->payload_count; field != 0u; --field) {
                char value[64];
                (void) snprintf(value, sizeof(value), "value.payload.case_%u.f%u", variant,
                                field - 1u);
                if (!emit_owned_value_drop(buffer, ir, row->payload_types[field - 1u], value,
                                           "origin"))
                    return false;
            }
            if (!append_text(buffer, "        break;\n"))
                return false;
        }
        if (!append_text(buffer, "    default: break;\n    }\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_CALLABLE) {
        if (!append_text(buffer, "    if (!value.capture) return;\n"
                                 "    switch (value.function_id) {\n"))
            return false;
        for (uint32_t target = 0u; target < ir->program->function_count; ++target) {
            const XrValidatedFunction *function = &ir->program->functions[target];
            if (!function->has_receiver || !callable_type_can_target(ir, type->type_id, target))
                continue;
            if (!append_format(buffer, "    case UINT32_C(%u):\n", target) ||
                !emit_owned_payload_drop(buffer, ir, function->parameter_types[0], "capture") ||
                !append_text(buffer, "        break;\n"))
                return false;
        }
        if (!append_text(buffer, "    default: break;\n    }\n"
                                 "    xr_aot_free(xr_ctx, value.capture);\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_EXISTENTIAL) {
        if (!append_text(buffer, "    if (!value.data) return;\n"
                                 "    switch (value.conformance_id) {\n"))
            return false;
        for (uint32_t index = 0u; index < ir->program->conformance_count; ++index) {
            const XrValidatedConformance *row = &ir->program->conformances[index];
            if (row->interface_id != type->interface_id)
                continue;
            if (!append_format(buffer, "    case UINT32_C(%u):\n", index) ||
                !emit_owned_payload_drop(buffer, ir, row->implementor_type_id, "data") ||
                !append_text(buffer, "        break;\n"))
                return false;
        }
        if (!append_text(buffer, "    default: break;\n    }\n"
                                 "    xr_aot_free(xr_ctx, value.data);\n"))
            return false;
    } else {
        return false;
    }
    return append_text(buffer, "}\n\n");
}

static bool emit_class_drop_helper(CBuffer *buffer, const XrBackendIR *ir,
                                   const XrValidatedType *type);

static bool emit_owned_drop_helpers(CBuffer *buffer, const XrBackendIR *ir) {
    uint32_t count = ir->program->type_count;
    bool *required = xr_calloc(count ? count : 1u, sizeof(*required));
    uint32_t *queue = xr_calloc(count ? count : 1u, sizeof(*queue));
    if (!required || !queue) {
        xr_free(required);
        xr_free(queue);
        buffer->failed = true;
        return false;
    }
    collect_owned_types(ir, required, queue, true);
    bool emitted = true;
    for (uint32_t index = 0u; emitted && index < count; ++index) {
        const XrValidatedType *type = &ir->program->types[index];
        if (required[index])
            emitted = append_format(buffer,
                                    "static inline void xr_aot_%sdrop_%u("
                                    "XrAotContext *, XrAotType%u, uint32_t);\n",
                                    xr_program_type_kind_is_reference_record(type->kind) ? "class_" : "",
                                    type->type_id, type->type_id);
    }
    for (uint32_t index = 0u; emitted && index < count; ++index) {
        const XrValidatedType *type = &ir->program->types[index];
        if (required[index])
            emitted = xr_program_type_kind_is_reference_record(type->kind)
                          ? emit_class_drop_helper(buffer, ir, type)
                          : emit_compound_drop_helper(buffer, ir, type);
    }
    xr_free(queue);
    xr_free(required);
    return emitted;
}

static bool emit_class_drop_helper(CBuffer *buffer, const XrBackendIR *ir,
                                   const XrValidatedType *type) {
    if (!append_format(
            buffer,
            "static inline void xr_aot_class_drop_%u(XrAotContext *xr_ctx, "
            "XrAotType%u value, uint32_t origin) {\n"
            "    if (!xr_ctx || !value || value->owners == UINT32_C(0)) return;\n"
            "    xr_aot_lifecycle_emit(xr_ctx, (XrAotLifecycleEvent){"
            ".kind = UINT32_C(7), .origin = origin, .type_id = UINT16_C(%u), "
            ".field_ordinal = UINT32_MAX, .identity = value->identity, "
            ".related_identity = UINT64_MAX});\n"
            "    if (--value->owners != UINT32_C(0)) return;\n"
            "    xr_aot_lifecycle_emit(xr_ctx, (XrAotLifecycleEvent){"
            ".kind = UINT32_C(8), .origin = origin, .type_id = UINT16_C(%u), "
            ".field_ordinal = UINT32_MAX, .identity = value->identity, "
            ".related_identity = UINT64_MAX});\n",
            type->type_id, type->type_id, type->type_id, type->type_id))
        return false;
    for (uint32_t field = type->field_count; field != 0u; --field) {
        uint16_t field_type_id = type->field_types[field - 1u];
        char value[32];
        (void) snprintf(value, sizeof(value), "value->f%u", field - 1u);
        if (!emit_owned_value_drop(buffer, ir, field_type_id, value, "UINT32_C(2)"))
            return false;
    }
    if (!append_format(
            buffer,
            "    xr_aot_lifecycle_emit(xr_ctx, (XrAotLifecycleEvent){"
            ".kind = UINT32_C(9), .origin = origin, .type_id = UINT16_C(%u), "
            ".field_ordinal = UINT32_MAX, .identity = value->identity, "
            ".related_identity = UINT64_MAX});\n"
            "    xr_aot_free(xr_ctx, value);\n"
            "}\n\n",
            type->type_id))
        return false;
    return true;
}

static bool emit_class_construct(CBuffer *buffer, const XrValidatedInstruction *instruction) {
    char storage[32];
    const char *type = type_c_name(instruction->result_type_id, storage);
    char object_type[32];
    (void) snprintf(object_type, sizeof(object_type), "XrAotClass%u",
                    instruction->result_type_id);
    if (!type || !emit_allocation_alignment(buffer, object_type, "        ") ||
        !append_format(buffer,
                       "        v%u = (%s)xr_aot_alloc(xr_ctx, sizeof(*v%u));\n"
                       "        if (!v%u) XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                       "        v%u->owners = UINT32_C(1);\n"
                       "        v%u->identity = xr_aot_next_class_identity(xr_ctx);\n",
                       instruction->result_id, type, instruction->result_id, instruction->result_id,
                       instruction->result_id, instruction->result_id))
        return false;
    for (uint32_t field = 0u; field < instruction->operand_count; ++field)
        if (!append_format(buffer, "        v%u->f%u = v%u;\n", instruction->result_id, field,
                           instruction->operands[field]))
            return false;
    char identity[48];
    (void) snprintf(identity, sizeof(identity), "v%u->identity", instruction->result_id);
    XrGeneratedClassEvent event = {
        .kind = 1u,
        .origin = 1u,
        .type_id = instruction->result_type_id,
        .field_ordinal = UINT32_MAX,
        .identity = identity,
    };
    return emit_generated_class_event(buffer, &event);
}

static bool emit_owner_alias(CBuffer *buffer, const XrBackendIR *ir,
                              const XrValidatedInstruction *instruction) {
    const XrValidatedType *type = xr_validated_program_type(ir->program, instruction->result_type_id);
    if (type && type->kind == XR_CORE_IR_TYPE_ARRAY)
        return append_format(buffer,
                             "        if (!v%u.storage || v%u.storage->owners == UINT32_MAX) "
                             "XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                             "        ++v%u.storage->owners;\n"
                             "        v%u = v%u;\n",
                             instruction->operands[0], instruction->operands[0], instruction->operands[0],
                             instruction->result_id, instruction->operands[0]);
    if (!append_format(buffer,
                       "        if (!v%u || v%u->owners == UINT32_MAX) "
                       "XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                       "        ++v%u->owners;\n"
                       "        v%u = v%u;\n",
                       instruction->operands[0], instruction->operands[0], instruction->operands[0],
                       instruction->result_id, instruction->operands[0]))
        return false;
    char identity[48];
    (void) snprintf(identity, sizeof(identity), "v%u->identity", instruction->result_id);
    XrGeneratedClassEvent event = {
        .kind = 2u,
        .origin = 1u,
        .type_id = instruction->result_type_id,
        .field_ordinal = UINT32_MAX,
        .identity = identity,
        .related_identity = identity,
    };
    return emit_generated_class_event(buffer, &event);
}

static bool emit_class_field_load(CBuffer *buffer, const XrBackendIR *ir,
                                  const XrValidatedFunction *function,
                                  const XrValidatedInstruction *instruction) {
    uint32_t receiver = instruction->operands[0];
    uint16_t receiver_type_id = function->value_types[receiver];
    if (!type_is_reference_record(ir, receiver_type_id) ||
        !append_format(buffer,
                       "        if (!v%u || v%u->owners == UINT32_C(0)) "
                       "XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                       "        v%u = v%u->f%u;\n",
                       receiver, receiver, instruction->result_id, receiver,
                       instruction->immediate.field_ordinal))
        return false;
    char identity[48];
    (void) snprintf(identity, sizeof(identity), "v%u->identity", receiver);
    XrGeneratedClassEvent event = {
        .kind = 4u,
        .origin = 1u,
        .type_id = receiver_type_id,
        .field_ordinal = instruction->immediate.field_ordinal,
        .identity = identity,
    };
    return emit_generated_class_event(buffer, &event);
}

static bool emit_class_field_place(CBuffer *buffer, const XrBackendIR *ir,
                                   const XrValidatedFunction *function,
                                   const XrValidatedInstruction *instruction) {
    uint32_t receiver = instruction->operands[0];
    uint16_t receiver_type_id = function->value_types[receiver];
    if (!type_is_reference_record(ir, receiver_type_id) ||
        !append_format(buffer,
                       "        if (!v%u || v%u->owners == UINT32_C(0)) "
                       "XR_AOT_FAIL(xr_aot_make(4, 0, 0));\n"
                       "        v%u = &v%u->f%u;\n",
                       receiver, receiver, instruction->result_id, receiver,
                       instruction->immediate.field_ordinal))
        return false;
    char identity[48];
    (void) snprintf(identity, sizeof(identity), "v%u->identity", receiver);
    XrGeneratedClassEvent event = {
        .kind = 5u,
        .origin = 1u,
        .type_id = receiver_type_id,
        .field_ordinal = instruction->immediate.field_ordinal,
        .identity = identity,
    };
    return emit_generated_class_event(buffer, &event);
}

static bool emit_place_exchange(CBuffer *buffer, const XrBackendIR *ir,
                                const XrValidatedFunction *function,
                                const XrValidatedInstruction *instruction) {
    uint32_t place = instruction->operands[0];
    uint32_t replacement = instruction->operands[1];
    if (!append_format(buffer,
                       "        v%u = *v%u;\n"
                       "        *v%u = v%u;\n",
                       instruction->result_id, place, place, replacement))
        return false;
    if (!has_class_reference_types(ir))
        return true;
    char identity[80];
    char related[80];
    const char *identity_expression = NULL;
    const char *related_expression = NULL;
    if (type_is_reference_record(ir, instruction->result_type_id)) {
        (void) snprintf(identity, sizeof(identity),
                        "v%u ? v%u->identity : UINT64_MAX", instruction->result_id,
                        instruction->result_id);
        (void) snprintf(related, sizeof(related),
                        "v%u ? v%u->identity : UINT64_MAX", replacement, replacement);
        identity_expression = identity;
        related_expression = related;
    }
    char old_i64[32];
    char replacement_i64[32];
    const char *old_i64_expression = NULL;
    const char *replacement_i64_expression = NULL;
    if (instruction->result_type_id == XR_CORE_TYPE_I64) {
        (void) snprintf(old_i64, sizeof(old_i64), "v%u", instruction->result_id);
        (void) snprintf(replacement_i64, sizeof(replacement_i64), "v%u", replacement);
        old_i64_expression = old_i64;
        replacement_i64_expression = replacement_i64;
    }
    XrGeneratedClassEvent event = {
        .kind = 6u,
        .origin = 1u,
        .type_id = instruction->result_type_id,
        .field_ordinal = UINT32_MAX,
        .identity = identity_expression,
        .related_identity = related_expression,
        .old_i64 = old_i64_expression,
        .replacement_i64 = replacement_i64_expression,
    };
    (void) function;
    return emit_generated_class_event(buffer, &event);
}
