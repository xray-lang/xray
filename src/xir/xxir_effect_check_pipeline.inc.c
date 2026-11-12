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
#include "xxir_view_loan_internal.h"
#include "xxir_view_liveness_internal.h"
#include "xxir_view_types_internal.h"
#include "xxir_view_access_internal.h"

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
        const XrXirFunction *function = &module->functions[f];
        uint32_t count = function->parameter_count;
        contracts[f].parameter_count = count;
        XrXirEffectParameter *parameters = count ? xir_compile_calloc(memory, count,
            sizeof(*parameters), &status) : NULL;
        if (count && !parameters) return status;
        contracts[f].parameters = parameters;
        for (uint32_t p = 0; p < count; ++p)
            parameters[p].kind = variable && xr_xir_type_is_callable(module->types, function->parameters[p]) &&
                !xr_xir_type_is_c_callable(module->types, function->parameters[p]) &&
                !xr_xir_function_parameter_escaping(function, p) ?
                XR_XIR_EFFECT_PARAMETER_VARIABLE : XR_XIR_EFFECT_PARAMETER_FIXED;
    }
    return status;
}

static XrXirStatus effect_checked_bound_cause(const XrXirCompileContext *memory,
    const EffectCheckedRoots *roots, uint32_t node, bool unresolved, EffectCheckCause *output);

