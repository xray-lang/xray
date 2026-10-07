/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_capacity_runtime.h - Actual reserve failure and cancellation frontiers
 *
 * KEY CONCEPT:
 *   A real publication observer selects old or committed capacity outcomes.
 *   Every injected attempt keeps an independent second Instance and old alias.
 */
#ifndef XIR_ARRAY_CAPACITY_RUNTIME_H
#define XIR_ARRAY_CAPACITY_RUNTIME_H
static int64_t capacity_array_state(const XrXirValue *value) {
    XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(1048576,&reader)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(value),reader,NULL,NULL,100000,1048576};
    int64_t length=-1,cap=-1;
    CHECK(xr_xir_array_len(value,&admission,&length)==XR_XIR_VALUE_OK&&length==3);
    CHECK(xr_xir_array_capacity(value,&admission,&cap)==XR_XIR_VALUE_OK&&cap>=length&&cap<=INT64_MAX);
    static const int64_t expected[]={1,2,3};
    for(int64_t i=0;i<3;++i){XrXirValue item={0};XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_get(value,i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
        CHECK(item.type==XR_XIR_I64&&item.payload==expected[i]);xr_xir_value_drop(&item);
    }
    xr_xir_domain_drop(reader);return cap;
}
static XrXirCallStatus capacity_transaction(CapacityCompile *run,XrXirInstance *instance,const char *name) {
    XrXirCallStatus status=xr_xir_instance_start(instance,capacity_find(run->module,name),NULL,0);
    while(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    return status;
}
static void capacity_transaction_state(CapacityCompile *run,XrXirInstance *first,
    XrXirInstance *second,const XrXirValue *alias,int64_t old_cap,unsigned publications) {
    CHECK(publications<=1);capacity_watched=NULL;
    CHECK(capacity_array_state(alias)==old_cap);
    XrXirValue value=capacity_run(first,capacity_find(run->module,"saved"));
    int64_t cap=capacity_array_state(&value);
    if(publications)CHECK(cap>=13);
    else CHECK(cap==old_cap&&value.payload==alias->payload);
    xr_xir_value_drop(&value);
    value=capacity_run(second,capacity_find(run->module,"saved"));
    CHECK(capacity_array_state(&value)==old_cap);xr_xir_value_drop(&value);
}
static void capacity_failed_output(XrXirInstance *instance) {
    XrXirValue output={0};
    CHECK(xr_xir_instance_take_result(instance,&output)==XR_XIR_CALL_BAD_STATE);
    CHECK(!output.type&&!output.reserved&&!output.payload);
    output=(XrXirValue){XR_XIR_I64,0,77};const XrXirValue before=output;
    CHECK(xr_xir_instance_take_result(instance,&output)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(&output,&before,sizeof(output)));
    XrXirCallResult failure={0},original=failure;
    CHECK(xr_xir_instance_copy_failure(instance,&failure)==XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&failure,&original,sizeof(failure)));
}
static void capacity_transaction_census(CapacityCompile *run,const char *name) {
    XrXirInstance *first=capacity_open(run),*second=capacity_open(run);
    XrXirValue alias=capacity_run(first,capacity_find(run->module,"saved"));
    const int64_t old_cap=capacity_array_state(&alias);
    const XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(first->domain);
    capacity_watch(first);runtime_attempts=0;
    CHECK(capacity_transaction(run,first,name)==XR_XIR_CALL_RETURNED&&capacity_publications==1);
    const size_t sites=runtime_attempts,published=capacity_publication_site;
    const XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(first->domain);
    CHECK(after.requested_bytes>=before.requested_bytes&&after.work>=before.work);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(first,&result)==XR_XIR_CALL_RETURNED);
    CHECK(capacity_array_state(&result)>=13);xr_xir_value_drop(&result);
    capacity_transaction_state(run,first,second,&alias,old_cap,1);xr_xir_value_drop(&alias);
    CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY&&xr_xir_instance_free(second)==XR_XIR_CALL_READY);
    CHECK(!runtime_live&&!runtime_bytes&&sites&&published<=sites);
    size_t pre=0,post=0;
    for(size_t failure=0;failure<sites;++failure){
        first=capacity_open(run);second=capacity_open(run);
        alias=capacity_run(first,capacity_find(run->module,"saved"));
        CHECK(capacity_array_state(&alias)==old_cap);
        XrXirDomain *domain=first->domain;CHECK(xr_xir_domain_retain(domain));
        const XrXirDomainBudgetStats fee_before=xr_xir_domain_budget_stats(domain);
        capacity_watch(first);runtime_attempts=0;runtime_fail_at=failure;
        XrXirCallStatus status=capacity_transaction(run,first,name);
        runtime_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_CALL_OOM&&runtime_attempts>failure);
        const XrXirDomainBudgetStats charged=xr_xir_domain_budget_stats(domain);
        CHECK(first->domain==domain&&charged.requested_bytes>=fee_before.requested_bytes);
        CHECK(charged.requested_call_bytes>=fee_before.requested_call_bytes&&charged.work>fee_before.work);
        CHECK(charged.requested_bytes>fee_before.requested_bytes||charged.requested_call_bytes>fee_before.requested_call_bytes);
        const unsigned publications=capacity_publications;
        if(publications)++post;else ++pre;
        capacity_failed_output(first);
        capacity_transaction_state(run,first,second,&alias,old_cap,publications);
        const XrXirDomainBudgetStats observed=xr_xir_domain_budget_stats(domain);
        CHECK(observed.requested_bytes>=charged.requested_bytes&&observed.requested_call_bytes>=charged.requested_call_bytes);
        CHECK(observed.work>=charged.work&&first->state==XR_XIR_INSTANCE_READY);
        capacity_watch(first);CHECK(capacity_transaction(run,first,name)==XR_XIR_CALL_RETURNED&&capacity_publications==1);
        const XrXirDomainBudgetStats retried=xr_xir_domain_budget_stats(domain);
        CHECK(first->domain==domain&&retried.requested_bytes>observed.requested_bytes&&retried.work>observed.work);
        result=(XrXirValue){0};CHECK(xr_xir_instance_take_result(first,&result)==XR_XIR_CALL_RETURNED);
        CHECK(capacity_array_state(&result)>=13);xr_xir_value_drop(&result);
        capacity_transaction_state(run,first,second,&alias,old_cap,1);xr_xir_value_drop(&alias);
        const XrXirDomainBudgetStats before_release=xr_xir_domain_budget_stats(domain);
        CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY&&xr_xir_instance_free(second)==XR_XIR_CALL_READY);
        const XrXirDomainBudgetStats released=xr_xir_domain_budget_stats(domain);
        CHECK(released.requested_bytes==before_release.requested_bytes&&released.requested_call_bytes==before_release.requested_call_bytes);
        CHECK(released.work>=before_release.work&&!released.metadata_live&&!released.call_live);
        CHECK(xr_xir_domain_stats(domain).live_bytes==sizeof(XrXirDomain));xr_xir_domain_drop(domain);
        CHECK(!runtime_live&&!runtime_bytes);
    }
    printf("capacity census %s same-Instance same-Domain OOM retries%zu charges retained release no refund dual physical0\n",name,sites);
    CHECK(pre&&pre+post==sites);
    if(!strcmp(name,"committedThenAllocate"))CHECK(post);
    printf("capacity census %s sites%zu publication-site%zu requested%llu work%llu pre%zu post%zu dual physical0\n",
        name,sites,published,(unsigned long long)(after.requested_bytes-before.requested_bytes),
        (unsigned long long)(after.work-before.work),pre,post);
}
static void capacity_runtime_limits(CapacityCompile *run) {
    for(unsigned kind=0;kind<4;++kind){
        XrXirInstance *first=capacity_open(run),*second=capacity_open(run);
        XrXirValue alias=capacity_run(first,capacity_find(run->module,"saved"));
        const int64_t old_cap=capacity_array_state(&alias);XirObject *backing=object_pointer(&alias);CHECK(backing);
        _Atomic(uint32_t) *references=kind==1?&backing->references:
            kind==2?&first->domain->references:&run->program->arena->references;
        const uint32_t original=atomic_load(references);const uint64_t limit=first->domain->limit;
        capacity_watch(first);
        if(!kind)first->domain->limit=first->domain->stats.live_bytes;else atomic_store(references,UINT32_MAX);
        XrXirCallStatus status=capacity_transaction(run,first,"committed");
        first->domain->limit=limit;if(kind)atomic_store(references,original);
        CHECK(status==XR_XIR_CALL_LIMIT&&!capacity_publications);
        capacity_failed_output(first);capacity_transaction_state(run,first,second,&alias,old_cap,0);
        xr_xir_value_drop(&alias);
        CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY&&xr_xir_instance_free(second)==XR_XIR_CALL_READY);
        CHECK(!runtime_live&&!runtime_bytes);
    }
    puts("capacity domain/backing/domainref/arenaref LIMIT precommit identity+cap dual physical0");
}
static void capacity_initialization_census(CapacityCompile *run) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.value_limit=1048576;XrXirInstance *instance=NULL;runtime_attempts=0;
    CHECK(xr_xir_instance_new(run->program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue result=capacity_run(instance,run->program->declarations->entry_function);
    const size_t sites=runtime_attempts;
    CHECK(result.type==XR_XIR_I64&&!result.payload);xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    size_t creation=0,prepared=0,latched=0;
    for(size_t failure=0;failure<sites;++failure){
        runtime_attempts=0;runtime_fail_at=failure;instance=NULL;
        XrXirCallStatus status=xr_xir_instance_new(run->program,&config,&instance);
        XrXirDomain *domain=NULL;XrXirDomainBudgetStats before_start={0};
        if(status==XR_XIR_CALL_READY){
            domain=instance->domain;CHECK(xr_xir_domain_retain(domain));
            before_start=xr_xir_domain_budget_stats(domain);
            status=xr_xir_instance_start(instance,run->program->declarations->entry_function,NULL,0);
            while(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
        }
        runtime_fail_at=SIZE_MAX;CHECK(status==XR_XIR_CALL_OOM&&runtime_attempts>failure);
        if(instance){
            CHECK(domain&&instance->domain==domain);
            const XrXirDomainBudgetStats charged=xr_xir_domain_budget_stats(domain);
            CHECK(charged.requested_bytes>=before_start.requested_bytes&&charged.requested_call_bytes>before_start.requested_call_bytes);
            CHECK(charged.work>before_start.work);
            result=(XrXirValue){0};
            CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_BAD_STATE&&!result.type&&!result.reserved&&!result.payload);
            if(instance->state==XR_XIR_INSTANCE_FAILED){
                XrXirCallResult copy={0};
                CHECK(xr_xir_instance_copy_failure(instance,&copy)==XR_XIR_CALL_OOM&&copy.status==XR_XIR_CALL_OOM);
                xr_xir_call_result_drop(&copy);
                const size_t before_retry=runtime_attempts;
                CHECK(xr_xir_instance_start(instance,run->program->declarations->entry_function,NULL,0)==XR_XIR_CALL_OOM);
                const XrXirDomainBudgetStats sticky=xr_xir_domain_budget_stats(domain);
                CHECK(sticky.requested_bytes==charged.requested_bytes&&sticky.requested_call_bytes==charged.requested_call_bytes);
                CHECK(sticky.work==charged.work&&runtime_attempts==before_retry);
                ++latched;
            }else {
                CHECK(instance->state==XR_XIR_INSTANCE_NEW);
                result=capacity_run(instance,run->program->declarations->entry_function);
                CHECK(result.type==XR_XIR_I64&&!result.reserved&&!result.payload);xr_xir_value_drop(&result);
                const XrXirDomainBudgetStats retried=xr_xir_domain_budget_stats(domain);
                CHECK(instance->domain==domain&&instance->state==XR_XIR_INSTANCE_READY);
                CHECK(retried.requested_bytes>=charged.requested_bytes&&retried.requested_call_bytes>charged.requested_call_bytes);
                CHECK(retried.work>charged.work);++prepared;
            }
            const XrXirDomainBudgetStats before_release=xr_xir_domain_budget_stats(domain);
            CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
            const XrXirDomainBudgetStats released=xr_xir_domain_budget_stats(domain);
            CHECK(released.requested_bytes==before_release.requested_bytes&&released.requested_call_bytes==before_release.requested_call_bytes);
            CHECK(released.work>=before_release.work&&!released.metadata_live&&!released.call_live);
            CHECK(xr_xir_domain_stats(domain).live_bytes==sizeof(XrXirDomain));xr_xir_domain_drop(domain);
        }else ++creation;
        CHECK(!runtime_live&&!runtime_bytes);
    }
    CHECK(sites&&creation&&latched&&creation+prepared+latched==sites);
    printf("capacity initialization sites%zu creation%zu pre-start%zu latched%zu physical0\n",sites,creation,prepared,latched);
    printf("capacity initialization same-Domain fees retained prepared-retries%zu sticky-no-attempt%zu release no refund physical0\n",prepared,latched);
}
static void capacity_runtime_census(CapacityCompile *run) {
    capacity_initialization_census(run);
    capacity_transaction_census(run,"committed");
    capacity_transaction_census(run,"committedThenAllocate");capacity_runtime_limits(run);
    CHECK(capacity_stats(&run->context).live_bytes==run->stats.live_bytes);
}
static void capacity_transaction_cancel(CapacityCompile *run,const char *name) {
    XrXirInstance *instance=capacity_open(run);capacity_watch(instance);
    CHECK(xr_xir_instance_start(instance,capacity_find(run->module,name),NULL,0)==XR_XIR_CALL_READY);
    size_t ticks=0;XrXirCallStatus status=XR_XIR_CALL_READY;
    while(status==XR_XIR_CALL_READY){CHECK(++ticks<4096);status=xr_xir_instance_poll_bounded(instance,1).outcome.status;}
    CHECK(status==XR_XIR_CALL_RETURNED&&capacity_publications==1);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(capacity_array_state(&result)>=13);xr_xir_value_drop(&result);capacity_watched=NULL;
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    size_t pre=0,post=0,late=0;
    for(size_t prefix=0;prefix<ticks;++prefix){
        XrXirInstance *first=capacity_open(run),*second=capacity_open(run);
        XrXirValue alias=capacity_run(first,capacity_find(run->module,"saved"));
        const int64_t old_cap=capacity_array_state(&alias);capacity_watch(first);
        CHECK(xr_xir_instance_start(first,capacity_find(run->module,name),NULL,0)==XR_XIR_CALL_READY);
        for(size_t i=0;i<prefix;++i)CHECK(xr_xir_instance_poll_bounded(first,1).outcome.status==XR_XIR_CALL_READY);
        const unsigned publications=capacity_publications;runtime_attempts=0;
        CHECK(xr_xir_instance_cancel_current(first)==XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_poll_bounded(first,UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED);
        late+=runtime_attempts;CHECK(capacity_publications==publications);
        if(publications)++post;else ++pre;
        capacity_failed_output(first);capacity_transaction_state(run,first,second,&alias,old_cap,publications);
        xr_xir_value_drop(&alias);
        CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY&&xr_xir_instance_free(second)==XR_XIR_CALL_READY);
        CHECK(!runtime_live&&!runtime_bytes);
    }
    CHECK(pre&&pre+post==ticks&&!late);
    if(!strcmp(name,"committedThenAllocate"))CHECK(post);
    printf("capacity cancel %s prefixes%zu pre%zu post%zu late-sites%zu dual physical0\n",name,ticks,pre,post,late);
}
static void capacity_runtime_cancel(CapacityCompile *run) {
    capacity_transaction_cancel(run,"committed");capacity_transaction_cancel(run,"committedThenAllocate");
    CHECK(capacity_stats(&run->context).live_bytes==run->stats.live_bytes);
}
#endif // XIR_ARRAY_CAPACITY_RUNTIME_H
