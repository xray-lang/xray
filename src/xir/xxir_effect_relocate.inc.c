/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_relocate.inc.c - Mechanical relocation of unpublished equations
 */
#include "xxir_view_normalize_internal.h"

typedef struct EffectRelocation {
    const EffectCheckedRoots *source;
    const XirViewNormalization *normalization;
    EffectCheckFunction *functions;
    EffectCheckNode *nodes;
    XrXirRootEffectWitness *terminals;
    uint32_t *values;
    uint32_t count;
    XrCompileRequestControl *control;
    uint32_t progress;
    XrXirStatus status;
} EffectRelocation;

static bool effect_relocation_step(EffectRelocation *relocation) {
    if (relocation->status == XR_XIR_OK)
        relocation->status = xir_receive_step(relocation->control, &relocation->progress);
    return relocation->status == XR_XIR_OK;
}

static uint32_t effect_relocation_node(EffectRelocation *relocation, uint32_t node) {
    if (node == UINT32_MAX) return node;
    if (node >= relocation->source->nodes || relocation->values[node] >= relocation->count) {
        relocation->status = XR_XIR_BAD_STRUCTURE;
        return UINT32_MAX;
    }
    return relocation->values[node];
}

static uint32_t effect_relocation_instruction(EffectRelocation *relocation,
    uint32_t function, uint32_t instruction) {
    if (instruction == UINT32_MAX) return instruction;
    const EffectCheckFunction *source = &relocation->source->function[function];
    const XirViewNormalizedFunction *map = &relocation->normalization->functions[source->declaration];
    if (instruction >= source->values - source->parameters) {
        relocation->status = XR_XIR_BAD_STRUCTURE;
        return UINT32_MAX;
    }
    if (!map->instructions) return instruction;
    return map->values[source->parameters + instruction] - source->parameters;
}

static uint32_t effect_relocation_frontier(EffectRelocation *relocation,
    uint32_t function, uint32_t frontier) {
    if (!frontier) return 0;
    uint32_t instruction = effect_relocation_instruction(relocation, function, frontier - 1);
    return instruction == UINT32_MAX ? 0 : instruction + 1;
}

static uint32_t effect_relocation_value(EffectRelocation *relocation,
    uint32_t function, uint32_t value) {
    const EffectCheckFunction *old = &relocation->source->function[function];
    const EffectCheckFunction *next = &relocation->functions[function];
    if (value >= old->values) return next->values + (value - old->values);
    return effect_relocation_node(relocation, old->first + value) - next->first;
}

