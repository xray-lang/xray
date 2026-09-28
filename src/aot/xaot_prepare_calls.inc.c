/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_prepare_calls.inc.c - Traversal-owned call facts for representation preparation
 *
 * KEY CONCEPT:
 *   Only AOT representation plans change while these borrowed call facts live.
 */

typedef struct PrepareCallFacts {
    XaotBoundaryFunctionCalls *functions;
    XaotBoundaryCallTargets *targets;
    uint32_t count;
} PrepareCallFacts;

static void prepare_call_facts_dispose(PrepareCallFacts *facts) {
    if (!facts)
        return;
    xr_free(facts->targets);
    xr_free(facts->functions);
    memset(facts, 0, sizeof(*facts));
}

static bool prepare_call_facts_build(const XaotBundle *bundle, PrepareCallFacts *output) {
    if (!bundle || !output || output->functions || output->targets || output->count ||
        (bundle->nfunc_plans && !bundle->func_plans) ||
        bundle->nfunc_plans > UINT32_MAX / sizeof(XaotBoundaryFunctionCalls))
        return false;
    uint64_t total = 0;
    for (uint32_t i = 0; i < bundle->nfunc_plans; ++i) {
        const XaotFuncPlan *plan = &bundle->func_plans[i];
        if (!plan->func)
            return false;
        if (plan->reachable)
            total += plan->func->next_value_id;
        if (total > UINT32_MAX / sizeof(XaotBoundaryCallTargets))
            return false;
    }
    PrepareCallFacts facts = {0};
    if (bundle->nfunc_plans) {
        facts.functions = xr_calloc(bundle->nfunc_plans, sizeof(*facts.functions));
        if (!facts.functions)
            return false;
    }
    if (total) {
        facts.targets = xr_calloc((size_t) total, sizeof(*facts.targets));
        if (!facts.targets) {
            prepare_call_facts_dispose(&facts);
            return false;
        }
    }
    uint32_t offset = 0;
    for (uint32_t i = 0; i < bundle->nfunc_plans; ++i) {
        const XaotFuncPlan *plan = &bundle->func_plans[i];
        uint32_t count = plan->reachable ? plan->func->next_value_id : 0;
        facts.functions[i] = (XaotBoundaryFunctionCalls) {
            .function = plan->func, .target_count = count,
            .targets = count ? facts.targets + offset : NULL};
        offset += count;
    }
    if (!xaot_boundary_resolve_call_batches(bundle, facts.functions, bundle->nfunc_plans)) {
        prepare_call_facts_dispose(&facts);
        return false;
    }
    facts.count = bundle->nfunc_plans;
    *output = facts;
    return true;
}

static const XaotBoundaryCallTargets *prepare_call_fact(
    const XaotBoundaryFunctionCalls *facts, const XiFunc *function, const XiValue *call) {
    if (!facts || facts->function != function || !call || call->id >= facts->target_count ||
        !facts->targets || facts->coverage == XAOT_BOUNDARY_FUNCTION_INVALID)
        return NULL;
    return &facts->targets[call->id];
}
