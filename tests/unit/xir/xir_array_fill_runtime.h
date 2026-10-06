/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_fill_runtime.h - Actual failures preserve the observed commit boundary
 *
 * KEY CONCEPT:
 *   A real publication observer selects fixed old or new outcomes. Cancellation
 *   and allocation refusal cannot partially publish a private replacement.
 */
#ifndef XIR_ARRAY_FILL_RUNTIME_H
#define XIR_ARRAY_FILL_RUNTIME_H
static void fill_transaction_state(FillCompile *run,XrXirInstance *first,
    XrXirInstance *second,const XrXirValue *alias,unsigned publications) {
    static const int64_t old[]={1,2,3},committed[]={1,7,3};
    CHECK(publications<=1);fill_watched=NULL;
    XrXirValue value=fill_run(first,fill_find(run->module,"saved"));
    fill_array(&value,publications?committed:old,3);
    if(!publications)CHECK(value.payload==alias->payload);
    xr_xir_value_drop(&value);fill_array(alias,old,3);
    value=fill_run(second,fill_find(run->module,"saved"));fill_array(&value,old,3);xr_xir_value_drop(&value);
}
static XrXirCallStatus fill_transaction_call(FillCompile *run,XrXirInstance *instance,const char *name) {
    XrXirCallStatus status=xr_xir_instance_start(instance,fill_find(run->module,name),NULL,0);
    while(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    return status;
}
static void fill_transaction_faults(FillCompile *run,const char *name) {
    XrXirInstance *first=fill_open(run),*second=fill_open(run);
    XrXirValue alias=fill_run(first,fill_find(run->module,"saved"));fill_watch(first);runtime_attempts=0;
    CHECK(fill_transaction_call(run,first,name)==XR_XIR_CALL_RETURNED&&fill_publications==1);
    const size_t sites=runtime_attempts;XrXirValue result={0};
    CHECK(xr_xir_instance_take_result(first,&result)==XR_XIR_CALL_RETURNED);xr_xir_value_drop(&result);
    fill_transaction_state(run,first,second,&alias,1);xr_xir_value_drop(&alias);
    CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY&&xr_xir_instance_free(second)==XR_XIR_CALL_READY);
    CHECK(!runtime_live&&!runtime_bytes&&sites);
    size_t pre=0,post=0;
    for(size_t failure=0;failure<sites;++failure) {
        first=fill_open(run);second=fill_open(run);alias=fill_run(first,fill_find(run->module,"saved"));
        fill_watch(first);runtime_attempts=0;runtime_fail_at=failure;
        XrXirCallStatus status=fill_transaction_call(run,first,name);
        CHECK(status==XR_XIR_CALL_OOM&&runtime_attempts>failure);runtime_fail_at=SIZE_MAX;
        unsigned publications=fill_publications;if(publications)++post;else ++pre;
        result=(XrXirValue){0};CHECK(xr_xir_instance_take_result(first,&result)==XR_XIR_CALL_BAD_STATE&&!result.type&&!result.payload);
        XrXirCallResult copy={0};CHECK(xr_xir_instance_copy_failure(first,&copy)==XR_XIR_CALL_BAD_STATE&&xr_xir_call_result_empty(&copy));
        fill_transaction_state(run,first,second,&alias,publications);xr_xir_value_drop(&alias);
        CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY&&xr_xir_instance_free(second)==XR_XIR_CALL_READY);
        CHECK(!runtime_live&&!runtime_bytes);
    }
    CHECK(pre+post==sites);
    if(!strcmp(name,"committedThenAllocate"))CHECK(post>0);
    printf("fill %s real OOM%zu precommit%zu postcommit%zu twoInstances root+alias physical0\n",name,sites,pre,post);
}
static void fill_transaction_cancel(FillCompile *run,const char *name) {
    XrXirInstance *instance=fill_open(run);fill_watch(instance);
    CHECK(xr_xir_instance_start(instance,fill_find(run->module,name),NULL,0)==XR_XIR_CALL_READY);
    size_t ticks=0;XrXirCallStatus status=XR_XIR_CALL_READY;
    while(status==XR_XIR_CALL_READY){CHECK(++ticks<4096);status=xr_xir_instance_poll_bounded(instance,1).outcome.status;}
    CHECK(status==XR_XIR_CALL_RETURNED&&fill_publications==1);XrXirValue result={0};
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);xr_xir_value_drop(&result);fill_watched=NULL;
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    size_t pre=0,post=0,late=0;
    for(size_t prefix=0;prefix<ticks;++prefix) {
        XrXirInstance *first=fill_open(run),*second=fill_open(run);
        XrXirValue alias=fill_run(first,fill_find(run->module,"saved"));fill_watch(first);
        CHECK(xr_xir_instance_start(first,fill_find(run->module,name),NULL,0)==XR_XIR_CALL_READY);
        for(size_t i=0;i<prefix;++i)CHECK(xr_xir_instance_poll_bounded(first,1).outcome.status==XR_XIR_CALL_READY);
        unsigned publications=fill_publications;runtime_attempts=0;
        CHECK(xr_xir_instance_cancel_current(first)==XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_poll_bounded(first,UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED);
        late+=runtime_attempts;CHECK(fill_publications==publications);if(publications)++post;else ++pre;
        result=(XrXirValue){0};CHECK(xr_xir_instance_take_result(first,&result)==XR_XIR_CALL_BAD_STATE&&!result.type&&!result.payload);
        fill_transaction_state(run,first,second,&alias,publications);xr_xir_value_drop(&alias);
        CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY&&xr_xir_instance_free(second)==XR_XIR_CALL_READY);
        CHECK(!runtime_live&&!runtime_bytes);
    }
    printf("fill %s cancel prefixes%zu precommit%zu postcommit%zu actual late sites%zu physical0\n",name,ticks,pre,post,late);
}
static void fill_transaction_limits(FillCompile *run) {
    for(unsigned kind=0;kind<4;++kind) {
        XrXirInstance *first=fill_open(run),*second=fill_open(run);
        XrXirValue alias=fill_run(first,fill_find(run->module,"saved"));XirObject *backing=object_pointer(&alias);CHECK(backing);
        _Atomic uint32_t *references=kind==1?&backing->references:kind==2?&first->domain->references:&run->program->arena->references;
        uint32_t original=atomic_load(references);uint64_t limit=first->domain->limit;
        fill_watch(first);if(!kind)first->domain->limit=first->domain->stats.live_bytes;else atomic_store(references,UINT32_MAX);
        XrXirCallStatus status=fill_transaction_call(run,first,"committed");first->domain->limit=limit;if(kind)atomic_store(references,original);
        CHECK(status==XR_XIR_CALL_LIMIT&&fill_publications==0);
        fill_transaction_state(run,first,second,&alias,0);xr_xir_value_drop(&alias);
        CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY&&xr_xir_instance_free(second)==XR_XIR_CALL_READY);
        CHECK(!runtime_live&&!runtime_bytes);
    }
    puts("fill domain/backing/domainref/arenaref LIMIT precommit root identity preserved physical0");
}
static void fill_exit_frontiers(FillCompile *run) {
    XrXirInstance *first=fill_open(run),*second=fill_open(run);
    XrXirValue alias=fill_run(first,fill_find(run->module,"saved"));fill_watch(first);
    CHECK(fill_transaction_call(run,first,"committedThenAssert")==XR_XIR_CALL_ASSERTION&&fill_publications==1);
    XrXirCallResult failure=xr_xir_instance_poll_bounded(first,UINT64_MAX).outcome;
    const char *message=NULL;size_t length=0;
    CHECK(failure.panic.detail.code==445&&xr_xir_string_view(&failure.panic.message,&message,&length));
    CHECK(length==10&&!memcmp(message,"after fill",10));
    fill_transaction_state(run,first,second,&alias,1);
    XrXirValue result=fill_run(first,fill_find(run->module,"currentTrace"));CHECK(result.payload==99);xr_xir_value_drop(&result);
    xr_xir_value_drop(&alias);CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY&&xr_xir_instance_free(second)==XR_XIR_CALL_READY);
    CHECK(!runtime_live&&!runtime_bytes);
    first=fill_open(run);second=fill_open(run);alias=fill_run(first,fill_find(run->module,"saved"));fill_watch(first);
    result=fill_run(first,fill_find(run->module,"caughtRange"));CHECK(result.type==XR_XIR_I64&&result.payload==422);xr_xir_value_drop(&result);
    CHECK(!fill_publications);fill_transaction_state(run,first,second,&alias,0);
    result=fill_run(first,fill_find(run->module,"currentTrace"));CHECK(result.payload==55);xr_xir_value_drop(&result);
    xr_xir_value_drop(&alias);CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY&&xr_xir_instance_free(second)==XR_XIR_CALL_READY);
    CHECK(!runtime_live&&!runtime_bytes);
    puts("fill postcommit assertion445 owned-message10/defer99 and protected range422/defer55 physical0");
}
static void fill_runtime(FillCompile *run) {
    fill_initialization_faults(run);
    fill_transaction_faults(run,"committed");fill_transaction_faults(run,"committedThenAllocate");
    fill_transaction_cancel(run,"committed");fill_transaction_cancel(run,"committedThenAllocate");
    fill_transaction_limits(run);fill_exit_frontiers(run);
}
#endif