static XrXirStatus effect_relocation_restrictions(const XrXirCompileContext *memory,
    EffectRelocation *relocation) {
    EffectCheckRestrictions *owner = &((EffectCheckedRoots *)relocation->source)->restrictions;
    if (!owner->view.functions) return XR_XIR_OK;
    uint64_t count = 0;
    uint32_t extra = 0;
    for (uint32_t f = 0; f < owner->view.function_count; ++f) {
        uint32_t added = relocation->functions[f].values - relocation->source->function[f].values;
        if (added > UINT32_MAX - owner->view.functions[f].value_count) return XR_XIR_OVERFLOW;
        count += (uint64_t)owner->view.functions[f].value_count + added;
        extra += added != 0;
    }
    if (count > SIZE_MAX / sizeof(*owner->intrinsic) || extra > UINT32_MAX - owner->view.formula_count)
        return XR_XIR_OVERFLOW;
    uint32_t formulas = owner->view.formula_count + extra;
    if ((uint64_t)formulas * sizeof(*owner->view.formulas) > SIZE_MAX ||
        (uint64_t)formulas * sizeof(*owner->owners) > SIZE_MAX) return XR_XIR_OVERFLOW;
    XrXirStatus status = XR_XIR_OK;
    uint32_t *intrinsic = count ? xir_compile_alloc(memory, (size_t)count * sizeof(*intrinsic), &status) : NULL;
    XirRestrictionFormulaRow *rows = formulas ? xir_compile_calloc(memory, formulas, sizeof(*rows), &status) : NULL;
    uint32_t *owners = formulas ? xir_compile_alloc(memory, (size_t)formulas * sizeof(*owners), &status) : NULL;
    if (status != XR_XIR_OK) {
        xr_compile_resources_free(intrinsic); xr_compile_resources_free(rows); xr_compile_resources_free(owners);
        return status;
    }
    if (owner->view.formula_count) {
        memcpy(rows, owner->view.formulas, (size_t)owner->view.formula_count * sizeof(*rows));
        memcpy(owners, owner->owners, (size_t)owner->view.formula_count * sizeof(*owners));
    }
    uint32_t *next = intrinsic;
    uint32_t formula = owner->view.formula_count;
    for (uint32_t f = 0; f < owner->view.function_count; ++f) {
        if (!effect_relocation_step(relocation)) {status = relocation->status; break;}
        XirRestrictionFunction *row = (XirRestrictionFunction *)owner->view.functions + f;
        uint32_t old_values = row->value_count;
        uint32_t physical = relocation->functions[f].values;
        uint32_t added = physical - row->physical_values;
        if (added) {
            for (uint32_t v = 0; v < old_values + added; ++v) next[v] = formula;
            owners[formula++] = f;
        }
        for (uint32_t v = 0; v < old_values; ++v)
            next[effect_relocation_value(relocation, f, v)] = row->intrinsic[v];
        for (uint32_t e = 0; e < row->flow_count; ++e) {
            XrXirValueFlow *flow = (XrXirValueFlow *)row->flows + e;
            flow->source = effect_relocation_value(relocation, f, flow->source);
            flow->target = effect_relocation_value(relocation, f, flow->target);
        }
        row->physical_values = physical;
        row->value_count = old_values + added;
        row->intrinsic = next;
        if (row->value_count) next += row->value_count;
    }
    for (uint32_t r = 0; status == XR_XIR_OK && r < owner->view.formula_count; ++r) {
        XirRestrictionFormulaRow row = rows[r];
        for (uint32_t i = 0; i < row.count; ++i) {
            uint32_t *input = (uint32_t *)owner->view.inputs + row.first + i;
            *input = effect_relocation_value(relocation, owners[r], *input);
        }
    }
    for (uint32_t p = 0; status == XR_XIR_OK && p < owner->view.point_count; ++p) {
        XirRestrictionPoint *point = (XirRestrictionPoint *)owner->view.points + p;
        point->instruction = effect_relocation_instruction(relocation, point->function, point->instruction);
        point->value = effect_relocation_value(relocation, point->function, point->value);
        point->carrier = effect_relocation_value(relocation, point->function, point->carrier);
    }
    for (uint32_t n = 0; status == XR_XIR_OK && n < owner->view.placement_count; ++n) {
        XirClosurePlacement *placement = (XirClosurePlacement *)owner->view.placements + n;
        placement->value = effect_relocation_value(relocation, placement->function, placement->value);
    }
    if (owner->view.pending.status == XR_XIR_UNRESOLVED)
        owner->view.pending.instruction = effect_relocation_instruction(relocation,
            owner->view.pending.function, owner->view.pending.instruction);
    xr_compile_resources_free(owner->intrinsic);
    xr_compile_resources_free((void *)owner->view.formulas);
    xr_compile_resources_free(owner->owners);
    owner->intrinsic = intrinsic;
    owner->view.formulas = rows;
    owner->owners = owners;
    owner->view.formula_count = formulas;
    return status == XR_XIR_OK ? relocation->status : status;
}

static void effect_relocation_clear(EffectRelocation *relocation) {
    if (relocation->functions) for (uint32_t f = 0; f < relocation->source->functions; ++f) {
        xr_compile_resources_free(relocation->functions[f].bits);
        xr_compile_resources_free(relocation->functions[f].suspend_bits);
    }
    xr_compile_resources_free(relocation->functions);
    xr_compile_resources_free(relocation->nodes);
    xr_compile_resources_free(relocation->terminals);
    xr_compile_resources_free(relocation->values);
}

