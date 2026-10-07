/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_capacity_budget.h - Source value-ledger limits and charged OOM retry
 *
 * KEY CONCEPT:
 *   Each Source executor measures its own limits on a fresh Instance.
 *   A retained Domain proves that failure and release never refill its ledger.
 */
#ifndef XIR_ARRAY_CAPACITY_BUDGET_H
#define XIR_ARRAY_CAPACITY_BUDGET_H
typedef struct CapacityBudgetCase {
    XrXirInstance *first, *second;
    XrXirDomain *domain;
    XrXirValue alias;
    int64_t old_cap;
    uint32_t state_slot;
    XrXirDomainBudgetStats before;
    XrXirDomainStats physical;
} CapacityBudgetCase;
typedef struct CapacityBudgetMeasure {
    XrXirDomainBudgetStats before, after;
    XrXirDomainStats physical_before, physical_after;
    size_t sites, publication_site;
} CapacityBudgetMeasure;
enum { CAPACITY_BUDGET_REQUESTED, CAPACITY_BUDGET_LIVE, CAPACITY_BUDGET_WORK };

static CapacityBudgetCase capacity_budget_open(CapacityCompile *run) {
    CapacityBudgetCase test={0};test.first=capacity_open(run);test.second=capacity_open(run);
    test.alias=capacity_run(test.first,capacity_find(run->module,"saved"));
    test.old_cap=capacity_array_state(&test.alias);test.domain=test.first->domain;
    CHECK(object_pointer(&test.alias)->domain==test.domain&&test.second->domain!=test.domain);
    CHECK(xr_xir_domain_retain(test.domain));test.state_slot=UINT32_MAX;
    const XrXirDeclarations *decl=run->program->declarations;
    for(uint32_t slot=0;slot<decl->slot_count;++slot){
        if(decl->slots[slot].module==decl->root_module&&test.first->slots[slot].type==test.alias.type&&
            test.first->slots[slot].payload==test.alias.payload){
            CHECK(test.state_slot==UINT32_MAX);test.state_slot=slot;
        }
    }
    CHECK(test.state_slot!=UINT32_MAX&&test.first->state==XR_XIR_INSTANCE_READY);
    CHECK(test.second->state==XR_XIR_INSTANCE_READY);
    test.before=xr_xir_domain_budget_stats(test.domain);test.physical=xr_xir_domain_stats(test.domain);
    CHECK(test.before.bound);capacity_watch(test.first);runtime_attempts=0;return test;
}
/* Borrowing initialized storage is an observer only; it grants no execution. */
static void capacity_budget_state(const CapacityBudgetCase *test,unsigned publications) {
    CHECK(publications<=1&&test->first->domain==test->domain);capacity_watched=NULL;
    CHECK(capacity_array_state(&test->alias)==test->old_cap);
    const XrXirValue *current=&test->first->slots[test->state_slot];
    CHECK(current->type==test->alias.type&&xr_xir_value_valid(current));
    int64_t cap=capacity_array_state(current);
    if(publications)CHECK(cap>=13);
    else CHECK(cap==test->old_cap&&current->payload==test->alias.payload);
    const XrXirValue *other=&test->second->slots[test->state_slot];
    CHECK(other->type==test->alias.type&&capacity_array_state(other)==test->old_cap);
}
static void capacity_budget_failed_output(XrXirInstance *instance,bool exhausted) {
    CHECK(instance->budget.exhausted==exhausted&&instance->state==XR_XIR_INSTANCE_READY);
    CHECK(xr_xir_task_executor_completion_status(instance->executor)==
        (exhausted?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));
    XrXirValue value={0},before=value;
    CHECK(xr_xir_instance_take_result(instance,&value)==
        (exhausted?XR_XIR_CALL_LIMIT:XR_XIR_CALL_BAD_STATE));
    CHECK(!memcmp(&value,&before,sizeof(value)));
    value=(XrXirValue){XR_XIR_I64,0,77};before=value;
    CHECK(xr_xir_instance_take_result(instance,&value)==
        (exhausted?XR_XIR_CALL_LIMIT:XR_XIR_CALL_BAD_ARGUMENT));
    CHECK(!memcmp(&value,&before,sizeof(value)));
    XrXirCallResult failure={0},original=failure;
    CHECK(xr_xir_instance_copy_failure(instance,&failure)==XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&failure,&original,sizeof(failure)));
}
static void capacity_budget_close(CapacityBudgetCase *test,bool exhausted) {
    const XrXirDomainBudgetStats spent=xr_xir_domain_budget_stats(test->domain);
    capacity_watched=NULL;xr_xir_value_drop(&test->alias);
    CHECK(xr_xir_instance_free(test->first)==(exhausted?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));
    CHECK(xr_xir_instance_free(test->second)==XR_XIR_CALL_READY);
    const XrXirDomainBudgetStats released=xr_xir_domain_budget_stats(test->domain);
    const XrXirDomainStats physical=xr_xir_domain_stats(test->domain);
    CHECK(released.requested_bytes==spent.requested_bytes&&released.requested_call_bytes==spent.requested_call_bytes);
    CHECK(released.work>=spent.work&&!released.metadata_live&&!released.call_live);
    CHECK(physical.live_bytes==sizeof(XrXirDomain)&&physical.allocations==physical.frees+1);
    xr_xir_domain_drop(test->domain);*test=(CapacityBudgetCase){0};CHECK(!runtime_live&&!runtime_bytes);
}
static CapacityBudgetMeasure capacity_budget_measure(CapacityCompile *run,const char *name) {
    CapacityBudgetCase test=capacity_budget_open(run);CapacityBudgetMeasure measure={0};
    measure.before=test.before;measure.physical_before=test.physical;
    CHECK(capacity_transaction(run,test.first,name)==XR_XIR_CALL_RETURNED&&capacity_publications==1);
    measure.after=xr_xir_domain_budget_stats(test.domain);measure.physical_after=xr_xir_domain_stats(test.domain);
    measure.sites=runtime_attempts;measure.publication_site=capacity_publication_site;
    CHECK(measure.sites&&measure.publication_site&&measure.publication_site<=measure.sites);
    CHECK(measure.after.requested_bytes>measure.before.requested_bytes&&measure.after.work>measure.before.work);
    CHECK(measure.physical_after.peak_bytes>measure.physical_before.peak_bytes);
    CHECK(measure.after.work-measure.before.work==(!strcmp(name,"committed")?27u:50u));
    CHECK(measure.after.requested_bytes-measure.before.requested_bytes==(!strcmp(name,"committed")?176u:392u));
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(test.first,&result)==XR_XIR_CALL_RETURNED);
    CHECK(capacity_array_state(&result)>=13);xr_xir_value_drop(&result);
    capacity_budget_state(&test,1);capacity_budget_close(&test,false);return measure;
}
static void capacity_budget_axis(CapacityCompile *run,const char *name,const CapacityBudgetMeasure *measure,
    unsigned axis,bool minus_one) {
    CapacityBudgetCase test=capacity_budget_open(run);
    CHECK(test.before.requested_bytes==measure->before.requested_bytes&&test.before.work==measure->before.work);
    CHECK(test.physical.live_bytes==measure->physical_before.live_bytes);
    uint64_t threshold=axis==CAPACITY_BUDGET_REQUESTED?measure->after.requested_bytes:
        axis==CAPACITY_BUDGET_LIVE?measure->physical_after.peak_bytes:measure->after.work;
    CHECK(threshold);const uint64_t limit=threshold-(minus_one?1u:0u);
    if(axis==CAPACITY_BUDGET_REQUESTED)test.domain->budget.requested_limit=limit;
    else if(axis==CAPACITY_BUDGET_LIVE)test.domain->limit=limit;
    else test.domain->budget.work_limit=limit;
    XrXirCallStatus status=capacity_transaction(run,test.first,name);
    const XrXirDomainBudgetStats spent=xr_xir_domain_budget_stats(test.domain);
    const unsigned publications=capacity_publications;
    const bool after_commit=axis==CAPACITY_BUDGET_WORK||!strcmp(name,"committedThenAllocate");
    CHECK(status==(minus_one?XR_XIR_CALL_LIMIT:XR_XIR_CALL_RETURNED));
    CHECK(publications==(!minus_one||after_commit?1u:0u));
    CHECK(spent.requested_bytes>=test.before.requested_bytes&&spent.work>=test.before.work);
    if(axis==CAPACITY_BUDGET_REQUESTED)CHECK(spent.requested_bytes<=limit);
    if(axis==CAPACITY_BUDGET_LIVE)CHECK(xr_xir_domain_stats(test.domain).peak_bytes<=limit);
    if(!minus_one&&axis==CAPACITY_BUDGET_REQUESTED)CHECK(spent.requested_bytes==limit);
    if(!minus_one&&axis==CAPACITY_BUDGET_LIVE)CHECK(xr_xir_domain_stats(test.domain).peak_bytes==limit);
    if(axis==CAPACITY_BUDGET_WORK)CHECK(spent.work==limit);
    const bool exhausted=minus_one&&axis==CAPACITY_BUDGET_WORK;
    if(minus_one)capacity_budget_failed_output(test.first,exhausted);
    else {
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(test.first,&result)==XR_XIR_CALL_RETURNED);
        CHECK(capacity_array_state(&result)>=13);xr_xir_value_drop(&result);
    }
    capacity_budget_state(&test,publications);capacity_budget_close(&test,exhausted);
    printf("capacity runtime axis %s axis%u %s limit%llu status%u publication%u dual physical0\n",
        name,axis,minus_one?"minus1":"exact",(unsigned long long)limit,status,publications);
}
static void capacity_budget_oom_retry(CapacityCompile *run,const CapacityBudgetMeasure *measure) {
    CapacityBudgetCase test=capacity_budget_open(run);
    const uint64_t required=sizeof(XirArray)+13*sizeof(int64_t);
    CHECK(measure->after.requested_bytes-measure->before.requested_bytes==required);
    CHECK(measure->publication_site>=2&&measure->sites==measure->publication_site);
    CHECK(required<=UINT64_MAX-test.before.requested_bytes);
    test.domain->budget.requested_limit=test.before.requested_bytes+required;
    const uint64_t limit=test.domain->budget.requested_limit;
    const size_t failure=measure->publication_site-1;runtime_fail_at=failure;
    XrXirCallStatus first=capacity_transaction(run,test.first,"committed");runtime_fail_at=SIZE_MAX;
    CHECK(first==XR_XIR_CALL_OOM&&runtime_attempts>failure&&!capacity_publications);
    const XrXirDomainBudgetStats failed=xr_xir_domain_budget_stats(test.domain);
    CHECK(failed.requested_bytes==limit&&failed.work>test.before.work);
    CHECK(xr_xir_domain_stats(test.domain).live_bytes==test.physical.live_bytes);
    capacity_budget_failed_output(test.first,false);capacity_budget_state(&test,0);
    CHECK(test.first->domain==test.domain&&test.domain->budget.requested_limit==limit);
    capacity_watch(test.first);runtime_attempts=0;
    XrXirCallStatus retry=capacity_transaction(run,test.first,"committed");
    CHECK(retry==XR_XIR_CALL_LIMIT&&!capacity_publications&&test.first->domain==test.domain);
    const XrXirDomainBudgetStats retried=xr_xir_domain_budget_stats(test.domain);
    CHECK(retried.requested_bytes==limit&&retried.requested_call_bytes>failed.requested_call_bytes&&retried.work>failed.work);
    CHECK(test.domain->budget.requested_limit==limit&&xr_xir_domain_stats(test.domain).live_bytes==test.physical.live_bytes);
    capacity_budget_failed_output(test.first,false);capacity_budget_state(&test,0);capacity_budget_close(&test,false);
    printf("capacity runtime same-domain OOM retry failure-site%zu charged%llu OOM then LIMIT no refill dual physical0\n",
        failure,(unsigned long long)required);
}
static void capacity_runtime_budget(CapacityCompile *run,unsigned mode) {
    capacity_runtime_census(run);capacity_runtime_cancel(run);
    static const char *names[]={"committed","committedThenAllocate"};
    CapacityBudgetMeasure reserve={0};
    for(unsigned operation=0;operation<2;++operation){
        const CapacityBudgetMeasure measure=capacity_budget_measure(run,names[operation]);
        if(!operation)reserve=measure;
        printf("capacity runtime budget measure mode%u %s sites%zu publication-site%zu requested%llu work%llu live-peak%llu\n",
            mode,names[operation],measure.sites,measure.publication_site,
            (unsigned long long)(measure.after.requested_bytes-measure.before.requested_bytes),
            (unsigned long long)(measure.after.work-measure.before.work),(unsigned long long)measure.physical_after.peak_bytes);
        for(unsigned axis=0;axis<3;++axis){
            capacity_budget_axis(run,names[operation],&measure,axis,false);
            capacity_budget_axis(run,names[operation],&measure,axis,true);
        }
    }
    capacity_budget_oom_retry(run,&reserve);
    CHECK(capacity_stats(&run->context).live_bytes==run->stats.live_bytes);
}
#endif // XIR_ARRAY_CAPACITY_BUDGET_H
