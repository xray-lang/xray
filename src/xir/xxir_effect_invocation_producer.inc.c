/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_producer.inc.c - Real owned producer correspondence
 *
 * KEY CONCEPT:
 *   A site view consumes the full real instruction and publishes no effect mask.
 */
/* A generator site identifies a complete parked-body argument vector. Its
 * Iterator type never becomes a callable signature or an execution permit. */
static XrXirStatus effect_producer_signature(const EffectInvocationCertificate *certificate,
    const XrXirInstruction *op,const EffectBodyFunction *target,const XrXirTypeNode **output) {
    const XrXirDeclarations *declarations=certificate->declarations;
    bool generator=declarations && declarations->functions &&
        (declarations->functions[op->immediate].promises&XR_XIR_FUNCTION_GENERATOR)!=0;
    if(generator!=(op->op==XR_XIR_GENERATOR_NEW)) return XR_XIR_BAD_STRUCTURE;
    if(generator) {
        const XrXirTypeNode *iterator=xr_xir_type_node(&certificate->terms.types,op->type);
        if(!iterator || iterator->kind!=XR_XIR_TYPE_ITERATOR || op->type!=target->result ||
            op->args[1]!=target->parameter_count) return XR_XIR_BAD_STRUCTURE;
        *output=NULL;return XR_XIR_OK;
    }
    const XrXirTypeNode *signature=xr_xir_callable_signature(&certificate->terms.types,op->type);
    if(!signature || op->args[1]>target->parameter_count ||
        signature->parameter_count!=target->parameter_count-op->args[1] ||
        signature->result!=target->result) return XR_XIR_BAD_STRUCTURE;
    *output=signature;return XR_XIR_OK;
}

