/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_physical_classification_cases.h - Physical source completeness and owned replay
 */
#ifndef XIR_INVOCATION_PHYSICAL_CLASSIFICATION_CASES_H
#define XIR_INVOCATION_PHYSICAL_CLASSIFICATION_CASES_H

/* These are storage ownership and generation assertions, not permission
 * from pointer identity. Complete membership is independently matched. */
static void conditional_physical_generations(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate,XrXirModule *module) {
    EffectGraph graph={0};
    CHECK(effect_graph_physical_refresh(context,module,&graph,NULL)==XR_XIR_OK && graph.physical_owned);
    EffectPhysicalDomain *initial=graph.physical;
    CHECK(initial && initial!=certificate->equations->physical &&
        effect_physical_match(context,initial,module)==XR_XIR_OK);
    size_t first_attempt=rp_attempts;uint64_t first_live=rp_stats(context).live_bytes;
    rp_fail_at=first_attempt;rp_injected=false;
    CHECK(effect_graph_physical_refresh(context,module,&graph,NULL)==XR_XIR_OUT_OF_MEMORY &&
        rp_injected && graph.physical==initial && graph.physical_owned &&
        rp_stats(context).live_bytes==first_live);
    rp_fail_at=SIZE_MAX;rp_injected=false;
    CHECK(effect_physical_match(context,initial,module)==XR_XIR_OK);
    XrXirInstruction *ops=(XrXirInstruction *)module->functions[1].instructions;
    XrXirOp original=ops[64].op;CHECK(original==XR_XIR_COPY);
    ops[64].op=XR_XIR_FUNCTION_WEAKEN;
    CHECK(effect_physical_match(context,initial,module)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_graph_physical_refresh(context,module,&graph,NULL)==XR_XIR_OK &&
        graph.physical_owned && graph.physical!=initial &&
        effect_physical_match(context,graph.physical,module)==XR_XIR_OK);
    bool found=false;
    for (uint32_t r=0;r<graph.physical->functions[1].count;++r) {
        EffectPhysicalRecord record=graph.physical->functions[1].records[r];
        if (record.value==66) { CHECK(record.mask==EFFECT_PHYSICAL_CALLABLE);found=true; }
    }
    CHECK(found);ops[64].op=original;
    CHECK(effect_graph_physical_refresh(context,module,&graph,NULL)==XR_XIR_OK);
    initial=graph.physical;EffectBodyMaterial *material=graph.material,*occupied=material;size_t attempts=rp_attempts;
    CHECK(effect_graph_material_take(context,module,&graph,&occupied)==XR_XIR_BAD_STRUCTURE &&
        occupied==material && graph.material==material && graph.physical==initial && graph.physical_owned && rp_attempts==attempts);
    EffectBodyMaterial *taken=NULL;
    CHECK(effect_graph_material_take(context,module,&graph,&taken)==XR_XIR_OK && taken==material && taken->physical==initial &&
        !graph.material && !graph.physical && !graph.physical_owned && rp_attempts==attempts);
    CHECK(effect_physical_match(context,taken->physical,module)==XR_XIR_OK);
    graph.material=taken;graph.physical=taken->physical;graph.physical_owned=false;effect_graph_free(&graph);
    CHECK(effect_physical_match(context,taken->physical,module)==XR_XIR_OK);
    graph.material=taken;graph.physical=taken->physical;graph.physical_owned=false;
    const bool escaping_empty[4]={false,false,true,true};
    for (uint32_t f=0;f<4;++f) {
        bool raw=!escaping_empty[f],shared=!escaping_empty[f];
        CHECK(error_empty_escaping(&module->functions[f],context,&raw)==XR_XIR_OK &&
            raw==escaping_empty[f]);
        CHECK(error_empty_escaping_graph(f,&graph,context,&shared)==XR_XIR_OK &&
            shared==escaping_empty[f]);
    }
    effect_graph_free(&graph);
    effect_body_material_free(taken);
    CHECK(effect_physical_match(context,certificate->equations->physical,module)==XR_XIR_OK);
}

