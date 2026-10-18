/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_parameter_coverage_cases.h - Complete incoming callable sources
 */
#ifndef XIR_INVOCATION_PARAMETER_COVERAGE_CASES_H
#define XIR_INVOCATION_PARAMETER_COVERAGE_CASES_H
typedef struct InvocationParameterFixture {
    InvocationReturnedFixture returned;
    XrXirFunction functions[6];XrXirFunctionIdentity identities[6];
    XrXirInstruction entry[4],apply[2];XrXirBlock entry_block,apply_block;
    XrXirType callback;uint32_t argument;
} InvocationParameterFixture;

/* The original return, Cell and barrier recipes stay unchanged. Only the
 * real entry now passes the returned callback into an ordinary apply frame. */
static void invocation_parameter_fixture(InvocationParameterFixture *f,uint32_t mode) {
    static const uint32_t recipes[5]={5,6,2,3,4};
    memset(f,0,sizeof(*f));invocation_returned_fixture(&f->returned,recipes[mode]);
    memcpy(f->functions,f->returned.functions,sizeof(f->returned.functions));
    memcpy(f->identities,f->returned.identities,sizeof(f->returned.identities));
    f->callback=f->functions[1].result;
    f->entry[0]=f->returned.entry[0];
    uint32_t call=1;
    if (mode==3) { f->entry[1]=f->returned.entry[1];f->argument=1;call=2; }
    f->entry[call]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=5,.args={0,1}};
    f->entry[call+1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={call,0}};
    f->entry_block=(XrXirBlock){.count=call+2};
    f->functions[0].instructions=f->entry;f->functions[0].instruction_count=call+2;
    f->functions[0].blocks=&f->entry_block;f->functions[0].operands=&f->argument;
    f->functions[0].operand_count=1;
    f->apply[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64};
    f->apply[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->apply_block=(XrXirBlock){.count=2};
    f->functions[5]=(XrXirFunction){.name="apply",.name_length=5,.result=XR_XIR_I64,
        .parameters=&f->callback,.parameter_count=1,.instructions=f->apply,.instruction_count=2,
        .blocks=&f->apply_block,.block_count=1};
    f->returned.declarations.functions=f->identities;
    f->returned.module.functions=f->functions;f->returned.module.function_count=6;
}

static uint32_t invocation_parameter_actual(const XrXirCompileContext *work,const EffectInvocationFinal *final) {
    uint32_t found=UINT32_MAX;EffectCaptureRead read={.work=work,.final=final,
        .body=effect_body_owned_request(work,final->material)};
    for(uint32_t e=0;e<final->edge_count;++e) {
        const EffectInvocationEdge *edge=&final->edges[e];
        if(final->nodes[edge->caller].body!=0 || final->nodes[edge->target].body!=5) continue;
        bool matched=false;CHECK(effect_capture_edge_complete(&read,e,&matched)==XR_XIR_OK);
        if(!matched) continue;
        CHECK(found==UINT32_MAX || found==edge->target);found=edge->target;
    }
    CHECK(found!=UINT32_MAX);return found;
}

static void invocation_parameter_insert(EffectInvocationFinal *final,EffectCaptureFact fact) {
    CHECK(final->capture.count<final->capture.capacity);uint32_t at=0;
    while(at<final->capture.count && effect_capture_before(&final->capture.facts[at],&fact)) ++at;
    memmove(final->capture.facts+at+1,final->capture.facts+at,(size_t)(final->capture.count-at)*sizeof(fact));
    final->capture.facts[at]=fact;++final->capture.count;
}

/* Complete-source clauses and key rows are checked at each mutation. Full
 * origin closure is checked before and after this allocation-free component. */
static XrXirStatus invocation_parameter_current(const XrXirCompileContext *work,EffectInvocationFinal *final) {
    EffectCaptureRead read={.work=work,.final=final};XrXirStatus status=effect_capture_frame(&read);
    EffectBodyCurrentRead current={0};
    if(status==XR_XIR_OK) status=effect_body_current_open(work,final->material,&current);
    if(status!=XR_XIR_OK) return status;
    read.body.current=&current;status=effect_capture_complete_clauses(&read);
    return status==XR_XIR_OK?effect_capture_inputs_match(&read):status;
}

static void invocation_parameter_expect_rejection(const XrXirCompileContext *work,
    EffectInvocationFinal *final,uint32_t mode,const char *mutation) {
    size_t before=rp_attempts;XrXirStatus actual=invocation_parameter_current(work,final);
    if(actual!=XR_XIR_BAD_STRUCTURE) fprintf(stderr,"PARAMETER_REJECTION mode%u %s status%u\n",mode,mutation,actual);
    CHECK(actual==XR_XIR_BAD_STRUCTURE && before==rp_attempts);
}

/* Mutations target the sole finite relation after the actual workspace dies.
 * Independent fixture literals distinguish known arguments from weak sources. */
