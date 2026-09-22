/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_emit_module.inc.c - Typed instance slots and physical owner transfer
 */

static bool boundary_function(const XrBackendIR *ir, uint32_t function) {
    if (function == ir->program->entry_function)
        return true;
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        if (ir->program->modules[module].initializer == function)
            return true;
    return false;
}

static bool first_boundary_error_type(const XrBackendIR *ir, uint32_t function) {
    uint16_t type = ir->program->functions[function].error_type_id;
    if (type == XR_CORE_TYPE_VOID || !boundary_function(ir, function))
        return false;
    for (uint32_t prior = 0u; prior < function; ++prior)
        if (boundary_function(ir, prior) && ir->program->functions[prior].error_type_id == type)
            return false;
    return true;
}

static bool function_needs_failure_storage(const XrBackendIR *ir,
                                           const XrValidatedFunction *function) {
    return function->error_type_id != XR_CORE_TYPE_VOID ||
           (function->panic_type_id != XR_CORE_TYPE_VOID && has_panic_messages(ir));
}

static bool needs_module_storage(const XrBackendIR *ir) {
    if (module_slot_count(ir))
        return true;
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        if (function_needs_failure_storage(ir,
                &ir->program->functions[ir->program->modules[module].initializer]))
            return true;
    return false;
}

static bool needs_failure_storage(const XrBackendIR *ir) {
    return needs_module_storage(ir) ||
           function_needs_failure_storage(ir, &ir->program->functions[ir->program->entry_function]);
}

static bool emit_boundary_outputs(CBuffer *buffer, const XrBackendIR *ir,
                                  const XrValidatedFunction *function, bool declarations) {
    uint16_t types[] = {function->result_type_id, function->error_type_id, function->panic_type_id};
    const char *names[] = {"init_result", "init_error", "init_panic"};
    for (uint32_t output = 0u; output < XR_COUNTOF(types); ++output) {
        bool present = output == 0u ? xr_validated_program_type(ir->program, types[output]) != NULL
                                    : types[output] != XR_CORE_TYPE_VOID;
        if (!present)
            continue;
        char storage[32];
        const char *type = type_c_name(types[output], storage);
        if (!type ||
            !(declarations ? append_format(buffer, "        %s %s = {0};\n", type, names[output])
                           : append_format(buffer, ", &%s", names[output])))
            return false;
    }
    return true;
}

/* Publish an error into its boundary owner before clearing frame or module
 * storage. The owner preserves the typed graph through the observer lifetime. */
static bool emit_boundary_output_cleanup(CBuffer *buffer, const XrBackendIR *ir,
                                         const XrValidatedFunction *function, bool initializer) {
    if (function->panic_type_id == XR_CORE_TYPE_PANIC_INFO) {
        if (!append_text(buffer, "        if (result.kind == UINT32_C(3)) {\n"))
            return false;
        if (has_panic_messages(ir) && !append_format(buffer,
                "            XrAotFailure *failure = %s;\n"
                "            failure->panic = init_panic;\n"
                "            xr_aot_promote_%u(&failure->storage, init_panic);\n",
                initializer ? "&xr_ctx->modules->failure" : "xr_ctx->failure",
                XR_CORE_TYPE_PANIC_INFO))
            return false;
        if (!append_text(buffer,
                "            result.panic_present = UINT32_C(1);\n"
                "            result.panic_info = init_panic;\n"
                "        }\n"))
            return false;
    }
    uint16_t types[] = {function->error_type_id, function->panic_type_id};
    const char *names[] = {"init_error", "init_panic"};
    for (uint32_t output = 0u; output < XR_COUNTOF(types); ++output) {
        if (output == 0u && types[output] != XR_CORE_TYPE_VOID) {
            if (!append_format(buffer,
                "        if (result.kind == UINT32_C(2)) {\n"
                "            XrAotFailure *failure = %s;\n"
                "            failure->type = UINT16_C(%u);\n"
                "            failure->payload.e_%u = init_error;\n"
                "            result.error_type_id = UINT16_C(%u);\n"
                "            result.error_value = &failure->payload.e_%u;\n",
                initializer ? "&xr_ctx->modules->failure" : "xr_ctx->failure",
                types[output], types[output], types[output], types[output]))
                return false;
            if (has_class_reference_types(ir) &&
                !append_text(buffer,
                    "            failure->storage.lifecycle_context = xr_ctx->lifecycle_context;\n"
                    "            failure->storage.lifecycle_event = xr_ctx->lifecycle_event;\n"))
                return false;
            if (type_needs_owned_drop(ir, types[output]) &&
                !append_format(buffer,
                    "            xr_aot_promote_%u(&failure->storage, init_error);\n",
                    types[output]))
                return false;
            if (!append_text(buffer, "        }\n"))
                return false;
            continue;
        }
        if (output == 1u || !type_needs_owned_drop(ir, types[output]))
            continue;
        if (!append_format(buffer, "        if (result.kind == UINT32_C(%u)) {\n", output + 2u) ||
            !emit_owned_value_drop(buffer, ir, types[output], names[output], "UINT32_C(4)") ||
            !append_text(buffer, "        }\n"))
            return false;
    }
    return true;
}

