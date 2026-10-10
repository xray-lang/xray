/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_publish_cases.h - Real common publisher and public consumption
 */
static XrXirStatus conditional_invocation_publish(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    if (mode>=2) return XR_XIR_BAD_STRUCTURE;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,mode);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirRootCauseTrace *trace=NULL;
    XrXirDiagnostic diagnostic={0};uint32_t phase=0;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(checked,&diagnostic); }
    if (status==XR_XIR_OK) { phase=2;status=xr_xir_compile_effects_analyze(checked,&effects); }
    const XrXirModule *module=checked?xr_xir_compile_artifact_module(checked):NULL;
    if (status==XR_XIR_OK && oracle) {
        CHECK(effects->invocations && !effects->contexts && effects->root[0].requires_root==(mode!=0) &&
            !effects->root[0].unresolved && !effects->root[1].requires_root && effects->root[1].unresolved);
        const XrXirRootFormula *formula=&effects->contracts[1].formula;
        CHECK(!formula->constant_mask && formula->term_count==1 &&
            formula->terms[0].kind==XR_XIR_ROOT_TERM_CONTEXT_CALL && !formula->terms[0].index);
        CHECK(formula->terms!=effects->invocations->solution->formulas[1].terms &&
            !memcmp(formula->terms,effects->invocations->solution->formulas[1].terms,sizeof(*formula->terms)));
        XirEffectEntryRootView entry={0};
        CHECK(xir_effects_entry_root(context,effects,1,&entry)==XR_XIR_OK &&
            entry.intrinsic_mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        uint32_t mask=UINT32_MAX;
        CHECK(effect_invocation_effect_edge_mask(context,effects,0,mode?2:3,1,&mask)==XR_XIR_OK &&
            mask==(mode?XR_XIR_CALLABLE_ROOT_REQUIRED:0));
        EffectInvocationCertificate *saved=effects->invocations;XrXirRootFormula prior=*formula;
        XrXirRootEffects fact=effects->root[0];EffectGraph graph={0};
        EffectInvocationDeclaredBounds malformed={.resources=context->resources};
        CHECK(effect_invocation_publish(context,module,effects,&graph,&malformed,NULL)==XR_XIR_BAD_STRUCTURE &&
            effects->invocations==saved && effects->contracts[1].formula.terms==prior.terms &&
            effects->root[0].requires_root==fact.requires_root && effects->root[0].unresolved==fact.unresolved);
        effect_graph_free(&graph);
        size_t attempts=rp_attempts;XrXirEffects *occupied=effects;
        CHECK(xr_xir_compile_effects_analyze(checked,&occupied)==XR_XIR_BAD_STRUCTURE &&
            occupied==effects && rp_attempts==attempts);
    }
    xr_xir_compile_artifact_free(checked);checked=NULL;memset(&fixture,0,sizeof(fixture));
    if (status==XR_XIR_OK) { phase=3;status=xr_xir_compile_root_cause_trace_copy(context,effects,1,&trace); }
    if (status==XR_XIR_OK && oracle) {
        uint32_t count=0;const XrXirRootCauseStep *step=xr_xir_root_cause_trace_steps(trace,true,&count);
        CHECK(count==1 && step && step->function==1 && !step->instruction &&
            step->cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL && step->callee==UINT32_MAX && !step->distance);
        size_t attempts=rp_attempts;XrXirRootCauseTrace *occupied=trace;
        CHECK(xr_xir_compile_root_cause_trace_copy(context,effects,1,&occupied)==XR_XIR_BAD_STRUCTURE &&
            occupied==trace && rp_attempts==attempts);
        RootParameterMark physical=rp_mark();XrXirCompileContext foreign=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&foreign).live_bytes;XrXirRootCauseTrace *untouched=NULL;
        CHECK(xr_xir_compile_root_cause_trace_copy(&foreign,effects,1,&untouched)==XR_XIR_BAD_STRUCTURE && !untouched);
        rp_owner_free(&foreign,baseline);rp_balanced(physical);
        XrXirRootFormula *published=&effects->contracts[1].formula;
        XrXirRootTerm *term=(XrXirRootTerm *)published->terms;uint32_t kind=term[0].kind;
        term[0].kind=XR_XIR_ROOT_TERM_PARAMETER;EffectInvocationCertificate *certificate=effects->invocations;
        CHECK(certificate->solution->formulas[1].terms[0].kind==XR_XIR_ROOT_TERM_CONTEXT_CALL);
        term[0].kind=kind;
    }
    xr_xir_compile_effects_free(effects);effects=NULL;
    if (status==XR_XIR_OK && oracle) {
        uint32_t count=0;const XrXirRootCauseStep *step=xr_xir_root_cause_trace_steps(trace,true,&count);
        CHECK(count==1 && step && step->cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL);
    }
    xr_xir_compile_root_cause_trace_free(trace);
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_PUBLISH mode=%u phase=%u status=%u f=%u b=%u i=%u\n",mode,phase,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    return status;
}