static XrXirStatus effect_relocation_allocate(const XrXirCompileContext *memory,
    EffectRelocation *relocation, const XrXirModule *module) {
    const EffectCheckedRoots *roots = relocation->source;
    relocation->functions = xir_compile_calloc(memory, roots->functions,
        sizeof(*relocation->functions), &relocation->status);
    relocation->values = xir_compile_alloc(memory, (size_t)roots->nodes * sizeof(*relocation->values),
        &relocation->status);
    if (relocation->status != XR_XIR_OK) return relocation->status;
    if (roots->nodes) memset(relocation->values, 0xFF, (size_t)roots->nodes * sizeof(*relocation->values));
    uint64_t nodes = 0;
    for (uint32_t f = 0; f < roots->functions; ++f) {
        if (!effect_relocation_step(relocation)) return relocation->status;
        EffectCheckFunction old = roots->function[f];
        const XrXirFunction *body = &module->functions[old.declaration];
        const XirViewNormalizedFunction *map = &relocation->normalization->functions[old.declaration];
        if (body->parameter_count != old.parameters ||
            (map->instructions ? map->old_instructions : body->instruction_count) != old.values - old.parameters)
            return XR_XIR_BAD_STRUCTURE;
        uint64_t values = (uint64_t)body->parameter_count + body->instruction_count;
        uint64_t first_tail = (uint64_t)old.first + (uint64_t)old.values * 2;
        if (first_tail > old.last || old.last >= roots->nodes ||
            old.root < first_tail || old.root > old.last ||
            old.result < first_tail || old.result > old.last ||
            old.result_contents < first_tail || old.result_contents > old.last)
            return XR_XIR_BAD_STRUCTURE;
        uint64_t tail_count = old.last - first_tail + 1;
        uint64_t expressions = values * 2 + tail_count;
        if (values > UINT32_MAX || expressions > UINT32_MAX - nodes ||
            expressions > SIZE_MAX / sizeof(uint64_t) / old.words) return XR_XIR_OVERFLOW;
        EffectCheckFunction next = old;
        next.first = (uint32_t)nodes;
        next.values = (uint32_t)values;
        next.contents = next.first + next.values;
        uint32_t next_tail = next.contents + next.values;
        next.result_contents = next_tail + (uint32_t)(old.result_contents - first_tail);
        next.root = next_tail + (uint32_t)(old.root - first_tail);
        next.result = next_tail + (uint32_t)(old.result - first_tail);
        next.last = next.first + (uint32_t)expressions - 1;
        next.bits = NULL;
        next.suspend_bits = NULL;
        relocation->functions[f] = next;
        relocation->functions[f].bits = xir_compile_calloc(memory, (size_t)expressions * old.words,
            sizeof(uint64_t), &relocation->status);
        relocation->functions[f].suspend_bits = xir_compile_calloc(memory, (size_t)expressions * old.words,
            sizeof(uint64_t), &relocation->status);
        if (relocation->status != XR_XIR_OK) return relocation->status;
        for (uint32_t n = 0; n < old.values; ++n) {
            if (map->instructions && (!map->values || map->values[n] >= next.values ||
                (n && map->values[n] <= map->values[n - 1]) ||
                (n < old.parameters && map->values[n] != n))) return XR_XIR_BAD_STRUCTURE;
            uint32_t value = map->instructions ? map->values[n] : n;
            relocation->values[old.first + n] = next.first + value;
            relocation->values[old.contents + n] = next.contents + value;
        }
        /* Preserve every non-value equation in the producer's order. Result
         * contents and future auxiliary banks must move with their identities. */
        for (uint32_t a = 0; a < tail_count; ++a)
            relocation->values[(uint32_t)first_tail + a] = next_tail + a;
        nodes += expressions;
    }
    if (nodes > SIZE_MAX / sizeof(*relocation->nodes) ||
        nodes > SIZE_MAX / sizeof(*relocation->terminals) / 2) return XR_XIR_OVERFLOW;
    relocation->count = (uint32_t)nodes;
    relocation->nodes = xir_compile_alloc(memory, (size_t)nodes * sizeof(*relocation->nodes), &relocation->status);
    relocation->terminals = xir_compile_calloc(memory, (size_t)nodes, 2 * sizeof(*relocation->terminals),
        &relocation->status);
    return relocation->status;
}

