/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture_reference.inc.c - Current FunctionRef identity typing
 *
 * KEY CONCEPT:
 *   Identity typing and the actual capture relation are separate obligations.
 */
static XrXirStatus effect_capture_reference_seed(const EffectCaptureRead *read,
    uint32_t site,bool *output) {
    *output=false;const EffectInvocationSite *reference=&read->final->sites[site];
    EffectBodyFunctionHead target={0};XrXirInstruction storage;const XrXirInstruction *op=NULL;
    XrXirStatus status=effect_body_request_head(&read->body,reference->target,&target);
    if(status==XR_XIR_OK) status=effect_body_request_instruction(&read->body,reference->function,reference->instruction,&storage,&op);
    if(status!=XR_XIR_OK) return status;
    if(reference->captures>target.parameter_count) return XR_XIR_BAD_STRUCTURE;
    if(op->op==XR_XIR_GENERATOR_NEW) {
        const XrXirDeclarations *d=read->body.domain.declarations;
        if(!d || !d->functions || !(d->functions[reference->target].promises&XR_XIR_FUNCTION_GENERATOR) ||
            op->type!=target.result || !effect_physical_iterator(read->body.domain.types,op->type) ||
            reference->captures!=target.parameter_count) return XR_XIR_BAD_TYPE;
        if(xr_xir_type_span(read->body.domain.types,target.result)) return XR_XIR_OK;
    } else {
        const XrXirTypeNode *signature=NULL;
        status=effect_final_edge_signature(read->work,read->body.domain.types,op->type,&signature);
        if(status!=XR_XIR_OK) return status;
        if(signature->parameter_span || xr_xir_type_span(read->body.domain.types,target.result)) return XR_XIR_OK;
        if(signature->result!=target.result || signature->parameter_count!=target.parameter_count-reference->captures)
            return XR_XIR_BAD_TYPE;
        for(uint32_t p=0;p<signature->parameter_count;++p) {
            if(!xir_compile_work(read->work,7)) return XR_XIR_BUDGET;
            const XrXirCallableParameter *parameter=&signature->parameters[p];
            if(!xr_xir_callable_parameter_storage_valid(read->body.domain.types,parameter) ||
                (parameter->mode==XR_PARAM_REF && reference->captures)) return XR_XIR_BAD_TYPE;
            if(parameter->type!=target.parameters[reference->captures+p]) return XR_XIR_BAD_TYPE;
        }
    }
    EffectBodyFunctionHead source={0};status=effect_body_request_head(&read->body,reference->function,&source);
    if(status!=XR_XIR_OK) return status;
    for(uint32_t p=0;p<reference->captures;++p) {
        if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
        XrXirType actual=0;status=effect_body_request_type(&read->body,reference->function,source.operands[op->args[0]+p],&actual);
        if(status!=XR_XIR_OK) return status;
        if(xr_xir_type_span(read->body.domain.types,target.parameters[p]) || xr_xir_type_span(read->body.domain.types,actual))
            return XR_XIR_OK;
    }
    *output=true;return XR_XIR_OK;
}
