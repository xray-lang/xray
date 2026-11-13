/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_restrictions.inc.c - Receiving ownership of lifetime equations
 */
static void effect_check_restrictions_clear(EffectCheckRestrictions *owner) {
    xr_compile_resources_free((void *)owner->view.functions);
    xr_compile_resources_free((void *)owner->view.formulas);
    xr_compile_resources_free((void *)owner->view.inputs);
    xr_compile_resources_free((void *)owner->view.points);
    xr_compile_resources_free((void *)owner->view.placements);
    xr_compile_resources_free(owner->intrinsic);
    xr_compile_resources_free(owner->flows);
    xr_compile_resources_free(owner->owners);
    *owner = (EffectCheckRestrictions){0};
}

static XrXirStatus effect_check_restriction_formula(EffectCheckRestrictions *owner,
    uint32_t function, uint32_t formula, uint32_t values) {
    const XirRestrictionView *view = &owner->view;
    if (formula >= view->formula_count) return XR_XIR_BAD_STRUCTURE;
    if (owner->owners[formula] != UINT32_MAX)
        return owner->owners[formula] == function ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
    XirRestrictionFormulaRow row = view->formulas[formula];
    if (row.first > view->input_count || row.count > view->input_count - row.first) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i < row.count; ++i) {
        uint32_t input = view->inputs[row.first + i];
        if (input >= values || (i && input <= view->inputs[row.first + i - 1])) return XR_XIR_BAD_STRUCTURE;
    }
    owner->owners[formula] = function;
    return XR_XIR_OK;
}

static bool effect_check_closure_placements_valid(const EffectCheckedRoots *roots, const XirRestrictionView *view) {
    XirStorageFlowView storage = {0};
    if (view->placement_count && roots->storage && xir_storage_flow_view(roots->storage, &storage) != XR_XIR_OK)
        return false;
    for (uint32_t n = 0; n < view->placement_count; ++n) {
        XirClosurePlacement row = view->placements[n];
        if (row.function >= roots->templates || row.ages > 3u || row.remainder > 15u ||
            (uint32_t)row.storage > XIR_CLOSURE_UNRESOLVED) return false;
        const EffectCheckFunction *function = &roots->function[row.function];
        if (row.value < function->parameters || row.value >= function->values ||
            (row.environment != XIR_EXECUTION_CURRENT_ENVIRONMENT &&
                row.environment != XIR_EXECUTION_UNKNOWN_ENVIRONMENT && row.environment >= storage.application_count) ||
            (row.storage == XIR_CLOSURE_FRAME &&
                (row.ages != 1u || row.remainder || row.environment == XIR_EXECUTION_UNKNOWN_ENVIRONMENT))) return false;
        bool constructor = false;
        const EffectCheckNode *node = &roots->node[function->first + row.value];
        for (uint32_t c = node->calls; c != UINT32_MAX; c = roots->call[c].next)
            constructor |= roots->call[c].protocol == 1 && roots->call[c].instruction == row.value - function->parameters;
        if (!constructor) return false;
        if (n) {
            XirClosurePlacement previous = view->placements[n - 1];
            if (previous.function > row.function || (previous.function == row.function &&
                (previous.value > row.value || (previous.value == row.value && previous.environment >= row.environment))))
                return false;
        }
    }
    return true;
}

