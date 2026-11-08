/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_access_query.inc.c - Read hidden access facts after body admission
 */
static XrXirStatus effect_checked_access_read(const XrXirCompileContext *context,
    const XrXirEffects *effects,const XrXirModule *module,uint32_t function) {
    if (!module || !effects || !effects->checked_roots ||
        !xir_effects_context_matches(context,effects,module->function_count) || function>=effects->count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectCheckedRoots *roots=effects->checked_roots;
    return roots->node[roots->function[function].root].access_denied ? XR_XIR_UNRESOLVED : XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_effects_default_access_site(const XrXirCompileContext *context,
    const XrXirEffects *effects,XirEffectInput input,uint32_t caller,uint32_t instruction) {
    const XrXirModule *module=input.module;
    if (!xir_compile_context_valid(context) || !module || caller>=module->function_count ||
        instruction>=module->functions[caller].instruction_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&module->functions[caller].instructions[instruction];
    XrXirStatus status=xr_xir_compile_default_call_verify(context,module,caller,op);
    const XrXirDefaultBinding *binding=NULL;
    if (status==XR_XIR_OK) {
        XirMetadataDomain domain=xir_metadata_domain_from_module(module);
        const uint32_t *identity=xr_xir_default_identity(op);
        status=xir_metadata_default_lookup(context,&domain,identity[0],identity[1],&binding);
    }
    if (status==XR_XIR_OK && !binding) status=XR_XIR_BAD_STRUCTURE;
    return status==XR_XIR_OK ? effect_checked_access_read(context,effects,module,binding->function) : status;
}
XR_FUNC XrXirStatus xir_effects_class_call_access_site(const XrXirCompileContext *context,
    const XrXirEffects *effects,XirEffectInput input,uint32_t caller,uint32_t instruction,XrXirType result) {
    const XrXirModule *module=input.module;
    if (!xir_compile_context_valid(context) || !module || caller>=module->function_count ||
        instruction>=module->functions[caller].instruction_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *source=&module->functions[caller];
    const XrXirInstruction *op=&source->instructions[instruction];
    if (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE) return XR_XIR_UNRESOLVED;
    if (op->type!=result || !xr_xir_type_is_class(module->types,result)) return XR_XIR_BAD_TYPE;
    if (op->immediate<0 || (uint64_t)op->immediate>=module->function_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t callee=(uint32_t)op->immediate;
    const XrXirFunction *target=&module->functions[callee];
    if (target->result!=result || target->parameter_count!=op->args[1]) return XR_XIR_UNRESOLVED;
    if (op->args[0]>source->operand_count || op->args[1]>source->operand_count-op->args[0])
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<target->parameter_count;++p)
        if (xr_xir_operand_type(source,source->operands[op->args[0]+p])!=target->parameters[p])
            return XR_XIR_UNRESOLVED;
    const XrXirDeclarations *d=module->declarations;
    if (!d || !d->functions || !d->modules || d->functions[caller].module>=d->module_count ||
        d->functions[callee].module>=d->module_count) return XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_INVOKE || d->functions[caller].module!=d->functions[callee].module) {
        bool declared_default=false;
        XrXirStatus status=xir_effect_evidence_is_instance(module) ?
            xir_compile_default_call_origin(context,module,caller,instruction,callee,&declared_default) : XR_XIR_OK;
        if (status!=XR_XIR_OK) return status;
        if (!declared_default) return XR_XIR_UNRESOLVED;
    }
    return effect_checked_access_read(context,effects,module,callee);
}
