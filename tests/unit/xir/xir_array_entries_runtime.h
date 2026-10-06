/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_entries_runtime.h - Actual failure and cancellation prefixes
 *
 * KEY CONCEPT:
 *   Every interrupted sequence releases its private result without mutating the snapshot.
 */
#ifndef XIR_ARRAY_ENTRIES_RUNTIME_H
#define XIR_ARRAY_ENTRIES_RUNTIME_H
#include "xir_array_entries_initialization.h"
static XrXirInstance *entries_ready(EntriesCompile *run) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.value_limit=1048576;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(run->program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue value=entries_run(instance,run->program->declarations->entry_function);
    CHECK(value.type==XR_XIR_I64&&!value.payload);xr_xir_value_drop(&value);return instance;
}
static void entries_cancel(EntriesCompile *run,uint32_t function) {
    const size_t live=runtime_live,bytes=runtime_bytes;
    XrXirInstance *instance=entries_ready(run);size_t ticks=0;
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    while(status==XR_XIR_CALL_READY) {
        CHECK(++ticks<4096);status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
    }
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);
    for(size_t prefix=0;prefix<ticks;++prefix) {
        instance=entries_ready(run);
        CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
        for(size_t tick=0;tick<prefix;++tick)CHECK(xr_xir_instance_poll_bounded(instance,1).outcome.status==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED);
        value=(XrXirValue){0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE&&!value.type&&!value.payload);
#if !defined(XR_ENTRIES_MATRIX)
        static const int64_t original[]={7,9};
        value=entries_run(instance,entries_find(run->module,"saved"));entries_numbers(&value,original,2,false);xr_xir_value_drop(&value);
        value=entries_run(instance,entries_find(run->module,"count"));CHECK(value.payload==0||value.payload==1);xr_xir_value_drop(&value);
#endif
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);
    }
    printf("entries function%u cancel-prefixes%zu no-result physical0\n",function,ticks);
}
#if !defined(XR_ENTRIES_MATRIX)
static void entries_faults(EntriesCompile *run,const char *name) {
    uint32_t function=entries_find(run->module,name);
    const size_t live=runtime_live,bytes=runtime_bytes;
    XrXirInstance *instance=entries_ready(run);runtime_attempts=0;
    XrXirValue value=entries_run(instance,function);size_t sites=runtime_attempts;xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);
    for(size_t failure=0;failure<sites;++failure) {
        instance=entries_ready(run);runtime_attempts=0;runtime_fail_at=failure;
        XrXirCallStatus status=xr_xir_instance_start(instance,function,NULL,0);
        while(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
        CHECK(status==XR_XIR_CALL_OOM&&runtime_attempts>failure);runtime_fail_at=SIZE_MAX;
        value=(XrXirValue){0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE&&!value.type&&!value.payload);
        static const int64_t original[]={7,9};
        value=entries_run(instance,entries_find(run->module,"saved"));entries_numbers(&value,original,2,false);xr_xir_value_drop(&value);
        value=entries_run(instance,entries_find(run->module,"count"));CHECK(value.payload==0||value.payload==1);xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);
    }
    printf("entries %s runtimeOOM%zu snapshot unchanged physical0\n",name,sites);
}
static void entries_runtime_limits(EntriesCompile *run) {
    const size_t live=runtime_live,bytes=runtime_bytes;
    for(unsigned n=0;n<4;++n) {
        XrXirInstance *instance=entries_ready(run);
        XrXirValue alias=entries_run(instance,entries_find(run->module,"saved"));
        XirObject *backing=object_pointer(&alias);CHECK(backing);
        _Atomic uint32_t *refs=n==1?&backing->references:n==2?&instance->domain->references:&run->program->arena->references;
        uint32_t original=atomic_load(refs);uint64_t limit=instance->domain->limit;
        if(!n)instance->domain->limit=instance->domain->stats.live_bytes;else atomic_store(refs,UINT32_MAX);
        XrXirCallStatus status=xr_xir_instance_start(instance,entries_find(run->module,"result"),NULL,0);
        if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
        instance->domain->limit=limit;if(n)atomic_store(refs,original);CHECK(status==XR_XIR_CALL_LIMIT);
        XrXirValue absent={0};CHECK(xr_xir_instance_take_result(instance,&absent)==XR_XIR_CALL_BAD_STATE&&!absent.type&&!absent.payload);
        static const int64_t expected[]={7,9};entries_numbers(&alias,expected,2,false);xr_xir_value_drop(&alias);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);
    }
    puts("entries domain/backing/domainref/arenaref LIMIT snapshot unchanged physical0");
}
#endif
#include "xir_array_entries_float.h"
#include "xir_array_entries_suspend.h"
static void entries_runtime(EntriesCompile *run) {
    entries_initialization_faults(run);
#if !defined(XR_ENTRIES_MATRIX)
    static const char *const names[]={"result","empty","text","retainedCallable"};
    for(unsigned i=0;i<4;++i) {entries_faults(run,names[i]);entries_cancel(run,entries_find(run->module,names[i]));}
    entries_runtime_limits(run);entries_float_bits(run);entries_suspend(run);
#else
    unsigned count=XR_ENTRIES_MATRIX==4?7:XR_ENTRIES_MATRIX==5?3:XR_ENTRIES_MATRIX==7||XR_ENTRIES_MATRIX==6?2:6;
    for(unsigned row=0;row<count;++row) {
        char name[16];CHECK(snprintf(name,sizeof(name),"case%u",row)>0);
        entries_cancel(run,entries_find(run->module,name));
    }
#endif
}
#endif
