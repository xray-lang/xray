/* The forest retains actual Final output while every dense staging array dies. */
static void pc_dense_retirement_literals(void) {
    for (uint32_t mutation=0;mutation<2;++mutation) {
        XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
        InvocationRotationFixture fixture;invocation_rotation_fixture(&fixture);
        XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
        CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
        const XrXirModule *input=xr_xir_compile_artifact_module(checked);
        CHECK(effect_infer_construct(&context,input,NULL,&effects)==XR_XIR_OK && effects->outcomes_work.contexts);
        EffectContextOwner *owner=effects->outcomes_work.contexts;
        EffectOrdinaryContexts *dense=owner->dense;XrXirEffects *uses=&dense->uses;
        const EffectFinalFunctions *result=&owner->forest->invocations->final->summary;
        CHECK(uses->result_borrow==result && uses->outcomes_work.contracts && uses->outcomes_work.root &&
            uses->outcomes_work.root_witnesses && uses->outcomes_work.unresolved_witnesses &&
            uses->formula_terminals==result->witnesses && !uses->invocations && !uses->generation);
        const XrXirRootTerm *terms=result->functions[1].formula.terms;
        CHECK(result->functions[1].formula.term_count==5 && terms && uses->count==result->count);
        pc_ordinary_block_count=0;pc_ordinary_current=NULL;
        pc_ordinary_track(uses->outcomes_work.contracts);pc_ordinary_contract_block=pc_ordinary_blocks[0];
        pc_ordinary_track(uses->outcomes_work.root);pc_ordinary_track(uses->outcomes_work.root_witnesses);
        pc_ordinary_track(uses->outcomes_work.unresolved_witnesses);pc_ordinary_track(uses->outcomes_work.functions);
        pc_ordinary_track(uses->outcomes_work.task_creation);uint32_t roles=0;
        for (uint32_t f=0;f<uses->count;++f) {
            const XrXirFunctionEffectContract *contract=&uses->outcomes_work.contracts[f];
            roles+=contract->parameter_count;pc_ordinary_track(contract->parameters);
            pc_ordinary_track(contract->values);pc_ordinary_track(contract->bindings);
        }
        CHECK(roles>=5 && pc_ordinary_block_count>=5);
        XrXirModule current=*input;XrXirFunction functions[5];XrXirInstruction rows[2];
        CHECK(input->function_count==5 && input->functions[2].instruction_count==2);
        memcpy(functions,input->functions,sizeof(functions));memcpy(rows,functions[2].instructions,sizeof(rows));
        current.functions=functions;functions[2].instructions=rows;
        pc_ordinary_current=&rows[0];pc_ordinary_mutate=mutation!=0;pc_ordinary_callbacks=0;
        XrCompileResourceStats before=pc_stats(&context);CHECK(!pc_free_hook);pc_free_hook=pc_ordinary_observe_free;
        XrXirStatus status=effect_outcomes_publish(&context,&current,effects);
        pc_free_hook=NULL;pc_ordinary_current=NULL;pc_ordinary_contract_block=NULL;
        CHECK(status==(mutation?XR_XIR_BAD_STRUCTURE:XR_XIR_OK) && pc_ordinary_callbacks==1 &&
            effect_ordinary_work_empty(dense) && effect_ordinary_work_empty(owner->ordinary) &&
            pc_stats(&context).live_bytes<before.live_bytes);
        for (uint32_t i=0;i<pc_ordinary_block_count;++i) CHECK(pc_ordinary_freed[i]==1);
        CHECK(result==&owner->forest->invocations->final->summary &&
            result->functions[1].formula.terms==terms && result->functions[1].formula.term_count==5);
        for (uint32_t p=0;p<5;++p) CHECK(terms[p].kind==XR_XIR_ROOT_TERM_PARAMETER && terms[p].index==p);
        if (!mutation) {
            pc_context_resurrection(&context,&current,effects,true);
            CHECK(xir_effects_source_terminal(&context,&current,effects)==XR_XIR_OK);
        } else CHECK(!effect_final_outcomes(effects) && !effects->invocations->final->outcomes);
        fprintf(stderr,"dense retirement mutation%u actual_freed%u roles%u callback%u status%u borrowed five terms live\n",
            mutation,pc_ordinary_block_count,roles,pc_ordinary_callbacks,status);
        rows[0].type=XR_XIR_I64;xr_xir_compile_effects_free(effects);effects=NULL;
        CHECK(effect_infer_construct(&context,input,NULL,&effects)==XR_XIR_OK);
        CHECK(effect_outcomes_publish(&context,input,effects)==XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
        memset(functions,0xCE,sizeof(functions));memset(rows,0xCE,sizeof(rows));
        owner=effects->invocations->final->outcomes->contexts;
        result=&owner->forest->invocations->final->summary;
        CHECK(effect_ordinary_work_empty(owner->ordinary) && effect_ordinary_work_empty(owner->dense) &&
            result->functions[1].formula.term_count==5);
        xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
    }
    fprintf(stderr,"dense retirement: physical arrays,32resurrections,current input callback,producer death,sameowner and physical0 passed\n");
}
