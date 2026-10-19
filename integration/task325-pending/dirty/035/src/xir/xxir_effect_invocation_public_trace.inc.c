/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_public_trace.inc.c - Exact public cause projection
 *
 * KEY CONCEPT:
 *   Equation environments certify each real edge. Public function ordinals
 *   identify declarations; descending paths are bounded by actual Final keys.
 */
/* Symbolic dependencies are public terminals at their actual root consumer,
 * as in the receiving canonical formula. The complete environment/atom cause
 * remains independently sealed and is verified before this projection. */
static XrXirStatus effect_invocation_public_transform(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,
    const EffectInvocationProjectedCause *selection,XrXirRootEffectWitness *output,bool *symbolic) {
    if (!xir_compile_context_valid(work) || !certificate || !certificate->final ||
        !certificate->final->summary.functions || function>=certificate->final->summary.count ||
        !selection || !selection->witness || !symbolic) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
    uint32_t atom=selection->atom;const XrXirRootEffectWitness *witness=selection->witness;
    uint32_t kind=0,index=0;*symbolic=false;
    if (atom!=UINT32_MAX && atom>=2) {
        const EffectInvocationFinal *equations=certificate->final;
        const EffectInvocationNode *node=&equations->nodes[equations->roots[function]];
        uint32_t parameters=certificate->body->functions[node->root].parameter_count;
        if (atom-2<parameters) { kind=XR_XIR_ROOT_TERM_PARAMETER;index=atom-2; }
        else if (atom-2-parameters<parameters) { kind=XR_XIR_ROOT_TERM_CELL_PARAMETER;index=atom-2-parameters; }
        else if (atom==(uint64_t)parameters*2+2) { kind=XR_XIR_ROOT_TERM_CONTEXT_CALL;index=witness->instruction; }
        else return XR_XIR_BAD_STRUCTURE;
        bool found=false;uint32_t terms=0;
        if (!xir_compile_work(work,sizeof(EffectInvocationTermTarget))) return XR_XIR_BUDGET;
        const EffectInvocationTermTarget target={certificate->final->summary.functions[function].capacity,kind,index,&found};
        XrXirStatus status=effect_invocation_certificate_terms(work,certificate,function,&target,&terms);
        if (status!=XR_XIR_OK) return status;
        if (!found || node->body!=function || (kind!=XR_XIR_ROOT_TERM_CONTEXT_CALL && node->root!=function) ||
            witness->instruction>=certificate->body->functions[function].instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        *symbolic=true;
    }
    /* Public length validation consumes only the symbolic decision. A real
     * witness output still constructs and charges its complete value. */
    if (output) {
        if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
        if (*symbolic) {
            XrXirRootEffectCause cause=kind==XR_XIR_ROOT_TERM_PARAMETER?XR_XIR_ROOT_CAUSE_PARAMETER:
                kind==XR_XIR_ROOT_TERM_CELL_PARAMETER?XR_XIR_ROOT_CAUSE_CELL_PARAMETER:XR_XIR_ROOT_CAUSE_CONTEXT_CALL;
            *output=(XrXirRootEffectWitness){cause,witness->instruction,UINT32_MAX,
                kind==XR_XIR_ROOT_TERM_CONTEXT_CALL?UINT32_MAX:index,0};
        } else *output=*witness;
    }
    return XR_XIR_OK;
}

/* The capacity comes from this call's owned output allocation. It must match
 * the newly proved path before a callback can cause any out-of-range write. */
typedef struct EffectInvocationPublicTraceOutput {
    XrXirRootCauseStep *steps;
    uint32_t count;
} EffectInvocationPublicTraceOutput;

static XrXirStatus effect_invocation_public_walk_selection(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectInvocationTraceRequest *request,
    uint32_t *length,const EffectInvocationPublicTraceOutput *output,const EffectInvocationTraceSelection *chosen) {
    if (!length || (output && !!output->steps!=!!output->count)) return XR_XIR_BAD_STRUCTURE;
    uint32_t count=0;EffectInvocationTraceSelection selection={UINT32_MAX,{0}};
    XrXirStatus status=effect_invocation_trace_walk_selected(work,certificate,request,&count,&selection,chosen);
    if (status!=XR_XIR_OK) return status;
    bool symbolic=false;
    if (!xir_compile_work(work,sizeof(EffectInvocationProjectedCause))) return XR_XIR_BUDGET;
    const EffectInvocationProjectedCause projected={selection.atom,&selection.witness};
    XrXirRootEffectWitness terminal={0};
    /* The trace already checked the complete public projection. A length
     * request uses only the selected atom's symbolic form from that same read. */
    if(output)status=effect_invocation_public_transform(work,certificate,request->function,&projected,&terminal,&symbolic);
    else symbolic=selection.atom!=UINT32_MAX && selection.atom>=2;
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationFinal *equations=certificate->final;
    uint32_t limit=equations->count;
    if (!limit || limit>equations->capacity) return XR_XIR_BAD_STRUCTURE;
    uint32_t published=symbolic && count?1:count;
    if (output && output->count!=published) return XR_XIR_BAD_STRUCTURE;
    if (!count) { *length=0;return XR_XIR_OK; }
    if (symbolic) {
        if (output) {
            if (!xir_compile_work(work,sizeof(*output->steps))) return XR_XIR_BUDGET;
            output->steps[0]=(XrXirRootCauseStep){certificate->origins[request->function].function,
                terminal.instruction,terminal.callee,terminal.slot,terminal.distance,terminal.cause};
        }
        *length=1;return XR_XIR_OK;
    }
    if (count>limit) return XR_XIR_BAD_STRUCTURE;
    uint32_t atom=selection.atom;
    uint32_t current=equations->roots[request->function];
    for (uint32_t at=0;at<count;++at) {
        if (!xir_compile_work(work,8)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&equations->nodes[current];
        const EffectInvocationCause *cause=&node->causes[atom];
        XrXirRootEffectWitness witness=effect_invocation_public_witness(certificate,node,cause);
        if (!witness.cause || witness.distance>=limit || witness.distance!=count-at-1)
            return XR_XIR_BAD_STRUCTURE;
        if (output) {
            if (!xir_compile_work(work,sizeof(*output->steps))) return XR_XIR_BUDGET;
            output->steps[at]=(XrXirRootCauseStep){certificate->origins[node->body].function,
                witness.instruction,witness.callee,witness.slot,witness.distance,witness.cause};
        }
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

/* Every public domain is proved before publication. Function and environment
 * identities are separate: several keys may execute one recursive body. */
static XrXirStatus effect_invocation_public_walk(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectInvocationTraceRequest *request,
    uint32_t *length,const EffectInvocationPublicTraceOutput *output) {
    return effect_invocation_public_walk_selection(work,certificate,request,length,output,NULL);
}

static XrXirStatus effect_invocation_public_gate(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate) {
    if (!xir_compile_context_valid(work) || !certificate || !effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || certificate->resources!=work->resources ||
        !certificate->final || !certificate->final->count ||
        certificate->final->count>certificate->final->capacity) return XR_XIR_BAD_STRUCTURE;
    uint32_t functions=effect_body_count(certificate->body);
    for (uint32_t f=0;f<functions;++f) {
        uint32_t atoms[4];XrXirStatus status=effect_invocation_projection_domains(work,certificate,f,0,4,atoms);
        if(status!=XR_XIR_OK) return status;
        for (uint32_t domain=0;domain<4;++domain) {
            if(!xir_compile_work(work,sizeof(uint32_t))) return XR_XIR_BUDGET;
            EffectInvocationTraceSelection selected={atoms[domain],{0}};
            status=effect_invocation_selected_witness(work,certificate,f,atoms[domain],&selected.witness);
            uint32_t length=0;EffectInvocationTraceRequest request={.function=f,.domain=domain};
            if(status==XR_XIR_OK)status=effect_invocation_public_walk_selection(work,certificate,&request,&length,NULL,&selected);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_public_trace(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,
    const XrXirRootEffects *facts,XrXirRootCauseTrace **output) {
    if (!xir_compile_context_valid(work) || !certificate || !effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources || certificate->resources!=work->resources ||
        !facts || !output || *output || function>=effect_body_count(certificate->body) ||
        !certificate->final) return XR_XIR_BAD_STRUCTURE;
    XrXirRootEffects expected={0};
    XrXirStatus fact_status=effect_invocation_certificate_facts(work,certificate,function,&expected);
    if (fact_status!=XR_XIR_OK) return fact_status;
    if (facts->requires_root!=expected.requires_root || facts->unresolved!=expected.unresolved)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t counts[2]={0};EffectInvocationTraceRequest request={.function=function};
    XrXirStatus status=effect_invocation_public_walk(work,certificate,&request,&counts[0],NULL);
    request.domain=1;
    if (status==XR_XIR_OK) status=effect_invocation_public_walk(work,certificate,&request,&counts[1],NULL);
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
        request.domain=d;uint32_t written=UINT32_MAX;
        const EffectInvocationPublicTraceOutput target={counts[d]?steps+copied:NULL,counts[d]};
        status=effect_invocation_public_walk(work,certificate,&request,&written,&target);
        if (status==XR_XIR_OK && written!=counts[d]) status=XR_XIR_BAD_STRUCTURE;
        if (status==XR_XIR_OK) copied+=written;
    }
    if (status==XR_XIR_OK && copied!=total) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(*trace))) status=XR_XIR_BUDGET;
    /* The sole allocation can invoke external code. Re-read the current Final
     * facts after filling; no temporary owner is freed after this comparison. */
    if (status==XR_XIR_OK) status=effect_invocation_certificate_facts(work,certificate,function,&expected);
    if (status==XR_XIR_OK && (facts->requires_root!=expected.requires_root || facts->unresolved!=expected.unresolved))
        status=XR_XIR_BAD_STRUCTURE;
    if (status!=XR_XIR_OK) { xr_xir_compile_root_cause_trace_free(trace);return status; }
    trace->facts=*facts;trace->counts[0]=counts[0];trace->counts[1]=counts[1];*output=trace;return XR_XIR_OK;
}
