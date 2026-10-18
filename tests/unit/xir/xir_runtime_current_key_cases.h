/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_runtime_current_key_cases.h - Genuine nonempty historical keys and exact current selection
 */
#ifndef XIR_RUNTIME_CURRENT_KEY_CASES_H
#define XIR_RUNTIME_CURRENT_KEY_CASES_H

typedef struct CurrentKeyFixture {
    InvocationDeferredFixture base;
    XrXirInstruction main[9],factory[2];
    XrXirBlock main_block,factory_block;
    XrXirFunction functions[5];XrXirFunctionIdentity identities[5];
} CurrentKeyFixture;
static void current_key_fixture(CurrentKeyFixture *f) {
    memset(f,0,sizeof(*f));invocation_deferred_fixture(&f->base,0);
    f->base.ref=(XrXirCallableParameter){XR_XIR_I64,XR_PARAM_READ};
    f->base.nodes[1].flags=XR_XIR_CALLABLE_ROOT_NONE;
    f->base.parameters[1]=XR_XIR_I64;f->base.cell=XR_XIR_I64;
    f->base.read[0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64};
    f->main[0]=f->base.entry[0];f->main[1]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64};
    f->main[2]=f->base.entry[2];
    f->main[3]=(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)257,.args={2,0}};
    f->main[4]=(XrXirInstruction){.op=XR_XIR_CALL,.type=(XrXirType)257,.immediate=4};
    f->main[5]=(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={3,4}};
    f->main[6]=(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=(XrXirType)257,.args={3,0}};
    f->main[7]=f->base.entry[3];f->main[8]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={7,0}};
    f->base.actuals[0]=6;f->main_block=(XrXirBlock){.count=9};
    f->factory[0]=f->base.entry[2];f->factory[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->factory_block=(XrXirBlock){.count=2};
    memcpy(f->functions,f->base.functions,sizeof(f->base.functions));
    memcpy(f->identities,f->base.identities,sizeof(f->base.identities));
    f->functions[0].instructions=f->main;f->functions[0].instruction_count=9;f->functions[0].blocks=&f->main_block;
    f->functions[4]=(XrXirFunction){.name="factory",.name_length=7,.result=(XrXirType)257,
        .blocks=&f->factory_block,.block_count=1,.instructions=f->factory,.instruction_count=2};
    f->base.module.functions=f->functions;f->base.module.function_count=5;f->base.declarations.functions=f->identities;
}
typedef struct CurrentKeyProbe {
    uint64_t remaining,work;uint32_t calls,reject,producer;
} CurrentKeyProbe;
static bool current_key_work(void *owner,uint64_t units) {
    CurrentKeyProbe *probe=owner;uint32_t call=probe->calls++;
    if(call==probe->reject || units>probe->remaining) return false;
    probe->remaining-=units;probe->work+=units;return true;
}
static uint32_t current_key_cell(void *owner,uint32_t p) {(void)owner;(void)p;return 0;}
static XrXirStatus current_key_value(void *owner,uint32_t p,const XirEffectRuntimeValue *parent,
    uint32_t capture,XirEffectRuntimeValue *output) {
    CHECK(!p && !parent && capture==UINT32_MAX);
    uint32_t producer=((CurrentKeyProbe *)owner)->producer;
    *output=(XirEffectRuntimeValue){257,0,producer,UINT32_MAX,(int64_t)producer+1};return XR_XIR_OK;
}
static void runtime_current_key_case(uint32_t less) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;CurrentKeyFixture fixture;current_key_fixture(&fixture);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.base.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    XirEffectProducerView first={0},returned={0};
    CHECK(xir_effects_producer(&context,effects,0,2,&first)==XR_XIR_OK && first.site==0 && !first.capture_count);
    CHECK(xir_effects_producer(&context,effects,4,0,&returned)==XR_XIR_OK && returned.site==1 && !returned.capture_count);
    EffectInvocationCertificate *certificate=effects->invocations;EffectInvocationFinal *owner=certificate->final;
    uint32_t parent=owner->roots[0],current=UINT32_MAX,old=UINT32_MAX,old_edge=UINT32_MAX;
    CHECK(owner->site_count==2 && !owner->binding_count && owner->nodes[parent].root==0);
    for(uint32_t e=owner->nodes[parent].edge_head;e!=UINT32_MAX;e=owner->edges[e].next) {
        const EffectInvocationEdge *edge=&owner->edges[e];const EffectInvocationNode *node=&owner->nodes[edge->target];
        if(edge->instruction!=7 || node->body!=1) continue;
        CHECK(node->parameter_count==2 && node->input && !node->input[1] && !node->input[2] && !node->input[3]);
        if(node->input[0]==UINT64_C(7)) current=edge->target;
        if(node->input[0]==UINT64_C(5)) {old=edge->target;old_edge=e;}
    }
    CHECK(current!=UINT32_MAX && old!=UINT32_MAX && current!=old && old_edge!=UINT32_MAX);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    CurrentKeyProbe probe={1000000,0,0,UINT32_MAX,first.site};
    XirEffectInvocationRequest request={.caller=0,.instruction=7,.target=1,.producer=UINT32_MAX,.count=2,
        .read={.owner=&probe,.work=current_key_work,.cell=current_key_cell,.value=current_key_value}};
    XirEffectInvocationSelection selected={0};size_t attempts=rp_attempts;XrCompileResourceStats before=rp_stats(&context);
    CHECK(xir_effects_execution_edge(effects,&request,&selected)==XR_XIR_OK && selected.node==current && selected.node!=old);
    uint64_t required=probe.work;uint32_t calls=probe.calls;CHECK(required && calls);
    for(uint32_t reject=0;reject<calls;++reject) {
        probe=(CurrentKeyProbe){1000000,0,0,reject,first.site};XirEffectInvocationSelection untouched=selected;
        CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_BUDGET &&
            !memcmp(&untouched,&selected,sizeof(selected)) && probe.calls==reject+1);
    }
    for(uint32_t actual=0;actual<2;++actual) {
        probe=(CurrentKeyProbe){1000000,0,0,UINT32_MAX,actual};XirEffectInvocationSelection again={0};
        CHECK(xir_effects_execution_edge(effects,&request,&again)==XR_XIR_OK && again.node==current);
    }
    uint64_t *input=owner->nodes[current].input;uint64_t words[4];memcpy(words,input,sizeof(words));
    for(uint32_t attack=0;attack<7;++attack) {
        if(attack==0) input[0]=5;
        if(attack==1) input[0]=1;
        if(attack==2) input[0]|=UINT64_C(8);
        if(attack==3) input[0]|=UINT64_C(1)<<63;
        if(attack==4) input[1]=1;
        if(attack==5) input[2]=1;
        if(attack==6) input[3]=1;
        probe=(CurrentKeyProbe){1000000,0,0,UINT32_MAX,first.site};XirEffectInvocationSelection untouched=selected;
        CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_BAD_TYPE &&
            !memcmp(&untouched,&selected,sizeof(selected)));
        memcpy(input,words,sizeof(words));probe=(CurrentKeyProbe){1000000,0,0,UINT32_MAX,first.site};
        CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_OK && untouched.node==current);
    }
    uint32_t old_target=owner->edges[old_edge].target;owner->edges[old_edge].target=current;
    probe=(CurrentKeyProbe){1000000,0,0,UINT32_MAX,first.site};XirEffectInvocationSelection untouched=selected;
    CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&untouched,&selected,sizeof(selected)));owner->edges[old_edge].target=old_target;
    probe=(CurrentKeyProbe){1000000,0,0,UINT32_MAX,owner->site_count};untouched=selected;
    CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_BAD_TYPE && !memcmp(&untouched,&selected,sizeof(selected)));
    probe=(CurrentKeyProbe){required-less,0,0,UINT32_MAX,first.site};untouched=selected;
    CHECK(xir_effects_execution_edge(effects,&request,&untouched)==(less?XR_XIR_BUDGET:XR_XIR_OK));
    CHECK(less?!memcmp(&untouched,&selected,sizeof(selected)):untouched.node==current);
    CHECK(rp_attempts==attempts && rp_stats(&context).allocated_bytes==before.allocated_bytes &&
        rp_stats(&context).live_bytes==before.live_bytes && rp_stats(&context).work==before.work);
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes);
    printf("CURRENT_KEY real old5/current7, two actual producers, seven complete-row attacks, unique current edge, %u refusals, exact/minus1, producer-dead, no allocations, physical0 PASS\n",calls);
}
static void runtime_current_original_cases(void) {
    for(uint32_t which=0;which<10;++which) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        XrXirStatus status=which<2?conditional_invocation_runtime_edge(&context,which,true):
            which<5?conditional_invocation_runtime_direct(&context,which-2,true):
            which<7?conditional_invocation_captured_prefix(&context,which-5,true):
            which<9?conditional_invocation_function_cell_row(&context,which-7,true):
            conditional_invocation_reference_result(&context,true);
        if(status!=XR_XIR_OK) fprintf(stderr,"Original runtime fixture%u status%u\n",which,(unsigned)status);
        CHECK(status==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);
        CHECK(!rp_live && !rp_live_bytes);
    }
}
static void runtime_current_key_cases(void) {
    runtime_current_key_case(0);runtime_current_key_case(1);runtime_current_original_cases();
}
#endif // XIR_RUNTIME_CURRENT_KEY_CASES_H