static XrXirStatus effect_check_restrictions_copy(const XrXirCompileContext *memory,
    const EffectCheckedRoots *roots, const XirRestrictionView *view, EffectCheckRestrictions *owner) {
    if (!view || view->function_count != roots->templates || !view->functions ||
        (view->formula_count && !view->formulas) || (view->input_count && !view->inputs) ||
        (view->point_count && !view->points) || (view->placement_count && !view->placements)) return XR_XIR_BAD_STRUCTURE;
    if (!effect_check_closure_placements_valid(roots, view)) return XR_XIR_BAD_STRUCTURE;
    if (view->pending.status != XR_XIR_OK && (view->pending.status != XR_XIR_UNRESOLVED ||
        view->pending.function >= roots->templates || view->pending.instruction >=
            roots->function[view->pending.function].values - roots->function[view->pending.function].parameters))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    uint64_t values = 0, flows = 0, inputs = 0;
    for (uint32_t f = 0; f < view->function_count; ++f) {
        XirRestrictionFunction row = view->functions[f];
        if (row.physical_values != roots->function[f].values || row.value_count < row.physical_values ||
            (row.value_count && !row.intrinsic) || (row.flow_count && !row.flows)) return XR_XIR_BAD_STRUCTURE;
        values += row.value_count; flows += row.flow_count;
    }
    for (uint32_t r = 0; r < view->formula_count; ++r) {
        XirRestrictionFormulaRow row = view->formulas[r];
        if (row.first > view->input_count || row.count > view->input_count - row.first)
            return XR_XIR_BAD_STRUCTURE;
        inputs += row.count;
    }
    if (values > SIZE_MAX / sizeof(*owner->intrinsic) || flows > SIZE_MAX / sizeof(*owner->flows)) return XR_XIR_OVERFLOW;
    if ((uint64_t)view->formula_count * sizeof(*view->formulas) > SIZE_MAX ||
        inputs > UINT32_MAX || inputs * sizeof(*view->inputs) > SIZE_MAX ||
        (uint64_t)view->point_count * sizeof(*view->points) > SIZE_MAX ||
        (uint64_t)view->placement_count * sizeof(*view->placements) > SIZE_MAX ||
        (uint64_t)view->formula_count * sizeof(*owner->owners) > SIZE_MAX) return XR_XIR_OVERFLOW;
    owner->view = *view;
    owner->view.input_count = (uint32_t)inputs;
    owner->view.functions = NULL; owner->view.formulas = NULL; owner->view.inputs = NULL; owner->view.points = NULL;
    owner->view.placements = NULL;
    owner->view.functions = xir_compile_calloc(memory, view->function_count, sizeof(*view->functions), &status);
    owner->view.formulas = view->formula_count ? xir_compile_copy(memory, view->formulas,
        (size_t)view->formula_count * sizeof(*view->formulas), &status) : NULL;
    owner->view.inputs = inputs ? xir_compile_alloc(memory, (size_t)inputs * sizeof(*view->inputs), &status) : NULL;
    owner->view.points = view->point_count ? xir_compile_copy(memory, view->points,
        (size_t)view->point_count * sizeof(*view->points), &status) : NULL;
    owner->view.placements = view->placement_count ? xir_compile_copy(memory, view->placements,
        (size_t)view->placement_count * sizeof(*view->placements), &status) : NULL;
    owner->intrinsic = values ? xir_compile_alloc(memory, (size_t)values * sizeof(*owner->intrinsic), &status) : NULL;
    owner->flows = flows ? xir_compile_alloc(memory, (size_t)flows * sizeof(*owner->flows), &status) : NULL;
    owner->owners = view->formula_count ? xir_compile_alloc(memory,
        (size_t)view->formula_count * sizeof(*owner->owners), &status) : NULL;
    if (status != XR_XIR_OK) return status;
    uint32_t first = 0;
    for (uint32_t r = 0; r < view->formula_count; ++r) {
        XirRestrictionFormulaRow *row = (XirRestrictionFormulaRow *)owner->view.formulas + r;
        if (row->count) memcpy((uint32_t *)owner->view.inputs + first, view->inputs + row->first,
            (size_t)row->count * sizeof(*view->inputs));
        row->first = first;
        first += row->count;
    }
    if (view->formula_count) memset(owner->owners, 255, (size_t)view->formula_count * sizeof(*owner->owners));
    uint32_t *intrinsic = owner->intrinsic;
    XrXirValueFlow *transport = owner->flows;
    for (uint32_t f = 0; f < view->function_count; ++f) {
        XirRestrictionFunction row = view->functions[f];
        if (row.value_count) memcpy(intrinsic, row.intrinsic, (size_t)row.value_count * sizeof(*intrinsic));
        if (row.flow_count) memcpy(transport, row.flows, (size_t)row.flow_count * sizeof(*transport));
        row.intrinsic = intrinsic; row.flows = transport;
        ((XirRestrictionFunction *)owner->view.functions)[f] = row;
        for (uint32_t v = 0; v < row.value_count; ++v) {
            status = effect_check_restriction_formula(owner, f, intrinsic[v], row.value_count);
            if (status != XR_XIR_OK) return status;
        }
        for (uint32_t e = 0; e < row.flow_count; ++e)
            if (transport[e].source >= row.value_count || transport[e].target >= row.value_count) return XR_XIR_BAD_STRUCTURE;
        if (row.value_count) intrinsic += row.value_count;
        if (row.flow_count) transport += row.flow_count;
    }
    for (uint32_t p = 0; p < view->point_count; ++p) {
        XirRestrictionPoint point = owner->view.points[p];
        if (point.function >= roots->templates) return XR_XIR_BAD_STRUCTURE;
        XirRestrictionFunction row = owner->view.functions[point.function];
        const EffectCheckFunction *function = &roots->function[point.function];
        if (point.instruction >= function->values - function->parameters ||
            point.value >= row.value_count || point.carrier >= row.value_count) return XR_XIR_BAD_STRUCTURE;
        if (p) {
            XirRestrictionPoint previous = owner->view.points[p - 1];
            if (previous.function > point.function || (previous.function == point.function &&
                (previous.instruction > point.instruction || (previous.instruction == point.instruction &&
                    previous.value >= point.value)))) return XR_XIR_BAD_STRUCTURE;
        }
        status = effect_check_restriction_formula(owner, point.function, point.formula, row.value_count);
        if (status != XR_XIR_OK) return status;
    }
    for (uint32_t f = 0; f < view->formula_count; ++f)
        if (owner->owners[f] == UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static XrXirStatus effect_check_restrictions_receive(void *context, const XirRestrictionView *view) {
    EffectCheckedRoots *roots = context;
    if (!roots || roots->restrictions.view.functions) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext memory = {.resources = roots->resources};
    EffectCheckRestrictions next = {0};
    XrXirStatus status = effect_check_restrictions_copy(&memory, roots, view, &next);
    if (status != XR_XIR_OK) {effect_check_restrictions_clear(&next); return status;}
    roots->restrictions = next;
    for (uint32_t f = 0; f < roots->functions; ++f) {
        EffectCheckFunction function = roots->function[f];
        XirRestrictionFunction row = next.view.functions[function.declaration];
        for (uint32_t v = 0; v < function.values; ++v)
            roots->node[function.first + v].restricted = next.view.formulas[row.intrinsic[v]].constant &&
                !next.view.formulas[row.intrinsic[v]].incomplete;
    }
    for (uint32_t p = 0; p < next.view.point_count; ++p) {
        XirRestrictionPoint point = next.view.points[p];
        const EffectCheckFunction *function = &roots->function[point.function];
        EffectCheckNode *instruction = &roots->node[function->first + function->parameters + point.instruction];
        if (instruction->weaken) instruction->restricted = next.view.formulas[point.formula].constant &&
            !next.view.formulas[point.formula].incomplete;
    }
    return XR_XIR_OK;
}
