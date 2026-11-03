/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_root_method.inc.c - Actual bound receiver parameter edges
 *
 * KEY CONCEPT:
 *   A method value captures its receiver separately from the explicit arguments.
 *   Every admitted override contributes the same physical parameter boundary.
 */
#include "xxir_class_method_internal.h"
static XrXirStatus view_root_reference_producer(ViewRootCheck *c,uint32_t f,uint32_t value,
    uint32_t *output,XrCompileRequestControl *control) {
    *output=UINT32_MAX;const XrXirFunction *body=&c->module->functions[f];uint32_t progress=0;
    for(uint32_t depth=0;value>=body->parameter_count && depth<body->instruction_count;++depth) {
        XrXirStatus status=xir_receive_step(control,&progress);if(status!=XR_XIR_OK)return status;
        uint32_t i=value-body->parameter_count;if(i>=body->instruction_count)return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *producer=&body->instructions[i];
        if(producer->op==XR_XIR_CLASS_METHOD_REF || producer->op==XR_XIR_FUNCTION_REF) {
            *output=i;return XR_XIR_OK;
        }
        if(producer->op!=XR_XIR_COPY && producer->op!=XR_XIR_OWNED_RETAIN &&
            producer->op!=XR_XIR_FUNCTION_WEAKEN)return XR_XIR_OK;
        if(producer->args[0]>=value)return XR_XIR_BAD_STRUCTURE;
        value=producer->args[0];
    }
    return XR_XIR_OK;
}
typedef struct ViewRootMethodEdges {
    ViewRootCheck *check;
    const XrXirInstruction *binding,*call;
    uint32_t function,*count;
    bool fill;
} ViewRootMethodEdges;
static XrXirStatus view_root_method_edge(ViewRootMethodEdges *q,uint32_t target,
    uint32_t parameter,uint32_t value) {
    ViewRootCheck *c=q->check;
    if(!view_root_tracked(c,c->module->functions[target].parameters[parameter]))return XR_XIR_OK;
    if(*q->count==UINT32_MAX)return XR_XIR_BUDGET;
    if(q->fill) {
        if(*q->count>=c->edge_count)return XR_XIR_BAD_STRUCTURE;
        uint32_t slot=c->offsets[target]+parameter;
        c->edges[*q->count]=(ViewRootEdge){q->function,value,c->heads[slot]};c->heads[slot]=*q->count;
    }
    ++*q->count;return XR_XIR_OK;
}
static XrXirStatus view_root_method_candidate(void *owner,const XirClassMethodTarget *target) {
    ViewRootMethodEdges *q=owner;ViewRootCheck *c=q->check;
    if(target->function>=c->module->function_count || !c->module->functions[target->function].parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *callee=&c->module->functions[target->function];
    XrXirStatus status=view_root_method_edge(q,target->function,0,q->binding->args[0]);
    if(!q->call || status!=XR_XIR_OK)return status;
    if(q->call->args[1]!=callee->parameter_count-1u)return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *caller=&c->module->functions[q->function];
    for(uint32_t p=0;p<q->call->args[1] && status==XR_XIR_OK;++p)
        status=view_root_method_edge(q,target->function,p+1u,caller->operands[q->call->args[0]+p]);
    return status;
}
static XrXirStatus view_root_method_edges(ViewRootMethodEdges *query,uint32_t instruction,
    bool *handled,XrCompileRequestControl *control) {
    ViewRootCheck *c=query->check;uint32_t f=query->function;
    *handled=false;const XrXirFunction *body=&c->module->functions[f];
    const XrXirInstruction *op=&body->instructions[instruction];uint32_t producer=UINT32_MAX;
    if(op->op==XR_XIR_CLASS_METHOD_REF)producer=instruction;
    else if(op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {
        if(op->immediate<0 || (uint64_t)op->immediate>UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=view_root_reference_producer(c,f,(uint32_t)op->immediate,&producer,control);
        if(status!=XR_XIR_OK)return status;
    }
    if(producer==UINT32_MAX)return XR_XIR_OK;
    const XrXirInstruction *binding=&body->instructions[producer];
    if(binding->op!=XR_XIR_CLASS_METHOD_REF)return XR_XIR_OK;
    XrXirType receiver=xr_xir_operand_type(body,binding->args[0]);XirClassMethodTarget selected={0};
    XrXirStatus status=xir_compile_class_method_site(c->module,f,producer,receiver,&selected,control);
    if(status!=XR_XIR_OK)return status;
    query->binding=binding;query->call=producer==instruction?NULL:op;
    *handled=true;
    return xir_compile_class_method_candidates(c->module,receiver,(uint32_t)binding->immediate,
        view_root_method_candidate,query,control);
}
