/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_program_storage.inc.c - Physical bounds for owned Program metadata
 *
 * KEY CONCEPT:
 *   Readable canonical storage is a precondition, never semantic authority.
 */
static XrXirStatus effect_program_range(const XrXirCompileContext *work,
    const void *memory, size_t count, size_t element, size_t alignment) {
    if (!count) return XR_XIR_OK;
    if ((uintptr_t)memory % alignment) return XR_XIR_BAD_STRUCTURE;
    if (count > SIZE_MAX / element) return XR_XIR_BUDGET;
    XrCompileResourceStatus status = xr_compile_resources_extent(work->resources, memory, count * element);
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
}

#define PROGRAM_STORAGE(pointer, count, type) do { \
    XrXirStatus range_status = effect_program_range(work, pointer, count, sizeof(type), _Alignof(type)); \
    if (range_status != XR_XIR_OK) return range_status; \
} while (0)

static XrXirStatus effect_program_types_storage(const XrXirCompileContext *work,
    const XrXirTypes *types) {
    PROGRAM_STORAGE(types, 1, XrXirTypes);
    if (types->interfaces) return XR_XIR_BAD_STRUCTURE;
    PROGRAM_STORAGE(types->nodes, types->count, XrXirTypeNode);
    for (uint32_t t = 0; t < types->count; ++t) {
        if (!xir_compile_work(work, 4)) return XR_XIR_BUDGET;
        const XrXirTypeNode *node = &types->nodes[t];
        PROGRAM_STORAGE(node->parameters, node->parameter_count, XrXirCallableParameter);
        PROGRAM_STORAGE(node->nominal.arguments, node->nominal.argument_count, XrXirType);
        PROGRAM_STORAGE(node->nominal.fields, node->nominal.field_count, XrXirType);
    }
    if (!types->nominals) return XR_XIR_OK;
    PROGRAM_STORAGE(types->nominals, 1, XrXirNominalTable);
    const XrXirNominalTable *nominals = types->nominals;
    if (nominals->declarations) return XR_XIR_BAD_STRUCTURE;
    PROGRAM_STORAGE(nominals->identities, nominals->count, XrXirNominalIdentity);
    for (uint32_t n = 0; n < nominals->count; ++n) {
        if (!xir_compile_work(work, 4)) return XR_XIR_BUDGET;
        const XrXirNominalIdentity *identity = &nominals->identities[n];
        PROGRAM_STORAGE(identity->module.bytes, identity->module.length, uint8_t);
        PROGRAM_STORAGE(identity->name.bytes, identity->name.length, uint8_t);
        PROGRAM_STORAGE(identity->variants, identity->variant_count, XrXirNominalVariant);
        PROGRAM_STORAGE(identity->fields, identity->field_count, XrXirNominalField);
        for (uint32_t v = 0; v < identity->variant_count; ++v) {
            if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
            PROGRAM_STORAGE(identity->variants[v].name.bytes, identity->variants[v].name.length, uint8_t);
        }
        for (uint32_t f = 0; f < identity->field_count; ++f) {
            if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
            PROGRAM_STORAGE(identity->fields[f].name.bytes, identity->fields[f].name.length, uint8_t);
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_program_declarations_storage(const XrXirCompileContext *work,
    const XrXirDeclarations *declarations, uint32_t functions) {
    PROGRAM_STORAGE(declarations, 1, XrXirDeclarations);
    if (declarations->implementations) return XR_XIR_BAD_STRUCTURE;
    PROGRAM_STORAGE(declarations->modules, declarations->module_count, XrXirSourceModule);
    PROGRAM_STORAGE(declarations->functions, functions, XrXirFunctionIdentity);
    PROGRAM_STORAGE(declarations->slots, declarations->slot_count, XrXirSlot);
    PROGRAM_STORAGE(declarations->literals, declarations->literal_count, XrXirLiteral);
    for (uint32_t m = 0; m < declarations->module_count; ++m) {
        if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
        const XrXirSourceModule *module = &declarations->modules[m];
        PROGRAM_STORAGE(module->name, module->name_length, char);
        PROGRAM_STORAGE(module->dependencies, module->dependency_count, uint32_t);
    }
    for (uint32_t l = 0; l < declarations->literal_count; ++l) {
        if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
        PROGRAM_STORAGE(declarations->literals[l].bytes, declarations->literals[l].length, uint8_t);
    }
    return XR_XIR_OK;
}

/* The complete owner chain is bounded before the existing shape reader can
 * follow it. These checks cover metadata and signatures, not the Final graph. */
static XrXirStatus effect_program_body_storage(const XrXirCompileContext *work,
    const XrXirEffects *effects, const EffectBodyMaterial **output) {
    PROGRAM_STORAGE(effects, 1, XrXirEffects);
    if (effects->resources != work->resources) return XR_XIR_BAD_STRUCTURE;
    PROGRAM_STORAGE(effects->invocations, 1, EffectInvocationCertificate);
    const EffectInvocationCertificate *certificate = effects->invocations;
    if (certificate->resources != work->resources) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyMaterial *body = certificate->body;
    uint32_t depth = 0;
    for (const EffectBodyMaterial *entry = body; entry; entry = entry->backbone) {
        if (++depth > 3) return XR_XIR_BAD_STRUCTURE;
        PROGRAM_STORAGE(entry, 1, EffectBodyMaterial);
        if (entry->resources != work->resources) return XR_XIR_BAD_STRUCTURE;
        if (entry->overlay) PROGRAM_STORAGE(entry->overlay, 1, EffectBodyOrdinaryOverlay);
    }
    if (!effect_body_material_shape(body) || body->stage != XR_XIR_LOWERED ||
        effect_body_count(body) != effects->count) return XR_XIR_BAD_STRUCTURE;
    PROGRAM_STORAGE(body->functions, effects->count, EffectBodyFunction);
    *output = body;
    return XR_XIR_OK;
}

static XrXirStatus effect_program_metadata_storage(const XrXirCompileContext *work,
    const XrXirEffects *effects) {
    const EffectBodyMaterial *body = NULL;
    XrXirStatus status = effect_program_body_storage(work, effects, &body);
    if (status != XR_XIR_OK) return status;
    for (uint32_t f = 0; f < effects->count; ++f) {
        if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
        PROGRAM_STORAGE(body->functions[f].parameters, body->functions[f].parameter_count, XrXirType);
    }
    status = effect_program_types_storage(work, body->domain.types);
    return status == XR_XIR_OK ?
        effect_program_declarations_storage(work, body->domain.declarations, effects->count) : status;
}
/* Current body correspondence reads these payloads after metadata preparation.
 * Capacity is checked against the genuine payload without reading its tail. */
static XrXirStatus effect_program_input_storage(const XrXirCompileContext *work,
    const XrXirEffects *effects, const EffectBodyMaterial **output) {
    XrXirStatus status = effect_program_metadata_storage(work, effects);
    if (status != XR_XIR_OK) return status;
    const EffectBodyMaterial *body = effects->invocations->body;
    if (body->generics) return XR_XIR_BAD_STRUCTURE;
    PROGRAM_STORAGE(body->defaults.records, body->defaults.count, XrXirDefaultBinding);
    for (uint32_t f = 0; f < effects->count; ++f) {
        if (!xir_compile_work(work, 4)) return XR_XIR_BUDGET;
        const EffectBodyFunction *function = &body->functions[f];
        if (function->run_count > function->run_capacity ||
            function->run_capacity > function->instruction_count) return XR_XIR_BAD_STRUCTURE;
        PROGRAM_STORAGE(function->name, function->name_length, char);
        PROGRAM_STORAGE(function->blocks, function->block_count, XrXirBlock);
        PROGRAM_STORAGE(function->operands, function->operand_count, uint32_t);
        PROGRAM_STORAGE(function->runs, function->run_capacity, EffectBodyRun);
        PROGRAM_STORAGE(function->initial_roles, function->parameter_count, uint8_t);
    }
    *output = body;
    return XR_XIR_OK;
}

static XrXirStatus effect_program_projection_storage(const XrXirCompileContext *work,
    const XrXirModule *module) {
    PROGRAM_STORAGE(module, 1, XrXirModule);
    if (module->stage != XR_XIR_LOWERED || module->generics) return XR_XIR_BAD_STRUCTURE;
    PROGRAM_STORAGE(module->functions, module->function_count, XrXirFunction);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (!xir_compile_work(work, 4)) return XR_XIR_BUDGET;
        const XrXirFunction *function = &module->functions[f];
        PROGRAM_STORAGE(function->name, function->name_length, char);
        PROGRAM_STORAGE(function->parameters, function->parameter_count, XrXirType);
        PROGRAM_STORAGE(function->blocks, function->block_count, XrXirBlock);
        PROGRAM_STORAGE(function->instructions, function->instruction_count, XrXirInstruction);
        PROGRAM_STORAGE(function->operands, function->operand_count, uint32_t);
    }
    if (module->provenance) PROGRAM_STORAGE(module->provenance, 1, XrXirProvenance);
    if (module->defaults) {
        PROGRAM_STORAGE(module->defaults, 1, XrXirDefaultTable);
        PROGRAM_STORAGE(module->defaults->records, module->defaults->count, XrXirDefaultBinding);
    }
    XrXirStatus status = module->types ? effect_program_types_storage(work, module->types) : XR_XIR_OK;
    return status == XR_XIR_OK ?
        effect_program_declarations_storage(work, module->declarations, module->function_count) : status;
}
#undef PROGRAM_STORAGE
