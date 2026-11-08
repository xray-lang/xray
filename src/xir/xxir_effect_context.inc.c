/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context.inc.c - Authentic ordinary environments for effect contexts
 *
 * KEY CONCEPT:
 *   A real instruction resolves through constraints and explicit implementation
 *   evidence. Neither a stored effect kind nor this lookup proves body effects.
 */
#include "xxir_implementation_verify.h"
#include "xxir_constraint_proof_internal.h"

typedef struct EffectContextRequest {
    const XrXirModule *source;
    const XrXirTypes *actual_types;
    const XrXirType *arguments;
    uint32_t argument_count, function, instruction;
} EffectContextRequest;

typedef struct EffectContextResolved {
    EffectTerms terms;
    XrCompileResources *resources;
    const XrXirType *arguments;
    const XrXirType *origin_arguments;
    uint32_t argument_count, function, instruction, target, family;
    uint32_t origin_argument_count;
    bool known;
} EffectContextResolved;

#ifdef XR_XIR_EFFECT_CONTEXT_TESTS
static void effect_context_free(EffectContextResolved *resolved) {
    if (!resolved) return;
    effect_terms_free(&resolved->terms);
    xr_compile_resources_free(resolved);
}

#endif

/* Requires completed descriptor/declaration admission. Resolution owns its
 * structural result and grants no Checked, worker, or execution permission. */
#ifdef XR_XIR_EFFECT_CONTEXT_TESTS
static XrXirStatus effect_context_resolve(const XrXirCompileContext *context,
    const EffectContextRequest *request, EffectContextResolved **output) {
    if (!xir_compile_context_valid(context) || !request || !request->source ||
        !request->source->types || !request->actual_types || !request->source->functions ||
        request->function>=request->source->function_count || !output || *output ||
        !!request->arguments!=!!request->argument_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=&request->source->functions[request->function];
    if (!function->instructions || request->instruction>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t family=effect_context_family(function->instructions[request->instruction].op);
    const XrXirGeneric *generic=request->source->generics ? &request->source->generics[request->function] : NULL;
    if (!family || request->argument_count!=(generic ? generic->parameter_count : 0))
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    EffectContextResolved *resolved=xir_compile_calloc(context,1,sizeof(*resolved),&status);
    if (!resolved) return status;
    resolved->terms.remaining=context;resolved->resources=context->resources;resolved->function=request->function;
    resolved->instruction=request->instruction;resolved->target=UINT32_MAX;
    resolved->family=family;resolved->known=true;
    status=effect_terms_seed(&resolved->terms,request->source->types);
    XrXirGeneric environment={0}, call={0};
    if (status==XR_XIR_OK) status=effect_context_arguments(resolved,request,&environment);
    if (status==XR_XIR_OK) status=effect_context_call_arguments(resolved,request,&environment,&call);
    if (status==XR_XIR_OK) status=family==XR_XIR_EFFECT_BINDING_REQUIREMENT ?
        effect_context_requirement(resolved,request,&environment,&call) :
        effect_context_direct(resolved,request,&call);
    if (status!=XR_XIR_OK) { effect_context_free(resolved);return status; }
    resolved->terms.remaining=NULL;
    *output=resolved;return XR_XIR_OK;
}

#endif

/* Internal expansion shares one structural owner. Temporary environments stay
 * in that owner's allocation list on both success and failure; no per-call
 * source pool or independent permission summary is created. */
