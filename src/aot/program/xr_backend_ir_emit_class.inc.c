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

static bool emit_class_drop_helpers(CBuffer *buffer, const XrBackendIR *ir) {
    for (uint32_t index = 0u; index < ir->program->type_count; ++index) {
        const XrValidatedType *type = &ir->program->types[index];
        if (type->kind == XR_CORE_IR_TYPE_CLASS_REFERENCE &&
            !append_format(buffer,
                           "static inline void xr_aot_class_drop_%u("
                           "XrAotContext *, XrAotType%u, uint32_t);\n",
                           type->type_id, type->type_id))
            return false;
    }
    if (!append_text(buffer, "\n"))
        return false;
    for (uint32_t index = 0u; index < ir->program->type_count; ++index) {
        const XrValidatedType *type = &ir->program->types[index];
        if (type->kind != XR_CORE_IR_TYPE_CLASS_REFERENCE)
            continue;
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
            if (type_is_class_reference(ir, field_type_id) &&
                !append_format(buffer,
                               "    xr_aot_class_drop_%u(xr_ctx, value->f%u, UINT32_C(2));\n",
                               field_type_id, field - 1u))
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
    }
    return true;
}

static bool emit_class_construct(CBuffer *buffer, const XrBackendInstruction *instruction) {
    char storage[32];
    const char *type = type_c_name(instruction->result_type_id, storage);
    char object_type[32];
    (void) snprintf(object_type, sizeof(object_type), "XrAotClass%u",
                    instruction->result_type_id);
    if (!type || !emit_allocation_alignment(buffer, object_type, "        ") ||
        !append_format(buffer,
                       "        v%u = (%s)xr_aot_alloc(xr_ctx, sizeof(*v%u));\n"
                       "        if (!v%u) return xr_aot_make(4, 0, 0);\n"
                       "        v%u->owners = UINT32_C(1);\n"
                       "        v%u->identity = ++xr_ctx->next_class_identity;\n",
                       instruction->result_id, type, instruction->result_id,
                       instruction->result_id, instruction->result_id,
                       instruction->result_id))
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

static bool emit_class_share(CBuffer *buffer, const XrBackendInstruction *instruction) {
    if (!append_format(buffer,
                       "        if (!v%u || v%u->owners == UINT32_MAX) "
                       "return xr_aot_make(4, 0, 0);\n"
                       "        ++v%u->owners;\n"
                       "        v%u = v%u;\n",
                       instruction->operands[0], instruction->operands[0],
                       instruction->operands[0], instruction->result_id,
                       instruction->operands[0]))
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
                                  const XrBackendFunction *function,
                                  const XrBackendInstruction *instruction) {
    uint32_t receiver = instruction->operands[0];
    uint16_t receiver_type_id = function->value_types[receiver];
    if (!type_is_class_reference(ir, receiver_type_id) ||
        !append_format(buffer,
                       "        if (!v%u || v%u->owners == UINT32_C(0)) "
                       "return xr_aot_make(4, 0, 0);\n"
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
                                   const XrBackendFunction *function,
                                   const XrBackendInstruction *instruction) {
    uint32_t receiver = instruction->operands[0];
    uint16_t receiver_type_id = function->value_types[receiver];
    if (!type_is_class_reference(ir, receiver_type_id) ||
        !append_format(buffer,
                       "        if (!v%u || v%u->owners == UINT32_C(0)) "
                       "return xr_aot_make(4, 0, 0);\n"
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
                                const XrBackendFunction *function,
                                const XrBackendInstruction *instruction) {
    uint32_t place = instruction->operands[0];
    uint32_t replacement = instruction->operands[1];
    if (!append_format(buffer,
                       "        v%u = *v%u;\n"
                       "        *v%u = v%u;\n",
                       instruction->result_id, place, place, replacement))
        return false;
    char identity[80];
    char related[80];
    const char *identity_expression = NULL;
    const char *related_expression = NULL;
    if (type_is_class_reference(ir, instruction->result_type_id)) {
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

static bool emit_class_owner_drop(CBuffer *buffer, const XrBackendFunction *function,
                                  const XrBackendInstruction *instruction) {
    uint32_t owner = instruction->operands[0];
    return append_format(buffer,
                         "        xr_aot_class_drop_%u(xr_ctx, v%u, UINT32_C(1));\n",
                         function->value_types[owner], owner);
}
