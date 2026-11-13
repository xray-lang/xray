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

XR_FUNC XrXirStatus xr_xir_compile_closure_placement(const XrXirArtifact *artifact,
    uint32_t function, uint32_t value, XirClosureStorage *output) {
    const XirClosurePlacement *rows = NULL;
    uint32_t count = 0;
    if (!output) return XR_XIR_BAD_STRUCTURE;
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
