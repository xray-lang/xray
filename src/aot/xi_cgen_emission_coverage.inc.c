/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_emission_coverage.inc.c - Invocation-owned function coverage facts
 */

/* Facts borrow live functions but own their array. No admission object or
 * successful verification survives the synchronous emission that owns them. */
typedef struct CgEmissionCoverage {
    XaotBoundaryFunctionCalls *functions;
    uint32_t count;
} CgEmissionCoverage;

/* Match the TargetPlan construction metadata byte budget. The fact table is
 * smaller than the function rows it describes and adds no language limit. */
#define CG_EMISSION_COVERAGE_MAX_BYTES UINT32_MAX

static void cg_emission_coverage_dispose(CgEmissionCoverage *coverage) {
    if (!coverage)
        return;
    xr_free(coverage->functions);
    memset(coverage, 0, sizeof(*coverage));
}

static int cg_emission_coverage_compare(const void *left, const void *right) {
    uintptr_t a = (uintptr_t) ((const XaotBoundaryFunctionCalls *) left)->function;
    uintptr_t b = (uintptr_t) ((const XaotBoundaryFunctionCalls *) right)->function;
    return (a > b) - (a < b);
}

static bool cg_emission_coverage_build(const XaotBundle *bundle, CgEmissionCoverage *out) {
    if (!out || out->functions || out->count)
        return false;
    if (!bundle)
        return true;
    uint32_t count = bundle->nfunc_plans;
    if (count > CG_EMISSION_COVERAGE_MAX_BYTES / sizeof(XaotBoundaryFunctionCalls) ||
        (count && !bundle->func_plans) ||
        count > SIZE_MAX / sizeof(XaotBoundaryFunctionCalls))
        return false;
    XaotBoundaryFunctionCalls *functions = NULL;
    if (count) {
        functions = (XaotBoundaryFunctionCalls *) xr_calloc(count, sizeof(*functions));
        if (!functions)
            return false;
        for (uint32_t i = 0; i < count; i++)
            functions[i].function = bundle->func_plans[i].func;
        qsort(functions, count, sizeof(*functions), cg_emission_coverage_compare);
        for (uint32_t i = 0; i < count; i++) {
            if (!functions[i].function ||
                (i && functions[i - 1].function == functions[i].function)) {
                xr_free(functions);
                return false;
            }
        }
    }
    if (!xaot_boundary_resolve_call_batches(bundle, functions, count)) {
        xr_free(functions);
        return false;
    }
    out->functions = functions;
    out->count = count;
    return true;
}

static XaotBoundaryFunctionCoverage cg_emission_coverage_lookup(
    const CgEmissionCoverage *coverage, const XiFunc *function) {
    if (!coverage || !function)
        return XAOT_BOUNDARY_FUNCTION_INVALID;
    uintptr_t key = (uintptr_t) function;
    uint32_t low = 0, high = coverage->count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        const XaotBoundaryFunctionCalls *row = &coverage->functions[middle];
        uintptr_t candidate = (uintptr_t) row->function;
        if (candidate == key)
            return row->coverage;
        if (candidate < key)
            low = middle + 1;
        else
            high = middle;
    }
    return XAOT_BOUNDARY_FUNCTION_INVALID;
}