static void invocation_parameter_rejections(const XrXirCompileContext *work,
    EffectInvocationFinal *final,uint32_t mode) {
    uint32_t n=invocation_parameter_actual(work,final),formal=final->roots[5];
    bool complete=false;CHECK(effect_final_callable_complete(work,final,n,0,&complete)==XR_XIR_OK);
    CHECK(complete==(mode<2));if(mode<2) CHECK(n!=formal);
    CHECK(effect_final_callable_complete(work,final,formal,0,&complete)==XR_XIR_OK && !complete);
    CHECK(effect_capture_match(work,final)==XR_XIR_OK);
    EffectCaptureProof saved=final->capture;XrXirStatus status=XR_XIR_OK;
    EffectCaptureFact *facts=xir_compile_alloc(work,((size_t)saved.count+2)*sizeof(*facts),&status);CHECK(facts);
    final->capture=(EffectCaptureProof){facts,saved.count,saved.count+2};
#define PARAMETER_RESET() do { memcpy(facts,saved.facts,(size_t)saved.count*sizeof(*facts));final->capture.count=saved.count; } while(0)
    PARAMETER_RESET();
    invocation_parameter_insert(final,(EffectCaptureFact){formal,0,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE,0});
    invocation_parameter_expect_rejection(work,final,mode,"external-formal-complete");PARAMETER_RESET();
    const EffectBodyFunction *formal_body=&final->material->functions[final->nodes[formal].body];
    uint32_t returned=formal_body->parameter_count+formal_body->instruction_count;
    invocation_parameter_insert(final,(EffectCaptureFact){formal,returned,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE,0});
    invocation_parameter_expect_rejection(work,final,mode,"noncallable-formal-return");PARAMETER_RESET();
    uint32_t formal_count=final->nodes[formal].parameter_count;
    final->nodes[formal].parameter_count=formal_count+1;
    invocation_parameter_expect_rejection(work,final,mode,"formal-dimension");
    final->nodes[formal].parameter_count=formal_count;
    final->capture.facts=NULL;
    invocation_parameter_expect_rejection(work,final,mode,"missing-capture-storage");
    final->capture.facts=facts;
    bool erased=false;
    for(uint32_t p=0;p<final->capture.count;++p) {
        EffectCaptureFact *fact=&facts[p];
        if(fact->node==n && !fact->value && fact->word==EFFECT_CAPTURE_FLAGS && fact->bit==EFFECT_CAPTURE_COMPLETE) {
            --final->capture.count;memmove(fact,fact+1,(size_t)(final->capture.count-p)*sizeof(*fact));erased=true;break;
        }
    }
    if(!erased) invocation_parameter_insert(final,(EffectCaptureFact){n,0,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE,0});
    invocation_parameter_expect_rejection(work,final,mode,"actual-completeness-flipped");PARAMETER_RESET();
    CHECK(final->capture.count);facts[0].rank=UINT64_MAX;
    invocation_parameter_expect_rejection(work,final,mode,"rank-overflow");PARAMETER_RESET();
    uint32_t count=final->nodes[n].parameter_count;final->nodes[n].parameter_count=count+1;
    invocation_parameter_expect_rejection(work,final,mode,"actual-dimension");final->nodes[n].parameter_count=count;
    final->roots[5]=final->count;
    invocation_parameter_expect_rejection(work,final,mode,"invalid-formal-root");final->roots[5]=formal;
    if(mode<2) {
        uint64_t input=final->nodes[n].input[0];final->nodes[n].input[0]^=UINT64_C(1);
        invocation_parameter_expect_rejection(work,final,mode,"changed-key");final->nodes[n].input[0]=input;
        uint32_t incoming=UINT32_MAX;
        for(uint32_t e=0;e<final->edge_count;++e) if(final->edges[e].target==n) { incoming=e;break; }
        CHECK(incoming!=UINT32_MAX);final->edges[incoming].target=formal;
        invocation_parameter_expect_rejection(work,final,mode,"missing-actual-incoming");final->edges[incoming].target=n;
        uint32_t factory=UINT32_MAX;
        for(uint32_t p=0;p<final->capture.count;++p) {
            const EffectCaptureFact *fact=&facts[p];
            if(final->nodes[fact->node].body!=1 || fact->word!=EFFECT_CAPTURE_FLAGS || fact->bit!=EFFECT_CAPTURE_COMPLETE) continue;
            const EffectBodyFunction *head=&final->material->functions[1];
            if(fact->value!=head->parameter_count+head->instruction_count) continue;
            factory=p;break;
        }
        CHECK(factory!=UINT32_MAX);--final->capture.count;
        memmove(facts+factory,facts+factory+1,(size_t)(final->capture.count-factory)*sizeof(*facts));
        invocation_parameter_expect_rejection(work,final,mode,"removed-factory-return");PARAMETER_RESET();
    }
    CHECK(effect_capture_match(work,final)==XR_XIR_OK);
    final->capture=saved;xr_compile_resources_free(facts);
#undef PARAMETER_RESET
}