/* A verified TEMPLATE keeps its real CALL_BIND declaration after Source's
 * temporary refinement owner has gone. The public owner, not a test driver,
 * must derive the root facts and seal the original bound. */
static XrXirStatus conditional_invocation_template_publish(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    if (mode>=2) return XR_XIR_BAD_STRUCTURE;
    RootParameterBindingFixture fixture;rp_binding_fixture(&fixture,context,mode!=0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirRootCauseTrace *trace=NULL;
    XrXirDiagnostic diagnostic={0};uint32_t phase=0;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(checked,&diagnostic); }
    if (status==XR_XIR_OK) { phase=2;status=xr_xir_compile_effects_analyze(checked,&effects); }
    if (status==XR_XIR_OK && oracle) {
        CHECK(effects->contexts && !effects->refinement && effects->invocations &&
            effects->contexts->forest->invocations && !effects->contexts->dense->uses.invocations);
        const XrXirModule *module=xr_xir_compile_artifact_module(checked);
        uint32_t mask=UINT32_MAX;
        CHECK(effect_invocation_bounds_mask(context,effects->invocations->declared,0,1,&mask)==XR_XIR_OK &&
            mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        CHECK(effect_invocation_bounds_mask(context,effects->contexts->forest->invocations->declared,
            0,1,&mask)==XR_XIR_OK && mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        CHECK(!effects->root[0].requires_root && !effects->root[0].unresolved &&
            !effects->root[1].requires_root && effects->root[1].unresolved);
        const XrXirRootFormula *formula=&effects->contracts[1].formula;
        CHECK(!formula->constant_mask && formula->term_count==1 &&
            formula->terms[0].kind==XR_XIR_ROOT_TERM_PARAMETER && !formula->terms[0].index);
        XrXirType *declared=NULL;size_t attempts=rp_attempts;
        CHECK(effect_invocation_source_declarations(context,module,0,NULL,&declared)==XR_XIR_OK && declared);
        CHECK(declared[1]==module->provenance->contracts[0].values[1].declared_type &&
            declared[1]!=module->functions[0].instructions[1].type);
        xr_compile_resources_free(declared);declared=NULL;
        XrXirType *occupied=(XrXirType *)module->functions[0].parameters;
        if (!occupied) occupied=(XrXirType *)module->functions[0].instructions;
        attempts=rp_attempts;
        CHECK(effect_invocation_source_declarations(context,module,0,NULL,&occupied)==XR_XIR_BAD_STRUCTURE &&
            occupied && rp_attempts==attempts);
    }
    xr_xir_compile_artifact_free(checked);checked=NULL;memset(&fixture,0,sizeof(fixture));
    if (status==XR_XIR_OK) { phase=3;status=xr_xir_compile_root_cause_trace_copy(context,effects,1,&trace); }
    if (status==XR_XIR_OK && oracle) {
        uint32_t count=0;const XrXirRootCauseStep *step=xr_xir_root_cause_trace_steps(trace,true,&count);
        CHECK(count==1 && step && step->function==1 && !step->instruction &&
            step->cause==XR_XIR_ROOT_CAUSE_PARAMETER && !step->slot && step->callee==UINT32_MAX && !step->distance);
        uint32_t mask=UINT32_MAX;
        CHECK(effect_invocation_bounds_mask(context,effects->contexts->forest->invocations->declared,
            0,1,&mask)==XR_XIR_OK && mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
    }
    xr_xir_compile_effects_free(effects);xr_xir_compile_root_cause_trace_free(trace);
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_TEMPLATE_PUBLISH mode=%u phase=%u status=%u f=%u b=%u i=%u\n",
        mode,phase,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    return status;
}
