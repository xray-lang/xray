/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_producer_cases.h - Owned exact real producer views
 *
 * KEY CONCEPT:
 *   A site view survives producer destruction and supplies no permission bit.
 */
static XrXirStatus conditional_invocation_producer(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    if (mode>=2) return XR_XIR_BAD_STRUCTURE;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,mode);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    uint32_t instruction=mode?1:2;XirEffectProducerView real={0};
    if (status==XR_XIR_OK) status=xir_effects_producer(context,effects,0,instruction,&real);
    if (status==XR_XIR_OK && oracle) {
        CHECK(!real.site && !real.function && real.instruction==instruction && real.target==2 &&
            !real.capture_count && real.type==(XrXirType)257 && effects->invocations);
        XirEffectProducerView frozen=real,untouched=real;
        CHECK(xir_effects_producer(context,effects,0,mode?2:3,&untouched)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&untouched,&frozen,sizeof(frozen)));
        CHECK(xir_effects_producer(context,effects,UINT32_MAX,instruction,&untouched)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&untouched,&frozen,sizeof(frozen)));
        EffectInvocationSite *site=&effects->invocations->equations->sites[real.site];
        uint32_t target=site->target;site->target=1;
        CHECK(xir_effects_producer(context,effects,0,instruction,&untouched)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&untouched,&frozen,sizeof(frozen)));
        site->target=target;
        RootParameterMark mark=rp_mark();
        XrXirCompileContext foreign=rp_owner(rp_caps());uint64_t baseline=rp_stats(&foreign).live_bytes;
        CHECK(xir_effects_producer(&foreign,effects,0,instruction,&untouched)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&untouched,&frozen,sizeof(frozen)));
        rp_owner_free(&foreign,baseline);rp_balanced(mark);
        const XrXirRootEffects *facts=xr_xir_effects_root(effects,0);
        CHECK(facts && facts->requires_root==(mode!=0) && !facts->unresolved);
    }
    xr_xir_compile_artifact_free(checked);memset(&fixture,0,sizeof(fixture));
    if (status==XR_XIR_OK) status=xir_effects_producer(context,effects,0,instruction,&real);
    if (status==XR_XIR_OK && oracle)
        CHECK(real.target==2 && real.instruction==instruction && !real.capture_count &&
            xr_xir_effects_root(effects,1)->unresolved);
    xr_xir_compile_effects_free(effects);return status;
}
