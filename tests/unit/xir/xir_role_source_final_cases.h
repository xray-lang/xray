/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_role_source_final_cases.h - Genuine terminal Source free mutations
 */
static EffectInvocationCertificate *rc_certificate;
static uint32_t rc_frees,rc_cut,rc_mutations,rc_mode;
static void role_source_free(void *memory,size_t bytes) {
    CHECK(memory && bytes);++rc_frees;
    if (rc_frees!=rc_cut || !rc_certificate) return;
    pc_free_hook=NULL;++rc_mutations;
    if (!rc_mode) {
        XrXirEffectParameter *parameters=(XrXirEffectParameter *)rc_certificate->final->outcomes->contracts[1].parameters;
        parameters[0].kind=XR_XIR_EFFECT_PARAMETER_FIXED;
        return;
    }
    EffectRoleProof *proof=&rc_certificate->final->role_proof;
    if (rc_mode==1) {
        for (uint32_t p=0;p<5;++p) {
            ((XrXirEffectParameter *)rc_certificate->final->outcomes->contracts[1].parameters)[p].kind=XR_XIR_EFFECT_PARAMETER_FIXED;
            proof->facts[proof->rows[1].first+p]=(EffectRoleFact){2,1,1,(p+4)%5};
        }
    } else if (rc_mode==2) {
        uint32_t selected=UINT32_MAX;
        for (uint32_t v=5;v<proof->rows[1].count;++v)
            if (proof->facts[proof->rows[1].first+v].level) { selected=proof->rows[1].first+v;break; }
        CHECK(selected!=UINT32_MAX);proof->facts[selected]=(EffectRoleFact){0};
    } else if (rc_mode==3) proof->facts=(void *)(uintptr_t)1;
    else if (rc_mode==4) proof->rows=(void *)(uintptr_t)1;
    else if (rc_mode==5) ++proof->total;
    else CHECK(false);
}
static void role_source_noalloc(const XrXirCompileContext *context,EffectInvocationCertificate *certificate) {
    size_t before_attempts=attempts;XrCompileResourceStats before=pc_stats(context);
    rc_frees=0;rc_cut=UINT32_MAX;rc_certificate=NULL;CHECK(!pc_free_hook);pc_free_hook=role_source_free;
    CHECK(effect_role_proof_match(context,certificate->body,&certificate->final->role_proof)==XR_XIR_OK);
    pc_free_hook=NULL;XrCompileResourceStats after=pc_stats(context);
    CHECK(attempts==before_attempts && !rc_frees && after.allocation_count==before.allocation_count &&
        after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes && after.work>before.work);
    fprintf(stderr,"role terminal V allocation0 free0 work%llu\n",(unsigned long long)(after.work-before.work));
}
static uint32_t role_source_once(uint32_t cut,uint32_t mode,bool attack,XrXirStatus *returned) {
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    InvocationRotationFixture fixture;invocation_rotation_fixture(&fixture);XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK && effects->invocations);
    EffectInvocationCertificate *certificate=effects->invocations;
    XrXirEffectParameter *parameters=(XrXirEffectParameter *)certificate->final->outcomes->contracts[1].parameters;
    XrXirEffectParameter saved_parameters[5];memcpy(saved_parameters,parameters,sizeof(saved_parameters));
    EffectRoleProof proof=certificate->final->role_proof;EffectRoleFact facts[8];
    CHECK(proof.rows && proof.facts && proof.rows[1].count==8 && proof.total==8);
    memcpy(facts,proof.facts,sizeof(facts));role_source_noalloc(&context,certificate);
    rc_frees=0;rc_cut=cut;rc_mode=mode;rc_mutations=0;rc_certificate=attack?certificate:NULL;
    CHECK(!pc_free_hook);pc_free_hook=role_source_free;
    XrXirStatus status=xir_effects_source_terminal(&context,xr_xir_compile_artifact_module(checked),effects);
    pc_free_hook=NULL;rc_certificate=NULL;uint32_t frees=rc_frees;
    CHECK(!attack || rc_mutations==1);
    certificate->final->role_proof=proof;memcpy(proof.facts,facts,sizeof(facts));
    memcpy(parameters,saved_parameters,sizeof(saved_parameters));
    fprintf(stderr,"role Source attack%u mode%u cut%u frees%u mutation%u status%u expected%u work%llu\n",
        attack,mode,cut,frees,rc_mutations,status,attack?XR_XIR_BAD_STRUCTURE:XR_XIR_OK,
        (unsigned long long)pc_stats(&context).work);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    role_source_noalloc(&context,certificate);
    xr_xir_compile_effects_free(effects);pc_release(&context,baseline);*returned=status;return frees;
}
static void pc_role_source_final_literals(void) {
    XrXirStatus control=XR_XIR_BAD_STRUCTURE;uint32_t frees=role_source_once(UINT32_MAX,0,false,&control);
    CHECK(control==XR_XIR_OK && frees);
    uint32_t modes=6;
    for (uint32_t mode=0;mode<modes;++mode) {
        XrXirStatus attack=XR_XIR_OK;CHECK(role_source_once(frees,mode,true,&attack)==frees);
        CHECK(attack==XR_XIR_BAD_STRUCTURE);
    }
    puts("terminal Source role mutations rejected, zero-allocation verifier, producer death and physical0; no full FI credit");
}
