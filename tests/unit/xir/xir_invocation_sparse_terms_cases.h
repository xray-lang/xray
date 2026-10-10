/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_sparse_terms_cases.h - Authentic canonical bitset boundaries
 *
 * KEY CONCEPT:
 *   Real deferred calls populate completed equations before enumeration.
 *   The unchanged finite owner pays for every fresh check, seal and replay.
 */
#ifndef XIR_INVOCATION_SPARSE_TERMS_CASES_H
#define XIR_INVOCATION_SPARSE_TERMS_CASES_H

static const uint32_t sparse_indices[5][3]={{0,0,0},{31,0,0},{62,63,64},{63,0,0},{64,0,0}};
static const uint32_t sparse_counts[5]={1,1,3,1,1};

/* The mutations attack a solved real owner, never manufacture a trusted row.
 * Restoration must precede the independent seal and producer destruction. */
static void conditional_sparse_rejections(EffectInvocationOwner *owner,
    EffectInvocationSolution *solution,uint32_t mode) {
    CHECK(mode<5 && owner->roots[1]<owner->count);
    EffectInvocationNode *node=&owner->nodes[owner->roots[1]];
    uint32_t last=sparse_indices[mode][sparse_counts[mode]-1];
    uint32_t words=(last+2+63)/64,count=UINT32_C(0x12345678);
    uint64_t saved=node->contexts[words-1];
    node->contexts[words-1]|=UINT64_C(1)<<63;
    CHECK(effect_invocation_terms(owner,1,NULL,&count)==XR_XIR_BAD_STRUCTURE && count==UINT32_C(0x12345678));
    CHECK(effect_invocation_closed(owner)==XR_XIR_BAD_STRUCTURE);
    node->contexts[words-1]=saved;
    saved=node->parameters[0];node->parameters[0]|=UINT64_C(1)<<63;
    CHECK(effect_invocation_terms(owner,1,NULL,&count)==XR_XIR_BAD_STRUCTURE && count==UINT32_C(0x12345678));
    CHECK(effect_invocation_closed(owner)==XR_XIR_BAD_STRUCTURE);
    node->parameters[0]=saved;
    node->parameters[0]|=UINT64_C(1)<<1;
    CHECK(effect_invocation_terms(owner,1,NULL,&count)==XR_XIR_BAD_STRUCTURE && count==UINT32_C(0x12345678));
    node->parameters[0]=saved;
    saved=node->cells[0];node->cells[0]|=UINT64_C(1);
    CHECK(effect_invocation_terms(owner,1,NULL,&count)==XR_XIR_BAD_STRUCTURE && count==UINT32_C(0x12345678));
    node->cells[0]=saved;
    saved=node->contexts[(last+1)/64];node->contexts[(last+1)/64]|=UINT64_C(1)<<((last+1)%64);
    CHECK(effect_invocation_terms(owner,1,NULL,&count)==XR_XIR_BAD_STRUCTURE && count==UINT32_C(0x12345678));
    node->contexts[(last+1)/64]=saved;
    const XrXirRootFormula *formula=&solution->formulas[1];
    CHECK(!formula->constant_mask && formula->term_count==sparse_counts[mode] && formula->terms);
    XrXirRootTerm *terms=(XrXirRootTerm *)formula->terms;
    uint32_t original=terms[0].index;terms[0].index=last+1;
    CHECK(effect_invocation_solution_match(owner,solution)==XR_XIR_BAD_STRUCTURE);
    terms[0].index=original;
    if (sparse_counts[mode]>1) {
        XrXirRootTerm first=terms[0];terms[0]=terms[1];terms[1]=first;
        CHECK(effect_invocation_solution_match(owner,solution)==XR_XIR_BAD_STRUCTURE);
        terms[1]=terms[0];terms[0]=first;
    }
    XrXirRootTerm copy[4]={{0}};copy[3]=(XrXirRootTerm){UINT32_C(0x12345678),UINT32_C(0x87654321)};
    CHECK(effect_invocation_terms(owner,1,copy,&count)==XR_XIR_OK && count==sparse_counts[mode]);
    for (uint32_t t=0;t<count;++t)
        CHECK(copy[t].kind==XR_XIR_ROOT_TERM_CONTEXT_CALL && copy[t].index==sparse_indices[mode][t]);
    CHECK(copy[3].kind==UINT32_C(0x12345678) && copy[3].index==UINT32_C(0x87654321));
}