static void conditional_physical_rejections(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate,XrXirModule *module) {
    EffectPhysicalDomain *domain=certificate->equations->physical;
    CHECK(effect_physical_match(context,domain,module)==XR_XIR_OK);
    EffectPhysicalFunction *body=&domain->functions[1];
    const uint32_t values[10]={0,3,4,5,6,7,65,66,96,97};
    const uint32_t masks[10]={1,4,8,8,8,8,27,9,2328,32};
    CHECK(domain->count==4 && body->parameters==2 && body->instructions==96 && body->values==98 &&
        body->rows==3 && body->count==10 && body->records);
    for (uint32_t r=0;r<10;++r)
        CHECK(body->records[r].value==values[r] && body->records[r].mask==masks[r]);
    const EffectPhysicalFunction *scalar=&domain->functions[3];
    const uint32_t scalar_values[7]={1,2,4,5,6,7,8};
    const uint32_t scalar_masks[7]={4,8,8,8,8,8,32};
    CHECK(!scalar->parameters && scalar->instructions==9 && scalar->values==9 && !scalar->rows &&
        scalar->count==7);
    for (uint32_t r=0;r<7;++r)
        CHECK(scalar->records[r].value==scalar_values[r] && scalar->records[r].mask==scalar_masks[r]);
    uint32_t count=body->count;body->count=count-1;
    CHECK(effect_physical_match(context,domain,module)==XR_XIR_BAD_STRUCTURE);body->count=count;
    uint32_t mask=body->records[1].mask;body->records[1].mask=0;
    CHECK(effect_physical_match(context,domain,module)==XR_XIR_BAD_STRUCTURE);body->records[1].mask=mask;
    uint32_t error_mask=body->records[8].mask;
    body->records[8].mask=error_mask&~EFFECT_PHYSICAL_ERROR_ESCAPE;
    CHECK(effect_physical_match(context,domain,module)==XR_XIR_BAD_STRUCTURE);
    body->records[8].mask=error_mask&~EFFECT_PHYSICAL_CONTROL;
    CHECK(effect_physical_match(context,domain,module)==XR_XIR_BAD_STRUCTURE);
    body->records[8].mask=error_mask;
    uint32_t value=body->records[2].value;body->records[2].value=3;
    CHECK(effect_physical_match(context,domain,module)==XR_XIR_BAD_STRUCTURE);body->records[2].value=value;
    uint32_t rows=body->rows;body->rows=rows-1;
    CHECK(effect_physical_match(context,domain,module)==XR_XIR_BAD_STRUCTURE);body->rows=rows;
    XrXirInstruction *ops=(XrXirInstruction *)module->functions[1].instructions;
    XrXirOp invalid=ops[63].op;ops[63].op=XR_XIR_INVALID;
    CHECK(effect_physical_match(context,domain,module)==XR_XIR_BAD_STRUCTURE);ops[63].op=invalid;
    XrXirOp opcode=ops[2].op;ops[2].op=XR_XIR_CONST_INT;
    CHECK(effect_physical_match(context,domain,module)==XR_XIR_BAD_STRUCTURE);ops[2].op=opcode;
    XrXirType type=ops[64].type;ops[64].type=XR_XIR_I64;
    CHECK(effect_physical_match(context,domain,module)==XR_XIR_BAD_STRUCTURE);ops[64].type=type;
    EffectPhysicalDomain *occupied=domain;size_t attempts=rp_attempts;
    CHECK(effect_physical_new(context,module,&occupied)==XR_XIR_BAD_STRUCTURE &&
        occupied==domain && rp_attempts==attempts);
    CHECK(effect_physical_match(context,domain,module)==XR_XIR_OK);
    conditional_physical_generations(context,certificate,module);
    EffectInvocationOwner replay=*certificate->equations;
    replay.work=context;replay.module=module;replay.physical=NULL;
    EffectInvocationFlow flow={.owner=&replay,.node=replay.roots[1]};
    CHECK(effect_invocation_origins(&flow)==XR_XIR_BAD_STRUCTURE && !flow.memory);
    RootParameterMark physical=rp_mark();XrXirCompileContext foreign=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&foreign).live_bytes;
    CHECK(effect_physical_match(&foreign,domain,module)==XR_XIR_BAD_STRUCTURE);
    rp_owner_free(&foreign,baseline);rp_balanced(physical);
}

/* The graph is the existing wide scalar/Cell/PHI/real callable fixture.
 * A separate receiving body survives every raw fixture and Artifact owner. */
