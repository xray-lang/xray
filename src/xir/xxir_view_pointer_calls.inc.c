/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_pointer_calls.inc.c - Complete targets for pointer return origins
 *
 * KEY CONCEPT:
 *   Every admitted override names the same parameter root or an independently
 *   proven origin. An unknown function value cannot prove either from a type.
 */
#include "xxir_class_method_internal.h"
typedef struct ViewPointerTargets {
    const XrXirModule *module;
    const uint32_t *candidates;
    bool present;
    uint32_t candidate;
} ViewPointerTargets;
static XrXirStatus view_pointer_target(void *owner,const XirClassMethodTarget *target) {
    ViewPointerTargets *query=owner;
    if(target->function>=query->module->function_count)return XR_XIR_BAD_STRUCTURE;
    uint32_t candidate=query->candidates[target->function];
    if(candidate<UINT32_MAX-1u) {
        if(!candidate)return XR_XIR_UNRESOLVED;
        --candidate;
    }
    if(candidate==UINT32_MAX || (query->present && candidate!=query->candidate))return XR_XIR_UNRESOLVED;
    query->present=true;query->candidate=candidate;return XR_XIR_OK;
}
static XrXirStatus view_pointer_indirect(const XrXirModule *module,uint32_t function,
    uint32_t value,const uint32_t *candidates,XrCompileRequestControl *control,uint32_t *output) {
    *output=UINT32_MAX;const XrXirFunction *body=module->functions+function;uint32_t progress=0;
    for(uint32_t depth=0;value>=body->parameter_count && depth<body->instruction_count;++depth) {
        XrXirStatus status=xir_receive_step(control,&progress);if(status!=XR_XIR_OK)return status;
        uint32_t instruction=value-body->parameter_count;
        if(instruction>=body->instruction_count)return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *producer=body->instructions+instruction;
        if(producer->op==XR_XIR_FUNCTION_REF) {
            if(producer->immediate<0 || (uint64_t)producer->immediate>=module->function_count)
                return XR_XIR_BAD_STRUCTURE;
            uint32_t candidate=candidates[producer->immediate];
            if(candidate==UINT32_MAX-1u)*output=candidate;
            else if(candidate<UINT32_MAX-1u && candidate>=producer->args[1])*output=candidate-producer->args[1];
            return XR_XIR_OK;
        }
        if(producer->op==XR_XIR_CLASS_METHOD_REF) {
            XrXirType receiver=xr_xir_operand_type(body,producer->args[0]);XirClassMethodTarget selected={0};
            status=xir_compile_class_method_site(module,function,instruction,receiver,&selected,control);
            if(status!=XR_XIR_OK)return status;
            ViewPointerTargets query={module,candidates,false,UINT32_MAX};
            status=xir_compile_class_method_candidates(module,receiver,(uint32_t)producer->immediate,
                view_pointer_target,&query,control);
            if(status==XR_XIR_OK && query.present)*output=query.candidate;
            return status;
        }
        if(producer->op!=XR_XIR_COPY && producer->op!=XR_XIR_OWNED_RETAIN &&
            producer->op!=XR_XIR_FUNCTION_WEAKEN)return XR_XIR_OK;
        if(producer->args[0]>=value)return XR_XIR_BAD_STRUCTURE;
        value=producer->args[0];
    }
    return XR_XIR_OK;
}
