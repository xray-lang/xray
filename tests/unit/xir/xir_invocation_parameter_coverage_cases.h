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

static uint32_t invocation_parameter_actual(EffectInvocationWorkspace *owner) {
    uint32_t found=UINT32_MAX;
    for (uint32_t e=0;e<owner->final.edge_count;++e) {
        const EffectInvocationEdge *edge=&owner->final.edges[e];
        if (owner->final.nodes[edge->caller].body!=0 || owner->final.nodes[edge->target].body!=5) continue;
        EffectInvocationFlow flow={.owner=owner,.node=edge->caller,.instruction=UINT32_MAX};
        XrXirStatus status=effect_invocation_origins(&flow);bool matched=false;
        if (status==XR_XIR_OK) status=effect_invocation_coverage_input(&flow,e,&matched);
        xr_compile_resources_free(flow.memory);CHECK(status==XR_XIR_OK);
        if (!matched) continue;
        CHECK(found==UINT32_MAX || found==edge->target);found=edge->target;
    }
    CHECK(found!=UINT32_MAX);return found;
}

/* A mismatch records the actual first status without adding another proof. */
static void invocation_parameter_expect_rejection(EffectInvocationWorkspace *owner,
    uint32_t mode,const char *mutation) {
    XrXirStatus actual=effect_invocation_coverage_verify(owner);
    if (actual!=XR_XIR_BAD_STRUCTURE) {
        XrCompileResourceStats stats=rp_stats(owner->work);
        fprintf(stderr,"PARAMETER_REJECTION mode=%u mutation=%s actual=%u expected=%u producer=%u nodes=%u pending=%u allocated=%llu live=%llu peak=%llu work=%llu\n",
            mode,mutation,(unsigned)actual,(unsigned)XR_XIR_BAD_STRUCTURE,
            (unsigned)owner->producer_enabled,owner->final.count,owner->pending,
            (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.live_bytes,
            (unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
        fflush(stderr);
    }
    CHECK(actual==XR_XIR_BAD_STRUCTURE);
}

/* Disabled coverage still owns real zero facts and exact root storage. */
static void invocation_parameter_disabled_rejections(EffectInvocationWorkspace *owner,uint32_t mode) {
    if (owner->producer_enabled) return;
    uint32_t formal=owner->final.roots[5];EffectInvocationNode *node=&owner->final.nodes[formal];
    CHECK(!node->return_coverage && node->parameter_count==1 && node->parameter_coverage);
    node->return_coverage=1;
    invocation_parameter_expect_rejection(owner,mode,"disabled-return");
    node->return_coverage=0;
    uint32_t count=node->parameter_count;node->parameter_count=count+1;
    invocation_parameter_expect_rejection(owner,mode,"disabled-dimension");
    node->parameter_count=count;
    uint64_t *stored=node->parameter_coverage;node->parameter_coverage=NULL;
    invocation_parameter_expect_rejection(owner,mode,"disabled-storage");
    node->parameter_coverage=stored;
    owner->final.roots[5]=owner->final.count;
    invocation_parameter_expect_rejection(owner,mode,"disabled-root");
    owner->final.roots[5]=formal;
    CHECK(effect_invocation_coverage_verify(owner)==XR_XIR_OK);
}

/* Each altered private fact is restored before the actual seal. These
 * controls supply neither a prepared source clause nor a permission bit. */
static void invocation_parameter_rejections(EffectInvocationWorkspace *owner,uint32_t mode) {
    uint32_t n=invocation_parameter_actual(owner),formal=owner->final.roots[5];
    EffectInvocationNode *node=&owner->final.nodes[n];
    CHECK(node->parameter_count==1 && node->parameter_coverage &&
        !owner->final.nodes[formal].parameter_coverage[0]);
    uint64_t original=node->parameter_coverage[0];
    if (mode<2) CHECK(n!=formal);
    CHECK((original!=0)==(mode<2));
    CHECK(effect_invocation_coverage_verify(owner)==XR_XIR_OK);
    owner->final.nodes[formal].parameter_coverage[0]=1;
    invocation_parameter_expect_rejection(owner,mode,"formal-rank");
    owner->final.nodes[formal].parameter_coverage[0]=0;
    invocation_parameter_disabled_rejections(owner,mode);
    node->parameter_coverage[0]=original?0:1;
    invocation_parameter_expect_rejection(owner,mode,"actual-rank");
    node->parameter_coverage[0]=original;
    node->parameter_coverage[0]=UINT64_MAX;
    invocation_parameter_expect_rejection(owner,mode,"rank-overflow");
    node->parameter_coverage[0]=original;
    if (mode<2) {
        uint32_t head=owner->consumers[n];CHECK(head!=UINT32_MAX);
        owner->consumers[n]=UINT32_MAX;
        invocation_parameter_expect_rejection(owner,mode,"missing-incoming");
        owner->consumers[n]=head;
        uint64_t input=node->input[0];node->input[0]^=UINT64_C(1);
        invocation_parameter_expect_rejection(owner,mode,"changed-key");
        node->input[0]=input;
        const EffectInvocationEdge *edge=&owner->final.edges[head];
        uint32_t caller=edge->caller;uint64_t returned=owner->final.nodes[caller].return_coverage;
        /* The real caller returns I64, so coverage must come from its CALL
         * result rather than a fabricated caller-wide returned certificate. */
        CHECK(!returned && owner->final.nodes[caller].body==0);
        uint32_t factory=UINT32_MAX;
        for (uint32_t e=owner->final.nodes[caller].edge_head;e!=UINT32_MAX;e=owner->final.edges[e].next)
            if (owner->final.nodes[owner->final.edges[e].target].body==1) factory=owner->final.edges[e].target;
        CHECK(factory!=UINT32_MAX && owner->final.nodes[factory].return_coverage);
        returned=owner->final.nodes[factory].return_coverage;owner->final.nodes[factory].return_coverage=0;
        invocation_parameter_expect_rejection(owner,mode,"removed-return");
        owner->final.nodes[factory].return_coverage=returned;
    }
    CHECK(effect_invocation_coverage_verify(owner)==XR_XIR_OK);
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
    EffectInvocationWorkspace *owner=NULL;EffectInvocationResult *solution=NULL;
    EffectInvocationCertificate *certificate=NULL;
    if (status==XR_XIR_OK) status=invocation_core_storage(context,module,&effects);
    if (status==XR_XIR_OK) status=effect_graph_build(module,&effects,&graph,(XrXirCompileContext *)context);
    if (status==XR_XIR_OK) status=effect_parameters_derive(module,&effects,&graph,context);
    if (status==XR_XIR_OK) status=effect_invocation_bounds_new(context,&bounds);
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_invocation_bounds_capture(context,bounds,module->types,&module->functions[f],f,NULL);
    if (status==XR_XIR_OK) status=effect_invocations_receive(context,module,&effects,&graph,bounds,&owner);
    if (status==XR_XIR_OK) status=effect_invocations_solve(owner);
    if (status==XR_XIR_OK && oracle) invocation_parameter_rejections(owner,mode);
    if (status==XR_XIR_OK) status=effect_invocation_stage(owner,&solution);
    if (status==XR_XIR_OK) {
        EffectInvocationSealRequest request={&owner,&solution,NULL};
        status=effect_invocation_seal(context,&request,&certificate,NULL);
    }
    effect_invocation_result_free(solution);effect_invocation_free(owner);
    effect_invocation_bounds_free(bounds);effect_graph_free(&graph);invocation_core_storage_free(&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0,sizeof(fixture));
    if (status==XR_XIR_OK) {
        EffectInvocationWorkspace replay={0};
        status=effect_invocation_read_open(context,published,certificate->declared,certificate->final,&replay);
        replay.work=context;replay.effects=published;
        replay.declared=certificate->declared;
        EffectBodyRequest request=effect_body_owned_request(context,certificate->body);
        if (status==XR_XIR_OK) status=effect_invocation_coverage_verify_read(&replay,&request);
        effect_invocation_read_close(&replay);
    }
    if (status==XR_XIR_OK && oracle) {
        CHECK(xr_xir_effects_error_unknown(published,0)==(mode>=2));
        CHECK(xr_xir_effects_error_unknown(published,5));
        const EffectInvocationNode *formal=&certificate->final->nodes[certificate->final->roots[5]];
        CHECK(!formal->parameter_coverage[0]);
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