static XrXirStatus conditional_physical_case(const XrXirCompileContext *context,bool oracle) {
    InvocationDeferredFixture fixture;XrXirInstruction wide[96],init[9];
    XrXirBlock blocks[3];uint32_t incoming[2];
    invocation_storage_fixture(&fixture,2,wide,init,blocks,incoming);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirDiagnostic diagnostic={0};uint32_t phase=0;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(checked,&diagnostic); }
    if (status==XR_XIR_OK) {
        phase=2;status=xr_xir_compile_effects_analyze(checked,&effects);
    }
    if (status==XR_XIR_OK && oracle) {
        XrXirModule current=*xr_xir_compile_artifact_module(checked);
        CHECK(effect_body_source_match(context,effects->invocations->body,&current)==XR_XIR_OK);
        conditional_physical_rejections(context,effects->invocations,&current);
    }
    xr_xir_compile_artifact_free(checked);checked=NULL;
    memset(&fixture,0xCE,sizeof(fixture));memset(wide,0xCE,sizeof(wide));memset(init,0xCE,sizeof(init));
    memset(blocks,0xCE,sizeof(blocks));memset(incoming,0xCE,sizeof(incoming));
    if (status==XR_XIR_OK) {
        phase=3;
        if (!effects->invocations || !effects->invocations->equations ||
            !effects->invocations->equations->physical) status=XR_XIR_BAD_STRUCTURE;
        else status=effect_body_physical_match(context,effects->invocations->equations->physical,
            effects->invocations->body);
    }
    if (status==XR_XIR_OK && oracle) {
        CHECK(!effects->root[0].requires_root && !effects->root[0].unresolved && effects->root[1].unresolved);
        CHECK(!effects->invocations->equations->module && !effects->invocations->equations->work &&
            effects->invocations->equations->physical->resources==context->resources);
        const XrXirTypeNode *signature=xr_xir_callable_signature(&effects->invocations->terms.types,
            effects->invocations->body->functions[1].parameters[0]);
        CHECK(signature && (signature->flags&XR_XIR_CALLABLE_ROOT_UNRESOLVED));
    }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_PHYSICAL phase=%u status=%u f=%u b=%u i=%u\n",phase,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    xr_xir_compile_effects_free(effects);return status;
}

static void conditional_physical_literals(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(conditional_physical_case(&context,true)==XR_XIR_OK);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void conditional_physical_resources(void) {
    size_t sites=0;XrCompileResourceStats single={0};
    for (size_t pass=0;pass<=sites;++pass) {
        RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_attempts=0;rp_injected=false;
        XrXirCompileContext context=rp_owner(rp_caps());XrCompileResourceStats entry=rp_stats(&context);
        RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
        rp_attempts=0;rp_fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=conditional_physical_case(&context,false);
        if (!pass) { CHECK(status==XR_XIR_OK);sites=rp_attempts;single=rp_stats(&context); }
        else {
            CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
            ConditionalRetryTrial retry={.which=73,.ordinal=pass-1,.sites=sites,.resources=identity,
                .initial=entry,.single=single,.retained=retained,.first=status};
            conditional_retry_operation(&context,&retry);
        }
        rp_fail_at=SIZE_MAX;rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
    }
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;size_t attempts=rp_attempts;
    CHECK(conditional_physical_case(&context,false)==XR_XIR_OK);
    XrCompileResourceStats census=rp_stats(&context);size_t census_sites=rp_attempts-attempts;
    rp_owner_free(&context,baseline);rp_balanced(physical);
    uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t pass=0;pass<2;++pass) {
        XrCompileResourceLimits limits=rp_caps();uint64_t value=measured[axis]-(pass?0:1);
        if (!axis) limits.allocated_bytes=value;else if (axis==1) limits.live_bytes=value;else limits.work=value;
        physical=rp_mark();context=rp_owner(limits);XrCompileResourceStats entry=rp_stats(&context);
        RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
        XrXirStatus status=conditional_physical_case(&context,false);
        CHECK(status==(pass?XR_XIR_OK:XR_XIR_BUDGET));
        ConditionalRetryTrial retry={.which=73,.ordinal=SIZE_MAX,.sites=census_sites,.resources=identity,
            .initial=entry,.single=census,.retained=retained,.first=status};
        conditional_retry_operation(&context,&retry);
        rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
    }
}
#endif // XIR_INVOCATION_PHYSICAL_CLASSIFICATION_CASES_H
