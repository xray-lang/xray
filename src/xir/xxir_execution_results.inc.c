/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_execution_results.inc.c - Contextual identities of returned callables
 *
 * Call records pair their value equations with execution applications.
 * Returned identities carry the application context, and their physical
 * values carry the declared callable effect bounds.
 */
typedef struct ExecutionResultSlot { uint32_t application; bool ambiguous; } ExecutionResultSlot;

static bool execution_result_call(const EffectCheckedRoots *roots, EffectCheckCall call) {
    EffectCheckFunction callee = roots->function[roots->node[call.target].function];
    EffectCheckFunction caller = roots->function[roots->node[call.output].function];
    return call.protocol == 2 && call.target == callee.result && call.output >= caller.first &&
        call.output - caller.first < caller.values &&
        effect_check_callable_result(xir_execution_types(roots),roots->node[call.output].type);
}

static uint32_t execution_result_hash(const EffectCheckedRoots *roots, EffectCheckCall call) {
    uint32_t fields[] = {roots->node[call.output].function,roots->node[call.target].function,
        call.instruction,call.count,call.type_count,
        call.has_environment ? call.environment : XIR_EXECUTION_CURRENT_ENVIRONMENT};
    uint32_t hash = xr_hash_bytes(fields,sizeof(fields));
    if (call.count) hash ^= xr_hash_bytes(roots->argument+call.first,(size_t)call.count*sizeof(*roots->argument));
    if (call.type_count) hash ^= xr_hash_bytes(roots->type_argument+call.type_first,
        (size_t)call.type_count*sizeof(*roots->type_argument));
    return hash;
}

static bool execution_result_equal(const EffectCheckedRoots *roots, EffectCheckCall a, EffectCheckCall b) {
    return roots->node[a.output].function == roots->node[b.output].function &&
        roots->node[a.target].function == roots->node[b.target].function && a.instruction == b.instruction &&
        a.count == b.count && a.type_count == b.type_count &&
        (a.has_environment ? a.environment : XIR_EXECUTION_CURRENT_ENVIRONMENT) ==
            (b.has_environment ? b.environment : XIR_EXECUTION_CURRENT_ENVIRONMENT) &&
        (!a.count || !memcmp(roots->argument+a.first,roots->argument+b.first,(size_t)a.count*sizeof(*roots->argument))) &&
        (!a.type_count || !memcmp(roots->type_argument+a.type_first,roots->type_argument+b.type_first,
            (size_t)a.type_count*sizeof(*roots->type_argument)));
}

static bool execution_result_application(const EffectCheckedRoots *roots, const XirStorageFlowView *view,
    uint32_t application) {
    XirStorageApplication app = view->applications[application];
    if (app.kind != XIR_EXECUTION_CALL || app.protocol != 2 || app.outcome != XIR_EXECUTION_NORMAL ||
        app.site >= roots->calls) return false;
    EffectCheckCall call = roots->call[app.site];
    EffectCheckFunction callee = roots->function[roots->node[call.target].function];
    EffectCheckFunction caller = roots->function[roots->node[call.output].function];
    return call.protocol == 2 && call.target == callee.root && call.output == caller.root &&
        effect_check_callable_result(xir_execution_types(roots),roots->node[callee.result].type);
}

static XrXirStatus execution_result_edges(const XrXirCompileContext *memory, const EffectCheckedRoots *roots,
    const XirStorageFlowView *view, EffectCheckOrigins *origins) {
    if (!view->application_count) return XR_XIR_OK;
    uint64_t capacity = 1;
    while (capacity < (uint64_t)view->application_count*2 && capacity <= UINT32_MAX/2u) capacity *= 2;
    if (capacity < (uint64_t)view->application_count*2 || capacity > SIZE_MAX/sizeof(ExecutionResultSlot))
        return XR_XIR_OVERFLOW;
    XrXirStatus status = XR_XIR_OK;
    ExecutionResultSlot *slots = xir_compile_calloc(memory,(size_t)capacity,sizeof(*slots),&status);
    if (!slots) return status;
    uint32_t mask = (uint32_t)capacity-1;
    for (uint32_t a = 0; a < view->application_count; ++a) {
        if (!execution_result_application(roots,view,a)) continue;
        EffectCheckCall call = roots->call[view->applications[a].site];
        uint32_t slot = execution_result_hash(roots,call)&mask;
        while (slots[slot].application) {
            uint32_t previous = slots[slot].application-1;
            EffectCheckCall found = roots->call[view->applications[previous].site];
            if (execution_result_equal(roots,call,found)) break;
            slot = (slot+1)&mask;
        }
        if (slots[slot].application) slots[slot].ambiguous = true;
        else slots[slot].application = a+1;
    }
    for (uint32_t c = 0; c < roots->calls; ++c) {
        EffectCheckCall call = roots->call[c];
        if (!execution_result_call(roots,call)) continue;
        uint32_t slot = execution_result_hash(roots,call)&mask;
        while (slots[slot].application) {
            uint32_t application = slots[slot].application-1;
            EffectCheckCall found = roots->call[view->applications[application].site];
            if (execution_result_equal(roots,call,found)) {
                if (slots[slot].ambiguous) break;
                uint32_t function = roots->node[call.target].function;
                EffectCheckFunction callee = roots->function[function];
                EffectCheckFunction caller = roots->function[roots->node[call.output].function];
                uint32_t value = call.output-caller.first;
                uint32_t target = origins->offsets[caller.declaration]+value;
                origins->edges[origins->count] = (EffectCheckOriginEdge){function,callee.result-callee.first,
                    application,origins->heads[target],false,true};
                origins->heads[target] = origins->count++;
                break;
            }
            slot = (slot+1)&mask;
        }
    }
    xr_compile_resources_free(slots);
    return status;
}
