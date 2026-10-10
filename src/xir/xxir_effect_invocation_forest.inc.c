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
static XrXirStatus effect_invocation_forest_types(const XrXirCompileContext *work,
    const EffectOrdinaryContexts *contexts,EffectTerms *output) {
    if (!xir_compile_context_valid(work) || !contexts || !output ||
        contexts->uses.resources!=work->resources || output->remaining!=work ||
        output->status!=XR_XIR_OK || output->types.count || output->types.nodes ||
        output->types.nominals || output->types.interfaces || output->owned_nominals ||
        output->owned_interfaces) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationCertificate *certificate=contexts->uses.invocations;
    if (!certificate || certificate->resources!=work->resources || certificate->terms.remaining ||
        certificate->bodies.types!=&certificate->terms.types || !certificate->equations ||
        certificate->equations->work || certificate->equations->module || certificate->equations->effects ||
        certificate->equations->graph || certificate->equations->declared ||
        certificate->bodies.function_count!=contexts->count ||
        certificate->terms.types.count!=contexts->terms.types.count) return XR_XIR_BAD_STRUCTURE;
    /* A query-local comparison owns all of its scratch. The complete semantic
     * declaration domain and every descriptor are checked on the real ledger. */
    EffectTerms comparison={.types=certificate->terms.types,.remaining=work};
    XrXirStatus status=effect_terms_snapshot_match(&comparison,&contexts->terms.types,
        contexts->terms.types.count);
    effect_terms_free(&comparison);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,sizeof(output->types))) return XR_XIR_BUDGET;
    output->types=certificate->terms.types;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_forest_record(const XrXirCompileContext *work,
    const EffectContextForest *forest,const EffectInvocationCertificate *certificate,
    uint32_t function,uint32_t domain,EffectContextCertificate *output) {
    if (!xir_compile_work(work,8)) return XR_XIR_BUDGET;
    uint32_t n=certificate->equations->roots[function];
    const EffectInvocationNode *node=&certificate->equations->nodes[n];
    uint32_t atom=certificate->projection_atoms[(size_t)domain*forest->count+function];
    XrXirRootEffectWitness witness={0};bool symbolic=false;
    XrXirStatus status=effect_invocation_public_selected(work,certificate,function,domain,&witness,&symbolic);
    if (status!=XR_XIR_OK) return status;
    uint32_t next=UINT32_MAX,parameter_owner=function;
    if (atom!=UINT32_MAX) {
        if (atom>=node->cause_count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationCause *cause=&node->causes[atom];
        if (cause->distance && !symbolic) {
            if (cause->next>=certificate->equations->count) return XR_XIR_BAD_STRUCTURE;
            next=certificate->equations->nodes[cause->next].body;
        } else if (!symbolic && cause->witness.callee!=UINT32_MAX) parameter_owner=cause->witness.callee;
        else if (node->root!=node->body && witness.cause==XR_XIR_ROOT_CAUSE_PARAMETER) parameter_owner=node->root;
    }
    uint32_t target=next==UINT32_MAX?parameter_owner:next;
    if (target>=forest->count || parameter_owner>=forest->count) return XR_XIR_BAD_STRUCTURE;
    if (witness.distance && witness.cause==XR_XIR_ROOT_CAUSE_CALL &&
        witness.instruction<certificate->bodies.functions[function].instruction_count &&
        forest->requirements[function] && forest->requirements[function][witness.instruction]) {
        if (certificate->bodies.functions[function].instructions[witness.instruction].op!=XR_XIR_CALL)
            return XR_XIR_BAD_STRUCTURE;
        witness.cause=XR_XIR_ROOT_CAUSE_REQUIREMENT;
    }
    if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(EffectContextCertificate){witness,next,parameter_owner,(uint8_t)domain,
        (uint8_t)(2+(domain&1)),forest->nodes[function],forest->nodes[target]};
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_forest_records(const XrXirCompileContext *work,
    EffectContextForest *forest,const EffectOrdinaryContexts *contexts) {
    const EffectInvocationCertificate *certificate=contexts->uses.invocations;
    if (!certificate || certificate->resources!=work->resources || certificate->bodies.function_count!=forest->count)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t domain=0;domain<4;++domain) for (uint32_t f=0;f<forest->count;++f) {
        size_t at=(size_t)domain*forest->count+f;
        XrXirStatus status=effect_invocation_forest_record(work,forest,certificate,f,domain,&forest->certificates[at]);
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(work,sizeof(*forest->witnesses))) return XR_XIR_BUDGET;
        forest->witnesses[at]=forest->certificates[at].witness;
    }
    return XR_XIR_OK;
}

/* Public metadata is a copy of this same immutable certificate. Recheck its
 * complete physical identities and selected causes, rather than following a
 * different declaration-only forest for an environment-dependent equation. */
