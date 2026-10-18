/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_final_metadata_cases.h - Owned conditional metadata after producer retirement
 */
static void pc_outcome_metadata_literals(void) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    ClosedCarrierFixture fixture;cc_fixture(&fixture,8);fixture.edge.module.stage=XR_XIR_BUILT;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.edge.module,&checked,NULL)==XR_XIR_OK);
    const XrXirModule *input=xr_xir_compile_artifact_module(checked);
    CHECK(effect_infer_construct(&context,input,NULL,&effects)==XR_XIR_OK && effects);
    XrXirFunctionEffectContract *contracts=effects->outcomes_work.contracts;
    XirEffectEntryRootView *entries=effects->outcomes_work.entry_roots;
    CHECK(contracts && entries && contracts[1].parameter_count==1 && contracts[1].parameters &&
        contracts[1].formula.term_count==1 && contracts[1].formula.terms[0].kind==XR_XIR_ROOT_TERM_CELL_PARAMETER &&
        !contracts[1].formula.terms[0].index && !entries[1].intrinsic_mask &&
        entries[1].parameter_count==1 && entries[1].parameters && !entries[1].parameters[0]);
    const XrXirEffectParameter *roles=contracts[1].parameters;
    const uint32_t *parameters=entries[1].parameters;
    XirEffectEntryRootView untouched={.intrinsic_mask=UINT32_MAX};
    XrCompileResourceStats before=pc_stats(&context);size_t physical_before=attempts;
    CHECK(xir_effects_entry_root(&context,effects,1,&untouched)==XR_XIR_BAD_STRUCTURE &&
        untouched.intrinsic_mask==UINT32_MAX && !untouched.parameter_count && !untouched.parameters &&
        pc_stats(&context).work==before.work && attempts==physical_before);
    CHECK(effect_outcomes_publish(&context,input,effects)==XR_XIR_OK);
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    CHECK(final && final->contracts==contracts && final->entry_roots==entries &&
        final->contracts[1].parameters==roles && final->entry_roots[1].parameters==parameters &&
        final->contracts[1].formula.terms==effects->invocations->final->summary.functions[1].storage &&
        effect_outcomes_empty(&effects->outcomes_work));
    xr_xir_compile_artifact_free(checked);
    memset(&fixture,0xCE,sizeof(fixture));
    XirEffectEntryRootView read={0};
    CHECK(xir_effects_entry_root(&context,effects,1,&read)==XR_XIR_OK &&
        !read.intrinsic_mask && read.parameter_count==1 && read.parameters==parameters && !read.parameters[0]);
    CHECK(xir_effects_contract(effects,1)==&final->contracts[1] &&
        final->contracts[1].parameter_count==1 && final->contracts[1].parameters==roles &&
        final->contracts[1].formula.term_count==1 && final->contracts[1].formula.terms[0].kind==XR_XIR_ROOT_TERM_CELL_PARAMETER);
    XrXirEffects fresh={0};CHECK(effect_legacy_error_oracle_prepare(&context,effects,&fresh)==XR_XIR_OK);
    CHECK(fresh.root_input==final && !fresh.outcomes_work.contracts &&
        effect_contracts_read(&fresh)==final->contracts && !fresh.outcomes_work.entry_roots);
    effect_legacy_error_oracle_free(&fresh);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
}
