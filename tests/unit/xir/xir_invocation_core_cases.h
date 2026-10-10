/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_core_cases.h - Authentic owned invocation certificate probes
 */
static XrXirStatus invocation_core_storage(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirEffects *effects) {
    *effects=(XrXirEffects){.resources=context->resources,.count=module->function_count};
    XrXirStatus status=XR_XIR_OK;uint32_t count=effects->count;
    effects->functions=xir_compile_calloc(context,count,sizeof(*effects->functions),&status);
    if (status==XR_XIR_OK) effects->task_creation=xir_compile_calloc(context,count,sizeof(*effects->task_creation),&status);
    if (status==XR_XIR_OK) effects->root=xir_compile_calloc(context,count,sizeof(*effects->root),&status);
    if (status==XR_XIR_OK) effects->root_witnesses=xir_compile_calloc(context,count,sizeof(*effects->root_witnesses),&status);
    if (status==XR_XIR_OK) effects->unresolved_witnesses=xir_compile_calloc(context,count,sizeof(*effects->unresolved_witnesses),&status);
    return status;
}

static void invocation_core_storage_free(XrXirEffects *effects) {
    effect_parameters_free(effects);
    xr_compile_resources_free(effects->functions);xr_compile_resources_free(effects->task_creation);
    xr_compile_resources_free(effects->root);xr_compile_resources_free(effects->root_witnesses);
    xr_compile_resources_free(effects->unresolved_witnesses);*effects=(XrXirEffects){0};
}

/* Full construction and Artifact checking precede the same-owner equation
 * lifecycle. No pre-existing ROOT formula is solved or lowered by this probe.
 * The certificate remains private and cannot admit a Program or runtime call. */
static XrXirStatus conditional_invocation_core(const XrXirCompileContext *context,uint32_t mode,bool oracle) {
    if (mode>=4) return XR_XIR_BAD_STRUCTURE;
    RootParameterBindingFixture fixture;rp_binding_fixture(&fixture,context,(mode&1)!=0);
    uint32_t self_operand=0;
    XrXirInstruction recursive[3]={{.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1,.args={0,1}},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirBlock recursive_block={.count=3};
    XrXirEffectParameter recursive_parameter={XR_XIR_EFFECT_PARAMETER_VARIABLE,
        XR_XIR_EFFECT_USE_INVOKE|XR_XIR_EFFECT_USE_FORWARD};
    if (mode&2) {
        fixture.functions[1].instructions=recursive;fixture.functions[1].instruction_count=3;
        fixture.functions[1].blocks=&recursive_block;fixture.functions[1].operands=&self_operand;
        fixture.functions[1].operand_count=1;fixture.contracts[1].parameters=&recursive_parameter;
    }
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    uint32_t phase=0;
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(checked,&diagnostic); }
    const XrXirModule *module=checked?xr_xir_compile_artifact_module(checked):NULL;
    XrXirEffects effects={0};EffectGraph graph={0};EffectInvocationDeclaredBounds *bounds=NULL;
    EffectInvocationCertificate *certificate=NULL;
    if (status==XR_XIR_OK) { phase=2;status=invocation_core_storage(context,module,&effects); }
    if (status==XR_XIR_OK) { phase=3;status=effect_graph_build(module,&effects,&graph,(XrXirCompileContext *)context); }
    if (status==XR_XIR_OK) { phase=4;status=effect_parameters_derive(module,&effects,&graph,context); }
    if (status==XR_XIR_OK) { phase=5;status=effect_invocation_bounds_new(context,&bounds); }
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_invocation_bounds_capture(context,bounds,module->types,&module->functions[f],f,NULL);
    if (status==XR_XIR_OK) {
        phase=6;EffectInvocationDriverInput input={context,module,&effects,&graph,bounds,NULL};
        status=effect_invocation_derive(&input,&certificate);
    }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_CORE mode=%u phase=%u status=%u f=%u b=%u i=%u\n",mode,phase,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    effect_graph_free(&graph);invocation_core_storage_free(&effects);
    effect_invocation_bounds_free(bounds);xr_xir_compile_artifact_free(checked);checked=NULL;
    memset(&fixture,0,sizeof(fixture));memset(recursive,0,sizeof(recursive));
    EffectInvocationCauseTrace *trace=NULL;
    if (status==XR_XIR_OK) {
        uint32_t caller_mask=UINT32_MAX,formal_mask=UINT32_MAX,edge_mask=UINT32_MAX;
        status=effect_invocation_certificate_node_mask(context,certificate,certificate->equations->roots[0],&caller_mask);
        if (status==XR_XIR_OK) status=effect_invocation_certificate_node_mask(context,certificate,
            certificate->equations->roots[1],&formal_mask);
        if (status==XR_XIR_OK) status=effect_invocation_certificate_edge_mask(context,certificate,0,2,1,&edge_mask);
        if (status==XR_XIR_OK) status=effect_invocation_certificate_trace(context,certificate,1,&trace);
        if (status==XR_XIR_OK && oracle) {
            CHECK(!caller_mask && formal_mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED && !edge_mask);
            CHECK(certificate->solution->formulas[1].term_count==1 &&
                certificate->solution->formulas[1].terms[0].kind==XR_XIR_ROOT_TERM_PARAMETER &&
                certificate->solution->formulas[1].terms[0].index==0);
            CHECK(trace && !trace->counts[0] && trace->counts[1]==1 &&
                trace->steps[0].function==1 && trace->steps[0].witness.cause==XR_XIR_ROOT_CAUSE_PARAMETER &&
                !trace->steps[0].witness.distance && trace->steps[0].witness.slot==0);
            size_t attempts=rp_attempts;EffectInvocationCertificate *occupied=certificate;
            EffectInvocationDriverInput empty={.work=context};
            CHECK(effect_invocation_derive(&empty,&occupied)==XR_XIR_BAD_STRUCTURE &&
                occupied==certificate && rp_attempts==attempts);
            EffectInvocationCauseTrace *occupied_trace=trace;
            CHECK(effect_invocation_certificate_trace(context,certificate,1,&occupied_trace)==XR_XIR_BAD_STRUCTURE &&
                occupied_trace==trace && rp_attempts==attempts);
            uint32_t n=certificate->equations->roots[0];
            certificate->equations->nodes[n].intrinsic_mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
            uint32_t untouched=UINT32_C(0x12345678);
            CHECK(effect_invocation_certificate_node_mask(context,certificate,n,&untouched)==XR_XIR_BAD_STRUCTURE &&
                untouched==UINT32_C(0x12345678));
            certificate->equations->nodes[n].intrinsic_mask&=~XR_XIR_CALLABLE_ROOT_UNRESOLVED;
            RootParameterMark physical=rp_mark();XrXirCompileContext foreign=rp_owner(rp_caps());
            uint64_t baseline=rp_stats(&foreign).live_bytes;
            CHECK(effect_invocation_certificate_node_mask(&foreign,certificate,n,&untouched)==XR_XIR_BAD_STRUCTURE &&
                untouched==UINT32_C(0x12345678));
            rp_owner_free(&foreign,baseline);rp_balanced(physical);
        }
    }
    effect_invocation_certificate_free(certificate);
    if (status==XR_XIR_OK && oracle) CHECK(trace && trace->counts[1]==1 &&
        trace->facts.unresolved && trace->steps[0].witness.cause==XR_XIR_ROOT_CAUSE_PARAMETER);
    effect_invocation_trace_free(trace);return status;
}
