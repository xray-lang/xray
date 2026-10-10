/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_driver.inc.c - One owned invocation equation lifecycle
 */
typedef struct EffectInvocationDriverInput {
    const XrXirCompileContext *work;
    const XrXirModule *module;
    XrXirEffects *effects;
    EffectGraph *graph;
    const EffectInvocationDeclaredBounds *declared;
    const EffectOrdinaryContexts *dense;
} EffectInvocationDriverInput;

/* The existing graph owns the only queue. The driver performs no legacy
 * ROOT solve and never lowers a precomputed permission. A success transfers
 * a completely sealed computation; publication belongs to the common caller. */
static inline XrXirStatus effect_invocation_derive(const EffectInvocationDriverInput *input,
    EffectInvocationCertificate **output) {
    if (!input || !output || *output || !xir_compile_context_valid(input->work))
        return XR_XIR_BAD_STRUCTURE;
    EffectInvocationOwner *equations=NULL;EffectInvocationSolution *solution=NULL;
    XrXirStatus status=effect_invocations_create(input->work,input->module,input->effects,
        input->graph,input->declared,&equations);
    if (status==XR_XIR_OK) status=effect_invocations_solve(equations);
    if (status==XR_XIR_OK) status=effect_invocation_stage(equations,&solution);
    EffectInvocationSealRequest request={&equations,&solution,input->dense};
    if (status==XR_XIR_OK) status=effect_invocation_seal(input->work,&request,output);
    effect_invocation_solution_free(solution);effect_invocation_free(equations);
    return status;
}
