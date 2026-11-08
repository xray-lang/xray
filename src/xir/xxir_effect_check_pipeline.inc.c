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
    bool variable = module->provenance != NULL;
    for (uint32_t f = 0; f < effects->count; ++f) {
        uint32_t count = module->functions[f].parameter_count;
        contracts[f].parameter_count = count;
        XrXirEffectParameter *parameters = count ? xir_compile_calloc(memory, count,
            sizeof(*parameters), &status) : NULL;
        if (count && !parameters) return status;
        contracts[f].parameters = parameters;
        for (uint32_t p = 0; p < count; ++p)
            parameters[p].kind = variable && xr_xir_type_is_callable(module->types,
                module->functions[f].parameters[p]) ? XR_XIR_EFFECT_PARAMETER_VARIABLE : XR_XIR_EFFECT_PARAMETER_FIXED;
    }
    if (!variable) return XR_XIR_OK;
    EffectParameterGraph graph = {.work = memory, .module = *module, .control = input.control};
    status = effect_parameter_graph_prepare(&graph, contracts);
    if (status == XR_XIR_OK) status = effect_parameter_graph_collect(&graph);
    if (status == XR_XIR_OK) status = effect_parameter_graph_finish(&graph, contracts);
    effect_parameter_graph_free(&graph);
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

static XrXirStatus effect_checked_control_targets(void *owner, XrXirType receiver,
    uint32_t slot, XirClassMethodVisitor visit, void *data) {
    const EffectCheckedRoots *roots = owner;
    if (!roots || !visit || !roots->dispatch_index_capacity) return XR_XIR_BAD_STRUCTURE;
    uint32_t mask = roots->dispatch_index_capacity - 1;
    uint32_t at = effect_check_dispatch_hash(receiver, slot) & mask;
    for (uint32_t probe = 0; probe < roots->dispatch_index_capacity; ++probe) {
        uint32_t index = roots->dispatch_index[at];
        if (!index) return XR_XIR_BAD_STRUCTURE;
        EffectCheckDispatch row = roots->dispatch[index - 1];
        if (row.receiver == receiver && row.slot == slot) {
            for (uint32_t t = 0; t < row.count; ++t) {
                XrXirStatus status = visit(data, &roots->dispatch_target[row.first + t]);
                if (status != XR_XIR_OK) return status;
            }
            return XR_XIR_OK;
        }
        at = (at + 1) & mask;
    }
    return XR_XIR_BAD_STRUCTURE;
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

static XrXirStatus effect_checked_construct(const XrXirCompileContext *memory,
    XirEffectInput input, const XrXirRootRefiner *refiner, EffectControlSummary *control,
    XrXirEffects **output) {
    if (!xir_compile_context_valid(memory) || !input.module || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    XrXirEffects *effects = xir_compile_calloc(memory, 1, sizeof(*effects), &status);
    if (!effects) return status;
    atomic_init(&effects->references, 1);
    effects->resources = memory->resources;
    effects->count = input.module->function_count;
    effects->outcomes_owner = xir_compile_calloc(memory, 1, sizeof(*effects->outcomes_owner), &status);
    if (status == XR_XIR_OK) status = effect_input_facts_new(memory, input, &effects->input_facts);
    if (status == XR_XIR_OK) status = effect_checked_parameters(memory, input, effects);
    if (status == XR_XIR_OK && refiner && refiner->prepare)
        status = refiner->prepare(refiner->context, effects);
    if (status == XR_XIR_OK) status = effect_checked_roots_build(memory, input.module,
        effects->outcomes_work.contracts, input.control, &effects->checked_roots);
    if (status == XR_XIR_OK && refiner && refiner->admit) {
        XrXirDiagnostic diagnostic={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
        status=refiner->admit(refiner->context,input.module,effects,&diagnostic);
    }
    if (status == XR_XIR_OK) status = effect_checked_project(memory, effects);
    if (status == XR_XIR_OK && refiner) {
        bool changed = false;
        status = refiner->update(refiner->context, effects, &changed);
        if (status == XR_XIR_OK && XR_XIR_PARANOID && changed) {
            changed = false;
            status = refiner->update(refiner->context, effects, &changed);
            XR_CHECK(status != XR_XIR_OK || !changed, "Refinement of the closed SSA summary is idempotent");
        }
    }
    EffectFinalOutcomes *records = &effects->outcomes_work;
    EffectSummaryDispatchSource dispatch = {effects->checked_roots, effect_checked_control_targets};
    if (status == XR_XIR_OK) status = control ?
        xir_effect_control_retain(memory, control, &records->control_summary) :
        effect_control_summary_build_from(memory, input.module, input.control, &dispatch, &records->control_summary);
    if (status == XR_XIR_OK && control && XR_XIR_PARANOID)
        status = effect_control_summary_recheck(memory, input.module, control);
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
    *output = effects;
    return XR_XIR_OK;
}
