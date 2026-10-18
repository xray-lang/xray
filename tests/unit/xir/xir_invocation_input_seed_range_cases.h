/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_input_seed_range_cases.h - Independent primitive seed range oracle
 */
#ifndef XIR_INVOCATION_INPUT_SEED_RANGE_CASES_H
#define XIR_INVOCATION_INPUT_SEED_RANGE_CASES_H

/* This scalar reference deliberately retains a separate full-row lookup at
 * every ordinal. It consumes neither the range schedule nor stored seeds. */
static XrXirStatus invocation_seed_scalar_reference(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,XrXirRootEffects *output) {
    const EffectBodyFunction *head=&body->functions[0];
    const XrXirDeclarations *decls=body->declarations;
    XrXirRootEffects facts={0};XrXirRootEffectWitness witnesses[2]={{0},{0}};
    if (decls && decls->modules[decls->functions[0].module].initializer==0) facts.requires_root=true;
    EffectBodyRead read=effect_body_compile_read(work);
    for (uint32_t i=0;i<head->instruction_count;++i) {
        XrXirInstruction op;XrXirStatus status=effect_body_instruction_get(&read,head,i,&op);
        if (status!=XR_XIR_OK) return status;
        if (op.op==XR_XIR_CALL_INDIRECT || op.op==XR_XIR_INVOKE_INDIRECT) continue;
        if (op.op==XR_XIR_SLOT_LOAD || op.op==XR_XIR_SLOT_PLACE || op.op==XR_XIR_SLOT_STORE) {
            if (!decls || !decls->slots || op.immediate<0 || (uint64_t)op.immediate>=decls->slot_count)
                return XR_XIR_BAD_STRUCTURE;
        }
        const EffectRootSeedRecord request={&body->domain,&op,0,i,XR_XIR_UNIT};
        status=effect_root_seed_output(&request,&facts,&witnesses[0],&witnesses[1],work);
        if (status!=XR_XIR_OK) return status;
        if (op.op==XR_XIR_CALL_REQUIREMENT) {facts.unresolved=false;witnesses[1]=(XrXirRootEffectWitness){0};}
    }
    *output=facts;return XR_XIR_OK;
}
static void invocation_seed_compare(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,XrXirStatus expected,bool required) {
    XrXirRootEffects scalar={true,true},ranges=scalar;
    CHECK(invocation_seed_scalar_reference(work,body,&scalar)==expected);
    CHECK(effect_invocation_input_seed(work,body,0,&ranges)==expected);
    CHECK(scalar.requires_root==ranges.requires_root && scalar.unresolved==ranges.unresolved);
    CHECK(expected!=XR_XIR_OK || (ranges.requires_root==required && !ranges.unresolved));
}
static void invocation_seed_range_literals(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext work=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&work).live_bytes;
    XrXirSlot slots[3]={{.mutable=1,.type=XR_XIR_I64},{.mutable=1,.type=XR_XIR_I64},{.mutable=1,.type=XR_XIR_I64}};
    XrXirSourceModule source={.initializer=UINT32_MAX};XrXirFunctionIdentity identity={0};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=&identity,.slots=slots,.slot_count=3};
    XrXirTypeNode callable={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,.flags=XR_XIR_CALLABLE_ROOT_REQUIRED};
    XrXirInterfaceMethod methods[3]={{.signature=256},{.signature=256},{.signature=256}};
    XrXirInterfaceDeclaration interface={.methods=methods,.method_count=3};
    XrXirInterfaceTable interfaces={.declarations=&interface,.count=1};
    EffectBodyRun run={.count=3,.mask=64,.base={.op=XR_XIR_CONST_INT}};run.delta[6]=1;
    EffectBodyFunction function={.instruction_count=3,.runs=&run,.run_count=1,.run_capacity=1};
    EffectBodyMaterial body={.resources=work.resources,.references=1,.functions=&function,.declarations=&declarations};
    body.terms.types=(XrXirTypes){.nodes=&callable,.count=1,.interfaces=&interfaces};
    body.domain=(XirMetadataDomain){.types=&body.terms.types,.declarations=&declarations,.function_count=1};
    size_t allocations=rp_attempts;
    for (uint32_t op=XR_XIR_INVALID+1;op<XR_XIR_OP_COUNT;++op) {
        run.base.op=(XrXirOp)op;
        bool required=op==XR_XIR_SLOT_LOAD || op==XR_XIR_SLOT_PLACE || op==XR_XIR_SLOT_STORE ||
            op==XR_XIR_SLOT_INIT || op==XR_XIR_SLOT_GROUP_INIT || op==XR_XIR_CALL_REQUIREMENT;
        invocation_seed_compare(&work,&body,XR_XIR_OK,required);
    }
    run.base.op=XR_XIR_CALL_REQUIREMENT;run.mask=32;run.delta[5]=1;
    invocation_seed_compare(&work,&body,XR_XIR_OK,true);
    run.delta[5]=2;invocation_seed_compare(&work,&body,XR_XIR_BAD_STRUCTURE,false);
    run.mask=16;run.delta[4]=1;invocation_seed_compare(&work,&body,XR_XIR_BAD_STRUCTURE,false);
    run.base.op=XR_XIR_SLOT_LOAD;run.mask=64;run.delta[6]=2;
    invocation_seed_compare(&work,&body,XR_XIR_BAD_STRUCTURE,false);
    run.delta[6]=1;run.base.immediate=INT64_MIN;
    invocation_seed_compare(&work,&body,XR_XIR_BAD_STRUCTURE,false);
    run.base.immediate=0;run.base.op=XR_XIR_CONST_INT;run.mask=1;
    run.delta[0]=(uint32_t)XR_XIR_SLOT_LOAD-XR_XIR_CONST_INT;
    /* A two-row changing opcode must observe the later slot contribution. */
    run.count=function.instruction_count=2;
    invocation_seed_compare(&work,&body,XR_XIR_OK,true);
    run.delta[0]=(uint32_t)XR_XIR_OP_COUNT-XR_XIR_CONST_INT;
    invocation_seed_compare(&work,&body,XR_XIR_BAD_STRUCTURE,false);
    run.base.op=XR_XIR_CONST_INT;run.count=function.instruction_count=3;
    for (uint32_t mode=0;mode<14;++mode) {
        run.mask=0;run.first=0;run.count=3;
        if (mode<10) {run.mask=UINT32_C(1)<<mode;run.delta[mode]=0;}
        else if (mode==10) run.mask=1024;
        else if (mode==11) run.first=1;
        else if (mode==12) run.count=0;
        else run.count=2;
        XrXirRootEffects failed={true,true};
        CHECK(effect_invocation_input_seed(&work,&body,0,&failed)==XR_XIR_BAD_STRUCTURE &&
            failed.requires_root && failed.unresolved);
    }
    run.first=0;
    run.base.op=XR_XIR_CONST_INT;run.count=function.instruction_count=512;run.mask=64;run.delta[6]=1;
    XrXirRootEffects output={true,true};uint64_t before=rp_stats(&work).work;
    CHECK(effect_invocation_input_seed(&work,&body,0,&output)==XR_XIR_OK && !output.requires_root && !output.unresolved);
    uint64_t cost=rp_stats(&work).work-before;
    run.count=function.instruction_count=1024;before=rp_stats(&work).work;
    CHECK(effect_invocation_input_seed(&work,&body,0,&output)==XR_XIR_OK && rp_stats(&work).work-before==cost);
    CHECK(rp_attempts==allocations);rp_owner_free(&work,baseline);rp_balanced(physical);
    for (uint32_t exact=0;exact<2;++exact) {
        work=rp_owner(rp_caps());baseline=rp_stats(&work).live_bytes;body.resources=work.resources;
        uint64_t available=cost-(exact?0u:1u),used=rp_stats(&work).work;
        CHECK(xr_compile_resources_work(work.resources,rp_caps().work-used-available)==XR_COMPILE_RESOURCE_OK);
        allocations=rp_attempts;output=(XrXirRootEffects){true,true};before=rp_stats(&work).work;
        CHECK(effect_invocation_input_seed(&work,&body,0,&output)==(exact?XR_XIR_OK:XR_XIR_BUDGET));
        CHECK(exact?(!output.requires_root && !output.unresolved):(output.requires_root && output.unresolved));
        CHECK(rp_stats(&work).work-before==cost-(exact?0u:sizeof(output)) && rp_attempts==allocations);
        rp_owner_free(&work,baseline);rp_balanced(physical);
    }
    printf("primitive Input seed ranges: all%u opcodes, scalar oracle, changing slot/requirement/opcode, signed/14 canonical rejections, constant512/1024, allocator0, work%llu exact/minus1 PASS\n",
        (unsigned)(XR_XIR_OP_COUNT-1),(unsigned long long)cost);
}

#endif // XIR_INVOCATION_INPUT_SEED_RANGE_CASES_H