static XrXirStatus conditional_sparse_case(const XrXirCompileContext *context,uint32_t mode,bool oracle) {
    if (mode>=5) return XR_XIR_BAD_STRUCTURE;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    uint32_t last=sparse_indices[mode][sparse_counts[mode]-1];
    XrXirInstruction wide[66]={{0}};
    for (uint32_t i=0;i<=last;++i)
        wide[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=i};
    /* Each call owns its canonical slice, even when it reads the same formal. */
    uint32_t references[3]={1,1,1};
    for (uint32_t t=0;t<sparse_counts[mode];++t) {
        wide[sparse_indices[mode][t]]=fixture.apply[0];wide[sparse_indices[mode][t]].args[0]=t;
    }
    wide[last+1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={last+2,0}};
    XrXirBlock block={.count=last+2};fixture.functions[1].blocks=&block;
    fixture.functions[1].instructions=wide;fixture.functions[1].instruction_count=last+2;
    fixture.functions[1].operands=references;fixture.functions[1].operand_count=sparse_counts[mode];
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};uint32_t phase=0;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(checked,&diagnostic); }
    const XrXirModule *module=checked?xr_xir_compile_artifact_module(checked):NULL;
    XrXirEffects effects={0};EffectGraph graph={0};EffectInvocationDeclaredBounds *bounds=NULL;
    EffectInvocationOwner *owner=NULL;EffectInvocationSolution *solution=NULL;
    EffectInvocationCertificate *certificate=NULL;
    if (status==XR_XIR_OK) { phase=2;status=invocation_core_storage(context,module,&effects); }
    if (status==XR_XIR_OK) { phase=3;status=effect_graph_build(module,&effects,&graph,(XrXirCompileContext *)context); }
    if (status==XR_XIR_OK) { phase=4;status=effect_parameters_derive(module,&effects,&graph,context); }
    if (status==XR_XIR_OK) { phase=5;status=effect_invocation_bounds_new(context,&bounds); }
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_invocation_bounds_capture(context,bounds,module->types,&module->functions[f],f,NULL);
    if (status==XR_XIR_OK) { phase=6;status=effect_invocations_create(context,module,&effects,&graph,bounds,&owner); }
    if (status==XR_XIR_OK) { phase=7;status=effect_invocations_solve(owner); }
    if (status==XR_XIR_OK) { phase=8;status=effect_invocation_stage(owner,&solution); }
    if (status==XR_XIR_OK && oracle) conditional_sparse_rejections(owner,solution,mode);
    EffectInvocationSealRequest request={&owner,&solution,NULL};
    if (status==XR_XIR_OK) { phase=9;status=effect_invocation_seal(context,&request,&certificate); }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_SPARSE mode=%u phase=%u status=%u f=%u b=%u i=%u\n",mode,phase,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    effect_invocation_free(owner);effect_invocation_solution_free(solution);
    effect_graph_free(&graph);invocation_core_storage_free(&effects);
    effect_invocation_bounds_free(bounds);xr_xir_compile_artifact_free(checked);
    memset(&fixture,0xCE,sizeof(fixture));memset(wide,0xCE,sizeof(wide));memset(&block,0xCE,sizeof(block));
    memset(references,0xCE,sizeof(references));
    if (status==XR_XIR_OK) {
        uint32_t caller=UINT32_MAX,formal=UINT32_MAX;
        phase=10;status=effect_invocation_certificate_node_mask(context,certificate,certificate->equations->roots[0],&caller);
        if (status==XR_XIR_OK) status=effect_invocation_certificate_node_mask(context,certificate,
            certificate->equations->roots[1],&formal);
        if (status==XR_XIR_OK && oracle) {
            CHECK(!caller && formal==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
            const XrXirRootFormula *formula=&certificate->solution->formulas[1];
            CHECK(!formula->constant_mask && formula->term_count==sparse_counts[mode] && formula->terms);
            for (uint32_t t=0;t<formula->term_count;++t)
                CHECK(formula->terms[t].kind==XR_XIR_ROOT_TERM_CONTEXT_CALL &&
                    formula->terms[t].index==sparse_indices[mode][t]);
            CHECK(certificate->equations->deferred_count==sparse_counts[mode]);
            CHECK(!certificate->equations->work && !certificate->equations->module &&
                !certificate->equations->effects && !certificate->equations->graph && !certificate->equations->declared);
        }
    }
    effect_invocation_certificate_free(certificate);return status;
}

static void conditional_sparse_literals(void) {
    for (uint32_t mode=0;mode<5;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_sparse_case(&context,mode,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}

static void conditional_sparse_resources(void) {
    for (uint32_t mode=0;mode<5;++mode) {
        size_t sites=0;
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_attempts=0;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
            rp_attempts=0;rp_fail_at=pass?pass-1:SIZE_MAX;
            XrXirStatus status=conditional_sparse_case(&context,mode,false);
            if (!pass) { CHECK(status==XR_XIR_OK);sites=rp_attempts; }
            else CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,baseline);rp_balanced(physical);
        }
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_sparse_case(&context,mode,false)==XR_XIR_OK);
        XrCompileResourceStats census=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
        uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
        for (uint32_t axis=0;axis<3;++axis) {
            CHECK(measured[axis]>0);
            for (uint32_t pass=0;pass<2;++pass) {
                XrCompileResourceLimits limits=rp_caps();uint64_t value=measured[axis]-(pass?0:1);
                if (axis==0) limits.allocated_bytes=value;
                else if (axis==1) limits.live_bytes=value;
                else limits.work=value;
                physical=rp_mark();context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
                CHECK(conditional_sparse_case(&context,mode,false)==(pass?XR_XIR_OK:XR_XIR_BUDGET));
                rp_owner_free(&context,baseline);rp_balanced(physical);
            }
        }
    }
}
#endif // XIR_INVOCATION_SPARSE_TERMS_CASES_H
