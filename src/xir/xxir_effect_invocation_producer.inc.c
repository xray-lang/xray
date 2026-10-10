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
XR_FUNC XrXirStatus xir_effects_producer(const XrXirCompileContext *context,
    const XrXirEffects *effects,uint32_t function,uint32_t instruction,XirEffectProducerView *output) {
    if (!output || !xir_effects_context_matches(context,effects,effects?effects->count:0) ||
        !effects->invocations || !effects->invocations->equations ||
        effects->invocations->resources!=context->resources) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationCertificate *certificate=effects->invocations;
    const EffectInvocationOwner *owner=certificate->equations;
    if (function>=certificate->bodies.function_count || function>=effects->count ||
        instruction>=certificate->bodies.functions[function].instruction_count ||
        (owner->site_count && !owner->sites)) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body=&certificate->bodies.functions[function];
    const XrXirInstruction *op=&body->instructions[instruction];
    if (!xir_compile_work(context,8)) return XR_XIR_BUDGET;
    if (op->op!=XR_XIR_FUNCTION_REF || op->immediate<0 ||
        (uint64_t)op->immediate>=certificate->bodies.function_count ||
        op->args[0]>body->operand_count || op->args[1]>body->operand_count-op->args[0] ||
        (op->args[1] && !body->operands)) return XR_XIR_BAD_STRUCTURE;
    uint32_t selected=UINT32_MAX;
    for (uint32_t site=0;site<owner->site_count;++site) {
        if (!xir_compile_work(context,7)) return XR_XIR_BUDGET;
        EffectInvocationSite real=owner->sites[site];
        if (real.function!=function || real.instruction!=instruction) continue;
        if (selected!=UINT32_MAX || real.target!=(uint64_t)op->immediate || real.captures!=op->args[1] ||
            real.binding>owner->binding_count || real.captures>owner->binding_count-real.binding)
            return XR_XIR_BAD_STRUCTURE;
        selected=site;
    }
    if (selected==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *target=&certificate->bodies.functions[op->immediate];
    const XrXirTypeNode *signature=xr_xir_callable_signature(&certificate->terms.types,op->type);
    if (!signature || op->args[1]>target->parameter_count ||
        signature->parameter_count!=target->parameter_count-op->args[1] ||
        signature->result!=target->result) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<op->args[1];++p) {
        if (!xir_compile_work(context,3)) return XR_XIR_BUDGET;
        uint32_t value=body->operands[op->args[0]+p];
        if (value>=(uint64_t)body->parameter_count+body->instruction_count) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=xr_xir_compile_call_type_matches(context,&certificate->bodies,function,op,
            target->parameters[p],xr_xir_operand_type(body,value));
        if (status!=XR_XIR_OK) return status==XR_XIR_BAD_TYPE?XR_XIR_BAD_STRUCTURE:status;
    }
    for (uint32_t p=0;p<signature->parameter_count;++p) {
        if (!xir_compile_work(context,3)) return XR_XIR_BUDGET;
        if (!xr_xir_callable_parameter_storage_valid(&certificate->terms.types,&signature->parameters[p]))
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=xr_xir_compile_call_type_matches(context,&certificate->bodies,function,op,
            target->parameters[p+op->args[1]],signature->parameters[p].type);
        if (status!=XR_XIR_OK) return status==XR_XIR_BAD_TYPE?XR_XIR_BAD_STRUCTURE:status;
    }
    if (!xir_compile_work(context,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(XirEffectProducerView){selected,function,instruction,(uint32_t)op->immediate,op->args[1],op->type};
    return XR_XIR_OK;
}