static XrXirStatus effect_relocation_nodes(EffectRelocation *relocation) {
    const EffectCheckedRoots *roots = relocation->source;
    for (uint32_t f = 0; f < roots->functions; ++f) {
        EffectCheckFunction old = roots->function[f], next = relocation->functions[f];
        for (uint32_t n = next.first; n <= next.last; ++n) relocation->nodes[n] = effect_check_empty_node(f);
        for (uint32_t n = old.first; n <= old.last; ++n) {
            if (!effect_relocation_step(relocation)) return relocation->status;
            uint32_t target = effect_relocation_node(relocation, n);
            if (relocation->status != XR_XIR_OK) return relocation->status;
            EffectCheckNode node = roots->node[n];
            if (node.component != UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            node.cleanup_parent = effect_relocation_frontier(relocation, f, node.cleanup_parent);
            node.cleanup_panic_frontier = effect_relocation_frontier(relocation, f, node.cleanup_panic_frontier);
            for (uint32_t bit = 0; bit < 2; ++bit) {
                node.reason[bit] = effect_relocation_node(relocation, node.reason[bit]);
                node.suspend_reason[bit] = effect_relocation_node(relocation, node.suspend_reason[bit]);
                node.suspend_instruction[bit] = effect_relocation_instruction(relocation, f, node.suspend_instruction[bit]);
                node.suspend_order[bit] = effect_relocation_instruction(relocation, f, node.suspend_order[bit]);
                node.suspend_seed_instruction[bit] = effect_relocation_instruction(relocation, f, node.suspend_seed_instruction[bit]);
                XrXirRootEffectWitness witness = roots->terminals[(size_t)n * 2 + bit];
                if (witness.cause && (witness.instruction != 0 || old.values != old.parameters))
                    witness.instruction = effect_relocation_instruction(relocation, f, witness.instruction);
                relocation->terminals[(size_t)target * 2 + bit] = witness;
            }
            relocation->nodes[target] = node;
            memcpy(next.bits + (size_t)(target - next.first) * next.words,
                old.bits + (size_t)(n - old.first) * old.words, (size_t)old.words * sizeof(uint64_t));
            memcpy(next.suspend_bits + (size_t)(target - next.first) * next.words,
                old.suspend_bits + (size_t)(n - old.first) * old.words, (size_t)old.words * sizeof(uint64_t));
        }
    }
    return relocation->status;
}

static XrXirStatus effect_relocation_edges(EffectRelocation *relocation) {
    EffectCheckedRoots *roots = (EffectCheckedRoots *)relocation->source;
    for (uint32_t n = 0; n < roots->nodes; ++n) {
        if (!effect_relocation_step(relocation)) return relocation->status;
        for (uint32_t s = roots->node[n].module_seeds; s != UINT32_MAX; s = roots->module_seed[s].next)
            roots->module_seed[s].instruction = effect_relocation_instruction(relocation,
                roots->node[n].function, roots->module_seed[s].instruction);
        for (uint32_t a = roots->node[n].aliases; a != UINT32_MAX; a = roots->alias[a].next)
            roots->alias[a].instruction = effect_relocation_instruction(relocation,
                roots->node[n].function, roots->alias[a].instruction);
    }
    for (uint32_t a = 0; a < roots->aliases; ++a)
        roots->alias[a].source = effect_relocation_node(relocation, roots->alias[a].source);
    if (roots->module_index_capacity)
        memset(roots->module_index, 0, (size_t)roots->module_index_capacity * sizeof(*roots->module_index));
    for (uint32_t m = 0; m < roots->module_members; ++m) {
        EffectModuleMember *member = &roots->module_member[m];
        member->node = effect_relocation_node(relocation, member->node);
        member->instruction = effect_relocation_instruction(relocation, member->function, member->instruction);
        uint32_t mask = roots->module_index_capacity - 1;
        uint32_t at = effect_module_hash(member->node, member->identity, member->kind, member->property) & mask;
        while (roots->module_index[at]) at = (at + 1) & mask;
        roots->module_index[at] = m + 1;
    }
    for (uint32_t d = 0; d < roots->dependencies; ++d) {
        roots->dependency[d].source = effect_relocation_node(relocation, roots->dependency[d].source);
        roots->dependency[d].target = effect_relocation_node(relocation, roots->dependency[d].target);
    }
    for (uint32_t b = 0; b < roots->bindings; ++b)
        roots->binding[b].source = effect_relocation_node(relocation, roots->binding[b].source);
    for (uint32_t e = 0; e < roots->elements; ++e)
        roots->element[e].source = effect_relocation_node(relocation, roots->element[e].source);
    for (uint32_t a = 0; a < roots->arguments; ++a)
        roots->argument[a] = effect_relocation_node(relocation, roots->argument[a]);
    for (uint32_t a = 0; a < roots->suspend_actuals; ++a)
        roots->suspend_actual[a].source = effect_relocation_node(relocation, roots->suspend_actual[a].source);
    for (uint32_t c = 0; c < roots->calls; ++c) {
        if (!effect_relocation_step(relocation)) return relocation->status;
        EffectCheckCall *call = &roots->call[c];
        uint32_t function = roots->node[call->output].function;
        call->instruction = effect_relocation_instruction(relocation, function, call->instruction);
        call->output = effect_relocation_node(relocation, call->output);
        call->target = effect_relocation_node(relocation, call->target);
    }
    for (uint32_t p = 0; p < roots->pending_count; ++p) {
        EffectCheckPending *pending = &roots->pending[p];
        pending->instruction = effect_relocation_instruction(relocation, pending->function, pending->instruction);
        pending->node = effect_relocation_node(relocation, pending->node);
        pending->receiver = effect_relocation_node(relocation, pending->receiver);
        pending->result = effect_relocation_node(relocation, pending->result);
    }
    for (uint32_t f = 0; f < roots->templates; ++f) {
        EffectCheckFunction function = roots->function[f];
        for (uint32_t e = 0; e < function.cleanup_count; ++e) {
            EffectCheckCleanupExit *exit = &roots->cleanup_exit[function.cleanup_first + e];
            exit->instruction = effect_relocation_instruction(relocation, f, exit->instruction);
            exit->from = effect_relocation_frontier(relocation, f, exit->from);
            exit->to = effect_relocation_frontier(relocation, f, exit->to);
        }
    }
    for (uint32_t u = 0; u < roots->cleanup_uses; ++u) {
        EffectCheckCleanupUse *use = &roots->cleanup_use[u];
        use->instruction = effect_relocation_instruction(relocation, use->function, use->instruction);
    }
    return relocation->status;
}

/* A failed private relocation remains destructible and must be discarded with
 * its Source request. No published owner or executable body is mutated. */
XR_FUNC XrXirStatus xir_effects_relocate_prepared(const XrXirCompileContext *memory,
    XrXirEffects *effects, const XrXirModule *module, const XirViewNormalization *normalization,
    XrCompileRequestControl *control) {
    if (!module || !normalization || !normalization->functions || !effects || !effects->checked_roots ||
        !xir_effects_context_matches(memory, effects, module->function_count) || effects->input_facts ||
        !effects->outcomes_owner || effects->final_outcomes || normalization->module.function_count != effects->count)
        return XR_XIR_BAD_STRUCTURE;
    EffectCheckedRoots *roots = effects->checked_roots;
    roots->liveness_checked = false;
    EffectRelocation relocation = {.source = roots, .normalization = normalization, .control = control};
    XrXirStatus status = effect_relocation_allocate(memory, &relocation, module);
    if (status == XR_XIR_OK) status = effect_relocation_nodes(&relocation);
    XirStorageRelocation *storage_maps = NULL;
    uint32_t *instructions = NULL;
    uint32_t *identity_values = NULL;
    if (status == XR_XIR_OK && roots->storage) {
        storage_maps = xir_compile_calloc(memory, roots->templates, sizeof(*storage_maps), &status);
        uint64_t count = 0, identities = 0;
        for (uint32_t f = 0; f < roots->templates; ++f) {
            count += roots->function[f].values - roots->function[f].parameters;
            if (!normalization->functions[f].instructions) identities += roots->function[f].values;
        }
        if (count > SIZE_MAX / sizeof(*instructions) || identities > SIZE_MAX / sizeof(*identity_values))
            status = XR_XIR_OVERFLOW;
        if (status == XR_XIR_OK && count)
            instructions = xir_compile_alloc(memory, (size_t)count * sizeof(*instructions), &status);
        if (status == XR_XIR_OK && identities)
            identity_values = xir_compile_alloc(memory, (size_t)identities * sizeof(*identity_values), &status);
        uint32_t *next = instructions;
        uint32_t *identity = identity_values;
        for (uint32_t f = 0; status == XR_XIR_OK && f < roots->templates; ++f) {
            const EffectCheckFunction *function = &roots->function[f];
            const XirViewNormalizedFunction *map = &normalization->functions[f];
            const uint32_t *values = map->values;
            if (!map->instructions) {
                values = identity;
                for (uint32_t v = 0; v < function->values; ++v) identity[v] = v;
                if (function->values) identity += function->values;
            }
            storage_maps[f] = (XirStorageRelocation){f, relocation.functions[f].values,
                module->functions[f].instruction_count, values, next};
            uint32_t length = function->values - function->parameters;
            for (uint32_t i = 0; i < length; ++i)
                next[i] = effect_relocation_instruction(&relocation, f, i);
            if (length) next += length;
            if (function->declaration != f) status = XR_XIR_BAD_STRUCTURE;
        }
        if (status == XR_XIR_OK) status = relocation.status;
        if (status == XR_XIR_OK)
            status = xir_storage_flow_relocate(roots->storage, storage_maps, roots->templates, control);
    }
    if (status == XR_XIR_OK) status = effect_relocation_restrictions(memory, &relocation);
    if (status == XR_XIR_OK) status = effect_relocation_edges(&relocation);
    if (status == XR_XIR_OK) {
        for (uint32_t f = 0; f < roots->functions; ++f) {
            xr_compile_resources_free(roots->function[f].bits);
            xr_compile_resources_free(roots->function[f].suspend_bits);
        }
        xr_compile_resources_free(roots->function);
        xr_compile_resources_free(roots->node);
        xr_compile_resources_free(roots->terminals);
        roots->function = relocation.functions; relocation.functions = NULL;
        roots->node = relocation.nodes; relocation.nodes = NULL;
        roots->terminals = relocation.terminals; relocation.terminals = NULL;
        roots->nodes = roots->node_capacity = roots->terminal_capacity = relocation.count;
        roots->function_capacity = roots->functions;
        roots->borrowed_types = module->types;
        if (roots->storage) status = xir_execution_refresh_storage(roots);
    }
    xr_compile_resources_free(storage_maps);
    xr_compile_resources_free(instructions);
    xr_compile_resources_free(identity_values);
    effect_relocation_clear(&relocation);
    return status;
}

XR_FUNC XrXirStatus xir_effects_accept_view_normalization(const XrXirCompileContext *context,
    XrXirEffects *effects,const XrXirModule *module,const XirViewNormalization *normalization,
    XrCompileRequestControl *control) {
    if (!effects || !effects->checked_roots || effects->final_outcomes || effects->input_facts ||
        !module || !module->functions || !xir_effects_context_matches(context,effects,module->function_count)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<module->function_count;++f)
        if (effects->checked_roots->function[f].values!=(uint64_t)module->functions[f].parameter_count+
            module->functions[f].instruction_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=xir_compile_view_normalization_matches(context,normalization,module,control);
    if (status==XR_XIR_OK) effects->checked_roots->liveness_checked=true;
    return status;
}
