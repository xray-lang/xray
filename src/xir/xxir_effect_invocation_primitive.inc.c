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
static XrXirStatus effect_invocation_primitive_read(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,uint32_t function,EffectInvocationPrimitive *output) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(body) || !output ||
        body->resources!=work->resources || function>=effect_body_count(body)) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *head=&body->functions[function];
    const XrXirDeclarations *decls=body->declarations;
    XrXirRootEffects facts={0};XrXirRootEffectWitness witnesses[2]={{0},{0}};
    if (decls) {
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (!decls->modules || !decls->functions || decls->functions[function].module>=decls->module_count)
            return XR_XIR_BAD_STRUCTURE;
        if (decls->modules[decls->functions[function].module].initializer==function) {
            facts.requires_root=true;
            witnesses[0]=(XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INITIALIZER,
                UINT32_MAX,UINT32_MAX,UINT32_MAX,0};
        }
    }
    EffectBodyRead read=effect_body_compile_read(work);
    if (!xir_compile_work(work,sizeof(EffectBodyRecordCursor)+sizeof(uint32_t))) return XR_XIR_BUDGET;
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    for (uint32_t r=0;r<head->run_count;++r) {
        const EffectBodyRun *run=NULL;
        XrXirStatus status=effect_body_range_ref(&read,head,&cursor,&run);
        if (status!=XR_XIR_OK) return status;
        uint32_t opcode=XR_XIR_INVALID;
        status=effect_body_run_word(&read,run,run->first,0,&opcode);
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        /* This is a primitive contribution from current owned Input. All
         * selected deltas and boundaries were checked above. Complete physical
         * correspondence still reads unused fields and signed immediate bits. */
        if (!(run->mask&UINT32_C(1)) && effect_root_seed_local((XrXirOp)opcode)) continue;
        if (!xir_compile_work(work,sizeof(uint32_t))) return XR_XIR_BUDGET;
        uint32_t visits=run->mask?run->count:1;
        for (uint32_t offset=0;offset<visits;++offset) {
            if (!xir_compile_work(work,sizeof(uint32_t)+1)) return XR_XIR_BUDGET;
            uint32_t i=run->first+offset;XrXirInstruction op;
            status=effect_body_instruction_ref_value(&read,run,i,&op);
            if (status!=XR_XIR_OK) return status;
            if (op.op==XR_XIR_CALL_INDIRECT || op.op==XR_XIR_INVOKE_INDIRECT) continue;
            if (op.op==XR_XIR_SLOT_LOAD || op.op==XR_XIR_SLOT_PLACE || op.op==XR_XIR_SLOT_STORE) {
                if (!decls || !decls->slots || op.immediate<0 || (uint64_t)op.immediate>=decls->slot_count)
                    return XR_XIR_BAD_STRUCTURE;
            }
            const EffectRootSeedRecord request={&body->domain,&op,function,i,XR_XIR_UNIT};
            status=effect_root_seed_output(&request,&facts,&witnesses[0],&witnesses[1],work);
            if (status!=XR_XIR_OK) return status;
            if (op.op==XR_XIR_CALL_REQUIREMENT) { facts.unresolved=false;witnesses[1]=(XrXirRootEffectWitness){0}; }
        }
    }
    if (cursor.first!=head->instruction_count || cursor.run!=head->run_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(EffectInvocationPrimitive){facts,{witnesses[0],witnesses[1]}};return XR_XIR_OK;
}
