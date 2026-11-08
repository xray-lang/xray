/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_check_pipeline.inc.c - Shared semantic effect construction
 *
 * KEY CONCEPT:
 *   Source refinement and specialization consume one numeric ROOT owner.
 *   No native frame or scheduling plan participates in semantic acceptance.
 */

static XrXirStatus effect_checked_parameters(const XrXirCompileContext *memory,
    XirEffectInput input, XrXirEffects *effects) {
    const XrXirModule *module = input.module;
    XrXirStatus status = XR_XIR_OK;
    XrXirFunctionEffectContract *contracts = xir_compile_calloc(memory, effects->count,
        sizeof(*contracts), &status);
    if (!contracts) return status;
    effects->outcomes_work.contracts = contracts;
    effects->outcomes_work.owns_formulas = true;
    for (uint32_t f = 0; f < effects->count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        uint32_t count = function->parameter_count;
        contracts[f].parameter_count = count;
        XrXirEffectParameter *parameters = count ? xir_compile_calloc(memory, count,
            sizeof(*parameters), &status) : NULL;
        if (count && !parameters) return status;
        contracts[f].parameters = parameters;
        for (uint32_t p = 0; p < count; ++p)
            parameters[p].kind = xr_xir_type_is_callable(module->types, function->parameters[p]) &&
                !xr_xir_function_parameter_escaping(function, p) ?
                XR_XIR_EFFECT_PARAMETER_VARIABLE : XR_XIR_EFFECT_PARAMETER_FIXED;
    }
    return status;
}

