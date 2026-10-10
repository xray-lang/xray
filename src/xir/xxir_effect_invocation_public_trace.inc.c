/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_public_trace.inc.c - Exact public cause projection
 *
 * KEY CONCEPT:
 *   Equation environments certify each real edge. The public owner retains
 *   its existing function bound and terminal ownership contract.
 */
/* Symbolic dependencies are public terminals at their actual root consumer,
 * as in the receiving canonical formula. The complete environment/atom cause
 * remains independently sealed and is verified before this projection. */
static XrXirStatus effect_invocation_public_selected(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,uint32_t domain,
    XrXirRootEffectWitness *output,bool *symbolic) {
    uint32_t atom=UINT32_MAX;XrXirRootEffectWitness witness={0};
    XrXirStatus status=effect_invocation_projection_select(work,certificate,function,domain,&atom,&witness);
    if (status!=XR_XIR_OK) return status;
    *symbolic=false;
    if (atom!=UINT32_MAX && atom>=2) {
        const EffectInvocationOwner *equations=certificate->equations;
        const EffectInvocationNode *node=&equations->nodes[equations->roots[function]];
        uint32_t parameters=certificate->bodies.functions[node->root].parameter_count;
        uint32_t kind=0,index=0;
        if (atom-2<parameters) { kind=XR_XIR_ROOT_TERM_PARAMETER;index=atom-2; }
        else if (atom-2-parameters<parameters) { kind=XR_XIR_ROOT_TERM_CELL_PARAMETER;index=atom-2-parameters; }
        else if (atom==(uint64_t)parameters*2+2) { kind=XR_XIR_ROOT_TERM_CONTEXT_CALL;index=witness.instruction; }
        else return XR_XIR_BAD_STRUCTURE;
        const XrXirRootFormula *formula=&certificate->solution->formulas[function];bool found=false;
        for (uint32_t t=0;t<formula->term_count;++t) {
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if (formula->terms[t].kind==kind && formula->terms[t].index==index) found=true;
        }
        if (!found || node->body!=function || (kind!=XR_XIR_ROOT_TERM_CONTEXT_CALL && node->root!=function) ||
            witness.instruction>=certificate->bodies.functions[function].instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        XrXirRootEffectCause cause=kind==XR_XIR_ROOT_TERM_PARAMETER?XR_XIR_ROOT_CAUSE_PARAMETER:
            kind==XR_XIR_ROOT_TERM_CELL_PARAMETER?XR_XIR_ROOT_CAUSE_CELL_PARAMETER:XR_XIR_ROOT_CAUSE_CONTEXT_CALL;
        witness=(XrXirRootEffectWitness){cause,witness.instruction,UINT32_MAX,
            kind==XR_XIR_ROOT_TERM_CONTEXT_CALL?UINT32_MAX:index,0};
        *symbolic=true;
    }
    if (!xir_compile_work(work,sizeof(witness))) return XR_XIR_BUDGET;
    *output=witness;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_public_walk(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectInvocationTraceRequest *request,
    uint32_t limit,uint32_t *length) {
    uint32_t count=0;
    XrXirStatus status=effect_invocation_trace_walk(work,certificate,request,&count);
    if (status!=XR_XIR_OK) return status;
    XrXirRootEffectWitness selected={0};bool symbolic=false;
    status=effect_invocation_public_selected(work,certificate,request->function,request->domain,&selected,&symbolic);
    if (status!=XR_XIR_OK) return status;
    if (!limit) return XR_XIR_BAD_STRUCTURE;
    if (!count) { *length=0;return XR_XIR_OK; }
    if (symbolic) { *length=1;return XR_XIR_OK; }
    if (count>limit) return XR_XIR_BAD_STRUCTURE;
    uint32_t atom=certificate->projection_atoms[(size_t)request->domain*
        certificate->bodies.function_count+request->function];
    const EffectInvocationOwner *equations=certificate->equations;
    uint32_t current=equations->roots[request->function];
    for (uint32_t at=0;at<count;++at) {
        if (!xir_compile_work(work,8)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&equations->nodes[current];
        const EffectInvocationCause *cause=&node->causes[atom];
        XrXirRootEffectWitness witness=effect_invocation_public_witness(certificate,node,cause);
        if (!witness.cause || witness.distance>=limit || witness.distance!=count-at-1)
            return XR_XIR_BAD_STRUCTURE;
        if (witness.distance) {
            if ((witness.cause!=XR_XIR_ROOT_CAUSE_CALL && witness.cause!=XR_XIR_ROOT_CAUSE_CLEANUP &&
                witness.cause!=XR_XIR_ROOT_CAUSE_REQUIREMENT) || witness.callee==UINT32_MAX ||
                witness.slot!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            current=cause->next;continue;
        }
        /* The current public Cell terminal belongs to its emitted function.
         * A foreign physical namespace requires a separate semantic cutover;
         * its private proof is retained, never relabeled as this terminal. */
        if ((witness.cause==XR_XIR_ROOT_CAUSE_CELL_ACCESS ||
            witness.cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER) && witness.callee!=UINT32_MAX)
            return XR_XIR_BAD_STRUCTURE;
        if (witness.cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL &&
            (!(request->domain&1) || witness.callee!=UINT32_MAX || witness.slot!=UINT32_MAX))
            return XR_XIR_BAD_STRUCTURE;
    }
    *length=count;return XR_XIR_OK;
}

/* Every public domain is proved before publication. The private equation
 * count bounds its own verifier and never becomes the public trace allowance. */
static XrXirStatus effect_invocation_public_gate(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t limit) {
    if (!xir_compile_context_valid(work) || !certificate || certificate->resources!=work->resources ||
        !limit || limit!=certificate->bodies.function_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<limit;++f) {
        for (uint32_t domain=0;domain<4;++domain) {
            uint32_t length=0;EffectInvocationTraceRequest request={.function=f,.domain=domain};
            XrXirStatus status=effect_invocation_public_walk(work,certificate,&request,limit,&length);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_public_trace(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t limit,uint32_t function,
    const XrXirRootEffects *facts,XrXirRootCauseTrace **output) {
    if (!xir_compile_context_valid(work) || !certificate || certificate->resources!=work->resources ||
        !facts || !output || *output || function>=certificate->bodies.function_count ||
        limit!=certificate->bodies.function_count) return XR_XIR_BAD_STRUCTURE;
    if (facts->requires_root!=certificate->facts[function].requires_root ||
        facts->unresolved!=certificate->facts[function].unresolved) return XR_XIR_BAD_STRUCTURE;
    uint32_t counts[2]={0};EffectInvocationTraceRequest request={.function=function};
    XrXirStatus status=effect_invocation_public_walk(work,certificate,&request,limit,&counts[0]);
    request.domain=1;
    if (status==XR_XIR_OK) status=effect_invocation_public_walk(work,certificate,&request,limit,&counts[1]);
    if (status!=XR_XIR_OK) return status;
    EffectInvocationCauseTrace *private_trace=NULL;
    status=effect_invocation_certificate_trace(work,certificate,function,&private_trace);
    if (status!=XR_XIR_OK) return status;
    uint64_t total=(uint64_t)counts[0]+counts[1];XrXirRootCauseTrace *trace=NULL;
    if (total>(SIZE_MAX-sizeof(*trace))/sizeof(XrXirRootCauseStep)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) {
        size_t bytes=sizeof(*trace)+(size_t)total*sizeof(XrXirRootCauseStep);
        trace=xir_compile_calloc(work,1,bytes,&status);
        if (!trace) status=status==XR_XIR_OK?XR_XIR_OUT_OF_MEMORY:status;
    }
    XrXirRootCauseStep *steps=trace?effect_root_trace_storage(trace):NULL;
    uint32_t copied=0;
    for (uint32_t d=0;d<2 && status==XR_XIR_OK;++d) {
        XrXirRootEffectWitness selected={0};bool symbolic=false;
        status=effect_invocation_public_selected(work,certificate,function,d,&selected,&symbolic);
        if (status!=XR_XIR_OK) break;
        if (symbolic) {
            if (counts[d]!=1 || !xir_compile_work(work,sizeof(*steps))) {
                status=counts[d]!=1?XR_XIR_BAD_STRUCTURE:XR_XIR_BUDGET;break;
            }
            steps[copied++]=(XrXirRootCauseStep){certificate->origins[function].function,
                selected.instruction,selected.callee,selected.slot,selected.distance,selected.cause};continue;
        }
        if (private_trace->counts[d]!=counts[d]) { status=XR_XIR_BAD_STRUCTURE;break; }
        uint32_t offset=d?private_trace->counts[0]:0;
        for (uint32_t i=0;i<counts[d];++i) {
            if (!xir_compile_work(work,sizeof(*steps))) { status=XR_XIR_BUDGET;break; }
            EffectInvocationTraceStep step=private_trace->steps[offset+i];XrXirRootEffectWitness w=step.witness;
            steps[copied++]=(XrXirRootCauseStep){step.function,w.instruction,w.callee,w.slot,w.distance,w.cause};
        }
    }
    if (status==XR_XIR_OK && copied!=total) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(*trace))) status=XR_XIR_BUDGET;
    effect_invocation_trace_free(private_trace);
    if (status!=XR_XIR_OK) { xr_xir_compile_root_cause_trace_free(trace);return status; }
    trace->facts=*facts;trace->counts[0]=counts[0];trace->counts[1]=counts[1];*output=trace;return XR_XIR_OK;
}
