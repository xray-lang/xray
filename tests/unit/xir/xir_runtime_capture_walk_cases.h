/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_runtime_capture_walk_cases.h - Real equations and complete physical capture reads
 */
#ifndef XIR_RUNTIME_CAPTURE_WALK_CASES_H
#define XIR_RUNTIME_CAPTURE_WALK_CASES_H

typedef struct CaptureWalkFixture {
    InvocationReturnedFixture base;XrXirType function;
    XrXirInstruction entry[6],wrap[2],body[2],sink[2];uint32_t operands[4],capture;
    XrXirBlock blocks[4];XrXirFunction functions[6];XrXirFunctionIdentity identities[6];
} CaptureWalkFixture;
static void capture_walk_fixture(CaptureWalkFixture *f) {
    memset(f,0,sizeof(*f));invocation_returned_fixture(&f->base,0);f->function=(XrXirType)256;
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=f->function,.immediate=2};
    for(uint32_t n=1;n<4;++n)f->entry[n]=(XrXirInstruction){.op=XR_XIR_CALL,.type=f->function,
        .immediate=1,.args={n-1,1}};
    f->entry[4]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=4,.args={3,1}};
    f->entry[5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={4}};
    f->wrap[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=f->function,.immediate=3,.args={0,1}};
    f->wrap[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    f->body[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64};
    f->body[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};memcpy(f->sink,f->body,sizeof(f->sink));
    for(uint32_t n=0;n<4;++n){f->operands[n]=n;f->blocks[n]=(XrXirBlock){.count=n?2:6};}
    memcpy(f->functions,f->base.functions,sizeof(f->base.functions));
    f->functions[0].instructions=f->entry;f->functions[0].instruction_count=6;f->functions[0].blocks=&f->blocks[0];
    f->functions[0].operands=f->operands;f->functions[0].operand_count=4;
    f->functions[1].parameters=&f->function;f->functions[1].parameter_count=1;
    f->functions[1].instructions=f->wrap;f->functions[1].blocks=&f->blocks[1];f->functions[1].instruction_count=2;
    f->functions[1].operands=&f->capture;f->functions[1].operand_count=1;
    f->functions[3]=(XrXirFunction){.name="body",.name_length=4,.result=XR_XIR_I64,
        .parameters=&f->function,.parameter_count=1,.blocks=&f->blocks[2],.block_count=1,
        .instructions=f->body,.instruction_count=2};
    f->functions[4]=f->functions[3];f->functions[4].name="sink";f->functions[4].instructions=f->sink;
    f->functions[4].blocks=&f->blocks[3];f->functions[5]=f->base.functions[4];
    f->base.source.initializer=5;f->base.module.functions=f->functions;f->base.module.function_count=6;
    f->base.declarations.functions=f->identities;
}
typedef struct CaptureWalkProbe {
    uint64_t remaining,work,allocated,live,peak,allocated_limit,live_limit;
    uint32_t calls,reject,reads,read_reject,allocations,allocation_reject,depth,leaf,wrapper,attack;
    bool cycle;
} CaptureWalkProbe;
static CaptureWalkProbe capture_walk_probe(uint32_t depth) {
    CaptureWalkProbe probe={0};probe.remaining=1000000;probe.allocated_limit=probe.live_limit=1000000;
    probe.reject=probe.read_reject=probe.allocation_reject=UINT32_MAX;probe.depth=depth;probe.leaf=0;probe.wrapper=1;
    return probe;
}
static bool capture_walk_work(void *owner,uint64_t units) {
    CaptureWalkProbe *probe=owner;uint32_t call=probe->calls++;
    if(call==probe->reject || units>probe->remaining)return false;
    probe->remaining-=units;probe->work+=units;return true;
}
static void *capture_walk_allocate(void *owner,size_t bytes,XrXirStatus *status) {
    CaptureWalkProbe *probe=owner;uint32_t ordinal=probe->allocations++;*status=XR_XIR_BUDGET;
    if(bytes>probe->allocated_limit-probe->allocated || bytes>probe->live_limit-probe->live)return NULL;
    if(ordinal==probe->allocation_reject){*status=XR_XIR_OUT_OF_MEMORY;return NULL;}
    void *memory=rp_malloc(bytes);if(!memory){*status=XR_XIR_OUT_OF_MEMORY;return NULL;}
    probe->allocated+=bytes;probe->live+=bytes;if(probe->peak<probe->live)probe->peak=probe->live;
    *status=XR_XIR_OK;return memory;
}
static void capture_walk_deallocate(void *owner,void *memory,size_t bytes) {
    CaptureWalkProbe *probe=owner;CHECK(bytes<=probe->live);probe->live-=bytes;rp_free(memory);
}
static uint32_t capture_walk_cell(void *owner,uint32_t p){(void)owner;(void)p;return 0;}
static XrXirStatus capture_walk_value(void *owner,uint32_t p,const XirEffectRuntimeValue *parent,
    uint32_t capture,XirEffectRuntimeValue *output) {
    CaptureWalkProbe *probe=owner;CHECK(!p);uint32_t read=probe->reads++;
    if(read==probe->read_reject)return XR_XIR_BAD_TYPE;
    if(!parent){CHECK(capture==UINT32_MAX);*output=(XirEffectRuntimeValue){256,0,probe->wrapper,UINT32_MAX,1};return XR_XIR_OK;}
    CHECK(capture==0 && parent->producer==probe->wrapper && parent->payload>0 && (uint64_t)parent->payload<=probe->depth);
    bool last=(uint64_t)parent->payload==probe->depth;
    uint32_t site=last && !probe->cycle?probe->leaf:probe->wrapper;
    int64_t payload=last && probe->cycle?1:parent->payload+1;
    *output=(XirEffectRuntimeValue){256,0,site,UINT32_MAX,payload};
    if(last && probe->attack==1)output->type=XR_XIR_I64;
    if(last && probe->attack==2)output->producer=UINT32_MAX;
    if(last && probe->attack==3)output->publication=1;
    if(last && probe->attack==4)output->publication=0;
    if(last && probe->attack==5)output->reserved=1;
    return XR_XIR_OK;
}
static void capture_walk_query_cases(const XrXirEffects *effects,const XirEffectInvocationRequest *request,
    CaptureWalkProbe *probe,const XirEffectInvocationSelection *selected) {
    uint32_t calls=probe->calls,allocations=probe->allocations;uint64_t work=probe->work,allocated=probe->allocated,peak=probe->peak;
    CHECK(!probe->live && calls && allocations);
    for(uint32_t reject=0;reject<calls;++reject) {
        *probe=capture_walk_probe(17);probe->reject=reject;XirEffectInvocationSelection untouched=*selected;
        CHECK(xir_effects_execution_edge(effects,request,&untouched)==XR_XIR_BUDGET &&
            !memcmp(&untouched,selected,sizeof(untouched)) && !probe->live && probe->calls==reject+1);
    }
    for(uint32_t reject=0;reject<18;++reject) {
        *probe=capture_walk_probe(17);probe->read_reject=reject;XirEffectInvocationSelection untouched=*selected;
        CHECK(xir_effects_execution_edge(effects,request,&untouched)==XR_XIR_BAD_TYPE &&
            !memcmp(&untouched,selected,sizeof(untouched)) && !probe->live && probe->reads==reject+1);
    }
    for(uint32_t reject=0;reject<allocations;++reject) {
        *probe=capture_walk_probe(17);probe->allocation_reject=reject;XirEffectInvocationSelection untouched=*selected;
        CHECK(xir_effects_execution_edge(effects,request,&untouched)==XR_XIR_OUT_OF_MEMORY &&
            !memcmp(&untouched,selected,sizeof(untouched)) && !probe->live);
        probe->allocation_reject=UINT32_MAX;
        CHECK(xir_effects_execution_edge(effects,request,&untouched)==XR_XIR_OK && untouched.node==selected->node && !probe->live);
    }
    for(uint32_t axis=0;axis<3;++axis)for(uint32_t less=0;less<2;++less) {
        *probe=capture_walk_probe(17);if(!axis)probe->remaining=work-less;
        else if(axis==1)probe->allocated_limit=allocated-less;else probe->live_limit=peak-less;
        XirEffectInvocationSelection untouched=*selected;
        CHECK(xir_effects_execution_edge(effects,request,&untouched)==(less?XR_XIR_BUDGET:XR_XIR_OK) && !probe->live);
        CHECK(less?!memcmp(&untouched,selected,sizeof(untouched)):untouched.node==selected->node);
    }
    for(uint32_t attack=0;attack<5;++attack) {
        *probe=capture_walk_probe(257);probe->attack=attack;XirEffectInvocationSelection untouched=*selected;
        CHECK(xir_effects_execution_edge(effects,request,&untouched)==(attack?XR_XIR_BAD_TYPE:XR_XIR_OK) && !probe->live);
        CHECK(probe->reads==258 && (attack?!memcmp(&untouched,selected,sizeof(untouched)):untouched.node==selected->node));
    }
    *probe=capture_walk_probe(257);probe->attack=5;XirEffectInvocationSelection reserved=*selected;
    XrXirStatus reserved_status=xir_effects_execution_edge(effects,request,&reserved);
    fprintf(stderr,"CAPTURE_RESERVED status%u reads%u physical_live%llu\n",reserved_status,probe->reads,(unsigned long long)probe->live);
    CHECK(reserved_status==XR_XIR_BAD_TYPE && !memcmp(&reserved,selected,sizeof(reserved)) && !probe->live);
    *probe=capture_walk_probe(17);probe->cycle=true;XirEffectInvocationSelection cycle={0};
    CHECK(xir_effects_execution_edge(effects,request,&cycle)==XR_XIR_OK && cycle.node==selected->node && probe->reads==18 && !probe->live);
    printf("CAPTURE_WALK real solver repeated-site relation, distinct257 handles, cycle identity, %u work/18 read refusals, %u OOM/same-owner retries, threeaxes, producer-dead, physical0 PASS\n",calls,allocations);
}
static void runtime_capture_walk_cases(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    CaptureWalkFixture fixture;capture_walk_fixture(&fixture);XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xir_fixture_check(&context,&fixture.base.module,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"Capture walk Checked status%u f%u i%u\n",status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    XirEffectProducerView leaf={0},wrapper={0};
    CHECK(xir_effects_producer(&context,effects,0,0,&leaf)==XR_XIR_OK && !leaf.capture_count && leaf.site==0);
    CHECK(xir_effects_producer(&context,effects,1,0,&wrapper)==XR_XIR_OK && wrapper.capture_count==1 && wrapper.site==1);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xBD,sizeof(fixture));
    CaptureWalkProbe probe=capture_walk_probe(17);
    XirEffectInvocationRequest request={.caller=0,.instruction=4,.target=4,.producer=UINT32_MAX,.count=1,
        .read={.owner=&probe,.work=capture_walk_work,.cell=capture_walk_cell,.value=capture_walk_value,
            .allocate=capture_walk_allocate,.deallocate=capture_walk_deallocate}};
    XirEffectInvocationSelection selected={0};XrCompileResourceStats before=rp_stats(&context);
    status=xir_effects_execution_edge(effects,&request,&selected);
    if(status!=XR_XIR_OK)fprintf(stderr,"Capture walk query status%u\n",status);
    CHECK(status==XR_XIR_OK && selected.owner==effects && !probe.live && probe.reads==18);
    capture_walk_query_cases(effects,&request,&probe,&selected);XrCompileResourceStats after=rp_stats(&context);
    CHECK(before.work==after.work && before.allocation_count==after.allocation_count && before.live_bytes==after.live_bytes);
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(!rp_live && !rp_live_bytes);
}
#include "xir_runtime_origin_identity_cases.h"
#endif // XIR_RUNTIME_CAPTURE_WALK_CASES_H