static XrXirStatus effect_checked_project(const XrXirCompileContext *memory, XrXirEffects *effects) {
    EffectFinalOutcomes *records = &effects->outcomes_work;
    EffectCheckedRoots *roots = effects->checked_roots;
    XrXirStatus status = XR_XIR_OK;
    if (!records->root) records->root = xir_compile_calloc(memory, effects->count, sizeof(*records->root), &status);
    if (!records->root_witnesses) records->root_witnesses = xir_compile_calloc(memory, effects->count, sizeof(*records->root_witnesses), &status);
    if (!records->unresolved_witnesses) records->unresolved_witnesses = xir_compile_calloc(memory, effects->count,
        sizeof(*records->unresolved_witnesses), &status);
    if (!records->go_safe) records->go_safe = xir_compile_calloc(memory, effects->count, sizeof(*records->go_safe), &status);
    if (!records->root || !records->root_witnesses || !records->unresolved_witnesses || !records->go_safe) return status;
    for (uint32_t f = 0; f < effects->count; ++f) {
        uint32_t node = roots->function[f].root;
        uint8_t mask = roots->node[node].closed;
        records->root[f] = (XrXirRootEffects){!!(mask & 1), !!(mask & 2)};
        records->go_safe[f] = mask || roots->node[node].worker_blocked ? XR_XIR_BAD_TYPE : XR_XIR_OK;
        records->root_witnesses[f] = records->unresolved_witnesses[f] = (XrXirRootEffectWitness){0};
        XrXirRootFormula *formula = &records->contracts[f].formula;
        xr_compile_resources_free((void *)formula->terms);
        *formula = (XrXirRootFormula){0};
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
            if (status == XR_XIR_UNRESOLVED && !roots->node[node].deferred)
                status = effect_checked_bound_cause(memory, roots, node, bit != 0, &cause);
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

XR_FUNC XrXirStatus xir_effects_view_loan_facts(const XrXirCompileContext *context,
    const XrXirEffects *effects, XirViewLoanFacts *output) {
    if (!output || !effects || !effects->checked_roots ||
        !xir_effects_context_matches(context, effects, effects->count)) return XR_XIR_BAD_STRUCTURE;
    const EffectCheckedRoots *roots = effects->checked_roots;
    if (!roots->storage || roots->restrictions.view.function_count != effects->count)
        return XR_XIR_BAD_STRUCTURE;
    XirViewLoanFacts view = {.restrictions = roots->restrictions.view, .execution = roots};
    XrXirStatus status = xir_storage_flow_view(roots->storage, &view.storage);
    if (status != XR_XIR_OK) return status;
    if (view.storage.function_count != effects->count) return XR_XIR_BAD_STRUCTURE;
    *output = view;
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
        for (uint64_t p = 0; p < (uint64_t)callee.parameters * 3; ++p) {
            if (!(bits[(p + 2) / 64] & (UINT64_C(1) << ((p + 2) % 64)))) continue;
            uint32_t parameter = (uint32_t)(p % callee.parameters);
            if (parameter >= call.count) continue;
            uint32_t source = roots->argument[call.first + parameter];
            if (p >= (uint64_t)callee.parameters * 2) source = effect_check_actual_contents(roots, source);
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

/* A hidden capture can obtain its closed bit from an actual outer binding
 * after its local symbolic Cell term was discharged. Read that same numeric
 * relation used by full traces without turning it into a constant effect. */
static XrXirStatus effect_checked_bound_cause(const XrXirCompileContext *memory,
    const EffectCheckedRoots *roots, uint32_t node, bool unresolved, EffectCheckCause *output) {
    XrXirStatus status = XR_XIR_OK;
    EffectCheckTraceWalk walk = {.roots = roots};
    walk.visited = xir_compile_calloc(memory, roots->nodes, sizeof(*walk.visited), &status);
    if (!walk.visited) return status;
    uint32_t function = roots->function[roots->node[node].function].declaration;
    status = XR_XIR_BAD_STRUCTURE;
    while (node != UINT32_MAX && node < roots->nodes && !walk.visited[node]) {
        walk.visited[node] = 1;
        uint32_t next = UINT32_MAX;
        XrXirRootEffectWitness witness = {0};
        status = effect_checked_trace_next(&walk, node, unresolved, &next, &witness);
        if (status != XR_XIR_OK) break;
        if (witness.cause) {
            *output = (EffectCheckCause){.function = function, .witness = witness};
            break;
        }
        node = next;
        status = XR_XIR_BAD_STRUCTURE;
    }
    xr_compile_resources_free(walk.visited);
    return status;
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
    summary->source_nodes = xir_compile_calloc(memory, summary->functions, sizeof(*summary->source_nodes), &status);
    for (uint32_t f = 0; status == XR_XIR_OK && f < summary->functions; ++f)
        summary->source_nodes[f] = roots->function[f].cleanup ? UINT32_MAX : roots->function[f].root;
    /* A cleanup's hidden parameters describe its registration captures. Its
     * declaration obligation consumes those actual applications, while the
     * shared function equations retain their original symbolic parameters. */
    for (uint32_t c = 0; status == XR_XIR_OK && c < roots->calls; ++c) {
        EffectCheckCall call = roots->call[c];
        EffectCheckFunction callee = roots->function[roots->node[call.target].function];
        if (call.protocol != 4 || !callee.cleanup || call.target != callee.root ||
            callee.declaration >= summary->functions) continue;
        uint32_t f = callee.declaration, selected = summary->source_nodes[f];
        if (call.count != callee.parameters || roots->node[call.output].cleanup_call != c) {
            status = XR_XIR_BAD_STRUCTURE;
            break;
        }
        uint8_t mask = effect_checked_suspend_mask(roots, call.output);
        uint8_t previous = selected == UINT32_MAX ? 0 : effect_checked_suspend_mask(roots, selected);
        if (selected == UINT32_MAX || ((mask & 2) && !(previous & 2)) || (!previous && mask))
            summary->source_nodes[f] = call.output;
        if (roots->node[call.output].task_creation) summary->task_creation[f] = XR_XIR_EFFECT_MAY;
    }
    for (uint32_t f = 0; status == XR_XIR_OK && f < summary->functions; ++f) {
        const EffectCheckFunction *function = &roots->function[f];
        uint32_t node = summary->source_nodes[f];
        if (node == UINT32_MAX) node = summary->source_nodes[f] = function->root;
        uint8_t mask = effect_checked_suspend_mask(roots, node);
        summary->facts[f] = (XrXirFunctionEffects){mask & 2 ? XR_XIR_EFFECT_MAY :
            mask & 1 ? XR_XIR_EFFECT_UNKNOWN : XR_XIR_EFFECT_NONE, (XrXirEffect)function->throws};
        if (roots->node[node].task_creation) summary->task_creation[f] = XR_XIR_EFFECT_MAY;
        if (!mask) continue;
        uint32_t bit = mask & 2 ? 1 : 0;
        uint32_t source = node;
        if (function->cleanup &&
            (effect_check_suspend_bits(roots, function->root)[0] & (UINT64_C(1) << bit)))
            source = function->root;
        const EffectCheckNode *reason = &roots->node[source];
        XrXirEffectWitness *witness = &summary->causes[f];
        witness->callee = UINT32_MAX;
        witness->instruction = UINT32_MAX;
        witness->distance = reason->suspend_distance[bit];
        if (!(effect_check_suspend_bits(roots, source)[0] & (UINT64_C(1) << bit))) {
            witness->cause = XR_XIR_EFFECT_CAUSE_INDIRECT;
            witness->distance = 0;
            continue;
        }
        uint32_t current = source;
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
        if (function->cleanup && roots->function[roots->node[source].function].declaration != function->declaration)
            witness->instruction = UINT32_MAX;
    }
    if (status != XR_XIR_OK) { xir_effect_control_free(summary); return status; }
    *output = summary;
    return XR_XIR_OK;
}

/* Symbolic application has already closed in the shared equations. Recover
 * intermediate helper names from those same edges when a cleanup capture
 * supplies the eventual constant cause. No facts are changed by presentation. */
typedef struct EffectSuspendParameterTrace {
    XirSuspendCauseVisitor visit;
    void *context;
    uint32_t creator;
    bool emitted;
} EffectSuspendParameterTrace;

static XrXirStatus effect_checked_suspend_parameter_trace(const EffectCheckedRoots *roots,
    uint32_t node, uint32_t term, EffectSuspendParameterTrace *walk) {
    XrXirCompileContext memory = {.resources = roots->resources};
    XrXirStatus status = XR_XIR_OK;
    uint32_t *scratch = xir_compile_calloc(&memory, roots->nodes, 3 * sizeof(*scratch), &status);
    if (!scratch) return status;
    uint32_t *queue = scratch, *visited = scratch + roots->nodes, *seen = visited + roots->nodes;
    for (uint32_t epoch = 1; epoch <= roots->nodes; ++epoch) {
        EffectCheckFunction function = roots->function[roots->node[node].function];
        if (term >= (uint64_t)function.parameters * 2) { status = XR_XIR_BAD_STRUCTURE; break; }
        uint32_t parameter = term % function.parameters;
        uint32_t seed = term < function.parameters ? function.first + parameter : function.contents + parameter;
        if (seen[seed]) break;
        seen[seed] = 1;
        uint32_t count = 1, next_node = UINT32_MAX, next_term = UINT32_MAX;
        queue[0] = node;
        visited[node] = epoch;
        for (uint32_t cursor = 0; cursor < count && next_node == UINT32_MAX; ++cursor) {
            uint32_t current = queue[cursor];
            for (uint32_t c = roots->node[current].calls; c != UINT32_MAX; c = roots->call[c].next) {
                EffectCheckCall call = roots->call[c];
                EffectCheckFunction callee = roots->function[roots->node[call.target].function];
                if (call.fixed_effects || call.protocol == 8 ||
                    (call.target == callee.result && !call.default_result)) continue;
                const uint64_t *bits = effect_check_suspend_bits(roots, call.target);
                for (uint32_t p = 0; p < callee.parameters * 2u; ++p) {
                    if (!(bits[(p + 2) / 64] & (UINT64_C(1) << ((p + 2) % 64)))) continue;
                    uint32_t ordinal = p % callee.parameters;
                    if (ordinal >= call.count) continue;
                    uint32_t actual = roots->argument[call.first + ordinal];
                    if (p >= callee.parameters) {
                        EffectCheckFunction caller = roots->function[roots->node[actual].function];
                        actual = caller.contents + actual - caller.first;
                    }
                    const uint64_t *argument = effect_check_suspend_bits(roots, actual);
                    if (!(argument[(term + 2) / 64] & (UINT64_C(1) << ((term + 2) % 64)))) continue;
                    XirSuspendCauseStep step = {walk->emitted ? function.declaration : walk->creator, call.instruction,
                        callee.declaration, ordinal, false, true, false};
                    status = walk->visit(walk->context, &step);
                    walk->emitted = true;
                    next_node = call.target;
                    next_term = p;
                    break;
                }
                if (next_node != UINT32_MAX) break;
            }
            for (uint32_t a = roots->node[current].aliases; next_node == UINT32_MAX &&
                a != UINT32_MAX; a = roots->alias[a].next) {
                EffectCheckAlias alias = roots->alias[a];
                if (!alias.suspension || visited[alias.source] == epoch ||
                    roots->node[alias.source].function != roots->node[current].function) continue;
                const uint64_t *bits = effect_check_suspend_bits(roots, alias.source);
                if (!(bits[(term + 2) / 64] & (UINT64_C(1) << ((term + 2) % 64)))) continue;
                visited[alias.source] = epoch;
                queue[count++] = alias.source;
            }
        }
        if (status != XR_XIR_OK || next_node == UINT32_MAX) break;
        node = next_node;
        term = next_term;
    }
    xr_compile_resources_free(scratch);
    return status;
}

XR_FUNC XrXirStatus xir_effects_suspend_trace(const XrXirEffects *effects,
    uint32_t function, XirSuspendCauseVisitor visit, void *context) {
    if (!effects || !effect_roots_read(effects)->control_summary || function >= effects->count || !visit)
        return XR_XIR_BAD_STRUCTURE;
    const EffectCheckedRoots *roots = effects->checked_roots;
    uint32_t node = effect_roots_read(effects)->control_summary->source_nodes[function];
    uint8_t mask = effect_checked_suspend_mask(roots, node);
    if (!mask) return XR_XIR_OK;
    uint32_t bit = mask & 2 ? 1 : 0;
    const uint64_t *bits = effect_check_suspend_bits(roots, node);
    if (!(bits[0] & (UINT64_C(1) << bit))) {
        const EffectCheckFunction *body = &roots->function[roots->node[node].function];
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
            EffectSuspendParameterTrace walk = {visit, context, current, false};
            XrXirStatus status = XR_XIR_OK;
            if (call->protocol == 4 && next != node && step.parameter_dependency && step.parameter < call->count) {
                uint32_t actual = roots->argument[call->first + step.parameter];
                EffectCheckFunction caller = roots->function[roots->node[actual].function];
                EffectCheckFunction target = roots->function[roots->node[call->target].function];
                uint32_t term = step.parameter;
                if (next == caller.contents + actual - caller.first) term += target.parameters;
                status = effect_checked_suspend_parameter_trace(roots, call->target, term, &walk);
                if (status != XR_XIR_OK) return status;
            }
            if (!walk.emitted) status = visit(context, &step);
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
    if (status == XR_XIR_OK && refiner && refiner->prepare) {
        XirRestrictionReceiver receiver = {effects->checked_roots, effect_check_restrictions_receive};
        status = refiner->prepare(refiner->context, effects->checked_roots, effects->checked_roots->storage, &receiver);
    }
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

#include "xxir_effect_refinement.inc.c"

XR_FUNC XrXirStatus xir_effects_defer_lifetime(const XrXirCompileContext *memory,
    XrXirEffects *effects, const XrXirDiagnostic *diagnostic) {
    if (!diagnostic || diagnostic->status != XR_XIR_UNRESOLVED ||
        !xir_effects_prepared_execution(effects) ||
        !xir_effects_context_matches(memory, effects, effects->count)) return XR_XIR_BAD_STRUCTURE;
    XrXirDiagnostic *pending = &effects->checked_roots->restrictions.view.pending;
    if (pending->status == XR_XIR_OK) *pending = *diagnostic;
    return XR_XIR_OK;
}

/* Declaration diagnostics borrow the sole closed constructor before its final
 * ownership move. Public queries still require successful publication. */
XR_FUNC XrXirEffectFacts xir_effects_declaration_read(const XrXirCompileContext *memory,
    const XrXirEffects *effects, uint32_t function, XrXirEffect *task_creation) {
    XrXirEffectFacts result = {.status=XR_XIR_BAD_STRUCTURE};
    if (!effects || !xir_effects_context_matches(memory,effects,effects->count) || function>=effects->count)
        return result;
    const EffectFinalOutcomes *records=effect_roots_read(effects);
    if (!records->control_summary || !records->root || !records->witnesses || !records->root_witnesses ||
        !records->unresolved_witnesses || !records->task_creation) return result;
    result.status=XR_XIR_OK; result.control=records->functions[function]; result.root=records->root[function];
    result.suspend_witness=records->witnesses[function]; result.root_witness=records->root_witnesses[function];
    result.unresolved_witness=records->unresolved_witnesses[function];
    if (task_creation) *task_creation=records->task_creation[function];
    return result;
}

static XrXirStatus effect_checked_task_transfers(const XrXirCompileContext *memory,
    const XrXirModule *module, const EffectCheckedRoots *roots, XrCompileRequestControl *control,
    XrXirDiagnostic *diagnostic) {
    uint32_t progress=0;
    for (uint32_t c=0;c<roots->calls;++c) {
        XrXirStatus status=xir_receive_step(control,&progress);
        if (status!=XR_XIR_OK) return status;
        EffectCheckCall call=roots->call[c];
        uint32_t caller=roots->node[call.output].function;
        if (call.protocol!=8 || caller>=roots->templates) continue;
        uint32_t callee=roots->function[roots->node[call.target].function].declaration;
        status=xir_compile_task_transfer_verified(memory,module,callee);
        if (status!=XR_XIR_OK) {
            if (diagnostic) *diagnostic=(XrXirDiagnostic){status,caller,
                roots->node[call.output].block,call.instruction,
                status==XR_XIR_BAD_TYPE ? XR_XIR_DIAGNOSTIC_TASK_SENDABLE : XR_XIR_DIAGNOSTIC_NONE};
            return status;
        }
    }
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_finish_prepared_input(const XrXirCompileContext *memory,
    XirEffectInput input, const XrXirRootRefiner *refiner, XrXirEffects **pending, XrXirEffects **output,
    XrXirDiagnostic *diagnostic) {
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
    if (status == XR_XIR_OK) {
        status = xir_compile_view_affine_loans_verified(memory,input.module,effects,input.control,diagnostic);
        if (status == XR_XIR_UNRESOLVED) {
            XrXirDiagnostic location=diagnostic ? *diagnostic :
                (XrXirDiagnostic){XR_XIR_UNRESOLVED,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
            location.status=XR_XIR_UNRESOLVED;
            status=xir_effects_defer_lifetime(memory,effects,&location);
        }
    }
    if (status == XR_XIR_OK && !effects->checked_roots->liveness_checked) {
        XirViewLiveRequest request={.diagnostic=diagnostic,.control=input.control};
        status=xir_compile_view_liveness_loans_verified(memory,input.module,effects,&request);
        if (status==XR_XIR_OK) effects->checked_roots->liveness_checked=true;
        if (status==XR_XIR_UNRESOLVED) {
            XrXirDiagnostic location=diagnostic ? *diagnostic :
                (XrXirDiagnostic){XR_XIR_UNRESOLVED,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
            location.status=XR_XIR_UNRESOLVED;
            status=xir_effects_defer_lifetime(memory,effects,&location);
        }
    }
    EffectCheckBuild build = {.memory = memory, .module = input.module,
        .roots = effects->checked_roots, .control = input.control};
    if (status == XR_XIR_OK) status = effect_check_effect_projection(&build);
    if (status == XR_XIR_OK) status = effect_check_cell_roles(&build);
    if (status == XR_XIR_OK) status = effect_check_close(&build);
    if (status==XR_XIR_OK && (!refiner || !refiner->admit) &&
        effects->checked_roots->restrictions.view.pending.status!=XR_XIR_OK)
        status=xir_compile_view_cell_access_verified_input(memory,input,effects,diagnostic);
    if (status == XR_XIR_OK && refiner && refiner->admit) {
        XrXirDiagnostic location={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
        status=refiner->admit(refiner->context,input.module,effects,&location);
        if (status!=XR_XIR_OK && diagnostic) *diagnostic=location;
    }
    if (status == XR_XIR_OK)
        status=effect_checked_task_transfers(memory,input.module,effects->checked_roots,input.control,diagnostic);
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
        status = xir_compile_local_errors_verify(memory, input.module, diagnostic);
        if (status==XR_XIR_OK) effects->local_errors_checked=true;
    }
    if (status == XR_XIR_OK && effects->checked_roots->restrictions.view.pending.status != XR_XIR_OK) {
        XrXirDiagnostic location={XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
        status=xir_compile_check_effect_declarations(memory,input,effects,&location);
        if (status==XR_XIR_BAD_TYPE && refiner && refiner->explain)
            refiner->explain(refiner->context,input.module,effects,&location);
        if (status==XR_XIR_OK) {status=XR_XIR_UNRESOLVED; location=effects->checked_roots->restrictions.view.pending;}
        if (diagnostic) *diagnostic=location;
    }
    if (status == XR_XIR_OK) status = effect_outcomes_publish(memory, input.module, effects);
    if (status != XR_XIR_OK) {
        xr_xir_compile_effects_free(effects);
        return status;
    }
    effects->checked_roots->borrowed_types = NULL;
    *output = effects;
    return XR_XIR_OK;
}

static XrXirStatus effect_checked_construct(const XrXirCompileContext *memory,
    XirEffectInput input, const XrXirRootRefiner *refiner, XrXirEffects **output, XrXirDiagnostic *diagnostic) {
    if (!output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirEffects *pending = NULL;
    XrXirStatus status = xir_effects_prepare_verified_input(memory, input, refiner, &pending);
    if (status == XR_XIR_OK)
        status = xir_effects_finish_prepared_input(memory, input, refiner, &pending, output, diagnostic);
    xr_xir_compile_effects_free(pending);
    return status;
}
