/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_control_primitive.inc.c - Current owned Input control terminals
 *
 * Opcode and actual signature fields define local suspension contributions.
 * Input retains no computed Control row. Both construction and receiving use
 * these read-only primitives within one callback-free interval.
 */
/* An opaque I/O body has no synthetic SUSPEND instruction. Its complete
 * owned declaration is the terminal, and receiving repeats this same read. */
static XrXirStatus effect_control_native_suspend(const EffectBodyRequest *input,uint32_t function,bool *output) {
    const EffectBodyMaterial *body=input->owner;
    if(!body || function>=effect_body_count(body) || !output)return XR_XIR_BAD_STRUCTURE;
    *output=false;const XrXirDeclarations *decls=body->declarations;
    if(!decls || !decls->functions)return XR_XIR_OK;
    uint32_t native=decls->functions[function].native_callable;
    bool io=xir_io_native_can_suspend(native);
    if(!io && !xir_sys_pipe_can_suspend(native))return XR_XIR_OK;
    uint32_t id=decls->functions[function].native_callable,declaration=function;
    const EffectBodyMaterial *canonical=body;
    if(body->ordinary_identities) {
        declaration=body->ordinary_identities[function].declaration;
        if(declaration>=body->ordinary_source_count)return XR_XIR_BAD_STRUCTURE;
        if(body->canonical_source)canonical=body->canonical_source;
        else {
            while(canonical->backbone)canonical=canonical->backbone;
            if(canonical->ordinary_identities)return XR_XIR_BAD_STRUCTURE;
        }
    }
    if(declaration>=effect_body_count(canonical) || !canonical->declarations || !canonical->declarations->functions ||
        canonical->declarations->functions[declaration].native_callable!=id)return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *head=body->functions+function,*formal=canonical->functions+declaration;
    if(!xir_native_callable_identity(decls->functions+function) ||
        !xir_native_callable_identity(canonical->declarations->functions+declaration) ||
        (io?!xir_io_native_module_scope(decls,function,effect_body_count(body)):
            !xir_sys_native_scope(decls,function,effect_body_count(body))) ||
        (io?!xir_io_native_module_scope(canonical->declarations,declaration,effect_body_count(canonical)):
            !xir_sys_native_scope(canonical->declarations,declaration,effect_body_count(canonical))) ||
        head->block_count || head->instruction_count || head->run_count || head->operand_count || head->consume_parameter_count ||
        formal->block_count || formal->instruction_count || formal->run_count || formal->operand_count || formal->consume_parameter_count ||
        (body->generics && (body->generics[function].parameter_count || body->generics[function].argument_count)) ||
        (canonical->generics && (canonical->generics[declaration].parameter_count || canonical->generics[declaration].argument_count)) ||
        (io?!xir_io_native_signature(id,body->domain.types,head->parameters,head->parameter_count,head->result,head->error_type):
            !xir_sys_pipe_signature(id,body->domain.types,head->parameters,head->parameter_count,head->result,head->error_type)) ||
        (io?!xir_io_native_signature(id,canonical->domain.types,formal->parameters,formal->parameter_count,formal->result,formal->error_type):
            !xir_sys_pipe_signature(id,canonical->domain.types,formal->parameters,formal->parameter_count,formal->result,formal->error_type)))
        return XR_XIR_BAD_TYPE;
    if(io) {
        const XirIoNativeContract *contract=xir_io_native_contract(id);
        for(uint32_t p=0;p<contract->paths;++p) {
            uint32_t field=0;
            if(!xir_io_native_scope(decls,body->domain.types,function,effect_body_count(body),head->parameters[p],&field) ||
                !xir_io_native_scope(canonical->declarations,canonical->domain.types,declaration,
                    effect_body_count(canonical),formal->parameters[p],&field))return XR_XIR_BAD_TYPE;
        }
    }
    *output=true;return XR_XIR_OK;
}
static bool effect_control_direct(XrXirOp opcode) {
    return opcode==XR_XIR_CALL || opcode==XR_XIR_INVOKE || opcode==XR_XIR_CALL_DEFAULT ||
        opcode==XR_XIR_INVOKE_DEFAULT || opcode==XR_XIR_CLEANUP_REGISTER;
}

/* Only field-dependent seeds decode a full row. Direct call identities are
 * separate evidence obligations and are read by their actual consumer. */
static XrXirStatus effect_control_primitive(const EffectBodyRequest *input,uint32_t function,
    const EffectBodyRun *run,uint32_t instruction,XrXirInstruction *operation,XrXirFunctionEffects *local) {
    uint32_t opcode=XR_XIR_INVALID;
    XrXirStatus status=effect_body_run_word(&input->read,run,instruction,0,&opcode);
    if(status!=XR_XIR_OK) return status;
    *local=(XrXirFunctionEffects){0};*operation=(XrXirInstruction){.op=(XrXirOp)opcode};
    EffectSeedProjection projection=effect_seed_opcode((XrXirOp)opcode,local);
    if(projection==EFFECT_SEED_INVALID) return XR_XIR_BAD_STRUCTURE;
    if(projection==EFFECT_SEED_FIELDS) {
        status=effect_body_instruction_ref_value(&input->read,run,instruction,operation);
        if(status!=XR_XIR_OK) return status;
        XrXirType callable=XR_XIR_UNIT;
        if(operation->op==XR_XIR_CALL_INDIRECT || operation->op==XR_XIR_INVOKE_INDIRECT) {
            const EffectBodyMaterial *body=input->owner;const EffectBodyFunction *head=&body->functions[function];
            if(operation->immediate<0 || (uint64_t)operation->immediate>=(uint64_t)head->parameter_count+head->instruction_count)
                return XR_XIR_BAD_STRUCTURE;
            status=effect_body_request_type(input,function,(uint32_t)operation->immediate,&callable);
            if(status!=XR_XIR_OK) return status;
        }
        if(!effect_seed_type(input->domain.types,callable,operation,local)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_control_run_seed(const EffectBodyRequest *input,uint32_t function,
    const EffectBodyRun *run,XrXirFunctionEffects *aggregate) {
    uint32_t points=run->count;
    for(uint32_t i=0;i<points;++i) {
        XrXirInstruction op;XrXirFunctionEffects local={0},projection={0};
        XrXirStatus status=effect_control_primitive(input,function,run,run->first+i,&op,&local);
        if(status!=XR_XIR_OK) return status;
        if(!run->mask || (!(run->mask&UINT32_C(1)) &&
            effect_seed_opcode(op.op,&projection)==EFFECT_SEED_LOCAL)) points=1;
        if(!(((&input->read)->execution ? effect_body_execution_work((&input->read)->execution, 2+sizeof(*aggregate)) : xir_compile_context_valid((&input->read)->owner)))) return XR_XIR_BUDGET;
        if(local.suspend>aggregate->suspend) aggregate->suspend=local.suspend;
    }
    return XR_XIR_OK;
}
