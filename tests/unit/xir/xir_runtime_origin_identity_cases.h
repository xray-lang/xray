/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_runtime_origin_identity_cases.h - Complete physical identity on real equations
 *
 * KEY CONCEPT:
 *   A physical handle cannot change producer while closing a back edge.
 *   Scalar captures keep their canonical reserved fields as well.
 */
#ifndef XIR_RUNTIME_ORIGIN_IDENTITY_CASES_H
#define XIR_RUNTIME_ORIGIN_IDENTITY_CASES_H

typedef struct OriginIdentityFixture {
    CaptureWalkFixture base;XrXirFunction functions[7];XrXirFunctionIdentity identities[7];
    XrXirInstruction wrapper[2];
} OriginIdentityFixture;
static void origin_identity_fixture(OriginIdentityFixture *f) {
    memset(f,0,sizeof(*f));capture_walk_fixture(&f->base);
    memcpy(f->functions,f->base.functions,sizeof(f->base.functions));
    memcpy(f->wrapper,f->base.wrap,sizeof(f->wrapper));
    f->functions[6]=f->functions[1];f->functions[6].name="other_wrap";f->functions[6].name_length=10;
    f->functions[6].instructions=f->wrapper;f->base.entry[2].immediate=6;
    f->base.base.module.functions=f->functions;f->base.base.module.function_count=7;
    f->base.base.declarations.functions=f->identities;
}
typedef struct OriginIdentityProbe {
    CaptureWalkProbe budget;uint32_t mode,leaf,first,second;
} OriginIdentityProbe;
static XrXirStatus origin_identity_value(void *owner,uint32_t parameter,
    const XirEffectRuntimeValue *parent,uint32_t capture,XirEffectRuntimeValue *output) {
    OriginIdentityProbe *probe=owner;CHECK(!parameter);++probe->budget.reads;
    if(!parent) {
        CHECK(capture==UINT32_MAX);*output=(XirEffectRuntimeValue){256,0,probe->first,UINT32_MAX,1};
    } else {
        CHECK(!capture);
        if(parent->producer==probe->first && parent->payload==1)
            *output=(XirEffectRuntimeValue){256,0,probe->second,UINT32_MAX,probe->mode==2?1:2};
        else if(parent->producer==probe->second)
            *output=(XirEffectRuntimeValue){256,0,probe->first,UINT32_MAX,probe->mode==1?1:3};
        else {
            CHECK(parent->producer==probe->first && parent->payload==3);
            *output=(XirEffectRuntimeValue){256,0,probe->leaf,UINT32_MAX,4};
        }
    }
    return XR_XIR_OK;
}
static void origin_identity_cycle_cases(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    OriginIdentityFixture fixture;origin_identity_fixture(&fixture);XrXirArtifact *checked=NULL;
    XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&fixture.base.base.module,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"Origin identity Checked status%u f%u i%u\n",status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    XirEffectProducerView leaf={0},first={0},second={0};
    CHECK(xir_effects_producer(&context,effects,0,0,&leaf)==XR_XIR_OK && !leaf.capture_count);
    CHECK(xir_effects_producer(&context,effects,1,0,&first)==XR_XIR_OK && first.capture_count==1);
    CHECK(xir_effects_producer(&context,effects,6,0,&second)==XR_XIR_OK && second.capture_count==1 && first.site!=second.site);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xBD,sizeof(fixture));
    for(uint32_t mode=0;mode<3;++mode) {
        OriginIdentityProbe probe={.budget=capture_walk_probe(0),.mode=mode,.leaf=leaf.site,
            .first=first.site,.second=second.site};
        XirEffectInvocationRequest request={.caller=0,.instruction=4,.target=4,.producer=UINT32_MAX,.count=1,
            .read={.owner=&probe,.work=capture_walk_work,.cell=capture_walk_cell,.value=origin_identity_value,
                .allocate=capture_walk_allocate,.deallocate=capture_walk_deallocate}};
        XirEffectInvocationSelection selected;memset(&selected,0xA5,sizeof(selected));
        unsigned char saved[sizeof(selected)];memcpy(saved,&selected,sizeof(saved));
        status=xir_effects_execution_edge(effects,&request,&selected);
        fprintf(stderr,"ORIGIN_IDENTITY mode%u status%u reads%u live%llu\n",mode,status,probe.budget.reads,
            (unsigned long long)probe.budget.live);
        CHECK(status==(mode==2?XR_XIR_BAD_TYPE:XR_XIR_OK) && !probe.budget.live);
        if(mode==2)CHECK(!memcmp(&selected,saved,sizeof(selected)) && probe.budget.reads==2);
        else CHECK(selected.owner==effects && probe.budget.reads==(mode?3u:4u));
        if(mode!=2)for(uint32_t reject=0;reject<probe.budget.allocations;++reject) {
            CHECK(rp_fail_at==SIZE_MAX);RootParameterMark retained=rp_mark();size_t attempts=rp_attempts;
            bool prior_injected=rp_injected;rp_injected=false;rp_fail_at=attempts+reject;
            OriginIdentityProbe fault={.budget=capture_walk_probe(0),.mode=mode,.leaf=leaf.site,
                .first=first.site,.second=second.site};
            request.read.owner=&fault;memcpy(&selected,saved,sizeof(selected));
            XrXirStatus failed=xir_effects_execution_edge(effects,&request,&selected);rp_fail_at=SIZE_MAX;
            CHECK(failed==XR_XIR_OUT_OF_MEMORY && rp_injected && !fault.budget.live &&
                rp_attempts-attempts==reject+1 && !memcmp(&selected,saved,sizeof(selected)));
            rp_balanced(retained);uint64_t consumed=fault.budget.work,allocated=fault.budget.allocated;
            CHECK(xir_effects_execution_edge(effects,&request,&selected)==XR_XIR_OK &&
                selected.owner==effects && !fault.budget.live && fault.budget.work>consumed &&
                fault.budget.allocated>=allocated);rp_balanced(retained);rp_injected=prior_injected;
            printf("ORIGIN_SCRATCH_OOM mode%u ordinal%u suffix0 same_owner_retry1 physical0 PASS\n",mode,reject);
        }
        uint64_t measured[3]={probe.budget.work,probe.budget.allocated,probe.budget.peak};
        for(uint32_t axis=0;axis<(mode==2?1u:3u);++axis)for(uint32_t less=0;less<2;++less) {
            OriginIdentityProbe retry={.budget=capture_walk_probe(0),.mode=mode,.leaf=leaf.site,
                .first=first.site,.second=second.site};
            CHECK(measured[axis]);
            if(!axis)retry.budget.remaining=measured[axis]-less;
            else if(axis==1)retry.budget.allocated_limit=measured[axis]-less;
            else retry.budget.live_limit=measured[axis]-less;
            request.read.owner=&retry;memcpy(&selected,saved,sizeof(selected));
            XrXirStatus expected=less?XR_XIR_BUDGET:mode==2?XR_XIR_BAD_TYPE:XR_XIR_OK;
            CHECK(xir_effects_execution_edge(effects,&request,&selected)==expected && !retry.budget.live);
            if(expected!=XR_XIR_OK)CHECK(!memcmp(&selected,saved,sizeof(selected)));
            else CHECK(selected.owner==effects);
        }
    }
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
}

