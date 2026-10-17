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
    const EffectBodyRequest *body;
    XrXirEffects *effects;
    EffectGraph *graph;
    const EffectInvocationDeclaredBounds *declared;
    const EffectOrdinaryContexts *dense;
} EffectInvocationDriverInput;

/* Current raw correspondence is checked at an actual intake or after all
 * projection callbacks. It is never a reader or permission flag inside Eq. */
static XrXirStatus effect_invocation_current_input_match(const XrXirCompileContext *work,
    const XrXirModule *module,const EffectBodyMaterial *material) {
    XrXirStatus status=effect_physical_match(work,material?material->physical:NULL,module);
    if (status==XR_XIR_OK) status=effect_body_type_domain_match(work,material,module);
    return status;
}

/* This boundary alone accepts current mutable raw input. A changed Source
 * first rebuilds the complete generation. The internal queue retains only
 * the owned reader, physical/control inventories and independently owned bounds. */
static XrXirStatus effect_invocation_collect(const XrXirCompileContext *work,
    const XrXirModule *module,XrXirEffects *effects,EffectGraph *graph,
    const EffectInvocationDeclaredBounds *declared,EffectBodyRequest *output) {
    if (!xir_compile_context_valid(work) || !module || !module->functions || !graph || !effects ||
        !effect_body_request_empty(output) || !xir_effects_context_matches(work,effects,module->function_count) ||
        !effects->contracts || !declared || declared->resources!=work->resources ||
        declared->count!=module->function_count || !declared->functions) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,3+sizeof(bool))) return XR_XIR_BUDGET;
    bool local_bounds=graph->material && declared==graph->material->declared;
    XrXirStatus status=XR_XIR_OK;
    if (!graph->material || (graph->material!=effects->generation && !graph->physical_owned))
        status=effect_graph_physical_refresh(work,module,graph,effects->refinement);
    if (status!=XR_XIR_OK) return status;
    EffectBodyMaterial *material=graph->material;
    if (!effect_body_material_shape(material) || material->resources!=work->resources ||
        graph->physical!=material->physical || !material->cells || !material->declared)
        return XR_XIR_BAD_STRUCTURE;
    /* Recollection retires the old owned bound child with its generation.
     * An independent caller-owned bound remains a live comparison input. */
    if (local_bounds) {
        if (!xir_compile_work(work,sizeof(declared))) return XR_XIR_BUDGET;
        declared=material->declared;
    }
    if (status==XR_XIR_OK && declared!=material->declared) {
        EffectInvocationDeclaredBounds *copy=NULL;
        status=effect_invocation_bounds_copy_physical(work,declared,material->physical,&copy);
        if (status==XR_XIR_OK && !xir_compile_work(work,2)) status=XR_XIR_BUDGET;
        if (status!=XR_XIR_OK) { effect_invocation_bounds_free(copy);return status; }
        effect_invocation_bounds_free(material->declared);material->declared=copy;
    }
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=effect_body_owned_request(work,material);return XR_XIR_OK;
}

/* Raw literal construction enters the same actual intake boundary. No
 * failed collection status is converted to a structural error or retried. */
static inline XrXirStatus effect_invocations_receive(const XrXirCompileContext *work,
    const XrXirModule *module,XrXirEffects *effects,EffectGraph *graph,
    const EffectInvocationDeclaredBounds *declared,EffectInvocationWorkspace **output) {
    if (!output || *output || !xir_compile_context_valid(work)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(EffectBodyRequest))) return XR_XIR_BUDGET;
    EffectBodyRequest input={0};
    XrXirStatus status=effect_invocation_collect(work,module,effects,graph,declared,&input);
    if (status==XR_XIR_OK) status=effect_invocation_current_input_match(work,module,graph->material);
    if (status==XR_XIR_OK) status=effect_invocations_create(work,&input,effects,graph,
        graph->material->declared,output);
    return status;
}

/* The existing graph owns the only queue. The driver performs no legacy
 * ROOT solve and never lowers a precomputed permission. A success transfers
 * a completely sealed computation; publication belongs to the common caller. */
static inline XrXirStatus effect_invocation_derive(const EffectInvocationDriverInput *input,
    EffectInvocationCertificate **output,const EffectInvocationProjectionSink *sink,
    EffectFinalFunctions **receiving) {
    if (!input || !output || *output || !receiving || *receiving || !xir_compile_context_valid(input->work) || !input->body ||
        !effect_invocation_projection_sink_matches(input->work,sink,input->body->domain.function_count))
        return XR_XIR_BAD_STRUCTURE;
    EffectInvocationWorkspace *equations=NULL;EffectInvocationRows physical_rows={0};
    XrXirStatus status=effect_invocations_create(input->work,input->body,input->effects,
        input->graph,input->declared,&equations);
    if (status==XR_XIR_OK) {
        equations->physical_rows=&physical_rows;status=effect_invocations_solve(equations);
    }
    if (status==XR_XIR_OK) status=effect_invocation_stage(equations,receiving);
    EffectInvocationSealRequest request={&equations,receiving,input->dense};
    if (status==XR_XIR_OK) status=effect_invocation_seal(input->work,&request,output,sink);
    effect_invocation_rows_drop(&physical_rows);effect_invocation_free(equations);
    return status;
}
