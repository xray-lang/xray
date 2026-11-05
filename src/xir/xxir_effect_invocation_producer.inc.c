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

XR_FUNC XrXirStatus xir_effects_producer(const XrXirCompileContext *context,
    const XrXirEffects *effects,uint32_t function,uint32_t instruction,XirEffectProducerView *output) {
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
        XrXirStatus status=xir_metadata_call_type_matches(context,&certificate->body->domain,function,op,
            target->parameters[p+op->args[1]],signature->parameters[p].type);
        if (status!=XR_XIR_OK) return status==XR_XIR_BAD_TYPE?XR_XIR_BAD_STRUCTURE:status;
    }

    *output=(XirEffectProducerView){selected,function,instruction,(uint32_t)op->immediate,op->args[1],op->type};
    return XR_XIR_OK;
}
