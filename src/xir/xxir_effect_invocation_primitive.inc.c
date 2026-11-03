/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_primitive.inc.c - Actual owned Input ROOT terminals
 *
 * Primitive facts are derived from current owned Input. Only constructor
 * scratch retains these proposals; neither Input nor Final retains primitive
 * ROOT seed rows.
 */
/* A native terminal is read from the actual owned declaration and complete
 * callable head. It is neither an initializer nor a synthetic storage access.
 * The same reader is used by construction and independent Final replay. */
static XrXirStatus effect_invocation_native_root(const EffectBodyMaterial *body,uint32_t function,bool *output) {
    if (!body || !output || function>=effect_body_count(body)) return XR_XIR_BAD_STRUCTURE;
    const XrXirDeclarations *decls=body->declarations;
    *output=false;
    if (!decls || !decls->functions || decls->functions[function].native_callable!=XR_NATIVE_CALLABLE_SYS_ON_SIGNAL)
        return XR_XIR_OK;
    const EffectBodyFunction *head=&body->functions[function];
    /* Ordinary environments specialize actual callable bounds. Their owned
     * declaration coordinate still names the canonical native header; the
     * specialized actual is not a replacement declaration or a new grant. */
    const EffectBodyMaterial *declaration_body=body;
    uint32_t declaration=function;
    if (body->ordinary_identities) {
        declaration=body->ordinary_identities[function].declaration;
        if (declaration>=body->ordinary_source_count) return XR_XIR_BAD_STRUCTURE;
        while (declaration_body->backbone) declaration_body=declaration_body->backbone;
    }
    if (declaration>=effect_body_count(declaration_body) || !declaration_body->declarations ||
        !declaration_body->declarations->functions ||
        declaration_body->declarations->functions[declaration].native_callable!=XR_NATIVE_CALLABLE_SYS_ON_SIGNAL)
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *canonical=&declaration_body->functions[declaration];
    const XrXirTypeNode *actual=head->parameter_count==2 && head->parameters?
        xr_xir_type_node(body->domain.types,head->parameters[1]):NULL;
    if (!xir_native_callable_identity(&decls->functions[function]) ||
        !xir_sys_native_scope(decls,function,effect_body_count(body)) ||
        !xir_native_callable_identity(&declaration_body->declarations->functions[declaration]) ||
        !xir_sys_native_scope(declaration_body->declarations,declaration,effect_body_count(declaration_body)) ||
        head->block_count || head->instruction_count || head->run_count || head->operand_count ||
        head->consume_parameter_count || head->parameter_count!=2 || !head->parameters || head->parameters[0]!=XR_XIR_I64 ||
        head->result!=XR_XIR_BOOL || head->error_type!=XR_XIR_NEVER || !actual ||
        actual->kind!=XR_XIR_TYPE_CALLABLE || actual->parameter_count || actual->parameters || actual->parameter_span ||
        actual->result!=XR_XIR_UNIT || actual->element!=XR_XIR_NEVER ||
        !xr_xir_callable_flags_valid(actual->flags) || (actual->flags&XR_XIR_CALLABLE_REST) ||
        (body->generics && (body->generics[function].parameter_count || body->generics[function].argument_count)) ||
        !xir_sys_native_signature(XR_NATIVE_CALLABLE_SYS_ON_SIGNAL,declaration_body->domain.types,
            canonical->parameters,canonical->parameter_count,canonical->result,canonical->error_type)) return XR_XIR_BAD_TYPE;
    *output=true;return XR_XIR_OK;
}
static XrXirStatus effect_invocation_primitive_attempt(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,uint32_t function,EffectInvocationPrimitive *output,XirReadScratch *memory) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(body) || !output ||
        body->resources!=work->resources || function>=effect_body_count(body)) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *head=&body->functions[function];
    const XrXirDeclarations *decls=body->declarations;
    XrXirRootEffects facts={0};XrXirRootEffectWitness witnesses[2]={{0},{0}};
    bool native_root=false;
    XrXirStatus native_status=effect_invocation_native_root(body,function,&native_root);
    if (native_status!=XR_XIR_OK) return native_status;
    if (native_root) {
        facts.requires_root=true;
        witnesses[0]=(XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_NATIVE,UINT32_MAX,UINT32_MAX,UINT32_MAX,0};
    }
    if (decls) {

        if (!decls->modules || !decls->functions || decls->functions[function].module>=decls->module_count)
            return XR_XIR_BAD_STRUCTURE;
        if (decls->modules[decls->functions[function].module].initializer==function) {
            facts.requires_root=true;
            witnesses[0]=(XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INITIALIZER,
                UINT32_MAX,UINT32_MAX,UINT32_MAX,0};
        }
    }
    EffectBodyRead read=(EffectBodyRead){.owner=work};

    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    for (uint32_t r=0;r<head->run_count;++r) {
        const EffectBodyRun *run=NULL;
        XrXirStatus status=effect_body_range_ref(&read,head,&cursor,&run);
        if (status!=XR_XIR_OK) return status;
        uint32_t opcode=XR_XIR_INVALID;
        status=effect_body_run_word(&read,run,run->first,0,&opcode);
        if (status!=XR_XIR_OK) return status;

        /* This is a primitive contribution from current owned Input. All
         * selected deltas and boundaries were checked above. Complete physical
         * correspondence still reads unused fields and signed immediate bits. */
        if (!(run->mask&UINT32_C(1)) && effect_root_seed_local((XrXirOp)opcode)) continue;

        uint32_t visits=run->mask?run->count:1;
        for (uint32_t offset=0;offset<visits;++offset) {

            uint32_t i=run->first+offset;XrXirInstruction op;
            status=effect_body_instruction_ref_value(&read,run,i,&op);
            if (status!=XR_XIR_OK) return status;
            if (op.op==XR_XIR_CALL_INDIRECT || op.op==XR_XIR_INVOKE_INDIRECT) continue;
            if (op.op==XR_XIR_SLOT_LOAD || op.op==XR_XIR_SLOT_PLACE || op.op==XR_XIR_SLOT_STORE) {
                if (!decls || !decls->slots || op.immediate<0 || (uint64_t)op.immediate>=decls->slot_count)
                    return XR_XIR_BAD_STRUCTURE;
            }
            const EffectRootSeedRecord request={&body->domain,&op,function,i,XR_XIR_UNIT};
            status=effect_root_seed_output(&request,&facts,&witnesses[0],&witnesses[1],work,memory);
            if (status!=XR_XIR_OK) return status;
            if (op.op==XR_XIR_CALL_REQUIREMENT) { facts.unresolved=false;witnesses[1]=(XrXirRootEffectWitness){0}; }
        }
    }
    if (cursor.first!=head->instruction_count || cursor.run!=head->run_count) return XR_XIR_BAD_STRUCTURE;

    *output=(EffectInvocationPrimitive){facts,{witnesses[0],witnesses[1]}};return XR_XIR_OK;
}

