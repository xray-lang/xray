/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_effects_flow_storage.c - Bounded scalar replay flags preserve ownership and rejection
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"Flow storage %d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir/xxir_effects.c"

static void flow_storage_case(uint32_t values, bool callable, bool fail) {
    XrCompileResourceLimits limits={65536,65536,1000000};
    XrXirCompileContext context={0}; context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0}; CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    EffectPhysicalRecord record={0,EFFECT_PHYSICAL_CALLABLE};
    EffectPhysicalFunction physical={.instructions=values,.values=values,.rows=callable?1u:0u,
        .count=callable?1u:0u,.capacity=callable?1u:0u,.records=callable?&record:NULL};
    EffectPhysicalDomain domain={context.resources,1,&physical};
    EffectInvocationNode node={0}; EffectInvocationOwner owner={.work=&context,.physical=&domain,.nodes=&node,.count=1};
    EffectBodyFunctionHead head={.instruction_count=values};
    EffectInvocationFlow flow={.owner=&owner,.function=&head,.basis={.fn_words=1}};
    bool local=values && values<=64 && !callable;
    instance_compile_injected=false; size_t attempts=instance_compile_attempts;
    if(fail) instance_compile_fail_at=attempts;
    XrXirStatus status=effect_invocation_flow_storage(&flow);
    if(fail && !local) CHECK(status==XR_XIR_OUT_OF_MEMORY && instance_compile_injected && !flow.memory);
    else {
        CHECK(status==XR_XIR_OK && !instance_compile_injected);
        CHECK(instance_compile_attempts-attempts==(local?0u:1u));
        CHECK(local?(!flow.memory && flow.fixed==flow.local_flags):flow.memory!=NULL);
        CHECK(effect_invocation_flags_match(&flow)==XR_XIR_OK);
        bool changed=false,read=false;
        CHECK(effect_invocation_flag_put(&flow,values-1,false,&changed)==XR_XIR_OK && changed);
        CHECK(effect_invocation_flag_get(&flow,values-1,false,&read)==XR_XIR_OK && read);
        CHECK(effect_invocation_flag_put(&flow,values-1,true,NULL)==XR_XIR_OK);
        CHECK(effect_invocation_flag_get(&flow,values-1,true,&read)==XR_XIR_OK && read);
        CHECK(effect_invocation_flag_put(&flow,values,false,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(effect_invocation_flags_match(&flow)==XR_XIR_OK);
        uint32_t tail=values%64;
        if(tail) {
            flow.certified[(values-1)/64]|=UINT64_C(1)<<tail;
            CHECK(effect_invocation_flags_match(&flow)==XR_XIR_BAD_STRUCTURE);
            flow.certified[(values-1)/64]&=~(UINT64_C(1)<<tail);
        }
        if(local) {
            uint64_t substitute[2]={flow.fixed[0],flow.certified[0]};
            flow.fixed=substitute;flow.certified=substitute+1;
            CHECK(effect_invocation_flags_match(&flow)==XR_XIR_BAD_STRUCTURE);
            flow.fixed=flow.local_flags;flow.certified=flow.local_flags+1;
            physical.rows=1;
            CHECK(effect_invocation_flags_match(&flow)==XR_XIR_BAD_STRUCTURE);
            physical.rows=0;
        }
        CHECK(effect_invocation_flags_match(&flow)==XR_XIR_OK);
    }
    instance_compile_fail_at=SIZE_MAX;
    xr_compile_resources_free(flow.memory);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes);
    xr_compile_resources_release(context.resources);instance_compile_zero();
    printf("Flow storage values=%u callable=%u fail=%u local=%u typedStatus=%u tailAndAliasRejected compilerPhysical0/0\n",
        values,(unsigned)callable,(unsigned)fail,(unsigned)local,(unsigned)status);
}
static void flow_storage_budget(void) {
    XrCompileResourceLimits limits={65536,65536,1};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    EffectPhysicalFunction physical={.instructions=64,.values=64};
    EffectPhysicalDomain domain={context.resources,1,&physical};EffectInvocationNode node={0};
    EffectInvocationOwner owner={.work=&context,.physical=&domain,.nodes=&node,.count=1};
    EffectBodyFunctionHead head={.instruction_count=64};
    EffectInvocationFlow flow={.owner=&owner,.function=&head,.basis={.fn_words=1}};
    size_t attempts=instance_compile_attempts;
    CHECK(effect_invocation_flow_storage(&flow)==XR_XIR_BUDGET && !flow.memory && !flow.fixed && !flow.certified);
    CHECK(attempts==instance_compile_attempts);
    xr_compile_resources_release(context.resources);instance_compile_zero();
    puts("Flow storage inlineWorkExhaustion typedBudget noAllocation physical0/0");
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--fi")));
    bool fail=argc==2;
    const uint32_t sizes[]={1,63,64,65};
    for(unsigned i=0;i<4;++i) flow_storage_case(sizes[i],false,fail);
    flow_storage_case(1,true,fail);
    flow_storage_budget();return 0;
}
