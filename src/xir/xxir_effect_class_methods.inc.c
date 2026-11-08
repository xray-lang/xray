/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_class_methods.inc.c - Class slots in the complete owned body
 *
 * KEY CONCEPT:
 *   Candidate headers are read from the retained body, never a reconstructed Module.
 */
static XrXirStatus effect_class_method_header(const void *owner,uint32_t entry,XirClassMethodSignature *output) {
    const EffectBodyMaterial *material=owner;
    if(!effect_body_material_shape(material) || !output || entry>=effect_body_count(material))return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *function=&material->functions[entry];
    *output=(XirClassMethodSignature){function->name,function->name_length,function->parameters,function->parameter_count,
        function->result,function->error_type,function->consume_parameters,function->consume_parameter_count};return XR_XIR_OK;
}
static XrXirStatus effect_class_method_candidates(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,uint32_t function,uint32_t instruction,XirClassMethodVisitor visit,void *owner) {
    if(!effect_body_material_shape(material) || !work || material->resources!=work->resources ||
        function>=effect_body_count(material) || instruction>=material->functions[function].instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyRead read={.owner=work};XrXirInstruction op;
    const EffectBodyFunction *body=&material->functions[function];
    XrXirStatus status=effect_body_instruction_get(&read,body,instruction,&op);if(status!=XR_XIR_OK)return status;
    if(op.op!=XR_XIR_CLASS_METHOD_REF || op.immediate<0 || (uint64_t)op.immediate>=effect_body_count(material) ||
        op.args[0]>=(uint64_t)body->parameter_count+instruction || op.args[1] || op.targets[0] || op.targets[1] ||
        op.type_arguments[0] || op.type_arguments[1])return XR_XIR_BAD_STRUCTURE;
    XrXirType receiver;
    status=effect_body_operand_type(&read,body,op.args[0],&receiver);if(status!=XR_XIR_OK)return status;
    const EffectBodyFunction *anchor=&material->functions[op.immediate];
    const XrXirTypes *types=material->domain.types;
    const XrXirTypeNode *signature=xr_xir_callable_signature(types,op.type);
    if(!signature || !anchor->parameter_count || !anchor->parameters ||
        signature->parameter_count!=anchor->parameter_count-1 || signature->result!=anchor->result ||
        signature->element!=anchor->error_type || (signature->flags&XR_XIR_CALLABLE_REST) ||
        !xr_xir_class_is_subtype(types,receiver,anchor->parameters[0]) ||
        ((signature->flags&XR_XIR_CALLABLE_NO_SUSPEND) &&
         !(material->domain.declarations->functions[op.immediate].promises&XR_XIR_FUNCTION_NO_SUSPEND)))return XR_XIR_BAD_TYPE;
    for(uint32_t p=0;p<signature->parameter_count;++p)
        if(!xr_xir_callable_parameter_storage_valid(types,&signature->parameters[p]) ||
            signature->parameters[p].type!=anchor->parameters[p+1] ||
            signature->parameters[p].mode!=xr_xir_declared_parameter_mode(types,anchor->parameters[p+1],
                anchor->consume_parameters,anchor->consume_parameter_count,p+1))return XR_XIR_BAD_TYPE;
    XirClassMethodRead input={&material->domain,material,effect_class_method_header};
    /* The existing owned equation reader has no request-control field. */
    return xir_compile_class_method_candidates_read(&input,receiver,(uint32_t)op.immediate,visit,owner,NULL);
}
typedef struct EffectClassMethodTargetMatch { uint32_t target,count;XrXirType receiver; } EffectClassMethodTargetMatch;
