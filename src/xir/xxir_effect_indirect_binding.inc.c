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
/* The immutable request checks each metadata directory once, independently of
 * how many arguments consult it. This view owns nothing and never outlives the
 * current verification; the public candidate opens a fresh view each time. */
XR_FUNC XrXirStatus xir_effect_indirect_bindings_open(const XrXirCompileContext *context,
    const XrXirModule *module,XirEffectIndirectBindings *empty) {
    if(!xir_compile_context_valid(context) || !module || !module->types || !module->functions ||
        !module->function_count || !empty || empty->module) return XR_XIR_BAD_STRUCTURE;
    const XrXirProvenance *p=module->provenance;
    if(!p) return XR_XIR_BAD_TYPE;
    const XrXirModule *source=module;XrXirStatus status=XR_XIR_OK;
    if(xir_effect_kind_is_closed(p->kind)) {
        status=effect_contract_instance(context,module,p);
        if(status!=XR_XIR_OK) return status;
        source=&p->source->module;
    } else if(p->kind!=XR_XIR_EVIDENCE_TEMPLATE ||
        (module->stage!=XR_XIR_BUILT && module->stage!=XR_XIR_CHECKED)) return XR_XIR_BAD_TYPE;
    if(!source->types || !source->functions || !source->function_count || !source->provenance ||
        source->provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE) return XR_XIR_BAD_STRUCTURE;
    for(uint32_t f=0;f<source->function_count;++f) {
        const XrXirFunction *function=source->functions+f;
        if((function->instruction_count && !function->instructions) ||
            (function->parameter_count && !function->parameters)) return XR_XIR_BAD_STRUCTURE;
    }
    status=effect_contract_template(source,source->provenance);
    if(status!=XR_XIR_OK) return status;
    *empty=(XirEffectIndirectBindings){module,source,p};return XR_XIR_OK;
}
static const XrXirEffectCallBinding *effect_contract_indirect_row(const XrXirFunctionEffectContract *consumer,
    uint32_t instruction,uint32_t parameter) {
    uint32_t first=0,last=consumer->binding_count;
    while(first<last) {
        uint32_t middle=first+(last-first)/2;const XrXirEffectCallBinding *row=consumer->bindings+middle;
        if(row->family<XR_XIR_EFFECT_BINDING_INDIRECT || (row->family==XR_XIR_EFFECT_BINDING_INDIRECT &&
            (row->instruction<instruction || (row->instruction==instruction && row->parameter<parameter)))) first=middle+1;
        else last=middle;
    }
    if(first==consumer->binding_count) return NULL;
    const XrXirEffectCallBinding *row=consumer->bindings+first;
    return row->family==XR_XIR_EFFECT_BINDING_INDIRECT && row->instruction==instruction && row->parameter==parameter?row:NULL;
}
static const XrXirRootValueIdentity *effect_contract_indirect_identity(const XrXirFunctionEffectContract *consumer,
    uint32_t producer) {
    uint32_t first=0,last=consumer->value_count;
    while(first<last) {
        uint32_t middle=first+(last-first)/2;
        if(consumer->values[middle].instruction<producer) first=middle+1;
        else last=middle;
    }
    return first<consumer->value_count && consumer->values[first].instruction==producer?consumer->values+first:NULL;
}
XR_FUNC XrXirStatus xir_effect_indirect_binding_candidate(const XrXirCompileContext *context,
    const XirEffectIndirectBindings *view,uint32_t caller,uint32_t instruction,uint32_t parameter) {
    if(!xir_compile_context_valid(context) || !view || !view->module || !view->source || !view->provenance ||
        !view->source->provenance || view->source->provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE ||
        view->module->provenance!=view->provenance) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module=view->module;const XrXirProvenance *p=view->provenance;
    XrXirType formal=XR_XIR_UNIT;
    XrXirStatus status=effect_contract_indirect_parameter(module,caller,instruction,parameter,&formal);
    if(status!=XR_XIR_OK) return status;
    const XrXirFunction *function=module->functions+caller;
    const XrXirInstruction *op=function->instructions+instruction;
    const XrXirModule *source=view->source;uint32_t source_function=caller;
    const XrXirOrigin *origin=NULL;
    if(xir_effect_kind_is_closed(p->kind)) {
        origin=p->origins+caller;source_function=origin->function;
    }
    const XrXirProvenance *record=source->provenance;
    if(source_function>=source->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *original=source->functions+source_function;
    XrXirType original_formal=XR_XIR_UNIT;
    status=effect_contract_indirect_parameter(source,source_function,instruction,parameter,&original_formal);
    if(status!=XR_XIR_OK) return status;
    const XrXirInstruction *before=original->instructions+instruction;
    if(function->parameter_count!=original->parameter_count || op->op!=before->op ||
        op->immediate!=before->immediate) return XR_XIR_BAD_TYPE;
    const XrXirFunctionEffectContract *consumer=record->contracts+source_function;
    const XrXirEffectCallBinding *binding=effect_contract_indirect_row(consumer,instruction,parameter);
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
        copy->args[0]>=(uint64_t)function->parameter_count+function->instruction_count || copy->args[0]==value ||
        original_copy->args[0]>=(uint64_t)original->parameter_count+original->instruction_count ||
        original_copy->args[0]!=copy->args[0] ||
        copy->type!=xr_xir_operand_type(function,copy->args[0])) return XR_XIR_BAD_TYPE;
    const XrXirRootValueIdentity *identity=effect_contract_indirect_identity(consumer,producer);
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
static XrXirStatus effect_contract_indirect_candidate(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t caller,uint32_t instruction,uint32_t parameter) {
    XirEffectIndirectBindings view={0};
    XrXirStatus status=xir_effect_indirect_bindings_open(context,module,&view);
    return status==XR_XIR_OK?xir_effect_indirect_binding_candidate(context,&view,caller,instruction,parameter):status;
}
