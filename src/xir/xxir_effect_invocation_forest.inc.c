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

static XrXirStatus effect_invocation_forest_record(const XrXirCompileContext *work,
    const EffectContextForest *forest,const EffectInvocationCertificate *certificate,
    uint32_t function,const EffectInvocationForestProjection *request,EffectContextCauseValue *output) {
    if (!request || !request->selection || request->domain>=4) return XR_XIR_BAD_STRUCTURE;

    uint32_t domain=request->domain;
    const uint32_t *selection=request->selection;
    uint32_t n=certificate->final->roots[function];
    const EffectInvocationNode *node=&certificate->final->nodes[n];
    uint32_t atom=*selection;XrXirRootEffectWitness selected_witness;
    XrXirStatus status=effect_invocation_selected_witness(work,certificate,function,atom,&selected_witness);
    if (status!=XR_XIR_OK) return status;
    status=effect_invocation_projection_result_match(work,certificate,function,domain,atom,&selected_witness);
    if (status!=XR_XIR_OK) return status;

    const EffectInvocationProjectedCause projected={atom,&selected_witness};
    XrXirRootEffectWitness witness={0};bool symbolic=false;
    status=effect_invocation_public_transform(work,certificate,function,&projected,&witness,&symbolic);
    if (status!=XR_XIR_OK) return status;
    uint32_t next=UINT32_MAX,parameter_owner=function;
    if (atom!=UINT32_MAX) {
        if (atom>=node->cause_count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationCause *cause=&node->causes[atom];
        if (cause->distance && !symbolic) {
            if (cause->next>=certificate->final->count) return XR_XIR_BAD_STRUCTURE;
            next=certificate->final->nodes[cause->next].body;
        } else if (!symbolic && cause->witness.callee!=UINT32_MAX) parameter_owner=cause->witness.callee;
        else if (node->root!=node->body && witness.cause==XR_XIR_ROOT_CAUSE_PARAMETER) parameter_owner=node->root;
    }
    uint32_t target=next==UINT32_MAX?parameter_owner:next;
    if (target>=forest->count || parameter_owner>=forest->count) return XR_XIR_BAD_STRUCTURE;
    if (witness.distance && witness.cause==XR_XIR_ROOT_CAUSE_CALL &&
        witness.instruction<certificate->body->functions[function].instruction_count &&
        forest->requirements[function] && forest->requirements[function][witness.instruction]) {
        EffectBodyRead read=(EffectBodyRead){.owner=work};XrXirInstruction decoded;
        status=effect_body_instruction_get(&read,&certificate->body->functions[function],witness.instruction,&decoded);
        if (status!=XR_XIR_OK) return status;
        if (decoded.op!=XR_XIR_CALL)
            return XR_XIR_BAD_STRUCTURE;
        witness.cause=XR_XIR_ROOT_CAUSE_REQUIREMENT;
    }

    *output=(EffectContextCauseValue){witness,next,parameter_owner,(uint8_t)domain,
        (uint8_t)(2+(domain&1))};
    return XR_XIR_OK;
}

/* Admission consumes every actual domain and retains no projection copy. */
static XrXirStatus effect_invocation_forest_admit(const XrXirCompileContext *work,
    const EffectContextForest *forest,const EffectInvocationCertificate *certificate) {
    if (!certificate || !effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || certificate->resources!=work->resources ||
        effect_body_count(certificate->body)!=forest->count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<forest->count;++f) {
        uint32_t selected[4];
        XrXirStatus status=effect_invocation_projection_domains(work,certificate,f,0,4,selected);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t domain=0;domain<4;++domain) {
            EffectContextCauseValue value;

            EffectInvocationForestProjection request={&selected[domain],domain};
            status=effect_invocation_forest_record(work,forest,certificate,f,&request,&value);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

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
static XrXirStatus effect_invocation_forest_match(const XrXirCompileContext *work,
    const EffectContextForest *forest) {
    const EffectInvocationCertificate *certificate=forest->invocations;
    if (!certificate || !effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || certificate->resources!=work->resources || effect_body_count(certificate->body)!=forest->count ||
        !forest->nodes || !forest->requirements)
        return XR_XIR_BAD_STRUCTURE;
    if (forest->terms.types.count!=certificate->terms.types.count) return XR_XIR_BAD_STRUCTURE;
    /* Open base bodies retain their verified declaration scope. Compare both
     * complete owned domains before binding the same symbolic parameters. */
    EffectTerms comparison={.types=forest->terms.types,.remaining=work};
    XrXirStatus status=effect_terms_snapshot_match(&comparison,&certificate->terms.types,
        certificate->terms.types.count);
    for (uint32_t f=0;f<forest->count && status==XR_XIR_OK;++f) {
        const EffectForestNode *node=&forest->nodes[f];const XrXirOrigin *origin=&certificate->origins[f];
        const EffectBodyFunction *body=&certificate->body->functions[f];
        uint32_t scope=certificate->generics?certificate->generics[f].parameter_count:0;
        if (scope>XR_XIR_TYPE_PARAMETER_LIMIT-XR_XIR_TYPE_PARAMETER_BASE) { status=XR_XIR_BAD_STRUCTURE;break; }

        if (node->declaration!=origin->function || node->owner!=certificate->owners[f] ||
            node->argument_count!=origin->argument_count || node->parameter_count!=body->parameter_count ||
            node->constant_mask!=certificate->final->nodes[certificate->final->roots[f]].intrinsic_mask) { status=XR_XIR_BAD_STRUCTURE;break; }
        for (uint32_t a=0;a<node->argument_count && status==XR_XIR_OK;++a)
            status=effect_terms_domain_type(&comparison,&certificate->terms.types,
                node->arguments[a],origin->arguments[a],scope);
        for (uint32_t p=0;p<node->parameter_count && status==XR_XIR_OK;++p) {

            status=effect_terms_domain_type(&comparison,&certificate->terms.types,
                node->physical_types[p],body->parameters[p],scope);
        }
    }
    if(status==XR_XIR_OK) status=effect_invocation_forest_admit(work,forest,certificate);
    effect_terms_free(&comparison);
    if (status==XR_XIR_OK) status=effect_parameter_summary_shape(work, &certificate->final->parameter_summary, effect_body_count(certificate->body));
    return status==XR_XIR_BAD_TYPE?XR_XIR_BAD_STRUCTURE:status;
}



static XrXirStatus effect_invocation_context_edge_mask(const XrXirCompileContext *work,
    const EffectContextForest *forest,uint32_t caller,uint32_t instruction,uint32_t target,uint32_t *output) {
    if (!xir_compile_context_valid(work) || !forest || forest->resources!=work->resources || !output)
        return XR_XIR_BAD_STRUCTURE;
    return effect_invocation_certificate_edge_mask(work,forest->invocations,caller,instruction,target,output);
}
