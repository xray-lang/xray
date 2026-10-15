/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_final_control_proof_cases.h - Least control facts from real Input terminals
 */
typedef struct PcControlFixture {
    XrXirInstruction rows[3][3];XrXirBlock blocks[3];XrXirFunction functions[3];XrXirModule module;
} PcControlFixture;

static void pc_control_fixture(PcControlFixture *fixture,bool diamond) {
    memset(fixture,0,sizeof(*fixture));uint32_t count=diamond?3:2;
    for (uint32_t f=0;f<count;++f) {
        if (diamond && f) {
            fixture->rows[f][0]=(XrXirInstruction){.op=XR_XIR_SUSPEND};
            fixture->rows[f][1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
            fixture->rows[f][2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
            fixture->blocks[f].count=3;
        } else {
            fixture->rows[f][0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1-f};
            fixture->rows[f][1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
            fixture->blocks[f].count=2;
        }
        fixture->functions[f]=(XrXirFunction){.name="f",.name_length=1,.result=XR_XIR_I64,
            .instructions=fixture->rows[f],.instruction_count=fixture->blocks[f].count,
            .blocks=&fixture->blocks[f],.block_count=1};
    }
    if (diamond) {
        fixture->rows[0][1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=2};
        fixture->rows[0][2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
        fixture->blocks[0].count=3;fixture->functions[0].instruction_count=3;
    }
    fixture->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=fixture->functions,.function_count=count};
}

static void pc_control_cycle_literals(bool diamond) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    PcControlFixture fixture;pc_control_fixture(&fixture,diamond);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    EffectFinalOutcomes *final=effects->invocations->final->outcomes;CHECK(final);
    CHECK(effect_final_control_verify(&context,effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if (diamond) {
        CHECK(final->count==3 && final->functions[0].suspend==XR_XIR_EFFECT_MAY &&
            effect_control_cause_is(&final->witnesses[0],XR_XIR_EFFECT_CAUSE_CALL,0,1,1));
        XrXirEffectWitness saved=final->witnesses[0];
        final->witnesses[0]=(XrXirEffectWitness){XR_XIR_EFFECT_CAUSE_CALL,1,2,1};
        CHECK(effect_final_control_verify(&context,effects)==XR_XIR_BAD_STRUCTURE);
        final->witnesses[0]=saved;CHECK(effect_final_control_verify(&context,effects)==XR_XIR_OK);
    } else {
        CHECK(final->count==2 && !final->functions[0].suspend && !final->functions[1].suspend &&
            !final->task_creation[0] && !final->task_creation[1]);
        for (uint32_t f=0;f<2;++f) {
            final->functions[f].suspend=XR_XIR_EFFECT_MAY;
            final->witnesses[f]=(XrXirEffectWitness){XR_XIR_EFFECT_CAUSE_CALL,0,1-f,1};
        }
        CHECK(effect_final_control_verify(&context,effects)==XR_XIR_BAD_STRUCTURE);
        for (uint32_t f=0;f<2;++f) {
            final->functions[f].suspend=XR_XIR_EFFECT_NONE;final->witnesses[f]=(XrXirEffectWitness){0};
            final->task_creation[f]=XR_XIR_EFFECT_MAY;
        }
        CHECK(effect_final_control_verify(&context,effects)==XR_XIR_BAD_STRUCTURE);
        for (uint32_t f=0;f<2;++f) final->task_creation[f]=XR_XIR_EFFECT_NONE;
        CHECK(effect_final_control_verify(&context,effects)==XR_XIR_OK);
    }
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
}

static void pc_control_owned_literals(void) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    ClosedCarrierFixture fixture;cc_fixture(&fixture,8);fixture.edge.module.stage=XR_XIR_BUILT;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.edge.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    EffectFinalOutcomes *final=effects->invocations->final->outcomes;
    CHECK(final && final->entry_roots[1].parameter_count==1 && !final->entry_roots[1].parameters[0]);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    CHECK(effect_final_control_verify(&context,effects)==XR_XIR_OK);
    uint32_t *parameter=(uint32_t *)final->entry_roots[1].parameters;*parameter=1;
    CHECK(effect_final_control_verify(&context,effects)==XR_XIR_BAD_STRUCTURE);*parameter=0;
    uint32_t mask=final->entry_roots[1].intrinsic_mask;final->entry_roots[1].intrinsic_mask^=XR_XIR_CALLABLE_ROOT_REQUIRED;
    CHECK(effect_final_control_verify(&context,effects)==XR_XIR_BAD_STRUCTURE);final->entry_roots[1].intrinsic_mask=mask;
    XrXirStatus go=final->go_safe[1];final->go_safe[1]=go==XR_XIR_OK?XR_XIR_BAD_TYPE:XR_XIR_OK;
    CHECK(effect_final_control_verify(&context,effects)==XR_XIR_BAD_STRUCTURE);final->go_safe[1]=go;
    XrXirEffectWitness witness=final->witnesses[1];final->witnesses[1].instruction=1;
    CHECK(effect_final_control_verify(&context,effects)==XR_XIR_BAD_STRUCTURE);final->witnesses[1]=witness;
    CHECK(effect_final_control_verify(&context,effects)==XR_XIR_OK && effect_outcomes_empty(&effects->outcomes_work));
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);
}

static void pc_control_proof_literals(void) {
    pc_control_cycle_literals(false);pc_control_cycle_literals(true);pc_control_owned_literals();
}
