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
    const void *memory, size_t count, size_t element) {
    if (!count) return XR_XIR_OK;
    if (count > SIZE_MAX / element) return XR_XIR_BUDGET;
    XrCompileResourceStatus status = xr_compile_resources_extent(work->resources, memory, count * element);
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
}

#define PROGRAM_STORAGE(pointer, count) do { \
    XrXirStatus range_status = effect_program_range(work, pointer, count, sizeof(*(pointer))); \
    if (range_status != XR_XIR_OK) return range_status; \
} while (0)

static XrXirStatus effect_program_types_storage(const XrXirCompileContext *work,
    const XrXirTypes *types) {
    PROGRAM_STORAGE(types, 1);
    if (types->interfaces) return XR_XIR_BAD_STRUCTURE;
    PROGRAM_STORAGE(types->nodes, types->count);
    for (uint32_t t = 0; t < types->count; ++t) {
        if (!xir_compile_work(work, 4)) return XR_XIR_BUDGET;
        const XrXirTypeNode *node = &types->nodes[t];
        PROGRAM_STORAGE(node->parameters, node->parameter_count);
        PROGRAM_STORAGE(node->nominal.arguments, node->nominal.argument_count);
        PROGRAM_STORAGE(node->nominal.fields, node->nominal.field_count);
    }
    if (!types->nominals) return XR_XIR_OK;
    PROGRAM_STORAGE(types->nominals, 1);
    const XrXirNominalTable *nominals = types->nominals;
    if (nominals->declarations) return XR_XIR_BAD_STRUCTURE;
    PROGRAM_STORAGE(nominals->identities, nominals->count);
    for (uint32_t n = 0; n < nominals->count; ++n) {
        if (!xir_compile_work(work, 4)) return XR_XIR_BUDGET;
        const XrXirNominalIdentity *identity = &nominals->identities[n];
        PROGRAM_STORAGE(identity->module.bytes, identity->module.length);
        PROGRAM_STORAGE(identity->name.bytes, identity->name.length);
        PROGRAM_STORAGE(identity->variants, identity->variant_count);
        PROGRAM_STORAGE(identity->fields, identity->field_count);
        for (uint32_t v = 0; v < identity->variant_count; ++v) {
            if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
            PROGRAM_STORAGE(identity->variants[v].name.bytes, identity->variants[v].name.length);
        }
        for (uint32_t f = 0; f < identity->field_count; ++f) {
            if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
            PROGRAM_STORAGE(identity->fields[f].name.bytes, identity->fields[f].name.length);
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_program_declarations_storage(const XrXirCompileContext *work,
    const XrXirDeclarations *declarations, uint32_t functions) {
    PROGRAM_STORAGE(declarations, 1);
    if (declarations->implementations) return XR_XIR_BAD_STRUCTURE;
    PROGRAM_STORAGE(declarations->modules, declarations->module_count);
    PROGRAM_STORAGE(declarations->functions, functions);
    PROGRAM_STORAGE(declarations->slots, declarations->slot_count);
    PROGRAM_STORAGE(declarations->literals, declarations->literal_count);
    for (uint32_t m = 0; m < declarations->module_count; ++m) {
        if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
        const XrXirSourceModule *module = &declarations->modules[m];
        PROGRAM_STORAGE(module->name, module->name_length);
        PROGRAM_STORAGE(module->dependencies, module->dependency_count);
    }
    for (uint32_t l = 0; l < declarations->literal_count; ++l) {
        if (!xir_compile_work(work, 2)) return XR_XIR_BUDGET;
        PROGRAM_STORAGE(declarations->literals[l].bytes, declarations->literals[l].length);
    }
    return XR_XIR_OK;
}

/* The complete owner chain is bounded before the existing shape reader can
 * follow it. These checks cover metadata and signatures, not the Final graph. */
static XrXirStatus effect_program_body_storage(const XrXirCompileContext *work,
    const XrXirEffects *effects, const EffectBodyMaterial **output) {
    PROGRAM_STORAGE(effects, 1);
    if (effects->resources != work->resources) return XR_XIR_BAD_STRUCTURE;
    PROGRAM_STORAGE(effects->invocations, 1);
    const EffectInvocationCertificate *certificate = effects->invocations;
    if (certificate->resources != work->resources) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyMaterial *body = certificate->body;
    uint32_t depth = 0;
    for (const EffectBodyMaterial *entry = body; entry; entry = entry->backbone) {
        if (++depth > 3) return XR_XIR_BAD_STRUCTURE;
        PROGRAM_STORAGE(entry, 1);
        if (entry->resources != work->resources) return XR_XIR_BAD_STRUCTURE;
        if (entry->overlay) PROGRAM_STORAGE(entry->overlay, 1);
    }
    if (!effect_body_material_shape(body) || body->stage != XR_XIR_LOWERED ||
        effect_body_count(body) != effects->count) return XR_XIR_BAD_STRUCTURE;
    PROGRAM_STORAGE(body->functions, effects->count);
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
        PROGRAM_STORAGE(body->functions[f].parameters, body->functions[f].parameter_count);
    }
    status = effect_program_types_storage(work, body->domain.types);
    return status == XR_XIR_OK ?
        effect_program_declarations_storage(work, body->domain.declarations, effects->count) : status;
}
#undef PROGRAM_STORAGE
