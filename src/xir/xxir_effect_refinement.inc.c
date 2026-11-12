/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_refinement.inc.c - Conversion deltas on owned effect equations
 *
 * KEY CONCEPT:
 *   Source normalization updates an existing physical value's conversion.
 *   Constants and formal leaves remain equation inputs.
 */
XR_FUNC XrXirStatus xir_effects_construction_weaken(const XrXirCompileContext *memory,
    const XrXirEffects *effects,uint32_t function,uint32_t instruction,uint32_t mask,bool may_suspend) {
    EffectCheckedRoots *roots = effects ? effects->checked_roots : NULL;
    if (!roots || !xir_effects_prepared_execution(effects) ||
        !xir_effects_context_matches(memory,effects,effects->count) || function >= roots->templates ||
        (mask & ~(XR_XIR_CALLABLE_ROOT_REQUIRED | XR_XIR_CALLABLE_ROOT_UNRESOLVED))) return XR_XIR_BAD_STRUCTURE;
    EffectCheckFunction row = roots->function[function];
    if (instruction >= row.values-row.parameters) return XR_XIR_BAD_STRUCTURE;
    EffectCheckNode *node = &roots->node[row.first+row.parameters+instruction];
    uint8_t bound = (mask & XR_XIR_CALLABLE_ROOT_REQUIRED ? 1u : 0u) |
        (mask & XR_XIR_CALLABLE_ROOT_UNRESOLVED ? 2u : 0u);
    if (node->weaken && !node->refined_weaken && node->root_seed == bound) return XR_XIR_OK;
    if (node->refined_weaken && node->bound == bound && node->suspend_bound == may_suspend) return XR_XIR_OK;
    node->refined_weaken = true;
    node->bound = bound;
    node->suspend_bound = may_suspend;
    roots->refinement_dirty = true;
    return XR_XIR_OK;
}

static void effect_refinement_weakening(EffectCheckedRoots *roots,uint32_t function,
    uint32_t value,const EffectCheckNode *source) {
    EffectCheckFunction row = roots->function[function];
    uint8_t bound = source->bound, suspend_bound = source->suspend_bound;
    uint32_t id = row.first+value;
    EffectCheckNode *node = &roots->node[id];
    node->weaken = node->erased = true;
    node->bound = bound;
    node->suspend_bound = suspend_bound;
    node->root_seed = 0;
    effect_check_seed(roots,id,bound,(XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INDIRECT,
        value-row.parameters,UINT32_MAX,UINT32_MAX,0});
    bool precise_suspend = node->restricted || row.default_callback;
    for (uint32_t a = node->aliases; a != UINT32_MAX; a = roots->alias[a].next) {
        roots->alias[a].root = false;
        roots->alias[a].suspension = precise_suspend;
    }
    node->suspend_seed = 0;
    if (!precise_suspend && suspend_bound)
        effect_check_suspend_seed(roots,id,suspend_bound,value-row.parameters);
}

/* Derived unions are disposable. Their original constants and formal leaves
 * were recorded by the same producers, including generic instantiation and
 * capture ownership discharge. The SCC solver consumes those inputs and the
 * existing numeric dependency edges. */
static void effect_suspend_reset(EffectCheckedRoots *roots) {
    for (uint32_t f = 0; f < roots->functions; ++f) {
        EffectCheckFunction row = roots->function[f];
        for (uint32_t id = row.first; id <= row.last; ++id) {
            EffectCheckNode *node = &roots->node[id];
            uint64_t *suspension = effect_check_suspend_bits(roots,id);
            memset(suspension,0,(size_t)row.words*sizeof(*suspension));
            suspension[0] = node->suspend_seed;
            uint32_t bit = node->suspend_parameter_seed;
            if (bit) suspension[bit/64] |= UINT64_C(1) << (bit%64);
            for (uint32_t b = 0; b < 2; ++b) {
                bool seed = !!(node->suspend_seed & (1u << b));
                node->suspend_reason[b] = seed ? id : UINT32_MAX;
                node->suspend_call[b] = node->suspend_parameter[b] = UINT32_MAX;
                node->suspend_instruction[b] = node->suspend_order[b] = seed ? node->suspend_seed_instruction[b] : UINT32_MAX;
                node->suspend_distance[b] = seed ? 0 : UINT32_MAX;
            }
        }
        for (uint32_t p = 0; p < row.parameters; ++p) {
            uint32_t bit = roots->node[row.first+p].suspend_parameter_seed;
            if (bit) effect_check_suspend_bits(roots,row.root)[bit/64] |= UINT64_C(1) << (bit%64);
        }
    }
}

static void effect_refinement_reset(EffectCheckedRoots *roots) {
    effect_suspend_reset(roots);
    for (uint32_t f = 0; f < roots->functions; ++f) {
        EffectCheckFunction row = roots->function[f];
        for (uint32_t id = row.first; id <= row.last; ++id) {
            EffectCheckNode *node = &roots->node[id];
            uint64_t *bits = effect_check_bits(roots,id);
            memset(bits,0,(size_t)row.words*sizeof(*bits));
            bits[0] = node->root_seed;
            uint32_t bit = node->parameter_seed;
            if (bit) bits[bit/64] |= UINT64_C(1) << (bit%64);
            for (uint32_t b = 0; b < 2; ++b)
                node->reason[b] = node->root_seed & (1u << b) ? id : UINT32_MAX;
            node->closed = 0;
            node->component = UINT32_MAX;
        }
    }
}

XR_FUNC XrXirStatus xir_effects_construction_close(const XrXirCompileContext *memory,
    const XrXirEffects *borrowed,XrCompileRequestControl *control) {
    if (!xir_effects_prepared_execution(borrowed) ||
        !xir_effects_context_matches(memory,borrowed,borrowed->count)) return XR_XIR_BAD_STRUCTURE;
    XrXirEffects *effects = (XrXirEffects *)borrowed;
    EffectCheckedRoots *roots = effects->checked_roots;
    if (!roots->refinement_dirty) return XR_XIR_OK;
    for (uint32_t f = 0; f < roots->functions; ++f) {
        EffectCheckFunction row = roots->function[f], declaration = roots->function[row.declaration];
        for (uint32_t v = row.parameters; v < row.values; ++v) {
            const EffectCheckNode *source = &roots->node[declaration.first+v];
            if (source->refined_weaken) effect_refinement_weakening(roots,f,v,source);
        }
    }
    effect_refinement_reset(roots);
    EffectCheckBuild build = {.memory=memory,.roots=roots,.control=control};
    XrXirStatus status = effect_check_effect_projection(&build);
    if (status == XR_XIR_OK) status = effect_check_cell_roles(&build);
    if (status == XR_XIR_OK) status = effect_check_close(&build);
    if (status == XR_XIR_OK) status = effect_checked_project(memory,effects);
    if (status == XR_XIR_OK) roots->refinement_dirty = false;
    return status;
}