static XrXirStatus conditional_parameter_case(const XrXirCompileContext *context,uint32_t mode,bool oracle) {
    if (mode>=5) return XR_XIR_BAD_STRUCTURE;
    InvocationParameterFixture fixture;invocation_parameter_fixture(&fixture,mode);
    XrXirArtifact *checked=NULL;XrXirEffects *published=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.returned.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&published);
    const XrXirModule *module=checked?xr_xir_compile_artifact_module(checked):NULL;
    XrXirEffects effects={0};EffectGraph graph={0};EffectInvocationDeclaredBounds *bounds=NULL;
    EffectInvocationWorkspace *owner=NULL;EffectFinalFunctions *solution=NULL;
    EffectInvocationCertificate *certificate=NULL;
    if (status==XR_XIR_OK) status=invocation_core_storage(context,module,&effects);
    if (status==XR_XIR_OK) status=effect_graph_build(module,&effects,&graph,(XrXirCompileContext *)context);
    if (status==XR_XIR_OK) status=effect_parameters_derive(module,&effects,&graph,context);
    if (status==XR_XIR_OK) status=effect_invocation_bounds_new(context,&bounds);
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_invocation_bounds_capture(context,bounds,module->types,&module->functions[f],f,NULL);
    if (status==XR_XIR_OK) status=effect_invocations_receive(context,module,&effects,&graph,bounds,&owner);
    if (status==XR_XIR_OK) status=effect_invocations_solve(owner);
    if (status==XR_XIR_OK) status=effect_invocation_stage(owner,&solution);
    if (status==XR_XIR_OK) {
        EffectInvocationSealRequest request={&owner,&solution,NULL};
        status=effect_invocation_seal(context,&request,&certificate,NULL);
    }
    effect_invocation_result_free(solution);effect_invocation_free(owner);
    effect_invocation_bounds_free(bounds);effect_graph_free(&graph);invocation_core_storage_free(&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0,sizeof(fixture));
    if(status==XR_XIR_OK) status=effect_capture_match(context,certificate->final);
    if (status==XR_XIR_OK && oracle) {
        CHECK(xr_xir_effects_error_unknown(published,0)==(mode>=2));
        CHECK(xr_xir_effects_error_unknown(published,5));
        invocation_parameter_rejections(context,certificate->final,mode);
        CHECK((certificate->final->material!=NULL));
    }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,"INVOCATION_PARAMETER mode=%u status=%u f=%u b=%u i=%u\n",
        mode,(unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    xr_xir_compile_effects_free(published);effect_invocation_certificate_free(certificate);return status;
}

static void conditional_parameter_literals(void) {
    for (uint32_t mode=0;mode<5;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(conditional_parameter_case(&context,mode,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}

static void conditional_parameter_resources(void) {
    for (uint32_t mode=0;mode<5;++mode) {
        size_t sites=0;XrCompileResourceStats single={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_attempts=0;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());XrCompileResourceStats entry=rp_stats(&context);
            RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
            rp_attempts=0;rp_fail_at=pass?pass-1:SIZE_MAX;
            XrXirStatus status=conditional_parameter_case(&context,mode,false);
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=rp_attempts;single=rp_stats(&context);
                fprintf(stderr,"CONDITIONAL_PARAMETER_CENSUS mode=%u sites=%zu allocated=%llu peak=%llu work=%llu\n",
                    mode,sites,(unsigned long long)single.allocated_bytes,
                    (unsigned long long)single.peak_bytes,(unsigned long long)single.work);
            } else {
                CHECK(rp_injected && status==XR_XIR_OUT_OF_MEMORY);
                ConditionalRetryTrial retry={.which=82+mode,.ordinal=pass-1,.sites=sites,.resources=identity,
                    .initial=entry,.single=single,.retained=retained,.first=status};
                conditional_retry_operation(&context,&retry);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
        }
        uint64_t measured[3]={single.allocated_bytes,single.peak_bytes,single.work};
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t pass=0;pass<2;++pass) {
            RootParameterMark physical=rp_mark();XrCompileResourceLimits limits=rp_caps();
            CHECK(measured[axis]);uint64_t value=measured[axis]-(pass?0:1);
            if (!axis) limits.allocated_bytes=value;else if (axis==1) limits.live_bytes=value;else limits.work=value;
            XrXirCompileContext context=rp_owner(limits);XrCompileResourceStats entry=rp_stats(&context);
            RootParameterMark retained=rp_mark();XrCompileResources *identity=context.resources;
            XrXirStatus status=conditional_parameter_case(&context,mode,false);
            CHECK(status==(pass?XR_XIR_OK:XR_XIR_BUDGET));
            ConditionalRetryTrial retry={.which=82+mode,.ordinal=SIZE_MAX,.sites=sites,.resources=identity,
                .initial=entry,.single=single,.retained=retained,.first=status};
            conditional_retry_operation(&context,&retry);
            rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
        }
    }
}
#endif // XIR_INVOCATION_PARAMETER_COVERAGE_CASES_H
