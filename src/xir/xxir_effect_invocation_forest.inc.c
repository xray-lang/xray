/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_forest.inc.c - Original public owner and exact equations
 */
/* The independently sealed certificate owns the complete immutable type
 * snapshot. Prove its real ordered correspondence before retaining its view;
 * metadata allocations cannot intern, substitute or grow this type pool. */

/* A record request borrows only this call's fresh four-domain selection.
 * It carries no authority and is not retained by the forest or certificate. */
typedef struct EffectInvocationForestProjection {
    const uint32_t *selection;
    uint32_t domain;
} EffectInvocationForestProjection;

/* Admission consumes every actual domain and retains no projection copy. */

/* These are fresh values from the canonical Final. Requirement spelling is a
 * presentation of the actual instruction identity, never a permission cache. */
static XrXirStatus effect_invocation_forest_values(const XrXirCompileContext *work,
    const EffectContextForest *forest,uint32_t function,EffectContextRootValue *output) {
    if (!output || !xir_compile_context_valid(work)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(work,forest,1,sizeof(*forest));
    if (status!=XR_XIR_OK) return status;
    if (forest->resources!=work->resources || function>=forest->count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationCertificate *certificate=forest->invocations;
    status=effect_role_range(work,certificate,1,sizeof(*certificate));if(status!=XR_XIR_OK) return status;
    status=effect_role_range(work,certificate->final,1,sizeof(*certificate->final));
    if(status!=XR_XIR_OK) return status;
    status=effect_role_material_frame(work,certificate->body);if(status!=XR_XIR_OK) return status;
    const EffectFinalFunctions *summary=&certificate->final->summary;
    if(certificate->resources!=work->resources || summary->resources!=work->resources ||
        summary->count!=forest->count || summary->capacity!=summary->count ||
        !effect_body_material_shape(certificate->body) ||
        effect_body_count(certificate->body)!=forest->count) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,summary->witnesses,(uint64_t)summary->count*4,sizeof(*summary->witnesses));
    if(status==XR_XIR_OK) status=effect_role_range(work,certificate->body->functions,forest->count,sizeof(*certificate->body->functions));
    if(status==XR_XIR_OK) status=effect_role_range(work,forest->requirements,forest->count,sizeof(*forest->requirements));
    if(status!=XR_XIR_OK) return status;
    const EffectBodyFunction *head=&certificate->body->functions[function];
    const uint8_t *requirements=forest->requirements[function];
    if(requirements) {
        status=effect_role_range(work,requirements,head->instruction_count,sizeof(*requirements));
        if(status!=XR_XIR_OK) return status;
    }

    XrXirRootEffects facts;
    status=effect_invocation_certificate_facts(work,certificate,function,&facts);
    if(status!=XR_XIR_OK) return status;
    EffectContextRootValue value={facts,summary->witnesses[function],summary->witnesses[summary->count+function]};
    XrXirRootEffectWitness *witnesses[2]={&value.root,&value.unresolved};
    for(uint32_t domain=0;domain<2;++domain) {
        XrXirRootEffectWitness *witness=witnesses[domain];

        if(!witness->distance || witness->cause!=XR_XIR_ROOT_CAUSE_CALL ||
            witness->instruction>=head->instruction_count || !requirements || !requirements[witness->instruction]) continue;
        EffectBodyRead read=(EffectBodyRead){.owner=work};XrXirInstruction operation;
        status=effect_body_instruction_get(&read,head,witness->instruction,&operation);
        if(status!=XR_XIR_OK) return status;
        if(operation.op!=XR_XIR_CALL) return XR_XIR_BAD_STRUCTURE;
        witness->cause=XR_XIR_ROOT_CAUSE_REQUIREMENT;
    }

    *output=value;return XR_XIR_OK;
}

/* Public metadata is a copy of this same immutable certificate. Recheck its
 * complete physical identities and selected causes, rather than following a
 * different declaration-only forest for an environment-dependent equation. */

static XrXirStatus effect_invocation_context_edge_mask(const XrXirCompileContext *work,
    const EffectContextForest *forest,uint32_t caller,uint32_t instruction,uint32_t target,uint32_t *output) {
    if (!xir_compile_context_valid(work) || !forest || forest->resources!=work->resources || !output)
        return XR_XIR_BAD_STRUCTURE;
    return effect_invocation_certificate_edge_mask(work,forest->invocations,caller,instruction,target,output);
}
