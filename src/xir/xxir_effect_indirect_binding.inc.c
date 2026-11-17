/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_indirect_binding.inc.c - Receiver-owned argument COPY proofs
 */
/* No inferred function target or previously accepted Source bit can stand in
 * for the actual receiver signature's READ/nonescaping ordinary Fn contract. */
static XrXirStatus effect_contract_indirect_parameter(const XrXirModule *module,uint32_t caller,
    uint32_t instruction,uint32_t parameter,XrXirType *output) {
    if(!module || !module->types || !module->functions || caller>=module->function_count || !output)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function=module->functions+caller;
    if(!function->instructions || instruction>=function->instruction_count ||
        (function->parameter_count && !function->parameters)) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=function->instructions+instruction;
    uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
    if((op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) || op->immediate<0 ||
        (uint64_t)op->immediate>=values || parameter>=op->args[1] ||
        op->args[0]>function->operand_count || op->args[1]>function->operand_count-op->args[0] ||
        !function->operands) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,
        xr_xir_operand_type(function,(uint32_t)op->immediate));
    if(!signature || (signature->flags&XR_XIR_CALLABLE_C_ABI) || !signature->parameters ||
        signature->parameter_count!=op->args[1] || signature->result!=op->type ||
        signature->parameters[parameter].mode!=XR_PARAM_READ || signature->parameters[parameter].escaping ||
        !xr_xir_callable_signature(module->types,signature->parameters[parameter].type) ||
        xr_xir_type_is_c_callable(module->types,signature->parameters[parameter].type)) return XR_XIR_BAD_TYPE;
    *output=signature->parameters[parameter].type;return XR_XIR_OK;
}
static XrXirStatus effect_contract_indirect_candidate(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t caller,uint32_t instruction,uint32_t parameter) {
    const XrXirProvenance *p=module->provenance;
    XrXirType formal=XR_XIR_UNIT;
    XrXirStatus status=effect_contract_indirect_parameter(module,caller,instruction,parameter,&formal);
    if(status!=XR_XIR_OK) return status;
    const XrXirFunction *function=module->functions+caller;
    const XrXirInstruction *op=function->instructions+instruction;
    const XrXirModule *source=module;uint32_t source_function=caller;
    const XrXirOrigin *origin=NULL;
    if(xir_effect_kind_is_closed(p->kind)) {
        status=effect_contract_instance(context,module,p);
        if(status!=XR_XIR_OK) return status;
        origin=p->origins+caller;source=&p->source->module;source_function=origin->function;
    } else if(p->kind!=XR_XIR_EVIDENCE_TEMPLATE ||
        (module->stage!=XR_XIR_BUILT && module->stage!=XR_XIR_CHECKED)) return XR_XIR_BAD_TYPE;
    const XrXirProvenance *record=source->provenance;
    if(!record || record->kind!=XR_XIR_EVIDENCE_TEMPLATE || record->source || record->origins || record->count ||
        record->bindings || record->binding_count || record->contract_count!=source->function_count)
        return XR_XIR_BAD_STRUCTURE;
    status=effect_contract_vector(record->contracts,record->contract_count,sizeof(*record->contracts));
    if(status!=XR_XIR_OK || source_function>=source->function_count) return status!=XR_XIR_OK?status:XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *original=source->functions+source_function;
    XrXirType original_formal=XR_XIR_UNIT;
    status=effect_contract_indirect_parameter(source,source_function,instruction,parameter,&original_formal);
    if(status!=XR_XIR_OK) return status;
    const XrXirInstruction *before=original->instructions+instruction;
    if(function->parameter_count!=original->parameter_count || op->op!=before->op ||
        op->immediate!=before->immediate) return XR_XIR_BAD_TYPE;
    const XrXirFunctionEffectContract *consumer=record->contracts+source_function;
    status=effect_contract_parameters(source,source_function,consumer);
    if(status==XR_XIR_OK) status=effect_contract_values(source,source_function,consumer);
    if(status==XR_XIR_OK) status=effect_contract_bindings(original,consumer);
    if(status!=XR_XIR_OK) return status;
    const XrXirEffectCallBinding *binding=NULL;
    for(uint32_t b=0;b<consumer->binding_count;++b) {
        const XrXirEffectCallBinding *row=consumer->bindings+b;
        if(row->family==XR_XIR_EFFECT_BINDING_INDIRECT && row->instruction==instruction && row->parameter==parameter)
            binding=row;
    }
    if(!binding) return XR_XIR_BAD_TYPE;
    uint32_t value=function->operands[op->args[0]+parameter];
    uint32_t original_value=original->operands[before->args[0]+parameter];
    if(value!=binding->value || original_value!=binding->value || value<function->parameter_count ||
        value>=(uint64_t)function->parameter_count+function->instruction_count) return XR_XIR_BAD_TYPE;
    uint32_t producer=value-function->parameter_count;
    if(producer>=original->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *copy=function->instructions+producer,*original_copy=original->instructions+producer;
    bool retained=module->stage==XR_XIR_LOWERED && copy->op==XR_XIR_OWNED_RETAIN;
    if((copy->op!=XR_XIR_COPY && !retained) || original_copy->op!=XR_XIR_COPY ||
        copy->args[0]>=value || original_copy->args[0]!=copy->args[0] ||
        copy->type!=xr_xir_operand_type(function,copy->args[0])) return XR_XIR_BAD_TYPE;
    const XrXirRootValueIdentity *identity=NULL;
    for(uint32_t v=0;v<consumer->value_count;++v)
        if(consumer->values[v].instruction==producer) identity=consumer->values+v;
    if(!identity || identity->mode!=XR_XIR_EFFECT_VALUE_CALL_BIND) return XR_XIR_BAD_TYPE;
    status=xr_xir_compile_callable_weakening(context,source->types,identity->declared_type,original_formal);
    if(status!=XR_XIR_OK) return status;
    if(!origin) return xr_xir_compile_callable_weakening(context,module->types,copy->type,identity->declared_type);
    XirEffectCallableBound selected={source->types,module->types,origin->arguments,origin->argument_count,original_formal,formal};
    status=xir_effect_callable_bound_matches(context,&selected);
    XirEffectCallableBound declared={source->types,module->types,origin->arguments,origin->argument_count,identity->declared_type,formal};
    if(status==XR_XIR_OK) status=xir_effect_callable_bound_matches(context,&declared);
    XirEffectCallableBound actual={module->types,module->types,NULL,0,formal,copy->type};
    return status==XR_XIR_OK?xir_effect_callable_bound_matches(context,&actual):status;
}
