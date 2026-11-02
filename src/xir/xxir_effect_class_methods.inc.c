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
    XirClassMethodRead input={&material->domain,material,effect_class_method_header};
    /* The existing owned equation reader has no request-control field. */
    return xir_compile_class_method_candidates_read(&input,receiver,(uint32_t)op.immediate,visit,owner,NULL);
}
typedef struct EffectClassMethodTargetMatch { uint32_t target,count;XrXirType receiver; } EffectClassMethodTargetMatch;
static XrXirStatus effect_class_method_target_match(void *owner,const XirClassMethodTarget *target) {
    EffectClassMethodTargetMatch *match=owner;
    if(target->function==match->target) {
        if(match->count++)return XR_XIR_BAD_STRUCTURE;
        match->receiver=target->receiver_type;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_class_method_target(const XrXirCompileContext *work,const EffectBodyMaterial *material,
    uint32_t function,uint32_t instruction,uint32_t target,XirClassMethodTarget *output) {
    EffectClassMethodTargetMatch match={target,0,XR_XIR_UNIT};
    XrXirStatus status=effect_class_method_candidates(work,material,function,instruction,effect_class_method_target_match,&match);
    if(status==XR_XIR_OK && match.count!=1)status=XR_XIR_BAD_STRUCTURE;
    if(status==XR_XIR_OK)*output=(XirClassMethodTarget){target,match.receiver};return status;
}