static XrXirStatus effect_producer_class(const XrXirCompileContext *context,
    const EffectInvocationCertificate *certificate,uint32_t function,uint32_t instruction,
    uint32_t selected_target,XirEffectProducerView *output) {
    const EffectBodyMaterial *material=certificate->body;
    const EffectBodyFunction *body=&material->functions[function];
    if(selected_target>=effect_body_count(material) || !material->declarations ||
        !material->declarations->functions)return XR_XIR_BAD_STRUCTURE;
    EffectBodyRead read={.owner=context};XrXirInstruction op;
    XrXirStatus status=effect_body_instruction_get(&read,body,instruction,&op);if(status!=XR_XIR_OK)return status;
    if(op.op!=XR_XIR_CLASS_METHOD_REF || op.immediate<0 || (uint64_t)op.immediate>=effect_body_count(material) ||
        op.args[0]>=(uint64_t)body->parameter_count+instruction || op.args[1] || op.targets[0] || op.targets[1] ||
        op.type_arguments[0] || op.type_arguments[1])return XR_XIR_BAD_STRUCTURE;
    XirClassMethodTarget selected={0};
    status=effect_class_method_target(context,material,function,instruction,selected_target,&selected);
    if(status!=XR_XIR_OK)return status;
    const EffectBodyFunction *target=&material->functions[selected_target],*anchor=&material->functions[op.immediate];
    const XrXirFunctionIdentity *identity=&material->declarations->functions[selected_target];
    const XrXirTypes *types=material->domain.types;
    const XrXirTypeNode *signature=xr_xir_callable_signature(types,op.type);
    if(!identity->nominal_owner || identity->method_kind!=XR_XIR_READ_METHOD || identity->member_access ||
        identity->cleanup_owner || identity->native_callable || identity->test_role ||
        (identity->promises&(XR_XIR_FUNCTION_GENERATOR|XR_XIR_FUNCTION_REST)) ||
        !signature || !target->parameter_count || !target->parameters || !anchor->parameter_count || !anchor->parameters ||
        !target->name || !anchor->name || target->name_length!=anchor->name_length ||
        memcmp(target->name,anchor->name,target->name_length) ||
        signature->parameter_count!=target->parameter_count-1 || signature->result!=target->result ||
        !xr_xir_error_bound_accepts(types,target->error_type,signature->element) ||
        !xr_xir_class_is_subtype(types,target->parameters[0],anchor->parameters[0]) ||
        ((signature->flags&XR_XIR_CALLABLE_NO_SUSPEND) && !(identity->promises&XR_XIR_FUNCTION_NO_SUSPEND)))
        return XR_XIR_BAD_STRUCTURE;
    XrXirType receiver;
    status=effect_body_operand_type(&read,body,op.args[0],&receiver);if(status!=XR_XIR_OK)return status;
    if(!xr_xir_class_is_subtype(types,receiver,anchor->parameters[0]) ||
        (!xr_xir_class_is_subtype(types,target->parameters[0],receiver) &&
         !xr_xir_class_is_subtype(types,receiver,target->parameters[0])))return XR_XIR_BAD_STRUCTURE;
    for(uint32_t p=0;p<signature->parameter_count;++p)
        if(!xr_xir_callable_parameter_storage_valid(types,&signature->parameters[p]) ||
            signature->parameters[p].type!=target->parameters[p+1] ||
            signature->parameters[p].mode!=xr_xir_declared_parameter_mode(types,target->parameters[p+1],
                target->consume_parameters,target->consume_parameter_count,p+1))return XR_XIR_BAD_STRUCTURE;
    uint32_t selected_site=UINT32_MAX;
    for(uint32_t s=0;s<certificate->final->site_count;++s) {
        const EffectInvocationSite *site=&certificate->final->sites[s];
        if(site->function!=function || site->instruction!=instruction || site->target!=selected_target)continue;
        if(selected_site!=UINT32_MAX || site->captures!=1 || site->binding>=certificate->final->binding_count)
            return XR_XIR_BAD_STRUCTURE;
        selected_site=s;
    }
    if(selected_site==UINT32_MAX)return XR_XIR_BAD_STRUCTURE;
    *output=(XirEffectProducerView){selected_site,function,instruction,selected_target,1,op.type};return XR_XIR_OK;
}
static XrXirStatus effect_producer_query(const XrXirCompileContext *context,
    const XrXirEffects *effects,uint32_t function,uint32_t instruction,
    uint32_t requested_target,XirEffectProducerView *output) {
    if (!output || !xir_effects_context_matches(context,effects,effects?effects->count:0) ||
        !effects->invocations || !effects->invocations->final ||
        effects->invocations->resources!=context->resources) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationCertificate *certificate=effects->invocations;
    if (!effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=certificate->resources) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *owner=certificate->final;
    if (function>=effect_body_count(certificate->body) || function>=effects->count ||
        instruction>=certificate->body->functions[function].instruction_count ||
        (owner->site_count && !owner->sites)) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *body=&certificate->body->functions[function];
    EffectBodyRead read=(EffectBodyRead){.owner=context};XrXirInstruction decoded;
    XrXirStatus decoded_status=effect_body_instruction_get(&read,body,instruction,&decoded);
    if (decoded_status!=XR_XIR_OK) return decoded_status;
    const XrXirInstruction *op=&decoded;
    if(op->op==XR_XIR_CLASS_METHOD_REF)
        return effect_producer_class(context,certificate,function,instruction,requested_target,output);
    if(requested_target!=UINT32_MAX && (int64_t)requested_target!=op->immediate)return XR_XIR_BAD_STRUCTURE;

    if ((op->op!=XR_XIR_FUNCTION_REF && op->op!=XR_XIR_GENERATOR_NEW) || op->immediate<0 ||
        (uint64_t)op->immediate>=effect_body_count(certificate->body) ||
        op->args[0]>body->operand_count || op->args[1]>body->operand_count-op->args[0] ||
        (op->args[1] && !body->operands)) return XR_XIR_BAD_STRUCTURE;
    uint32_t selected=UINT32_MAX;
    for (uint32_t site=0;site<owner->site_count;++site) {

        EffectInvocationSite real=owner->sites[site];
        if (real.function!=function || real.instruction!=instruction) continue;
        if (selected!=UINT32_MAX || real.target!=(uint64_t)op->immediate || real.captures!=op->args[1] ||
            real.binding>owner->binding_count || real.captures>owner->binding_count-real.binding)
            return XR_XIR_BAD_STRUCTURE;
        selected=site;
    }
    if (selected==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *target=&certificate->body->functions[op->immediate];
    const XrXirTypeNode *signature=NULL;
    XrXirStatus signature_status=effect_producer_signature(certificate,op,target,&signature);
    if(signature_status!=XR_XIR_OK) return signature_status;
    for (uint32_t p=0;p<op->args[1];++p) {

        uint32_t value=body->operands[op->args[0]+p];
        if (value>=(uint64_t)body->parameter_count+body->instruction_count) return XR_XIR_BAD_STRUCTURE;
        XrXirType actual;XrXirStatus status=effect_body_operand_type(&read,body,value,&actual);
        if (status==XR_XIR_OK) status=xir_metadata_call_type_matches(context,&certificate->body->domain,function,op,
            target->parameters[p],actual);
        if (status!=XR_XIR_OK) return status==XR_XIR_BAD_TYPE?XR_XIR_BAD_STRUCTURE:status;
    }
    for (uint32_t p=0;signature && p<signature->parameter_count;++p) {

        if (!xr_xir_callable_parameter_storage_valid(&certificate->terms.types,&signature->parameters[p]))
            return XR_XIR_BAD_STRUCTURE;
        if(signature->parameters[p].mode!=xr_xir_declared_parameter_mode(&certificate->terms.types,
            target->parameters[p+op->args[1]],target->consume_parameters,target->consume_parameter_count,p+op->args[1]))
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=xir_metadata_call_type_matches(context,&certificate->body->domain,function,op,
            target->parameters[p+op->args[1]],signature->parameters[p].type);
        if (status!=XR_XIR_OK) return status==XR_XIR_BAD_TYPE?XR_XIR_BAD_STRUCTURE:status;
    }

    *output=(XirEffectProducerView){selected,function,instruction,(uint32_t)op->immediate,op->args[1],op->type};
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_producer(const XrXirCompileContext *context,
    const XrXirEffects *effects,uint32_t function,uint32_t instruction,XirEffectProducerView *output) {
    return effect_producer_query(context,effects,function,instruction,UINT32_MAX,output);
}
XR_FUNC XrXirStatus xir_effects_producer_target(const XrXirCompileContext *context,
    const XrXirEffects *effects,uint32_t function,uint32_t instruction,
    uint32_t target,XirEffectProducerView *output) {
    return effect_producer_query(context,effects,function,instruction,target,output);
}