typedef struct OriginScalarFixture {
    InvocationReturnedFixture base;XrXirInstruction entry[4],body,sink[2];
    uint32_t operands[2];XrXirType scalar,function;XrXirBlock blocks[3];
    XrXirFunction functions[4];XrXirFunctionIdentity identities[4];
} OriginScalarFixture;
static void origin_scalar_fixture(OriginScalarFixture *f) {
    memset(f,0,sizeof(*f));invocation_returned_fixture(&f->base,0);
    f->scalar=XR_XIR_I64;f->function=(XrXirType)256;
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=17};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=f->function,.immediate=1,.args={0,1}};
    f->entry[2]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=2,.args={1,1}};
    f->entry[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2}};
    f->body=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->sink[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64};
    f->sink[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};f->operands[1]=1;
    for(uint32_t i=0;i<3;++i)f->blocks[i]=(XrXirBlock){.count=i==0?4u:i==1?1u:2u};
    f->functions[0]=(XrXirFunction){.name="entry",.name_length=5,.result=XR_XIR_I64,
        .instructions=f->entry,.instruction_count=4,.blocks=&f->blocks[0],.block_count=1,
        .operands=f->operands,.operand_count=2};
    f->functions[1]=(XrXirFunction){.name="body",.name_length=4,.result=XR_XIR_I64,
        .parameters=&f->scalar,.parameter_count=1,.instructions=&f->body,.instruction_count=1,
        .blocks=&f->blocks[1],.block_count=1};
    f->functions[2]=(XrXirFunction){.name="sink",.name_length=4,.result=XR_XIR_I64,
        .parameters=&f->function,.parameter_count=1,.instructions=f->sink,.instruction_count=2,
        .blocks=&f->blocks[2],.block_count=1};f->functions[3]=f->base.functions[4];
    f->base.module.functions=f->functions;f->base.module.function_count=4;
    f->base.source.initializer=3;f->base.declarations.functions=f->identities;
}
typedef struct OriginScalarProbe {CaptureWalkProbe budget;uint32_t site;bool reserved;} OriginScalarProbe;
static XrXirStatus origin_scalar_value(void *owner,uint32_t parameter,
    const XirEffectRuntimeValue *parent,uint32_t capture,XirEffectRuntimeValue *output) {
    OriginScalarProbe *probe=owner;CHECK(!parameter);++probe->budget.reads;
    if(!parent) {CHECK(capture==UINT32_MAX);*output=(XirEffectRuntimeValue){256,0,probe->site,UINT32_MAX,1};}
    else {CHECK(!capture && parent->producer==probe->site);*output=(XirEffectRuntimeValue){XR_XIR_I64,probe->reserved?1u:0u,UINT32_MAX,UINT32_MAX,17};}
    return XR_XIR_OK;
}
static void origin_scalar_cases(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;OriginScalarFixture fixture;origin_scalar_fixture(&fixture);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&fixture.base.module,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"Origin scalar Checked status%u f%u i%u\n",status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    XirEffectProducerView producer={0};CHECK(xir_effects_producer(&context,effects,0,1,&producer)==XR_XIR_OK && producer.capture_count==1);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xBD,sizeof(fixture));
    for(uint32_t mode=0;mode<2;++mode) {
        OriginScalarProbe probe={.budget=capture_walk_probe(0),.site=producer.site,.reserved=mode!=0};
        XirEffectInvocationRequest request={.caller=0,.instruction=2,.target=2,.producer=UINT32_MAX,.count=1,
            .read={.owner=&probe,.work=capture_walk_work,.cell=capture_walk_cell,.value=origin_scalar_value,
                .allocate=capture_walk_allocate,.deallocate=capture_walk_deallocate}};
        XirEffectInvocationSelection selected;memset(&selected,0xA5,sizeof(selected));unsigned char saved[sizeof(selected)];
        memcpy(saved,&selected,sizeof(saved));status=xir_effects_execution_edge(effects,&request,&selected);
        fprintf(stderr,"ORIGIN_SCALAR reserved%u status%u reads%u live%llu\n",mode,status,probe.budget.reads,(unsigned long long)probe.budget.live);
        CHECK(status==(mode?XR_XIR_BAD_TYPE:XR_XIR_OK) && probe.budget.reads==2 && !probe.budget.live);
        if(mode)CHECK(!memcmp(&selected,saved,sizeof(selected)));else CHECK(selected.owner==effects);
    }
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
}
static void runtime_origin_identity_cases(void) {
    origin_scalar_cases();origin_identity_cycle_cases();
    puts("ORIGIN_IDENTITY real two-producer equations, complete physical cycle identity, scalar reserved refusal, producer-dead, physical0 PASS");
}
#endif // XIR_RUNTIME_ORIGIN_IDENTITY_CASES_H
