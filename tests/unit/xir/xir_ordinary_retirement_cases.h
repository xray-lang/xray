/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ordinary_retirement_cases.h - Actual ordinary allocation retirement
 */
static void *pc_ordinary_blocks[64];
static uint32_t pc_ordinary_freed[64],pc_ordinary_block_count;
static XrXirInstruction *pc_ordinary_current;
static void *pc_ordinary_contract_block;
static bool pc_ordinary_mutate;
static uint32_t pc_ordinary_callbacks;

/* Only the allocator's real live records locate these known genuine payloads.
 * This test never guesses a compiler header or grants production extent proof. */
static void pc_ordinary_track(const void *payload) {
    if (!payload) return;
    uintptr_t address=(uintptr_t)payload;void *block=NULL;
    for (size_t i=0;i<live;++i) {
        uintptr_t base=(uintptr_t)pc_physical[i].pointer;
        if (address>=base && address-base<pc_physical[i].bytes) { CHECK(!block);block=pc_physical[i].pointer; }
    }
    CHECK(block);
    for (uint32_t i=0;i<pc_ordinary_block_count;++i) if (pc_ordinary_blocks[i]==block) return;
    CHECK(pc_ordinary_block_count<64);
    pc_ordinary_blocks[pc_ordinary_block_count]=block;pc_ordinary_freed[pc_ordinary_block_count++]=0;
}
static void pc_ordinary_observe_free(void *memory,size_t bytes) {
    CHECK(memory && bytes);
    for (uint32_t i=0;i<pc_ordinary_block_count;++i)
        if (pc_ordinary_blocks[i]==memory) CHECK(++pc_ordinary_freed[i]==1);
    if (memory==pc_ordinary_contract_block) {
        CHECK(pc_ordinary_current && !pc_ordinary_callbacks++);
        CHECK(pc_ordinary_current->op==XR_XIR_CONST_INT && pc_ordinary_current->type==XR_XIR_I64);
        if (pc_ordinary_mutate) pc_ordinary_current->type=XR_XIR_BOOL;
    }
}
static void pc_context_resurrection(const XrXirCompileContext *context,const XrXirModule *source,
    XrXirEffects *effects,bool select_dense) {
    EffectContextOwner *owner=effects->invocations->final->outcomes->contexts;
    EffectOrdinaryContexts *ordinary=select_dense?owner->dense:owner->ordinary;
    CHECK(effect_ordinary_work_empty(ordinary));
    for (uint32_t field=0;field<32;++field) {
        XrXirEffects *uses=&ordinary->uses;EffectFinalOutcomes *out=&uses->outcomes_work;
        switch (field) {
        case 0:uses->resources=context->resources;break;
        case 1:uses->count=1;break;
        case 2:uses->outcomes_owner=(EffectFinalOutcomes *)(uintptr_t)1;break;
        case 3:uses->root_input=(const EffectFinalOutcomes *)(uintptr_t)1;break;
        case 4:uses->formula_terminals=(XrXirRootEffectWitness *)(uintptr_t)1;break;
        case 5:uses->constant_witnesses=(XrXirRootEffectWitness *)(uintptr_t)1;break;
        case 6:uses->cells=(XrXirCellProvenance *)(uintptr_t)1;break;
        case 7:uses->refinement=(EffectRefinementBounds *)(uintptr_t)1;break;
        case 8:uses->invocations=(EffectInvocationCertificate *)(uintptr_t)1;break;
        case 9:uses->result_borrow=(const EffectFinalFunctions *)(uintptr_t)1;break;
        case 10:uses->generation=(EffectBodyMaterial *)(uintptr_t)1;break;
        case 11:out->resources=context->resources;break;
        case 12:out->count=1;break;case 13:out->atom_count=1;break;
        case 14:out->atom_capacity=1;break;case 15:out->words=1;break;
        case 16:out->source_type_count=1;break;case 17:out->error_rows=1;break;
        case 18:out->functions=(XrXirFunctionEffects *)(uintptr_t)1;break;
        case 19:out->witnesses=(XrXirEffectWitness *)(uintptr_t)1;break;
        case 20:out->atoms=(EffectErrorAtom *)(uintptr_t)1;break;
        case 21:out->errors=(uint64_t *)(uintptr_t)1;break;
        case 22:out->task_errors=(XrXirStatus *)(uintptr_t)1;break;
        case 23:out->go_safe=(XrXirStatus *)(uintptr_t)1;break;
        case 24:out->task_creation=(XrXirEffect *)(uintptr_t)1;break;
        case 25:out->root=(XrXirRootEffects *)(uintptr_t)1;break;
        case 26:out->root_witnesses=(XrXirRootEffectWitness *)(uintptr_t)1;break;
        case 27:out->unresolved_witnesses=(XrXirRootEffectWitness *)(uintptr_t)1;break;
        case 28:out->contracts=(XrXirFunctionEffectContract *)(uintptr_t)1;break;
        case 29:out->entry_roots=(XirEffectEntryRootView *)(uintptr_t)1;break;
        case 30:out->contexts=(EffectContextOwner *)(uintptr_t)1;break;
        default:ordinary->resources=NULL;break;
        }
        XirEffectEntryRootView untouched={.intrinsic_mask=UINT32_MAX};
        size_t calls=attempts;XrCompileResourceStats before=pc_stats(context);
        CHECK(!effect_final_outcomes(effects) && !xir_effects_contract(effects,0));
        CHECK(xir_effects_entry_root(context,effects,0,&untouched)==XR_XIR_BAD_STRUCTURE &&
            untouched.intrinsic_mask==UINT32_MAX && !untouched.parameter_count && !untouched.parameters);
        CHECK(xir_effects_source_terminal(context,source,effects)==XR_XIR_BAD_STRUCTURE);
        CHECK(attempts==calls && pc_stats(context).work==before.work);
        *uses=(XrXirEffects){0};ordinary->resources=context->resources;
        CHECK(effect_final_outcomes(effects));
    }
    CHECK(xir_effects_source_terminal(context,source,effects)==XR_XIR_OK);
}
/* Actual callable roles have their own array. Ordinary intake has no
 * solved formula; publication retains the independent five-term public one. */
