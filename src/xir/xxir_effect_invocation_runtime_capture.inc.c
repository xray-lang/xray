/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_runtime_capture.inc.c - Current complete Class capture atoms
 *
 * The caller's sealed SSA relation retains every residual. A physical local
 * object cannot select an older smaller environment or erase uncertainty.
 */
/* The receiver is the real SSA value at this original frontier. Creation
 * arguments are not the receiver of a later ordinary Iterator demand. */
static XrXirStatus effect_runtime_closure_value(const XrXirInstruction *op,uint32_t *output) {
    if(!op || !output) return XR_XIR_BAD_STRUCTURE;
    if(effect_invocation_iterator_operation(op->op)) {*output=op->args[0];return XR_XIR_OK;}
    if((op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) ||
        op->immediate<0 || (uint64_t)op->immediate>=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    *output=(uint32_t)op->immediate;return XR_XIR_OK;
}
static XrXirStatus effect_runtime_capture_class_mask(const EffectInvocationCertificate *certificate,
    const XirEffectInvocationRequest *request,uint32_t value,uint32_t producer,
    uint32_t capture,uint32_t *output) {
    const EffectInvocationFinal *owner=certificate->final;
    uint32_t parent=request->contextual?request->node:owner->roots[request->caller];
    if(!output || producer>=owner->site_count || parent>=owner->count ||
        owner->nodes[parent].body!=request->caller) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationSite *site=&owner->sites[producer];
    if(site->target>=effect_body_count(certificate->body) || capture>=site->captures ||
        site->binding>owner->binding_count || capture>=owner->binding_count-site->binding)
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *target=&certificate->body->functions[site->target];
    const EffectBodyFunction *caller=&certificate->body->functions[request->caller];
    if(capture>=target->parameter_count ||
        !xr_xir_type_is_class(&certificate->terms.types,target->parameters[capture]) ||
        value>=(uint64_t)caller->parameter_count+caller->instruction_count) return XR_XIR_BAD_TYPE;
    EffectInvocationBasis basis={0};
    XrXirStatus status=effect_runtime_basis(certificate,owner->nodes[parent].root,&basis,&request->read);
    if(status!=XR_XIR_OK) return status;
    const EffectInvocationNode *node=&owner->nodes[parent];
    if(node->producer_count && !node->producers) return XR_XIR_BAD_STRUCTURE;
    uint64_t bits=(uint64_t)basis.parameters+3,binding=(uint64_t)site->binding+capture;
    uint32_t mask=0;bool identity=false;
    for(uint32_t p=0;p<node->producer_count;++p) {
        status=effect_runtime_work(&request->read,6);
        if(status!=XR_XIR_OK) return status;
        const EffectInvocationProducer *fact=&node->producers[p];
        if(fact->value!=value || fact->site!=producer) continue;
        if(!fact->atom) {identity=true;continue;}
        if((uint64_t)fact->atom-1>=(uint64_t)owner->binding_count*bits) continue;
        uint64_t column=((uint64_t)fact->atom-1)/bits,bit=((uint64_t)fact->atom-1)%bits;
        if(column!=binding) continue;
        if(bit<basis.parameters) return XR_XIR_BAD_STRUCTURE;
        mask|=bit==basis.parameters?EFFECT_CLASS_ROOT:
            bit==basis.parameters+1?EFFECT_CLASS_UNKNOWN:EFFECT_CLASS_KNOWN;
    }
    if(!identity || !(mask&EFFECT_CLASS_KNOWN) || (mask&~EFFECT_CLASS_VALID)) return XR_XIR_BAD_TYPE;
    *output=mask;return XR_XIR_OK;
}
