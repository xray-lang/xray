/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_closure_placement.inc.c - Immutable physical storage observations
 */
XR_FUNC XrXirStatus xr_xir_compile_closure_placements(const XrXirArtifact *artifact,
    const XirClosurePlacement **output, uint32_t *count) {
    if (!artifact || !output || !count) return XR_XIR_BAD_STRUCTURE;
    const XrXirEffects *effects = artifact->program_final ? artifact->program_final : artifact->checked_effects;
    if (!effects || !effects->checked_roots ||
        !xir_effects_context_matches(&artifact->context, effects, artifact->module.function_count))
        return XR_XIR_BAD_STAGE;
    const XirRestrictionView *view = &effects->checked_roots->restrictions.view;
    *output = view->placements;
    *count = view->placement_count;
    return XR_XIR_OK;
}

/* Specialization preserves the original value positions. Its owned provenance
 * keeps the producer alive, while a receiver joins its existing contexts. The
 * application ordinals remain in that producer's namespace. */
static const XrXirArtifact *closure_placement_source(const XrXirArtifact *artifact,
    uint32_t *function, uint32_t *value) {
    if (!artifact || *function >= artifact->module.function_count) return NULL;
    const XrXirProvenance *provenance = artifact->module.provenance;
    if (!provenance || provenance->kind != XR_XIR_EVIDENCE_INSTANCE) return artifact;
    const XrXirArtifact *source = provenance->source;
    if (!source || provenance->count != artifact->module.function_count || !provenance->origins)
        return NULL;
    uint32_t origin = provenance->origins[*function].function;
    if (origin >= source->module.function_count) return NULL;
    const XrXirFunction *current = &artifact->module.functions[*function];
    const XrXirFunction *original = &source->module.functions[origin];
    if (current->parameter_count != original->parameter_count || *value < current->parameter_count)
        return NULL;
    uint32_t instruction = *value - current->parameter_count;
    if (instruction >= current->instruction_count || instruction >= original->instruction_count)
        return NULL;
    const XrXirInstruction *a = &current->instructions[instruction], *b = &original->instructions[instruction];
    if (a->op != XR_XIR_FUNCTION_REF || b->op != XR_XIR_FUNCTION_REF || a->args[1] != b->args[1] ||
        a->immediate < 0 || (uint64_t)a->immediate >= provenance->count ||
        b->immediate < 0 || (uint64_t)b->immediate >= source->module.function_count ||
        provenance->origins[a->immediate].function != (uint32_t)b->immediate) return NULL;
    *function = origin;
    *value = original->parameter_count + instruction;
    return source;
}

XR_FUNC XrXirStatus xr_xir_compile_closure_placement(const XrXirArtifact *artifact,
    uint32_t function, uint32_t value, XirClosureStorage *output) {
    const XirClosurePlacement *rows = NULL;
    uint32_t count = 0;
    if (!output) return XR_XIR_BAD_STRUCTURE;
    artifact = closure_placement_source(artifact, &function, &value);
    if (!artifact) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xr_xir_compile_closure_placements(artifact, &rows, &count);
    if (status != XR_XIR_OK) return status;
    uint32_t first = 0, end = count;
    while (first < end) {
        uint32_t middle = first + (end - first) / 2;
        if (rows[middle].function < function ||
            (rows[middle].function == function && rows[middle].value < value)) first = middle + 1;
        else end = middle;
    }
    if (first == count || rows[first].function != function || rows[first].value != value) return XR_XIR_UNRESOLVED;
    XirClosureStorage storage = XIR_CLOSURE_FRAME;
    for (uint32_t n = first; n < count && rows[n].function == function && rows[n].value == value; ++n) {
        if (rows[n].storage == XIR_CLOSURE_OWNED) { storage = XIR_CLOSURE_OWNED; break; }
        if (rows[n].storage == XIR_CLOSURE_UNRESOLVED) storage = XIR_CLOSURE_UNRESOLVED;
    }
    *output = storage;
    return XR_XIR_OK;
}