/* A missing buffer unwinds all head/run/type borrows before actual allocation.
 * Every retry derives facts anew; capacity never carries a prior proof result. */
static XrXirStatus effect_invocation_primitive_read(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,uint32_t function,EffectInvocationPrimitive *output) {
    if(!xir_compile_context_valid(work)) return XR_XIR_BAD_STRUCTURE;
    XirReadScratch memory={.resources=work->resources};
    XrXirStatus status=effect_invocation_primitive_attempt(work,body,function,output,&memory);
    if(!memory.required) return status;
    if(status!=XR_XIR_BUDGET) return XR_XIR_BAD_STRUCTURE;
    uint32_t functions=effect_body_count(body);EffectInvocationPrimitive candidate={0};
    for(;;) {
        status=xir_read_scratch_grow(work,&memory);if(status!=XR_XIR_OK) break;
        status=effect_invocation_primitive_frame(work,body);
        if(status!=XR_XIR_OK || effect_body_count(body)!=functions) {
            if(status==XR_XIR_OK) status=XR_XIR_BAD_STRUCTURE;
            break;
        }
        status=xir_read_scratch_begin(work,&memory);if(status!=XR_XIR_OK) break;
        status=effect_invocation_primitive_attempt(work,body,function,&candidate,&memory);
        if(status!=XR_XIR_BUDGET || !memory.required) break;
    }
    xir_read_scratch_free(&memory);
    if(status==XR_XIR_OK) status=effect_invocation_primitive_frame(work,body);
    if(status==XR_XIR_OK && effect_body_count(body)!=functions) status=XR_XIR_BAD_STRUCTURE;
    if(status==XR_XIR_OK) {

        *output=candidate;
    }
    return status;
}
