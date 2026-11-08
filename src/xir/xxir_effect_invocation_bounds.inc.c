/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_bounds.inc.c - Original owned physical advertisements
 *
 * KEY CONCEPT:
 *   Capture real copied declarations before any private reference bottom or
 *   scalar refinement. A later narrower SSA type cannot replace this bound.
 */
typedef struct EffectInvocationValueBound {
    uint32_t mask;
    bool callable;
    uint32_t value;
} EffectInvocationValueBound;

typedef struct EffectInvocationFunctionBounds {
    uint32_t values,count;
    EffectInvocationValueBound *bounds;
} EffectInvocationFunctionBounds;

typedef struct EffectInvocationDeclaredBounds {
    XrCompileResources *resources;
    uint32_t count,capacity;
    EffectInvocationFunctionBounds *functions;
} EffectInvocationDeclaredBounds;

static void effect_invocation_bounds_free(EffectInvocationDeclaredBounds *bounds) {
    if (!bounds) return;
    for (uint32_t f=0;f<bounds->count;++f)
        xr_compile_resources_free(bounds->functions[f].bounds);
    xr_compile_resources_free(bounds->functions);xr_compile_resources_free(bounds);
}

static XrXirStatus effect_invocation_bounds_new(const XrXirCompileContext *work,
    EffectInvocationDeclaredBounds **output) {
    if (!xir_compile_context_valid(work) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    EffectInvocationDeclaredBounds *bounds=xir_compile_calloc(work,1,sizeof(*bounds),&status);
    if (!bounds) return status;
    bounds->resources=work->resources;*output=bounds;return XR_XIR_OK;
}

/* A sorted record names the original physical SSA, not its compact position.
 * The receiving owner proves completeness against every original value. */

/* This private hook is called exactly once for each newly copied dense body,
 * after its real physical vector is assigned and before FUNCTION_REF bottom.
 * Instruction types may come from the same owner's fully substituted
 * pre-bottom Source copy. No supplied permission mask table is accepted. */

/* Ledger equality is an ownership check. Admission comes from the verified
 * construction hook and the final full source/body/domain correspondence. */

#include "xxir_effect_invocation_instance_bounds.inc.c"

/* An authentic dense producer can supply a different pre-bottom vector on
 * this exact physical body. Copy and validate it once before any equation
 * consumes it; seal then transfers the same material's bound owner. */