static XrXirStatus effect_checked_project(const XrXirCompileContext *memory, XrXirEffects *effects) {
    EffectFinalOutcomes *records = &effects->outcomes_work;
    EffectCheckedRoots *roots = effects->checked_roots;
    XrXirStatus status = XR_XIR_OK;
    records->root = xir_compile_calloc(memory, effects->count, sizeof(*records->root), &status);
    records->root_witnesses = xir_compile_calloc(memory, effects->count, sizeof(*records->root_witnesses), &status);
    records->unresolved_witnesses = xir_compile_calloc(memory, effects->count,
        sizeof(*records->unresolved_witnesses), &status);
    records->go_safe = xir_compile_calloc(memory, effects->count, sizeof(*records->go_safe), &status);
    if (!records->root || !records->root_witnesses || !records->unresolved_witnesses || !records->go_safe) return status;
    for (uint32_t f = 0; f < effects->count; ++f) {
        uint32_t node = roots->function[f].root;
        uint8_t mask = roots->node[node].closed;
        records->root[f] = (XrXirRootEffects){!!(mask & 1), !!(mask & 2)};
        records->go_safe[f] = mask || roots->node[node].worker_blocked ? XR_XIR_BAD_TYPE : XR_XIR_OK;
        XrXirRootFormula *formula = &records->contracts[f].formula;
        status = effect_checked_root_formula(memory, roots, f, false, formula);
        if (status == XR_XIR_UNRESOLVED) {
            /* The owning summary retains the symbolic requirement. Its
             * presentation remains unqualified until program closure. */
            formula->constant_mask = XR_XIR_CALLABLE_ROOT_UNRESOLVED;
            status = XR_XIR_OK;
        }
        if (status != XR_XIR_OK) return status;
        for (uint32_t bit = 0; bit < 2; ++bit) {
            if (!(mask & (1u << bit))) continue;
            EffectCheckCause cause = {0};
            status = effect_checked_first_cause(roots, node, NULL, 0, bit != 0, &cause);
            if (status == XR_XIR_UNRESOLVED) {
                uint32_t instruction = UINT32_MAX;
                for (uint32_t t = 0; t < formula->term_count; ++t)
                    if (formula->terms[t].kind == XR_XIR_ROOT_TERM_CONTEXT_CALL) {
                        instruction = formula->terms[t].index;
                        break;
                    }
                cause.witness = (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_CONTEXT_CALL,
                    instruction, UINT32_MAX, UINT32_MAX, 0};
                status = XR_XIR_OK;
            }
            if (status != XR_XIR_OK) return status;
            if (bit) records->unresolved_witnesses[f] = cause.witness;
            else records->root_witnesses[f] = cause.witness;
        }
    }
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_value_restricted(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t function, uint32_t value, bool *output) {
    if (!output || !effects || !effects->checked_roots || function >= effects->count ||
        !xir_effects_context_matches(context, effects, effects->count)) return XR_XIR_BAD_STRUCTURE;
    const EffectCheckFunction *row = &effects->checked_roots->function[function];
    if (value >= row->values) return XR_XIR_BAD_STRUCTURE;
    *output = effects->checked_roots->node[row->first + value].restricted;
    return XR_XIR_OK;
}

static XrXirStatus effect_checked_reference_read(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t function, uint32_t instruction, uint32_t *output) {
    if (!output || !effects || !effects->checked_roots ||
        !xir_effects_context_matches(context, effects, effects->count) || function >= effects->count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectCheckFunction *row = &effects->checked_roots->function[function];
    if (instruction >= row->values - row->parameters) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = effect_checked_root_mask(effects->checked_roots,
        row->first + row->parameters + instruction, NULL, 0, output);
    if (status == XR_XIR_UNRESOLVED) {
        *output = XR_XIR_CALLABLE_ROOT_UNRESOLVED;
        return XR_XIR_OK;
    }
    return status;
}

XR_FUNC XrXirStatus xir_effects_construction_reference(const XrXirCompileContext *context,
    const XrXirEffects *effects,uint32_t function,uint32_t instruction,uint32_t *output) {
    return effect_checked_reference_read(context,effects,function,instruction,output);
}

static XrXirStatus effect_checked_reference_current(const XrXirCompileContext *context,
    const XrXirEffects *effects,const XrXirModule *module,
    uint32_t function,uint32_t instruction,uint32_t *output) {
    if (!module || !xir_effects_context_matches(context,effects,module->function_count) ||
        function>=module->function_count || instruction>=module->functions[function].instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirOp op=module->functions[function].instructions[instruction].op;
    if (op!=XR_XIR_FUNCTION_REF && op!=XR_XIR_CLASS_METHOD_REF) return XR_XIR_BAD_STRUCTURE;
    return effect_checked_reference_read(context,effects,function,instruction,output);
}

XR_FUNC XrXirStatus xir_effects_spawn_read(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t function, uint32_t instruction, XrXirRootEffects *output) {
    if (!output || !effects || !effects->checked_roots || function >= effects->count ||
        !xir_effects_context_matches(context, effects, effects->count)) return XR_XIR_BAD_STRUCTURE;
    const EffectCheckedRoots *roots = effects->checked_roots;
    EffectCheckFunction row = roots->function[function];
    if (instruction >= row.values - row.parameters) return XR_XIR_BAD_STRUCTURE;
    const EffectCheckNode *node = &roots->node[row.first + row.parameters + instruction];
    if (!node->spawn) return XR_XIR_BAD_STRUCTURE;
    *output = (XrXirRootEffects){!!(node->closed & 1), !!(node->closed & 2)};
    return node->closed || node->worker_blocked ? XR_XIR_BAD_TYPE : XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_scalar_witness(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t interface, uint32_t member, XrXirType receiver, uint32_t *output) {
    if (!output || !effects || !effect_final_outcomes(effects) || !effects->checked_roots ||
        !xir_effects_context_matches(context, effects, effects->count)) return XR_XIR_BAD_STRUCTURE;
    const EffectCheckedRoots *roots = effects->checked_roots;
    if (member || interface != roots->stringable_interface ||
        (receiver != XR_XIR_I64 && receiver != XR_XIR_STRING)) return XR_XIR_BAD_TYPE;
    uint32_t function = receiver == XR_XIR_I64 ? roots->stringable_integer : roots->stringable_string;
    if (function >= roots->templates) return XR_XIR_BAD_STRUCTURE;
    *output = function;
    return XR_XIR_OK;
}

typedef struct EffectCheckTraceWalk {
    const EffectCheckedRoots *roots;
    uint8_t *visited;
    XrXirRootCauseStep *steps;
    uint32_t count;
} EffectCheckTraceWalk;

static XrXirStatus effect_checked_trace_next(EffectCheckTraceWalk *walk,
    uint32_t node, uint32_t bit, uint32_t *next, XrXirRootEffectWitness *witness) {
    const EffectCheckedRoots *roots = walk->roots;
    EffectCheckFunction function = roots->function[roots->node[node].function];
    uint32_t wanted = 1u << bit;
    *next = UINT32_MAX;
    if (effect_check_bits(roots, node)[0] & wanted) {
        uint32_t reason = roots->node[node].reason[bit];
        if (reason >= roots->nodes) return XR_XIR_BAD_STRUCTURE;
        if (reason == node) {
            *witness = roots->terminals[(size_t)node * 2 + bit];
            return witness->cause ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
        }
        *next = reason;
        if (roots->node[reason].function != roots->node[node].function) {
            *witness = (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_CALL, UINT32_MAX,
                roots->function[roots->node[reason].function].declaration, UINT32_MAX, 0};
            for (uint32_t c = roots->node[node].calls; c != UINT32_MAX; c = roots->call[c].next)
                if (roots->call[c].target == reason) {
                    witness->instruction = roots->call[c].instruction;
                    if (roots->call[c].protocol == 4) witness->cause = XR_XIR_ROOT_CAUSE_CLEANUP;
                    break;
                }
        }
        return XR_XIR_OK;
    }
    if (node < function.first + function.parameters) {
        uint32_t parameter = node - function.first;
        *witness = (XrXirRootEffectWitness){roots->node[node].cell_parameter ?
            XR_XIR_ROOT_CAUSE_CELL_PARAMETER : XR_XIR_ROOT_CAUSE_PARAMETER,
            UINT32_MAX, UINT32_MAX, parameter, 0};
        for (uint32_t b = roots->node[node].bindings; b != UINT32_MAX; b = roots->binding[b].next) {
            uint32_t source = roots->binding[b].source;
            if ((roots->node[source].closed & wanted) && !walk->visited[source]) { *next = source; break; }
        }
        return XR_XIR_OK;
    }
    for (uint32_t a = roots->node[node].aliases; a != UINT32_MAX; a = roots->alias[a].next) {
        if (!roots->alias[a].root) continue;
        uint32_t source = roots->alias[a].source;
        if ((roots->node[source].closed & wanted) && !walk->visited[source]) { *next = source; return XR_XIR_OK; }
    }
    for (uint32_t c = roots->node[node].calls; c != UINT32_MAX; c = roots->call[c].next) {
        EffectCheckCall call = roots->call[c];
        EffectCheckFunction callee = roots->function[roots->node[call.target].function];
        const uint64_t *bits = effect_check_bits(roots, call.target);
        for (uint64_t p = 0; p < (uint64_t)callee.parameters * 2; ++p) {
            if (!(bits[(p + 2) / 64] & (UINT64_C(1) << ((p + 2) % 64)))) continue;
            uint32_t parameter = (uint32_t)(p % callee.parameters);
            if (parameter >= call.count) continue;
            uint32_t source = roots->argument[call.first + parameter];
            if (!(roots->node[source].closed & wanted) || walk->visited[source]) continue;
            *next = source;
            *witness = (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_PARAMETER,
                call.instruction, callee.declaration, parameter, 0};
            return XR_XIR_OK;
        }
    }
    if (bit && roots->node[node].deferred) {
        uint32_t instruction = UINT32_MAX;
        for (uint32_t p = 0; p < function.pending_count; ++p) {
            const EffectCheckPending *pending = &roots->pending[function.pending_first + p];
            if (!pending->linked && pending->node == node) {
                instruction = pending->instruction;
                break;
            }
        }
        *witness = (XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_CONTEXT_CALL,
            instruction, UINT32_MAX, UINT32_MAX, 0};
        return XR_XIR_OK;
    }
    return XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_checked_trace_domain(EffectCheckTraceWalk *walk, uint32_t node, uint32_t bit) {
    const EffectCheckedRoots *roots = walk->roots;
    memset(walk->visited, 0, roots->nodes);
    uint32_t first = walk->count;
    while (node != UINT32_MAX) {
        if (node >= roots->nodes || walk->visited[node]) return XR_XIR_BAD_STRUCTURE;
        walk->visited[node] = 1;
        XrXirRootEffectWitness witness = {0};
        uint32_t next = UINT32_MAX;
        XrXirStatus status = effect_checked_trace_next(walk, node, bit, &next, &witness);
        if (status != XR_XIR_OK) return status;
        if (witness.cause) {
            walk->steps[walk->count++] = (XrXirRootCauseStep){
                roots->function[roots->node[node].function].declaration,
                witness.instruction, witness.callee, witness.slot, 0, witness.cause};
        }
        node = next;
    }
    if (walk->count == first) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t s = first; s < walk->count; ++s) walk->steps[s].distance = walk->count - s - 1;
    return XR_XIR_OK;
}

static XrXirStatus effect_checked_trace_copy(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t function, XrXirRootCauseTrace **output) {
    if (!output || *output || !effects || !effect_final_outcomes(effects) || function >= effects->count ||
        !xir_effects_context_matches(context, effects, effects->count)) return XR_XIR_BAD_STRUCTURE;
    const EffectCheckedRoots *roots = effects->checked_roots;
    uint64_t capacity = (uint64_t)roots->nodes * 2;
    if (capacity > UINT32_MAX || capacity > (SIZE_MAX - sizeof(XrXirRootCauseTrace)) / sizeof(XrXirRootCauseStep))
        return XR_XIR_OVERFLOW;
    XrXirStatus status = XR_XIR_OK;
    EffectCheckTraceWalk walk = {.roots = roots};
    walk.visited = xir_compile_calloc(context, roots->nodes, sizeof(*walk.visited), &status);
    walk.steps = xir_compile_alloc(context, (size_t)capacity * sizeof(*walk.steps), &status);
    XrXirRootCauseTrace *trace = NULL;
    uint32_t counts[2] = {0};
    uint32_t node = roots->function[function].root;
    for (uint32_t bit = 0; status == XR_XIR_OK && bit < 2; ++bit) {
        uint32_t first = walk.count;
        if (roots->node[node].closed & (1u << bit)) status = effect_checked_trace_domain(&walk, node, bit);
        counts[bit] = walk.count - first;
    }
    if (status == XR_XIR_OK) trace = xir_compile_alloc(context,
        sizeof(*trace) + (size_t)walk.count * sizeof(*walk.steps), &status);
    if (trace) {
        trace->facts = effects->final_outcomes->root[function];
        trace->counts[0] = counts[0];
        trace->counts[1] = counts[1];
        if (walk.count) memcpy(effect_root_trace_storage(trace), walk.steps, (size_t)walk.count * sizeof(*walk.steps));
        *output = trace;
    }
    xr_compile_resources_free(walk.visited);
    xr_compile_resources_free(walk.steps);
    return status;
}

static uint8_t effect_checked_suspend_mask(const EffectCheckedRoots *roots, uint32_t node) {
    const EffectCheckFunction *function = &roots->function[roots->node[node].function];
    const uint64_t *bits = effect_check_suspend_bits(roots, node);
    uint8_t mask = (uint8_t)(bits[0] & 3u);
    for (uint64_t p = 0; p < (uint64_t)function->parameters * 2; ++p) {
        if (!(bits[(p + 2) / 64] & (UINT64_C(1) << ((p + 2) % 64)))) continue;
        uint32_t parameter = (uint32_t)(p % function->parameters);
        uint32_t source = p < function->parameters ? function->first + parameter : function->contents + parameter;
        mask |= roots->node[source].suspend_bound;
    }
    return mask;
}

static XrXirStatus effect_checked_control_project(const XrXirCompileContext *memory,
    const EffectCheckedRoots *roots, EffectControlSummary **output) {
    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    EffectControlSummary *summary = xir_compile_calloc(memory, 1, sizeof(*summary), &status);
    if (!summary) return status;
    summary->resources = memory->resources;
    summary->functions = roots->templates;
    summary->facts = xir_compile_calloc(memory, summary->functions, sizeof(*summary->facts), &status);
    summary->causes = xir_compile_calloc(memory, summary->functions, sizeof(*summary->causes), &status);
    summary->task_creation = xir_compile_calloc(memory, summary->functions, sizeof(*summary->task_creation), &status);
    for (uint32_t f = 0; status == XR_XIR_OK && f < summary->functions; ++f) {
        const EffectCheckFunction *function = &roots->function[f];
        uint32_t node = function->root;
        uint8_t mask = effect_checked_suspend_mask(roots, node);
        summary->facts[f] = (XrXirFunctionEffects){mask & 2 ? XR_XIR_EFFECT_MAY :
            mask & 1 ? XR_XIR_EFFECT_UNKNOWN : XR_XIR_EFFECT_NONE, (XrXirEffect)function->throws};
        summary->task_creation[f] = roots->node[node].task_creation ? XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_NONE;
        if (!mask) continue;
        uint32_t bit = mask & 2 ? 1 : 0;
        const EffectCheckNode *reason = &roots->node[node];
        XrXirEffectWitness *witness = &summary->causes[f];
        witness->callee = UINT32_MAX;
        witness->instruction = UINT32_MAX;
        witness->distance = reason->suspend_distance[bit];
        if (!(effect_check_suspend_bits(roots, node)[0] & (1u << bit))) {
            witness->cause = XR_XIR_EFFECT_CAUSE_INDIRECT;
            witness->distance = 0;
            continue;
        }
        uint32_t current = node;
        for (uint32_t step = 0; step < roots->nodes; ++step) {
            reason = &roots->node[current];
            uint32_t call = reason->suspend_call[bit];
            if (call != UINT32_MAX) {
                witness->cause = XR_XIR_EFFECT_CAUSE_CALL;
                witness->instruction = roots->call[call].instruction;
                witness->callee = roots->function[roots->node[roots->call[call].target].function].declaration;
                break;
            }
            uint32_t next = reason->suspend_reason[bit];
            if (next == current) {
                witness->cause = bit ? XR_XIR_EFFECT_CAUSE_SUSPEND : XR_XIR_EFFECT_CAUSE_INDIRECT;
                witness->instruction = reason->suspend_instruction[bit];
                break;
            }
            if (next >= roots->nodes) { status = XR_XIR_BAD_STRUCTURE; break; }
            current = next;
        }
        if (!witness->cause) status = XR_XIR_BAD_STRUCTURE;
    }
    if (status != XR_XIR_OK) { xir_effect_control_free(summary); return status; }
    *output = summary;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_suspend_trace(const XrXirEffects *effects,
    uint32_t function, XirSuspendCauseVisitor visit, void *context) {
    if (!effects || !effect_final_outcomes(effects) || function >= effects->count || !visit)
        return XR_XIR_BAD_STRUCTURE;
    const EffectCheckedRoots *roots = effects->checked_roots;
    uint32_t node = roots->function[function].root;
    uint8_t mask = effect_checked_suspend_mask(roots, node);
    if (!mask) return XR_XIR_OK;
    uint32_t bit = mask & 2 ? 1 : 0;
    const uint64_t *bits = effect_check_suspend_bits(roots, node);
    if (!(bits[0] & (1u << bit))) {
        const EffectCheckFunction *body = &roots->function[function];
        for (uint64_t p = 0; p < (uint64_t)body->parameters * 2; ++p) {
            if (!(bits[(p + 2) / 64] & (UINT64_C(1) << ((p + 2) % 64)))) continue;
            uint32_t parameter = (uint32_t)(p % body->parameters);
            uint32_t source = p < body->parameters ? body->first + parameter : body->contents + parameter;
            if (!(roots->node[source].suspend_bound & (1u << bit))) continue;
            XirSuspendCauseStep step = {function, UINT32_MAX, function, parameter, true, true, false};
            return visit(context, &step);
        }
        return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t count = 0; count < roots->nodes; ++count) {
        const EffectCheckNode *reason = &roots->node[node];
        uint32_t current = roots->function[reason->function].declaration;
        uint32_t call_index = reason->suspend_call[bit];
        uint32_t next = reason->suspend_reason[bit];
        if (call_index != UINT32_MAX) {
            if (call_index >= roots->calls) return XR_XIR_BAD_STRUCTURE;
            const EffectCheckCall *call = &roots->call[call_index];
            uint32_t callee = roots->function[roots->node[call->target].function].declaration;
            XirSuspendCauseStep step = {current, call->instruction, callee,
                reason->suspend_parameter[bit], next == node, reason->suspend_parameter[bit] != UINT32_MAX, false};
            XrXirStatus status = visit(context, &step);
            if (status != XR_XIR_OK || next == node) return status;
        } else if (next == node) {
            XirSuspendCauseStep step = {current, reason->suspend_instruction[bit], UINT32_MAX,
                UINT32_MAX, true, false, bit != 0};
            return visit(context, &step);
        }
        if (next >= roots->nodes) return XR_XIR_BAD_STRUCTURE;
        node = next;
    }
    return XR_XIR_BAD_STRUCTURE;
}

XR_FUNC XrXirStatus xir_effects_prepare_verified_input(const XrXirCompileContext *memory,
    XirEffectInput input, const XrXirRootRefiner *refiner, XrXirEffects **output) {
    if (!xir_compile_context_valid(memory) || !input.module || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = effect_input_shape(memory, input);
    if (status != XR_XIR_OK) return status;
    XrXirEffects *effects = xir_compile_calloc(memory, 1, sizeof(*effects), &status);
    if (!effects) return status;
    atomic_init(&effects->references, 1);
    effects->resources = memory->resources;
    effects->count = input.module->function_count;
    effects->outcomes_owner = xir_compile_calloc(memory, 1, sizeof(*effects->outcomes_owner), &status);
    if (status == XR_XIR_OK) status = effect_checked_parameters(memory, input, effects);
    if (status == XR_XIR_OK && refiner && refiner->begin)
        status = refiner->begin(refiner->context, effects);
    if (status == XR_XIR_OK) status = effect_checked_roots_build(memory, input.module,
        effects->outcomes_work.contracts, input.control, refiner, &effects->checked_roots, false);
    if (status == XR_XIR_OK && refiner && refiner->prepare)
        status = refiner->prepare(refiner->context, effects->checked_roots, effects->checked_roots->storage);
    if (status != XR_XIR_OK) {
        xr_xir_compile_effects_free(effects);
        return status;
    }
    *output = effects;
    return XR_XIR_OK;
}

XR_FUNC const XirExecutionOwner *xir_effects_prepared_execution(const XrXirEffects *effects) {
    return effects && effects->outcomes_owner && !effects->final_outcomes ? effects->checked_roots : NULL;
}

XR_FUNC XirStorageFlow *xir_effects_prepared_storage(const XrXirEffects *effects) {
    const EffectCheckedRoots *roots = xir_effects_prepared_execution(effects);
    return roots ? roots->storage : NULL;
}

XR_FUNC XrXirStatus xir_effects_finish_prepared_input(const XrXirCompileContext *memory,
    XirEffectInput input, const XrXirRootRefiner *refiner, XrXirEffects **pending, XrXirEffects **output) {
    if (!pending || !*pending) return XR_XIR_BAD_STRUCTURE;
    bool aliased = pending == output;
    XrXirEffects *effects = *pending;
    *pending = NULL;
    XrXirStatus status = !output || aliased || *output ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
    if (status == XR_XIR_OK && (!input.module || !xir_effects_context_matches(memory, effects, input.module->function_count) ||
        !effects->outcomes_owner || effects->input_facts || !effects->checked_roots || effects->final_outcomes))
        status = XR_XIR_BAD_STRUCTURE;
    if (status == XR_XIR_OK && effects->checked_roots->storage) {
        XirStorageFlowView storage = {0};
        status = xir_storage_flow_seal(effects->checked_roots->storage, &storage);
        if (status == XR_XIR_OK) status = xir_execution_refresh_storage(effects->checked_roots);
        uint32_t calls = effects->checked_roots->calls;
        EffectCheckBuild links = {.memory = memory, .roots = effects->checked_roots, .control = input.control};
        for (uint32_t p = 0; status == XR_XIR_OK && p < effects->checked_roots->pending_count; ++p)
            if (effects->checked_roots->pending[p].kind == EFFECT_CHECK_HIGHER_ORDER &&
                !effect_check_link_higher_order(&links, &effects->checked_roots->pending[p])) status = links.status;
        if (status == XR_XIR_OK && calls != effects->checked_roots->calls) status = XR_XIR_UNRESOLVED;
    }
    if (status == XR_XIR_OK) status = effect_input_facts_new(memory, input, &effects->input_facts);
    EffectCheckBuild build = {.memory = memory, .roots = effects->checked_roots, .control = input.control};
    if (status == XR_XIR_OK) status = effect_check_cell_roles(&build);
    if (status == XR_XIR_OK) status = effect_check_close(&build);
    if (status == XR_XIR_OK && refiner && refiner->admit) {
        XrXirDiagnostic diagnostic={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
        status=refiner->admit(refiner->context,input.module,effects,&diagnostic);
    }
    if (status == XR_XIR_OK) status = effect_checked_project(memory, effects);
    if (status == XR_XIR_OK && refiner && refiner->update) {
        bool changed = false;
        status = refiner->update(refiner->context, effects, &changed);
        if (status == XR_XIR_OK && XR_XIR_PARANOID && changed) {
            changed = false;
            status = refiner->update(refiner->context, effects, &changed);
            XR_CHECK(status != XR_XIR_OK || !changed, "Refinement of the closed SSA summary is idempotent");
        }
    }
    EffectFinalOutcomes *records = &effects->outcomes_work;
    if (status == XR_XIR_OK) status = effect_checked_control_project(memory,
        effects->checked_roots, &records->control_summary);
    if (status == XR_XIR_OK) {
        records->functions = records->control_summary->facts;
        records->witnesses = records->control_summary->causes;
        records->task_creation = records->control_summary->task_creation;
        status = xir_compile_local_errors_verify(memory, input.module, NULL);
    }
    if (status != XR_XIR_OK) {
        xr_xir_compile_effects_free(effects);
        return status;
    }
    effects->checked_roots->borrowed_types = NULL;
    *output = effects;
    return XR_XIR_OK;
}

static XrXirStatus effect_checked_construct(const XrXirCompileContext *memory,
    XirEffectInput input, const XrXirRootRefiner *refiner, XrXirEffects **output) {
    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirEffects *pending = NULL;
    XrXirStatus status = xir_effects_prepare_verified_input(memory, input, refiner, &pending);
    if (status == XR_XIR_OK)
        status = xir_effects_finish_prepared_input(memory, input, refiner, &pending, output);
    xr_xir_compile_effects_free(pending);
    return status;
}
