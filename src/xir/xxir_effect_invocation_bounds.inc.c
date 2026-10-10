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

static XrXirStatus effect_invocation_bounds_capacity(const XrXirCompileContext *work,
    EffectInvocationDeclaredBounds *bounds,uint32_t needed) {
    if (needed<=bounds->capacity) return XR_XIR_OK;
    if (needed>work->limits.functions) return XR_XIR_BUDGET;
    uint32_t capacity=bounds->capacity ? bounds->capacity : 8;
    if (capacity>work->limits.functions) capacity=work->limits.functions;
    while (capacity<needed) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        capacity=capacity>work->limits.functions/2 ? work->limits.functions : capacity*2;
    }
    if ((uint64_t)capacity>SIZE_MAX/sizeof(*bounds->functions)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    EffectInvocationFunctionBounds *functions=xir_compile_alloc(work,
        (size_t)capacity*sizeof(*functions),&status);
    if (!functions) return status;
    if (!xir_compile_work(work,(uint64_t)bounds->count*sizeof(*functions))) {
        xr_compile_resources_free(functions);return XR_XIR_BUDGET;
    }
    if (bounds->count) memcpy(functions,bounds->functions,(size_t)bounds->count*sizeof(*functions));
    xr_compile_resources_free(bounds->functions);bounds->functions=functions;
    bounds->capacity=capacity;return XR_XIR_OK;
}

/* A sorted record names the original physical SSA, not its compact position.
 * The receiving owner proves completeness against every original value. */
static const EffectInvocationValueBound *effect_invocation_bound_find(
    const EffectInvocationFunctionBounds *function,uint32_t value,uint32_t *comparisons) {
    if (!function || !comparisons || value>=function->values || function->count>function->values ||
        (!!function->bounds!=!!function->count)) return NULL;
    uint32_t first=0,last=function->count;
    while (first<last) {
        ++*comparisons;
        uint32_t middle=first+(last-first)/2;
        if (function->bounds[middle].value<value) first=middle+1;
        else last=middle;
    }
    return first<function->count && function->bounds[first].value==value ?
        &function->bounds[first] : NULL;
}

/* This private hook is called exactly once for each newly copied dense body,
 * after its real physical vector is assigned and before FUNCTION_REF bottom.
 * Instruction types may come from the same owner's fully substituted
 * pre-bottom Source copy. No supplied permission mask table is accepted. */
static XrXirStatus effect_invocation_bounds_capture(const XrXirCompileContext *work,
    EffectInvocationDeclaredBounds *bounds,const XrXirTypes *types,
    const XrXirFunction *function,uint32_t index,const XrXirType *declared_instructions) {
    if (!xir_compile_context_valid(work) || !bounds || bounds->resources!=work->resources ||
        !function || index!=bounds->count ||
        (function->parameter_count && !function->parameters) ||
        (function->instruction_count && !function->instructions)) return XR_XIR_BAD_STRUCTURE;
    uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
    if (values>UINT32_MAX || values>SIZE_MAX/sizeof(EffectInvocationValueBound)) return XR_XIR_BUDGET;
    if (index==UINT32_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=effect_invocation_bounds_capacity(work,bounds,index+1);
    if (status!=XR_XIR_OK) return status;
    uint32_t count=0;
    for (uint32_t v=0;v<(uint32_t)values;++v) {
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if (xr_xir_callable_signature(types,xr_xir_operand_type(function,v))) ++count;
    }
    EffectInvocationValueBound *records=count ? xir_compile_alloc(work,
        (size_t)count*sizeof(*records),&status) : NULL;
    if (count && !records) return status;
    uint32_t at=0;
    for (uint32_t v=0;v<(uint32_t)values;++v) {
        if (!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
        XrXirType type=v>=function->parameter_count && declared_instructions ?
            declared_instructions[v-function->parameter_count] : xr_xir_operand_type(function,v);
        bool actual_callable=xr_xir_callable_signature(types,xr_xir_operand_type(function,v))!=NULL;
        const XrXirTypeNode *signature=xr_xir_callable_signature(types,type);
        if (!!signature!=actual_callable || (signature && !xr_xir_callable_flags_valid(signature->flags))) {
            status=XR_XIR_BAD_TYPE;break;
        }
        if (signature) {
            if (at>=count) { status=XR_XIR_BAD_STRUCTURE;break; }
            if (!xir_compile_work(work,sizeof(*records))) { status=XR_XIR_BUDGET;break; }
            records[at++]=(EffectInvocationValueBound){.mask=signature->flags&
                (XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED),.callable=true,.value=v};
        }
    }
    if (status==XR_XIR_OK && at!=count) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(*bounds->functions)+1)) status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(records);return status; }
    bounds->functions[index]=(EffectInvocationFunctionBounds){(uint32_t)values,count,records};
    ++bounds->count;return XR_XIR_OK;
}

/* Ledger equality is an ownership check. Admission comes from the verified
 * construction hook and the final full source/body/domain correspondence. */
static inline XrXirStatus effect_invocation_bounds_mask(const XrXirCompileContext *work,
    const EffectInvocationDeclaredBounds *bounds,uint32_t function,uint32_t value,
    uint32_t *output) {
    if (!xir_compile_context_valid(work) || !bounds || bounds->resources!=work->resources ||
        !output || function>=bounds->count || value>=bounds->functions[function].values)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFunctionBounds *body=&bounds->functions[function];
    if (body->count>body->values || (!!body->bounds!=!!body->count)) return XR_XIR_BAD_STRUCTURE;
    uint32_t comparisons=0;
    const EffectInvocationValueBound *record=effect_invocation_bound_find(body,value,&comparisons);
    if (!xir_compile_work(work,2+(uint64_t)comparisons*3)) return XR_XIR_BUDGET;
    if (!record || !record->callable || record->mask&~
        (XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED)) return XR_XIR_BAD_TYPE;
    *output=record->mask;return XR_XIR_OK;
}

#include "xxir_effect_invocation_instance_bounds.inc.c"
