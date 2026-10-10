/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_trace.inc.c - Producer-independent actual equation cause traces
 */
typedef struct EffectInvocationTraceStep {
    uint32_t equation,root,function;
    XrXirRootEffectWitness witness;
} EffectInvocationTraceStep;
typedef struct EffectInvocationCauseTrace {
    XrCompileResources *resources;
    XrXirRootEffects facts;
    uint32_t counts[2];
    EffectInvocationTraceStep *steps;
} EffectInvocationCauseTrace;
typedef struct EffectInvocationTraceRequest {
    uint32_t function,domain;
    EffectInvocationTraceStep *steps;
} EffectInvocationTraceRequest;

static void effect_invocation_trace_free(EffectInvocationCauseTrace *trace) {
    xr_compile_resources_free(trace);
}

/* The selected atom retains the namespace's complete physical input identity
 * for the whole descent. Repeated declaration IDs cannot replace an actual
 * equation edge, and public ordinals are emitted only after that proof. */
static XrXirStatus effect_invocation_trace_walk(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,const EffectInvocationTraceRequest *request,
    uint32_t *length) {
    if (!certificate || certificate->resources!=work->resources || !request || !length ||
        request->domain>=4 || request->function>=certificate->bodies.function_count ||
        !certificate->facts || !certificate->witnesses || !certificate->projection_atoms)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t atom=UINT32_MAX;XrXirRootEffectWitness witness={0};
    XrXirStatus status=effect_invocation_projection_select(work,certificate,request->function,request->domain,
        &atom,&witness);
    if (status!=XR_XIR_OK) return status;
    size_t at=(size_t)request->domain*certificate->bodies.function_count+request->function;
    if (certificate->projection_atoms[at]!=atom ||
        !effect_invocation_witness_equal(&certificate->witnesses[at],&witness)) return XR_XIR_BAD_STRUCTURE;
    bool fact=(request->domain&2)?!!(certificate->solution->formulas[request->function].constant_mask&
        ((request->domain&1)?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED)):
        (request->domain&1)?certificate->facts[request->function].unresolved:
        certificate->facts[request->function].requires_root;
    if (fact!=(atom!=UINT32_MAX)) return XR_XIR_BAD_STRUCTURE;
    if (!fact) { *length=0;return XR_XIR_OK; }
    const EffectInvocationOwner *equations=certificate->equations;
    uint32_t current=equations->roots[request->function],count=0;
    for (;;) {
        if (!xir_compile_work(work,8)) return XR_XIR_BUDGET;
        if (current>=equations->count || count>=equations->count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationNode *node=&equations->nodes[current];
        if (node->body>=certificate->bodies.function_count || atom>=node->cause_count)
            return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationCause *cause=&node->causes[atom];
        if (cause->witness.callee!=UINT32_MAX && cause->witness.callee>=certificate->bodies.function_count)
            return XR_XIR_BAD_STRUCTURE;
        XrXirRootEffectWitness public_value=effect_invocation_public_witness(certificate,node,cause);
        if (request->steps) {
            if (!xir_compile_work(work,sizeof(*request->steps))) return XR_XIR_BUDGET;
            request->steps[count]=(EffectInvocationTraceStep){current,node->root,
                certificate->origins[node->body].function,public_value};
        }
        ++count;
        if (!cause->distance) {
            if (cause->next!=UINT32_MAX || cause->edge!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            *length=count;return XR_XIR_OK;
        }
        if (cause->next>=equations->count || cause->edge>=equations->edge_count ||
            equations->edges[cause->edge].caller!=current ||
            equations->edges[cause->edge].target!=cause->next ||
            equations->nodes[cause->next].causes[atom].distance+1!=cause->distance)
            return XR_XIR_BAD_STRUCTURE;
        current=cause->next;
    }
}

static inline XrXirStatus effect_invocation_certificate_trace(const XrXirCompileContext *work,
    const EffectInvocationCertificate *certificate,uint32_t function,EffectInvocationCauseTrace **output) {
    if (!xir_compile_context_valid(work) || !certificate || certificate->resources!=work->resources ||
        !output || *output || function>=certificate->bodies.function_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t counts[2]={0};EffectInvocationTraceRequest request={.function=function};
    XrXirStatus status=effect_invocation_trace_walk(work,certificate,&request,&counts[0]);
    request.domain=1;
    if (status==XR_XIR_OK) status=effect_invocation_trace_walk(work,certificate,&request,&counts[1]);
    if (status!=XR_XIR_OK) return status;
    uint64_t total=(uint64_t)counts[0]+counts[1];
    if (total>(SIZE_MAX-sizeof(EffectInvocationCauseTrace))/sizeof(EffectInvocationTraceStep)) return XR_XIR_BUDGET;
    size_t bytes=sizeof(EffectInvocationCauseTrace)+(size_t)total*sizeof(EffectInvocationTraceStep);
    EffectInvocationCauseTrace *trace=xir_compile_calloc(work,1,bytes,&status);
    if (!trace) return status;
    /* The trailing records share this allocation and its resource charge. */
    EffectInvocationTraceStep *steps=(EffectInvocationTraceStep *)(trace+1);uint32_t copied=0;
    trace->steps=steps;
    request=(EffectInvocationTraceRequest){function,0,steps};
    status=effect_invocation_trace_walk(work,certificate,&request,&copied);
    if (status==XR_XIR_OK && copied!=counts[0]) status=XR_XIR_BAD_STRUCTURE;
    request=(EffectInvocationTraceRequest){function,1,steps+counts[0]};
    if (status==XR_XIR_OK) status=effect_invocation_trace_walk(work,certificate,&request,&copied);
    if (status==XR_XIR_OK && copied!=counts[1]) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(*trace))) status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) { effect_invocation_trace_free(trace);return status; }
    trace->resources=work->resources;trace->facts=certificate->facts[function];
    trace->counts[0]=counts[0];trace->counts[1]=counts[1];
    *output=trace;return XR_XIR_OK;
}
