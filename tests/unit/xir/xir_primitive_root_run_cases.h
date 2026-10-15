/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_primitive_root_run_cases.h - Owned primitive ROOT range replay failures
 */
static void primitive_root_reject(const XrXirCompileContext *work,const EffectBodyMaterial *body,
    XrXirStatus status) {
    XrXirRootEffects output={true,true};
    CHECK(effect_invocation_input_seed(work,body,0,&output)==status);
    CHECK(output.requires_root && output.unresolved);
}

static void primitive_root_case(uint32_t mode,bool large) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,mode);
    XrXirInstruction operations[17]={0};
    if (large) {
        for (uint32_t i=0;i<16;++i)
            operations[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=i};
        operations[16]=(XrXirInstruction){.op=XR_XIR_RETURN};
        fixture.functions[0].instructions=operations;
        fixture.functions[0].instruction_count=fixture.entry_block.count=17;
        fixture.functions[0].operands=NULL;fixture.functions[0].operand_count=0;
    }
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    memset(&fixture,0xCE,sizeof(fixture));memset(operations,0xCE,sizeof(operations));
    EffectBodyMaterial *body=effects->invocations->body;
    for (uint32_t f=0;f<4;++f) {
        XrXirRootEffects facts={0};
        CHECK(effect_invocation_input_seed(&context,body,f,&facts)==XR_XIR_OK);
        CHECK(facts.requires_root==(f==3 || (!f && mode && !large)) && !facts.unresolved);
    }
    if (large) {
        EffectBodyFunction *head=&body->functions[0];
        CHECK(head->run_count==2 && head->runs[0].count==16);
        EffectBodyRun saved=head->runs[0],tail=head->runs[1];
        /* These mutations exercise primitive projection after real intake.
         * They do not grant Source validity or permission to the changed body. */
        for (uint32_t w=1;w<EFFECT_BODY_WORDS;++w) {
            head->runs[0].mask=UINT32_C(1)<<w;head->runs[0].delta[w]=UINT32_MAX;
            XrXirRootEffects facts={true,true};
            CHECK(effect_invocation_input_seed(&context,body,0,&facts)==XR_XIR_OK &&
                !facts.requires_root && !facts.unresolved);
            head->runs[0].delta[w]=0;primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);
            head->runs[0]=saved;
        }
        head->runs[0].base.op=XR_XIR_INVALID;primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);
        head->runs[0].base.op=XR_XIR_OP_COUNT;primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);
        head->runs[0]=saved;head->runs[0].base.op=(XrXirOp)(XR_XIR_INVALID+1);
        head->runs[0].mask|=1;head->runs[0].delta[0]=UINT32_MAX;
        primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);head->runs[0]=saved;
        head->runs[0].count=0;primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);head->runs[0]=saved;
        ++head->runs[0].first;primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);head->runs[0]=saved;
        head->runs[0].mask|=UINT32_C(1)<<10;
        primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);head->runs[0]=saved;
        ++head->runs[1].first;primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);head->runs[1]=tail;
        --head->run_count;primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);++head->run_count;
        --head->run_capacity;primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);++head->run_capacity;
        ++head->instruction_count;primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);--head->instruction_count;
        const XrXirTypes *types=body->domain.types;body->domain.types=NULL;
        primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);body->domain.types=types;
        uint32_t references=body->references;body->references=0;
        primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);body->references=references;
        XrXirCompileContext foreign=context;foreign.resources=NULL;
        primitive_root_reject(&foreign,body,XR_XIR_BAD_STRUCTURE);
        head->runs[1].base=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)256};
        if (mode) {
            XrXirRootEffects facts={0};
            CHECK(effect_invocation_input_seed(&context,body,0,&facts)==XR_XIR_OK &&
                facts.requires_root && !facts.unresolved);
        } else primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);
        head->runs[1]=tail;
        uint32_t count=head->instruction_count,runs=head->run_count,capacity=head->run_capacity;
        EffectBodyRun *storage=head->runs;
        head->instruction_count=head->run_count=head->run_capacity=0;head->runs=NULL;
        XrXirRootEffects facts={true,true};
        CHECK(effect_invocation_input_seed(&context,body,0,&facts)==XR_XIR_OK &&
            !facts.requires_root && !facts.unresolved);
        head->run_capacity=1;primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);head->run_capacity=0;
        head->runs=storage;primitive_root_reject(&context,body,XR_XIR_BAD_STRUCTURE);
        head->instruction_count=count;head->run_count=runs;head->run_capacity=capacity;
        XrCompileResourceStats before=rp_stats(&context);
        CHECK(effect_invocation_input_seed(&context,body,0,&facts)==XR_XIR_OK &&
            !facts.requires_root && !facts.unresolved);
        XrCompileResourceStats after=rp_stats(&context);
        CHECK(after.work>before.work && after.allocated_bytes==before.allocated_bytes &&
            after.live_bytes==before.live_bytes && after.peak_bytes==before.peak_bytes);
        uint64_t limit=rp_caps().work;
        CHECK(after.work+1<limit &&
            xr_compile_resources_work(context.resources,limit-after.work-1)==XR_COMPILE_RESOURCE_OK);
        primitive_root_reject(&context,body,XR_XIR_BUDGET);
    }
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes && !rp_allocation_observer);
}
