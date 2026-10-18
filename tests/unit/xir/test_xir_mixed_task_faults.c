/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_faults.c - Actual mixed callbacks preserve allocation faults
 *
 * KEY CONCEPT:
 *   Observe canonical bodies without running fallible success-only probes.
 */
#include "xir/xxir_call.h"
static XrXirAction fault_resume(XrXirCallView *view);
static void fault_release(XrXirCallView *view,XrXirCallStatus status);
#define CM_RESUME_OBSERVER fault_resume
#define CM_RELEASE_OBSERVER fault_release
#define main mixed_reference_main
#include "test_xir_child_callable_mixed.c"
#undef main
static unsigned fault_steps[CN_FUNCTIONS];
static XrXirAction fault_resume(XrXirCallView *view) {
    uint32_t function=cm_function(view);++fault_steps[function];
    return cm_observed->selected[function]->resume(view);
}
static void fault_release(XrXirCallView *view,XrXirCallStatus status) {
    uint32_t function=cm_function(view);
    cm_observed->selected[function]->release(view,status);
}
static XrXirOutputStatus fault_output(void *owner,const XrXirOutputGroup *group) {
    unsigned *outputs=owner;
    CHECK(group&&group->stream==XR_XIR_STDOUT&&group->line&&group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64&&group->values[0].payload==42);
    CHECK(!(*outputs)++);return XR_XIR_OUTPUT_OK;
}
static bool fault_operation(CmFixture *fixture,bool consume_program,const XrXirInstanceConfig *limits,
    XrXirDomainBudgetStats *cost) {
    XrXirInstance *instances[2]={0};unsigned outputs[2]={0};bool completed=true;
    XrXirDomain *domains[2]={0};
    memset(fault_steps,0,sizeof(fault_steps));
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        if(limits)config=*limits;
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,fault_output,&outputs[i]};
        XrXirCallStatus status=xr_xir_instance_new(fixture->base.program,&config,&instances[i]);
        if(status!=XR_XIR_CALL_READY){CHECK(status==(limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM));completed=false;}
        else{domains[i]=instances[i]->domain;CHECK(xr_xir_domain_retain(domains[i]));}
    }
    if(consume_program){xr_xir_compile_program_drop(fixture->base.program);fixture->base.program=NULL;}
    for(unsigned i=0;i<2;++i)if(instances[i]) {
        XrXirCallStatus status=xr_xir_instance_start(instances[i],CN_ROOT,NULL,0);
        if(status==XR_XIR_CALL_READY) {
            XrXirInstanceResult result={0};unsigned polls=0;
            do {result=xr_xir_instance_poll_bounded(instances[i],1);CHECK(++polls<32768);}while(result.outcome.status==XR_XIR_CALL_READY);
            if(result.outcome.status==XR_XIR_CALL_RETURNED) {
                CHECK(result.outcome.value.type==XR_XIR_I64&&result.outcome.value.payload==42&&outputs[i]==1);
                XrXirValue value={0};CHECK(xr_xir_instance_take_result(instances[i],&value)==XR_XIR_CALL_RETURNED);
                CHECK(value.type==XR_XIR_I64&&value.payload==42);xr_xir_value_drop(&value);
            } else {CHECK(result.outcome.status==(limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM));completed=false;}
        } else {CHECK(status==(limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM));completed=false;}
        status=xr_xir_instance_free(instances[i]);
        CHECK(status==XR_XIR_CALL_READY||status==(limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM));
        if(status!=XR_XIR_CALL_READY)completed=false;
        XrXirDomainBudgetStats spent=xr_xir_domain_budget_stats(domains[i]);
        CHECK(!spent.call_live&&!spent.metadata_live&&spent.call_allocations==spent.call_frees&&
            spent.metadata_allocations==spent.metadata_frees&&xr_xir_domain_stats(domains[i]).live_bytes==sizeof(*domains[i]));
        if(cost) {
            if(!i)*cost=spent;
            else CHECK(spent.work==cost->work&&spent.requested_bytes==cost->requested_bytes&&
                spent.requested_call_bytes==cost->requested_call_bytes);
        }
        xr_xir_domain_drop(domains[i]);
    }
    if(completed)CHECK(fault_steps[CN_ROOT]==6&&fault_steps[CN_CHILD]==10&&fault_steps[CN_RELAY]==4&&fault_steps[CN_PURE]==4);
    CHECK(!runtime_live&&!runtime_bytes);return completed;
}
static void fault_resource_axes(CmFixture *fixture) {
    XrXirDomainBudgetStats cost={0};CHECK(fault_operation(fixture,false,NULL,&cost));
    const uint64_t exact[]={cost.requested_bytes,cost.requested_call_bytes,cost.work};
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(exact[axis]);
        if(!axis)config.requested_value_limit=exact[axis]-minus;
        else if(axis==1)config.requested_call_limit=exact[axis]-minus;
        else config.work_limit=exact[axis]-minus;
        CHECK(fault_operation(fixture,false,&config,NULL)==!minus);
    }
    printf("MIXED_TASK_AXES value/call/work=%llu/%llu/%llu exact/minus1 twoDomains physical=0/0\n",
        (unsigned long long)exact[0],(unsigned long long)exact[1],(unsigned long long)exact[2]);
}
int main(int argc,char **argv) {
    /* The companion normal oracle owns the fallible success-only probes. */
    (void)&cm_resume;(void)&cm_release;
    CHECK(argc==3);bool root_native=!strcmp(argv[1],"native-vm"),fi=!strcmp(argv[2],"fi");
    CHECK(root_native||!strcmp(argv[1],"vm-native"));CHECK(fi||!strcmp(argv[2],"normal"));
    CmFixture fixture={0};cm_build(&fixture,root_native);
    fault_resource_axes(&fixture);
    runtime_attempts=0;runtime_fail_at=SIZE_MAX;CHECK(fault_operation(&fixture,!fi,NULL,NULL));size_t sites=runtime_attempts;
    if(fi)for(size_t site=0;site<sites;++site) {
        runtime_attempts=0;runtime_fail_at=site;CHECK(!fault_operation(&fixture,false,NULL,NULL)&&runtime_attempts>site);
    }
    runtime_fail_at=SIZE_MAX;if(fi)xr_xir_compile_program_drop(fixture.base.program);
    CHECK(fixture.base.code_releases==1&&!fixture.lowered&&!runtime_live&&!runtime_bytes);
    cm_observed=NULL;cn_observed=NULL;effects_source_owners_free();CHECK(!effects_compile_live&&!effects_compile_bytes);
    printf("MIXED_TASK_FAULTS %s %s sites=%zu twoInstances typed42 callbacks=C11-and-VM codeReleases=1 physical=0/0\n",argv[1],argv[2],sites);return 0;
}
