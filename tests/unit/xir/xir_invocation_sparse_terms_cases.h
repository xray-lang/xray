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

/* A complete real owner rejects missing physical Callable membership
 * before allocating or publishing a receiving certificate. Restore it before
 * the original seal; the independent raw classifier supplies the expected set. */
static void conditional_sparse_physical_bound_rejections(EffectInvocationOwner *owner,
    EffectInvocationResult *solution) {
    const XrXirCompileContext *context=owner->work;
    EffectPhysicalFunction *physical=&owner->physical->functions[1];
    CHECK(physical->count && physical->records[0].value==0 &&
        physical->records[0].mask&EFFECT_PHYSICAL_CALLABLE);
    EffectInvocationOwner *held=owner;EffectInvocationResult *staged=solution;
    EffectInvocationSealRequest request={&held,&staged,NULL};EffectInvocationCertificate *output=NULL;
    uint32_t original_mask=physical->records[0].mask;
    XrCompileResourceStats before=rp_stats(context);RootParameterMark retained=rp_mark();
    physical->records[0].mask&=~EFFECT_PHYSICAL_CALLABLE;
    CHECK(effect_invocation_seal(context,&request,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output &&
        held==owner && staged==solution && rp_stats(context).live_bytes==before.live_bytes &&
        rp_stats(context).allocation_count==before.allocation_count);
    physical->records[0].mask=original_mask;rp_balanced(retained);
    CHECK(effect_physical_match(context,owner->physical,owner->module)==XR_XIR_OK);
    EffectInvocationDeclaredBounds *bounds=(EffectInvocationDeclaredBounds *)owner->declared;
    EffectInvocationValueBound *bound=&bounds->functions[1].bounds[0];
    CHECK(bound->value==0 && bound->callable && bound->mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
}

/* The independent sparse indices define the expected canonical stream.
 * Matching may read it once, but must neither allocate an expected copy nor
 * write through either the staged storage or an aliased receiving request. */
static void conditional_sparse_receiver_rejections(EffectInvocationOwner *owner,
    EffectInvocationResult *solution,uint32_t mode) {
    const XrXirCompileContext *context=owner->work;
    XrXirRootFormula *formula=&solution->functions[1].formula;
    XrXirRootTerm *terms=(XrXirRootTerm *)formula->terms;
    CHECK(formula->term_count==sparse_counts[mode] && terms);
    size_t attempts=rp_attempts;RootParameterMark physical=rp_mark();
    XrCompileResourceStats before=rp_stats(context);
    CHECK(effect_invocation_result_match(owner,solution)==XR_XIR_OK && rp_attempts==attempts);
    uint32_t count=UINT32_C(0x12345678),limit=formula->term_count;
    CHECK(effect_invocation_terms_read(owner,1,terms,terms,limit,&count)==XR_XIR_BAD_STRUCTURE &&
        count==UINT32_C(0x12345678) && rp_attempts==attempts);
    CHECK(effect_invocation_terms_read(owner,1,NULL,terms,limit-1,&count)==XR_XIR_BAD_STRUCTURE &&
        count==UINT32_C(0x12345678) && rp_attempts==attempts);
    uint32_t kind=terms[0].kind;terms[0].kind=XR_XIR_ROOT_TERM_CELL_PARAMETER;
    CHECK(effect_invocation_result_match(owner,solution)==XR_XIR_BAD_STRUCTURE && rp_attempts==attempts);
    terms[0].kind=kind;
    const XrXirRootTerm *span=formula->terms;
    formula->term_count=0;formula->terms=NULL;
    CHECK(effect_invocation_result_match(owner,solution)==XR_XIR_BAD_STRUCTURE && rp_attempts==attempts);
    formula->term_count=limit;formula->terms=span+1;
    CHECK(effect_invocation_result_match(owner,solution)==XR_XIR_BAD_STRUCTURE && rp_attempts==attempts);
    formula->terms=span;formula->term_count=limit+1;
    CHECK(effect_invocation_result_match(owner,solution)==XR_XIR_BAD_STRUCTURE && rp_attempts==attempts);
    formula->term_count=limit;
    uint32_t root=owner->roots[1];owner->roots[1]=owner->count;
    CHECK(effect_invocation_result_match(owner,solution)==XR_XIR_BAD_STRUCTURE && rp_attempts==attempts);
    owner->roots[1]=root;
    CHECK(effect_invocation_result_match(owner,solution)==XR_XIR_OK && rp_attempts==attempts &&
        rp_stats(context).live_bytes==before.live_bytes &&
        rp_stats(context).allocated_bytes==before.allocated_bytes);
    for (uint32_t t=0;t<limit;++t)
        CHECK(terms[t].kind==XR_XIR_ROOT_TERM_CONTEXT_CALL && terms[t].index==sparse_indices[mode][t]);
    rp_balanced(physical);
}

/* The mutations attack a solved real owner, never manufacture a trusted row.
 * Restoration must precede the independent seal and producer destruction. */
static void conditional_sparse_rejections(EffectInvocationOwner *owner,
    EffectInvocationResult *solution,uint32_t mode) {
    CHECK(mode<5 && owner->roots[1]<owner->count);
    conditional_sparse_physical_bound_rejections(owner,solution);
    conditional_sparse_receiver_rejections(owner,solution,mode);
    EffectInvocationNode *node=&owner->nodes[owner->roots[1]];
    uint32_t instructions=node->instruction_count;
    CHECK(instructions==owner->material->functions[node->body].instruction_count && instructions);
    node->instruction_count=instructions-1;
    CHECK(effect_invocation_closed(owner)==XR_XIR_BAD_STRUCTURE);
    node->instruction_count=instructions+1;
    CHECK(effect_invocation_closed(owner)==XR_XIR_BAD_STRUCTURE);node->instruction_count=instructions;
    uint32_t causes=node->cause_count;node->cause_count=causes-1;
    CHECK(effect_invocation_closed(owner)==XR_XIR_BAD_STRUCTURE);node->cause_count=causes;
    node->cause_count=causes+2;
    CHECK(effect_invocation_closed(owner)==XR_XIR_BAD_STRUCTURE);node->cause_count=causes;
    uint64_t *cells=node->cells;node->cells=NULL;
    CHECK(effect_invocation_closed(owner)==XR_XIR_BAD_STRUCTURE);node->cells=cells;
    uint64_t *local_cells=node->local_cells;node->local_cells=NULL;
    CHECK(effect_invocation_closed(owner)==XR_XIR_BAD_STRUCTURE);node->local_cells=local_cells;
    CHECK(effect_invocation_closed(owner)==XR_XIR_OK);
    uint32_t last=sparse_indices[mode][sparse_counts[mode]-1];
    uint32_t words=(last+2+63)/64,count=UINT32_C(0x12345678);
    uint64_t *contexts=effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,false);
    uint64_t *parameters=effect_invocation_node_row(node,EFFECT_INVOCATION_PARAMETERS,false);
    uint64_t saved=contexts[words-1];
    contexts[words-1]|=UINT64_C(1)<<63;
    CHECK(effect_invocation_terms(owner,1,NULL,&count)==XR_XIR_BAD_STRUCTURE && count==UINT32_C(0x12345678));
    CHECK(effect_invocation_closed(owner)==XR_XIR_BAD_STRUCTURE);
    contexts[words-1]=saved;
    saved=parameters[0];parameters[0]|=UINT64_C(1)<<63;
    CHECK(effect_invocation_terms(owner,1,NULL,&count)==XR_XIR_BAD_STRUCTURE && count==UINT32_C(0x12345678));
    CHECK(effect_invocation_closed(owner)==XR_XIR_BAD_STRUCTURE);
    parameters[0]=saved;
    parameters[0]|=UINT64_C(1)<<1;
    CHECK(effect_invocation_terms(owner,1,NULL,&count)==XR_XIR_BAD_STRUCTURE && count==UINT32_C(0x12345678));
    parameters[0]=saved;
    saved=node->cells[0];node->cells[0]|=UINT64_C(1);
    CHECK(effect_invocation_terms(owner,1,NULL,&count)==XR_XIR_BAD_STRUCTURE && count==UINT32_C(0x12345678));
    node->cells[0]=saved;
    saved=contexts[(last+1)/64];contexts[(last+1)/64]|=UINT64_C(1)<<((last+1)%64);
    CHECK(effect_invocation_terms(owner,1,NULL,&count)==XR_XIR_BAD_STRUCTURE && count==UINT32_C(0x12345678));
    contexts[(last+1)/64]=saved;
    const XrXirRootFormula *formula=&solution->functions[1].formula;
    CHECK(!formula->constant_mask && formula->term_count==sparse_counts[mode] && formula->terms);
    XrXirRootTerm *terms=(XrXirRootTerm *)formula->terms;
    uint32_t original=terms[0].index;terms[0].index=last+1;
    CHECK(effect_invocation_result_match(owner,solution)==XR_XIR_BAD_STRUCTURE);
    terms[0].index=original;
    if (sparse_counts[mode]>1) {
        XrXirRootTerm first=terms[0];terms[0]=terms[1];terms[1]=first;
        CHECK(effect_invocation_result_match(owner,solution)==XR_XIR_BAD_STRUCTURE);
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
    EffectInvocationOwner *owner=NULL;EffectInvocationResult *solution=NULL;
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
    if (status==XR_XIR_OK) { phase=9;status=effect_invocation_seal(context,&request,&certificate,NULL); }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_SPARSE mode=%u phase=%u status=%u f=%u b=%u i=%u\n",mode,phase,status,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    effect_invocation_free(owner);effect_invocation_result_free(solution);
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
            const XrXirRootFormula *formula=&certificate->result->functions[1].formula;
            CHECK(!formula->constant_mask && formula->term_count==sparse_counts[mode] && formula->terms);
            for (uint32_t t=0;t<formula->term_count;++t)
                CHECK(formula->terms[t].kind==XR_XIR_ROOT_TERM_CONTEXT_CALL &&
                    formula->terms[t].index==sparse_indices[mode][t]);
            CHECK(certificate->equations->deferred_count==sparse_counts[mode]);
            uint32_t original_mask=UINT32_MAX;
            CHECK(effect_invocation_bounds_mask(context,certificate->declared,1,0,&original_mask)==XR_XIR_OK &&
                original_mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
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
        size_t sites=0;XrCompileResourceStats single={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_attempts=0;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());XrCompileResourceStats entry=rp_stats(&context);
            uint64_t baseline=entry.live_bytes;RootParameterMark retained=rp_mark();
            XrCompileResources *identity=context.resources;
            rp_attempts=0;rp_fail_at=pass?pass-1:SIZE_MAX;
            XrXirStatus status=conditional_sparse_case(&context,mode,false);
            if (!pass) { CHECK(status==XR_XIR_OK);sites=rp_attempts;single=rp_stats(&context); }
            else {
                CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
        ConditionalRetryTrial retry={.which=mode+61,.ordinal=pass-1,.sites=sites,
            .resources=identity,.initial=entry,.single=single,.retained=retained,.first=status};
        conditional_retry_operation(&context,&retry);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,baseline);rp_balanced(physical);
        }
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        size_t census_attempts=rp_attempts;
        CHECK(conditional_sparse_case(&context,mode,false)==XR_XIR_OK);
        XrCompileResourceStats census=rp_stats(&context);size_t census_sites=rp_attempts-census_attempts;
        rp_owner_free(&context,baseline);rp_balanced(physical);
        uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
        for (uint32_t axis=0;axis<3;++axis) {
            CHECK(measured[axis]>0);
            for (uint32_t pass=0;pass<2;++pass) {
                XrCompileResourceLimits limits=rp_caps();uint64_t value=measured[axis]-(pass?0:1);
                if (axis==0) limits.allocated_bytes=value;
                else if (axis==1) limits.live_bytes=value;
                else limits.work=value;
                physical=rp_mark();context=rp_owner(limits);XrCompileResourceStats entry=rp_stats(&context);
                baseline=entry.live_bytes;RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
                CHECK(conditional_sparse_case(&context,mode,false)==(pass?XR_XIR_OK:XR_XIR_BUDGET));
        ConditionalRetryTrial retry={.which=mode+61,.ordinal=SIZE_MAX,.sites=census_sites,
            .resources=identity,.initial=entry,.single=census,.retained=retained,.first=pass?XR_XIR_OK:XR_XIR_BUDGET};
        conditional_retry_operation(&context,&retry);
                rp_owner_free(&context,baseline);rp_balanced(physical);
            }
        }
    }
}
#endif // XIR_INVOCATION_SPARSE_TERMS_CASES_H