static bool emit_module_value_promotion(CBuffer *buffer, const XrBackendIR *ir, uint16_t type,
                                        const char *value) {
    if (!type_needs_owned_drop(ir, type))
        return true;
    return append_format(buffer, "    xr_aot_promote_%u(destination, %s);\n", type, value);
}

static bool emit_module_promotion_helper(CBuffer *buffer, const XrBackendIR *ir,
                                         const XrValidatedType *type) {
    if (!append_format(buffer,
                       "void xr_aot_promote_%u(XrAotContext *destination, XrAotType%u value) {\n"
                       "    (void)destination; (void)value;\n",
                       type->type_id, type->type_id))
        return false;
    bool object = xr_program_type_kind_is_reference_record(type->kind);
    if (object &&
        !append_text(buffer, "    if (!xr_aot_transfer_allocation(destination, value)) return;\n"))
        return false;
    if (object || type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
        for (uint32_t field = 0u; field < type->field_count; ++field) {
            char value[48];
            (void) snprintf(value, sizeof(value), object ? "value->f%u" : "value.f%u", field);
            if (!emit_module_value_promotion(buffer, ir, type->field_types[field], value))
                return false;
        }
    } else if (type->kind == XR_CORE_IR_TYPE_ATOMIC ||
               type->kind == XR_CORE_IR_TYPE_PROVIDER_RESOURCE) {
        /* Atomic and provider storage are owned by their handles independently
         * of an allocation arena. Publishing transfers that same owner. */
    } else if (type->kind == XR_CORE_IR_TYPE_ARRAY) {
        if (!append_text(buffer,
                "    if (!xr_aot_transfer_allocation(destination, value.storage)) return;\n"
                "    (void)xr_aot_transfer_allocation(destination, value.storage->data);\n"
                "    for (size_t i = 0u; i < value.storage->length; ++i) {\n") ||
            !emit_module_value_promotion(buffer, ir, type->array_element_type, "value.storage->data[i]") ||
            !append_text(buffer, "    }\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_VARIANT) {
        if (!append_text(buffer, "    switch (value.tag) {\n"))
            return false;
        for (uint32_t variant = 0u; variant < type->variant_count; ++variant) {
            const XrValidatedVariant *row = &type->variants[variant];
            if (!append_format(buffer, "    case UINT32_C(%u):\n", variant))
                return false;
            for (uint32_t field = 0u; field < row->payload_count; ++field) {
                char value[64];
                (void) snprintf(value, sizeof(value), "value.payload.case_%u.f%u", variant, field);
                if (!emit_module_value_promotion(buffer, ir, row->payload_types[field], value))
                    return false;
            }
            if (!append_text(buffer, "        break;\n"))
                return false;
        }
        if (!append_text(buffer, "    default: break;\n    }\n"))
            return false;
    } else if (type->kind == XR_CORE_IR_TYPE_CALLABLE ||
               type->kind == XR_CORE_IR_TYPE_EXISTENTIAL) {
        bool callable = type->kind == XR_CORE_IR_TYPE_CALLABLE;
        const char *payload = callable ? "capture" : "data";
        if (!append_format(buffer,
                           "    if (!xr_aot_transfer_allocation(destination, value.%s)) return;\n"
                           "    switch (value.%s) {\n",
                           payload, callable ? "function_id" : "conformance_id"))
            return false;
        uint32_t count = callable ? ir->program->function_count : ir->program->conformance_count;
        for (uint32_t index = 0u; index < count; ++index) {
            uint16_t child;
            if (callable) {
                const XrValidatedFunction *function = &ir->program->functions[index];
                if (!function->has_receiver || !callable_type_can_target(ir, type->type_id, index))
                    continue;
                child = function->parameter_types[0];
            } else {
                const XrValidatedConformance *row = &ir->program->conformances[index];
                if (row->interface_id != type->interface_id)
                    continue;
                child = row->implementor_type_id;
            }
            char storage[32], value[96];
            const char *name = type_c_name(child, storage);
            if (!name || !append_format(buffer, "    case UINT32_C(%u):\n", index))
                return false;
            (void) snprintf(value, sizeof(value), "*(%s *)value.%s", name, payload);
            if (!emit_module_value_promotion(buffer, ir, child, value) ||
                !append_text(buffer, "        break;\n"))
                return false;
        }
        if (!append_text(buffer, "    default: break;\n    }\n"))
            return false;
    } else {
        return false;
    }
    return append_text(buffer, "}\n\n");
}

static bool emit_module_storage(CBuffer *buffer, const XrBackendIR *ir) {
    uint32_t slots = module_slot_count(ir);
    bool classes = has_class_reference_types(ir);
    if (!needs_failure_storage(ir))
        return !classes ||
               append_text(buffer,
                   "static inline uint64_t xr_aot_next_class_identity(XrAotContext *context) {\n"
                   "    return ++context->next_class_identity;\n"
                   "}\n\n");
    bool checked, wrapping, arena, output;
    scan_helpers(ir, &checked, &wrapping, &arena, &output);
    if (!append_text(buffer,
        "struct XrAotFailure {\n"
        "    XrAotContext storage;\n"
        "    uint16_t type;\n"
        "    XrAotPanicInfo panic;\n"
        "    union { uint8_t absent;\n"))
        return false;
    for (uint32_t function = 0u; function < ir->program->function_count; ++function) {
        if (!first_boundary_error_type(ir, function))
            continue;
        uint16_t id = ir->program->functions[function].error_type_id;
        char storage[32];
        const char *type = type_c_name(id, storage);
        if (!type || !append_format(buffer, "        %s e_%u;\n", type, id))
            return false;
    }
    if (!append_format(buffer,
        "    } payload;\n};\n\n"
        "struct XrAotModules {\n"
        "    XrAotContext storage;\n"
        "    XrAotFailure failure;\n"
        "    uint32_t publication_count;\n"
        "    uint32_t publication_order[%u];\n"
        "    uint8_t initialized[%u];\n", slots ? slots : 1u, slots ? slots : 1u))
        return false;
    if (classes && !append_text(buffer, "    atomic_uint_least64_t next_identity;\n"))
        return false;
    uint32_t ordinal = 0u;
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        for (uint32_t slot = 0u; slot < ir->program->modules[module].slot_count;
             ++slot, ++ordinal) {
            char storage[32];
            const char *type =
                type_c_name(ir->program->modules[module].slots[slot].type_id, storage);
            if (!type || !append_format(buffer, "    %s slot_%u;\n", type, ordinal))
                return false;
        }
    if (!append_text(buffer, "};\n\n"))
        return false;
    if (classes &&
        !append_text(buffer,
                     "static inline uint64_t xr_aot_next_class_identity(XrAotContext *context) {\n"
                     "    if (context->modules) return atomic_fetch_add_explicit(\n"
                     "        &context->modules->next_identity, UINT64_C(1), memory_order_relaxed) "
                     "+ UINT64_C(1);\n"
                     "    return ++context->next_class_identity;\n"
                     "}\n\n"))
        return false;
    if (!append_text(buffer,
                     "uint32_t xr_aot_module_slot(XrAotContext *context, const void *address) {\n"
                     "    if (!context || !context->modules || !address) return UINT32_MAX;\n"))
        return false;
    if (slots && !append_text(buffer,
                             "    XrAotModules *modules = context->modules;\n"
                             "    uintptr_t target = (uintptr_t)address;\n"))
        return false;
    for (uint32_t slot = 0u; slot < slots; ++slot)
        if (!append_format(buffer,
                           "    if (target >= (uintptr_t)&modules->slot_%u &&\n"
                           "        target - (uintptr_t)&modules->slot_%u < "
                           "sizeof(modules->slot_%u)) return UINT32_C(%u);\n",
                           slot, slot, slot, slot))
            return false;
    if (!append_text(
            buffer,
            "    return UINT32_MAX;\n"
            "}\n"
            "int xr_aot_module_place_initialized(XrAotContext *context, const void *address) {\n"
            "    uint32_t slot = xr_aot_module_slot(context, address);\n"
            "    return slot == UINT32_MAX || context->modules->initialized[slot];\n"
            "}\n"
            "int xr_aot_module_place_persistent(XrAotContext *context, const void *address) {\n"
            "    if (!context || !context->modules || !address) return 0;\n"
            "    if (xr_aot_module_slot(context, address) != UINT32_MAX) return 1;\n"))
        return false;
    if (arena &&
        !append_text(
            buffer,
            "    uintptr_t target = (uintptr_t)address;\n"
            "    for (XrAotAllocation *allocation = context->modules->storage.allocations; "
            "allocation;\n"
            "         allocation = allocation->link.next) {\n"
            "        uintptr_t payload = (uintptr_t)(allocation + 1);\n"
            "        if (target >= payload && target - payload < allocation->link.size) return 1;\n"
            "    }\n"))
        return false;
    if (!append_text(buffer, "    return 0;\n}\n\n"))
        return false;
    if (arena &&
        !append_text(buffer,
                     "int xr_aot_transfer_allocation(XrAotContext *destination, void *payload) {\n"
                     "    if (!payload) return 0;\n"
                     "    XrAotAllocation *allocation = (XrAotAllocation *)payload - 1;\n"
                     "    if (allocation->link.owner == destination) return 0;\n"
                     "    if (allocation->link.previous) allocation->link.previous->link.next = "
                     "allocation->link.next;\n"
                     "    else allocation->link.owner->allocations = allocation->link.next;\n"
                     "    if (allocation->link.next) allocation->link.next->link.previous = "
                     "allocation->link.previous;\n"
                     "    allocation->link.owner = destination;\n"
                     "    allocation->link.previous = NULL;\n"
                     "    allocation->link.next = destination->allocations;\n"
                     "    if (destination->allocations) destination->allocations->link.previous = "
                     "allocation;\n"
                     "    destination->allocations = allocation;\n"
                     "    return 1;\n"
                     "}\n\n"))
        return false;
    if (has_string_values(ir) &&
        !append_format(buffer,
                       "void xr_aot_promote_%u(XrAotContext *destination, XrAotString *value) {\n"
                       "    (void)xr_aot_transfer_allocation(destination, value);\n"
                       "}\n\n",
                       XR_CORE_TYPE_STRING))
        return false;
    if (has_panic_messages(ir) && !append_format(buffer,
            "void xr_aot_promote_%u(XrAotContext *destination, XrAotPanicInfo value) {\n"
            "    (void)xr_aot_transfer_allocation(destination, value.message);\n"
            "}\n\n", XR_CORE_TYPE_PANIC_INFO))
        return false;
    uint32_t count = ir->program->type_count;
    bool *required = xr_calloc(count ? count : 1u, sizeof(*required));
    uint32_t *queue = xr_calloc(count ? count : 1u, sizeof(*queue));
    if (!required || !queue) {
        xr_free(required);
        xr_free(queue);
        buffer->failed = true;
        return false;
    }
    collect_owned_types(ir, required, queue, false);
    bool emitted = true;
    for (uint32_t index = 0u; emitted && index < count; ++index)
        if (required[index])
            emitted =
                append_format(buffer, "void xr_aot_promote_%u(XrAotContext *, XrAotType%u);\n",
                              ir->program->types[index].type_id, ir->program->types[index].type_id);
    for (uint32_t index = 0u; emitted && index < count; ++index)
        if (required[index])
            emitted = emit_module_promotion_helper(buffer, ir, &ir->program->types[index]);
    xr_free(required);
    xr_free(queue);
    if (!emitted ||
        !append_text(
            buffer,
            "void xr_aot_modules_clear(XrAotModules *modules) {\n"
            "    if (!modules) return;\n"
            "    XrAotContext *xr_ctx = &modules->storage; (void)xr_ctx;\n"
            "    while (modules->publication_count) {\n"
            "        uint32_t slot = modules->publication_order[--modules->publication_count];\n"
            "        if (!modules->initialized[slot]) continue;\n"
            "        modules->initialized[slot] = 0;\n"
            "        switch (slot) {\n"))
        return false;
    ordinal = 0u;
    for (uint32_t module = 0u; module < ir->program->module_count; ++module)
        for (uint32_t slot = 0u; slot < ir->program->modules[module].slot_count;
             ++slot, ++ordinal) {
            char value[64];
            (void) snprintf(value, sizeof(value), "modules->slot_%u", ordinal);
            if (!append_format(buffer, "        case UINT32_C(%u):\n", ordinal) ||
                !emit_owned_value_drop(buffer, ir, ir->program->modules[module].slots[slot].type_id,
                                       value, "UINT32_C(4)") ||
                !append_text(buffer, "            break;\n"))
                return false;
        }
    if (!append_text(buffer, "        default: break;\n        }\n    }\n"))
        return false;
    if (arena && !append_text(buffer, "    xr_aot_context_destroy(xr_ctx);\n"))
        return false;
    if (!append_text(buffer,
        "}\n\n"
        "void xr_aot_failure_destroy(XrAotFailure *failure) {\n"
        "    if (!failure) return;\n"
        "    XrAotContext *xr_ctx = &failure->storage; (void)xr_ctx;\n"
        "    switch (failure->type) {\n"))
        return false;
    for (uint32_t function = 0u; function < ir->program->function_count; ++function) {
        if (!first_boundary_error_type(ir, function))
            continue;
        uint16_t id = ir->program->functions[function].error_type_id;
        char value[48];
        (void) snprintf(value, sizeof(value), "failure->payload.e_%u", id);
        if (!append_format(buffer, "    case UINT16_C(%u):\n", id) ||
            !emit_owned_value_drop(buffer, ir, id, value, "UINT32_C(4)") ||
            !append_text(buffer, "        break;\n"))
            return false;
    }
    if (!append_text(buffer, "    default: break;\n    }\n    failure->type = 0;\n"))
        return false;
    if (has_panic_messages(ir) && !append_text(buffer,
            "    xr_aot_free(xr_ctx, failure->panic.message);\n"
            "    failure->panic = (XrAotPanicInfo){0};\n"))
        return false;
    if (arena && !append_text(buffer, "    xr_aot_context_destroy(xr_ctx);\n"))
        return false;
    return append_text(buffer,
        "}\n\n"
        "void xr_aot_modules_destroy(XrAotModules *modules) {\n"
        "    if (!modules) return;\n"
        "    xr_aot_modules_clear(modules);\n"
        "    xr_aot_failure_destroy(&modules->failure);\n"
        "}\n\n");
}

static bool emit_module_place_check(CBuffer *buffer, const XrBackendIR *ir, uint32_t place) {
    return module_slot_count(ir) == 0u ||
           append_format(buffer,
                         "        if (!xr_aot_module_place_initialized(xr_ctx, v%u)) "
                         "XR_AOT_FAIL(xr_aot_make(1, 0, 8));\n",
                         place);
}

static bool emit_module_place_promotion(CBuffer *buffer, const XrBackendIR *ir, uint32_t place,
                                        uint32_t value, uint16_t type) {
    if (module_slot_count(ir) == 0u || !type_needs_owned_drop(ir, type))
        return true;
    return append_format(buffer,
                         "        if (xr_aot_module_place_persistent(xr_ctx, v%u))\n"
                         "            xr_aot_promote_%u(&xr_ctx->modules->storage, v%u);\n",
                         place, type, value);
}
