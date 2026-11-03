/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_access_uncaptured.inc.c - Complete uncaptured callback return facts
 *
 * KEY CONCEPT:
 *   A real body returning only zero-capture FunctionRefs can prove an empty
 *   environment. This proves no ROOT, suspension, or referent-access promise.
 */
static XrXirStatus view_access_uncaptured_reference(ViewAccess *a,const XrXirFunction *body,
    uint32_t value,bool *output) {
    *output=false;const XrXirModule *module=a->resolver->check.module;uint32_t progress=0;
    for(uint32_t depth=0;value>=body->parameter_count && depth<body->instruction_count;++depth) {
        XrXirStatus status=xir_receive_step(a->cells?a->cells->control:NULL,&progress);if(status!=XR_XIR_OK)return status;
        uint32_t i=value-body->parameter_count;if(i>=body->instruction_count)return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *op=&body->instructions[i];
        if(op->op==XR_XIR_COPY || op->op==XR_XIR_OWNED_RETAIN || op->op==XR_XIR_FUNCTION_WEAKEN) {
            if(op->args[0]>=value || !xr_xir_callable_signature(module->types,op->type))return XR_XIR_OK;
            value=op->args[0];continue;
        }
        if(op->op!=XR_XIR_FUNCTION_REF || op->args[1] || op->immediate<0 ||
            (uint64_t)op->immediate>=module->function_count || op->type_arguments[0] || op->type_arguments[1])
            return XR_XIR_OK;
        uint32_t target=(uint32_t)op->immediate;
        if(module->generics && module->generics[target].parameter_count)return XR_XIR_OK;
        const XrXirFunction *function=&module->functions[target];
        const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,op->type);
        if(!signature || signature->parameter_count!=function->parameter_count ||
            signature->result!=function->result || signature->element!=function->error_type ||
            !module->declarations || !module->declarations->functions)return XR_XIR_OK;
        const XrXirFunctionIdentity *identity=&module->declarations->functions[target];
        if(((signature->flags&XR_XIR_CALLABLE_NO_SUSPEND) && !(identity->promises&XR_XIR_FUNCTION_NO_SUSPEND)) ||
            !!(signature->flags&XR_XIR_CALLABLE_REST)!=!!(identity->promises&XR_XIR_FUNCTION_REST))return XR_XIR_OK;
        for(uint32_t p=0;p<signature->parameter_count;++p) {
            status=xir_receive_step(a->cells?a->cells->control:NULL,&progress);if(status!=XR_XIR_OK)return status;
            const XrXirCallableParameter *parameter=&signature->parameters[p];
            if(!xr_xir_callable_parameter_storage_valid(module->types,parameter) ||
                parameter->type!=function->parameters[p] ||
                parameter->mode!=xr_xir_declared_parameter_mode(module->types,function->parameters[p],
                    function->consume_parameters,function->consume_parameter_count,p))return XR_XIR_OK;
        }
        *output=true;return XR_XIR_OK;
    }
    return XR_XIR_OK;
}
static XrXirStatus view_access_uncaptured_result(ViewAccess *a,uint32_t value,bool *output) {
    *output=false;const XrXirModule *module=a->resolver->check.module;uint32_t progress=0;
    const XrXirInstruction *call=NULL;
    for(uint32_t depth=0;value>=a->body->parameter_count && depth<a->body->instruction_count;++depth) {
        XrXirStatus status=xir_receive_step(a->cells?a->cells->control:NULL,&progress);if(status!=XR_XIR_OK)return status;
        uint32_t i=value-a->body->parameter_count;if(i>=a->body->instruction_count)return XR_XIR_BAD_STRUCTURE;
        call=&a->body->instructions[i];
        if(call->op!=XR_XIR_COPY && call->op!=XR_XIR_OWNED_RETAIN && call->op!=XR_XIR_FUNCTION_WEAKEN)break;
        if(call->args[0]>=value)return XR_XIR_BAD_STRUCTURE;
        value=call->args[0];call=NULL;
    }
    if(!call || call->op!=XR_XIR_CALL || call->immediate<0 ||
        (uint64_t)call->immediate>=module->function_count || call->type_arguments[0] || call->type_arguments[1])
        return XR_XIR_OK;
    uint32_t target=(uint32_t)call->immediate;
    if(!module->declarations || !module->declarations->functions ||
        module->declarations->functions[target].native_callable ||
        (module->generics && module->generics[target].parameter_count))return XR_XIR_OK;
    const XrXirFunction *body=&module->functions[target];uint32_t returns=0;
    if(body->result!=call->type || !xr_xir_callable_signature(module->types,body->result) ||
        !body->instructions)return XR_XIR_OK;
    for(uint32_t i=0;i<body->instruction_count;++i) {
        XrXirStatus status=xir_receive_step(a->cells?a->cells->control:NULL,&progress);if(status!=XR_XIR_OK)return status;
        const XrXirInstruction *op=&body->instructions[i];if(op->op!=XR_XIR_RETURN)continue;
        bool empty=false;status=view_access_uncaptured_reference(a,body,op->args[0],&empty);
        if(status!=XR_XIR_OK || !empty)return status;
        ++returns;
    }
    *output=returns!=0;return XR_XIR_OK;
}
