/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_closed_error_resources.c - Owned error budgets and allocation failures
 *
 * KEY CONCEPT:
 *   Real execution and post-code consumer work share each Instance's Domain.
 */
#define main retained_closed_error_main
#include "test_xir_task_closed_error_baseline.c"
#undef main

static bool closed_error_inspect_task(XrXirValue *value,unsigned family,bool limits,bool failed) {
    const XirTask *task=(const XirTask *)(uintptr_t)value->payload;
    CHECK(xr_xir_task_storage_valid(task) && task->state==XIR_TASK_TERMINAL && !task->executor && !task->call);
    bool completed=true;
    for (unsigned copy=0;copy<2;++copy) {
        XrXirCallResult result={0};XrXirCallStatus status=xr_xir_task_copy_outcome(value,&result);
        if (status==XR_XIR_CALL_THROWN) {
            if (!closed_error_value(&result.value,family)) completed=false;
        } else {
            CHECK((limits && status==XR_XIR_CALL_LIMIT) || (failed &&
                (status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_CANCELLED)));
            CHECK(!result.value.type);completed=false;
        }
        CHECK(!result.wake && xr_xir_panic_empty(&result.panic));xr_xir_call_result_drop(&result);
    }
    CHECK(task->outcome.status!=XR_XIR_CALL_THROWN || xr_xir_value_valid(&task->outcome.value));
    xr_xir_value_drop(value);return completed;
}
static bool closed_error_operation(ClosedErrorFixture *fixture,unsigned family,bool consume_program,
    const XrXirInstanceConfig *limits,XrXirDomainBudgetStats *cost) {
    XrXirInstance *instances[2]={0};XrXirDomain *domains[2]={0};XrXirValue tasks[2][2]={{{0}}};
    bool completed=true;
    for (unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        if (limits) config=*limits;
        XrXirCallStatus status=xr_xir_instance_new(fixture->program,&config,&instances[i]);
        if (status!=XR_XIR_CALL_READY) { CHECK(status==(limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM));completed=false; }
        else { domains[i]=instances[i]->domain;CHECK(xr_xir_domain_retain(domains[i])); }
    }
    if (consume_program) { xr_xir_compile_program_drop(fixture->program);fixture->program=NULL; }
    for (unsigned i=0;i<2;++i) if (instances[i]) {
        for (unsigned mode=0;mode<2;++mode) {
            XrXirCallStatus status=xr_xir_instance_start(instances[i],fixture->entry[family][mode],NULL,0);
            if (status!=XR_XIR_CALL_READY) {
                CHECK(status==(limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM));completed=false;break;
            }
            unsigned polls=0;XrXirInstanceResult result={0};
            do {
                result=xr_xir_instance_poll_bounded(instances[i],1);CHECK(++polls<4096);
                if (!tasks[i][mode].type && instances[i]->executor->active) {
                    XrXirValue task=task_core_task_value(instances[i]->executor->active);
                    XrXirValueStatus copied=xr_xir_value_copy(&task,&tasks[i][mode]);
                    CHECK(copied==XR_XIR_VALUE_OK);
                }
            } while (result.outcome.status==XR_XIR_CALL_READY);
            if (result.outcome.status==(mode?XR_XIR_CALL_RETURNED:XR_XIR_CALL_THROWN)) {
                XrXirValue value={0};CHECK(xr_xir_instance_take_result(instances[i],&value)==result.outcome.status);
                CHECK(tasks[i][mode].type);
                if (mode) CHECK(value.type==tasks[i][mode].type && value.payload==tasks[i][mode].payload);
                else if (!closed_error_value(&value,family)) completed=false;
                xr_xir_value_drop(&value);
            } else {
                CHECK(result.outcome.status==(limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM));completed=false;break;
            }
        }
        XrXirCallStatus freed=xr_xir_instance_free(instances[i]);
        CHECK(freed==XR_XIR_CALL_READY || freed==(limits?XR_XIR_CALL_LIMIT:XR_XIR_CALL_OOM));
        if (freed!=XR_XIR_CALL_READY) completed=false;
    }
    if (consume_program) CHECK(fixture->code_releases==1 && !fixture->lowered);
    for (unsigned i=0;i<2;++i) for (unsigned mode=0;mode<2;++mode) if (tasks[i][mode].type)
        if (!closed_error_inspect_task(&tasks[i][mode],family,limits!=NULL,!completed)) completed=false;
    for (unsigned i=0;i<2;++i) if (domains[i]) {
        XrXirDomainBudgetStats spent=xr_xir_domain_budget_stats(domains[i]);
        CHECK(!spent.call_live && !spent.metadata_live && spent.call_allocations==spent.call_frees &&
            spent.metadata_allocations==spent.metadata_frees && xr_xir_domain_stats(domains[i]).live_bytes==sizeof(*domains[i]));
        if (cost) {
            if (!i) *cost=spent;
            else CHECK(spent.work==cost->work && spent.requested_bytes==cost->requested_bytes &&
                spent.requested_call_bytes==cost->requested_call_bytes);
        }
        xr_xir_domain_drop(domains[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);return completed;
}
static void closed_error_resource_axes(ClosedErrorFixture *fixture,unsigned family) {
    XrXirDomainBudgetStats cost={0};CHECK(closed_error_operation(fixture,family,false,NULL,&cost));
    const uint64_t exact[]={cost.requested_bytes,cost.requested_call_bytes,cost.work};
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(exact[axis]);
        if (!axis) config.requested_value_limit=exact[axis]-minus;
        else if (axis==1) config.requested_call_limit=exact[axis]-minus;
        else config.work_limit=exact[axis]-minus;
        CHECK(closed_error_operation(fixture,family,false,&config,NULL)==!minus);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==4);unsigned family=CLOSED_ERROR_CASES;
    for (unsigned i=0;i<CLOSED_ERROR_CASES;++i) if (!strcmp(argv[1],closed_error_names[i])) family=i;
    CHECK(family<CLOSED_ERROR_CASES);bool fi=!strcmp(argv[3],"fi");CHECK(fi || !strcmp(argv[3],"normal"));
    ClosedErrorFixture fixture={0};closed_error_build(&fixture,argv[2]);closed_error_resource_axes(&fixture,family);
    runtime_attempts=0;runtime_fail_at=SIZE_MAX;
    CHECK(closed_error_operation(&fixture,family,!fi,NULL,NULL));size_t sites=runtime_attempts;
    if (fi) for (size_t site=0;site<sites;++site) {
        runtime_attempts=0;runtime_fail_at=site;
        CHECK(!closed_error_operation(&fixture,family,false,NULL,NULL) && runtime_attempts>site);
    }
    runtime_fail_at=SIZE_MAX;if (fi) xr_xir_compile_program_drop(fixture.program);
    CHECK(fixture.code_releases==1 && !fixture.lowered && !runtime_live && !runtime_bytes);
    effects_source_owners_free();CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("TASK_ERROR_RESOURCES %s %s %s sites=%zu exactMinus1Axes3 twoInstances stickyCopies2 codeLease1 physical=0/0\n",
        argv[1],argv[2],argv[3],sites);return 0;
}