static XrXirStatus effect_invocation_forest_match(const XrXirCompileContext *work,
    const EffectContextForest *forest) {
    const EffectInvocationCertificate *certificate=forest->invocations;
    if (!certificate || certificate->resources!=work->resources || certificate->bodies.function_count!=forest->count ||
        !forest->nodes || !forest->facts || !forest->witnesses || !forest->certificates || !forest->requirements)
        return XR_XIR_BAD_STRUCTURE;
    if (forest->terms.types.count!=certificate->terms.types.count) return XR_XIR_BAD_STRUCTURE;
    /* Open base bodies retain their verified declaration scope. Compare both
     * complete owned domains before binding the same symbolic parameters. */
    EffectTerms comparison={.types=forest->terms.types,.remaining=work};
    XrXirStatus status=effect_terms_snapshot_match(&comparison,&certificate->terms.types,
        certificate->terms.types.count);
    for (uint32_t f=0;f<forest->count && status==XR_XIR_OK;++f) {
        const EffectForestNode *node=&forest->nodes[f];const XrXirOrigin *origin=&certificate->origins[f];
        const XrXirFunction *body=&certificate->bodies.functions[f];
        uint32_t scope=certificate->generics?certificate->generics[f].parameter_count:0;
        if (scope>XR_XIR_TYPE_PARAMETER_LIMIT-XR_XIR_TYPE_PARAMETER_BASE) { status=XR_XIR_BAD_STRUCTURE;break; }
        if (!xir_compile_work(work,8)) { status=XR_XIR_BUDGET;break; }
        if (node->declaration!=origin->function || node->owner!=certificate->owners[f] ||
            node->argument_count!=origin->argument_count || node->parameter_count!=body->parameter_count ||
            node->constant_mask!=certificate->solution->formulas[f].constant_mask ||
            forest->facts[f].requires_root!=certificate->facts[f].requires_root ||
            forest->facts[f].unresolved!=certificate->facts[f].unresolved) { status=XR_XIR_BAD_STRUCTURE;break; }
        for (uint32_t a=0;a<node->argument_count && status==XR_XIR_OK;++a)
            status=effect_terms_domain_type(&comparison,&certificate->terms.types,
                node->arguments[a],origin->arguments[a],scope);
        for (uint32_t p=0;p<node->parameter_count && status==XR_XIR_OK;++p) {
            if (!xir_compile_work(work,1)) { status=XR_XIR_BUDGET;break; }
            if (node->parameter_kinds[p]!=certificate->parameter_kinds[f][p]) { status=XR_XIR_BAD_STRUCTURE;break; }
            status=effect_terms_domain_type(&comparison,&certificate->terms.types,
                node->physical_types[p],body->parameters[p],scope);
        }
        for (uint32_t d=0;d<4 && status==XR_XIR_OK;++d) {
            size_t at=(size_t)d*forest->count+f;EffectContextCertificate expected={0};
            status=effect_invocation_forest_record(work,forest,certificate,f,d,&expected);
            const EffectContextCertificate *actual=&forest->certificates[at];
            if (status!=XR_XIR_OK) break;
            if (!xir_compile_work(work,12)) { status=XR_XIR_BUDGET;break; }
            if (!effect_context_witness_same(&forest->witnesses[at],&expected.witness) ||
                !effect_context_witness_same(&actual->witness,&expected.witness) || actual->next!=expected.next ||
                actual->parameter_owner!=expected.parameter_owner || actual->domain!=expected.domain ||
                actual->next_domain!=expected.next_domain) { status=XR_XIR_BAD_STRUCTURE;break; }
            status=effect_context_identity_matches(work,forest,&actual->subject,f);
            uint32_t target=expected.next==UINT32_MAX?expected.parameter_owner:expected.next;
            if (status==XR_XIR_OK) status=effect_context_identity_matches(work,forest,&actual->target,target);
        }
    }
    effect_terms_free(&comparison);return status==XR_XIR_BAD_TYPE?XR_XIR_BAD_STRUCTURE:status;
}

static XrXirStatus effect_invocation_forest_trace(const XrXirCompileContext *work,
    const EffectContextForest *forest,uint32_t function,XrXirRootCauseTrace **output) {
    if (!xir_compile_context_valid(work) || !forest || forest->resources!=work->resources ||
        !output || *output || function>=forest->count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_invocation_forest_match(work,forest);
    if (status!=XR_XIR_OK) return status;
    XrXirRootCauseTrace *trace=NULL;
    status=effect_invocation_public_trace(work,forest->invocations,forest->count,function,&forest->facts[function],&trace);
    uint64_t count=trace?(uint64_t)trace->counts[0]+trace->counts[1]:0;
    XrXirRootCauseStep *steps=trace?effect_root_trace_storage(trace):NULL;
    for (uint64_t s=0;s<count && status==XR_XIR_OK;++s) {
        XrXirRootCauseStep *step=&steps[s];
        if (!step->distance || step->cause!=XR_XIR_ROOT_CAUSE_CALL) continue;
        bool found=false,requirement=false;
        for (uint32_t f=0;f<forest->count;++f) {
            if (!xir_compile_work(work,3)) { status=XR_XIR_BUDGET;break; }
            if (forest->nodes[f].declaration!=step->function) continue;
            if (step->instruction>=forest->invocations->bodies.functions[f].instruction_count) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
            bool actual=forest->requirements[f] && forest->requirements[f][step->instruction];
            if (found && actual!=requirement) { status=XR_XIR_BAD_STRUCTURE;break; }
            found=true;requirement=actual;
        }
        if (status==XR_XIR_OK && !found) status=XR_XIR_BAD_STRUCTURE;
        if (status==XR_XIR_OK && requirement) step->cause=XR_XIR_ROOT_CAUSE_REQUIREMENT;
    }
    if (status!=XR_XIR_OK) { xr_xir_compile_root_cause_trace_free(trace);return status; }
    *output=trace;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_context_edge_mask(const XrXirCompileContext *work,
    const EffectContextForest *forest,uint32_t caller,uint32_t instruction,uint32_t target,uint32_t *output) {
    if (!xir_compile_context_valid(work) || !forest || forest->resources!=work->resources || !output)
        return XR_XIR_BAD_STRUCTURE;
    return effect_invocation_certificate_edge_mask(work,forest->invocations,caller,instruction,target,output);
}