static void pc_ordinary_parameter_retirement(void) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    InvocationRotationFixture fixture;invocation_rotation_fixture(&fixture);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    const XrXirModule *input=xr_xir_compile_artifact_module(checked);
    CHECK(effect_infer_construct(&context,input,NULL,&effects)==XR_XIR_OK &&
        effects->outcomes_work.contexts && effects->outcomes_work.contexts->ordinary);
    EffectOrdinaryContexts *ordinary=effects->outcomes_work.contexts->ordinary;
    XrXirEffects *uses=&ordinary->uses;
    CHECK(uses->outcomes_work.contracts && uses->outcomes_work.contracts!=effects->outcomes_work.contracts &&
        !uses->result_borrow && uses->generation);
    pc_ordinary_block_count=0;pc_ordinary_contract_block=NULL;pc_ordinary_current=NULL;
    pc_ordinary_track(uses->outcomes_work.contracts);pc_ordinary_track(uses->formula_terminals);
    pc_ordinary_track(uses->constant_witnesses);pc_ordinary_track(uses->outcomes_work.root);
    pc_ordinary_track(uses->outcomes_work.root_witnesses);pc_ordinary_track(uses->outcomes_work.unresolved_witnesses);
    pc_ordinary_track(uses->outcomes_work.functions);pc_ordinary_track(uses->outcomes_work.task_creation);
    uint32_t roles=0,terms=0;
    for (uint32_t f=0;f<uses->count;++f) {
        const XrXirFunctionEffectContract *contract=&uses->outcomes_work.contracts[f];
        roles+=contract->parameter_count;terms+=contract->formula.term_count;
        pc_ordinary_track(contract->parameters);pc_ordinary_track(contract->formula.terms);
        pc_ordinary_track(contract->values);pc_ordinary_track(contract->bindings);
    }
    CHECK(roles==5 && !terms && pc_ordinary_block_count==2 && !pc_free_hook);
    XrCompileResourceStats before=pc_stats(&context);pc_free_hook=pc_ordinary_observe_free;
    CHECK(effect_outcomes_publish(&context,input,effects)==XR_XIR_OK);pc_free_hook=NULL;
    CHECK(effect_ordinary_work_empty(ordinary) && pc_stats(&context).live_bytes<before.live_bytes);
    for (uint32_t i=0;i<pc_ordinary_block_count;++i) CHECK(pc_ordinary_freed[i]==1);
    fprintf(stderr,"ordinary nonempty roles%u terms%u actual_freed%u before publication passed\n",
        roles,terms,pc_ordinary_block_count);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    const XrXirFunctionEffectContract *contract=xir_effects_contract(effects,1);
    CHECK(contract && contract->parameter_count==5 && contract->parameters &&
        contract->formula.term_count==5 && contract->formula.terms);
    for (uint32_t p=0;p<5;++p) CHECK(contract->parameters[p].kind==XR_XIR_EFFECT_PARAMETER_VARIABLE &&
        contract->formula.terms[p].kind==XR_XIR_ROOT_TERM_PARAMETER && contract->formula.terms[p].index==p);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
}
static void pc_ordinary_retirement_literals(void) {
    pc_ordinary_parameter_retirement();
    for (uint32_t mutation=0;mutation<2;++mutation) {
        XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
        InvocationRotationFixture fixture;invocation_rotation_fixture(&fixture);
        XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
        CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
        const XrXirModule *input=xr_xir_compile_artifact_module(checked);
        CHECK(effect_infer_construct(&context,input,NULL,&effects)==XR_XIR_OK);
        EffectOrdinaryContexts *ordinary=effects->outcomes_work.contexts->ordinary;
        XrXirEffects *uses=&ordinary->uses;CHECK(uses->outcomes_work.contracts && uses->generation);
        pc_ordinary_block_count=0;
        pc_ordinary_track(uses->outcomes_work.contracts);pc_ordinary_contract_block=pc_ordinary_blocks[0];
        pc_ordinary_track(uses->formula_terminals);pc_ordinary_track(uses->constant_witnesses);
        pc_ordinary_track(uses->outcomes_work.root);pc_ordinary_track(uses->outcomes_work.root_witnesses);
        pc_ordinary_track(uses->outcomes_work.unresolved_witnesses);
        pc_ordinary_track(uses->outcomes_work.functions);pc_ordinary_track(uses->outcomes_work.task_creation);
        for (uint32_t f=0;f<uses->count;++f) {
            const XrXirFunctionEffectContract *contract=&uses->outcomes_work.contracts[f];
            pc_ordinary_track(contract->parameters);pc_ordinary_track(contract->formula.terms);
            pc_ordinary_track(contract->values);pc_ordinary_track(contract->bindings);
        }
        CHECK(pc_ordinary_block_count>1 && !uses->result_borrow);
        XrXirModule current=*input;XrXirFunction functions[5];XrXirInstruction rows[2];
        CHECK(input->function_count==5 && input->functions[2].instruction_count==2);
        memcpy(functions,input->functions,sizeof(functions));memcpy(rows,functions[2].instructions,sizeof(rows));
        current.functions=functions;functions[2].instructions=rows;
        pc_ordinary_current=&rows[0];pc_ordinary_mutate=mutation!=0;pc_ordinary_callbacks=0;
        CHECK(!pc_free_hook);pc_free_hook=pc_ordinary_observe_free;
        XrXirStatus status=effect_outcomes_publish(&context,&current,effects);
        pc_free_hook=NULL;pc_ordinary_current=NULL;pc_ordinary_contract_block=NULL;
        CHECK(status==(mutation?XR_XIR_BAD_STRUCTURE:XR_XIR_OK) && pc_ordinary_callbacks==1);
        CHECK(effect_ordinary_work_empty(ordinary));
        for (uint32_t i=0;i<pc_ordinary_block_count;++i) CHECK(pc_ordinary_freed[i]==1);
        if (!mutation) pc_context_resurrection(&context,&current,effects,false);
        else CHECK(effects->outcomes_owner && !effects->invocations->final->outcomes && !effect_final_outcomes(effects));
        rows[0].type=XR_XIR_I64;
        fprintf(stderr,"ordinary retirement mutation%u actual_freed%u callback%u status%u\n",
            mutation,pc_ordinary_block_count,pc_ordinary_callbacks,status);
        xr_xir_compile_effects_free(effects);effects=NULL;
        /* A rejected receiving candidate leaves the same real ledger usable. */
        CHECK(effect_infer_construct(&context,input,NULL,&effects)==XR_XIR_OK);
        CHECK(effect_outcomes_publish(&context,input,effects)==XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
        memset(functions,0xCE,sizeof(functions));memset(rows,0xCE,sizeof(rows));
        CHECK(xir_effects_contract(effects,0));
        xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
    }
    fprintf(stderr,"ordinary retirement: 32 staging/ledger resurrections rejected; producer death, sameowner retry and physical0 passed\n");
}
