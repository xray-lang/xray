/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_persistent_host_cases.h - Opaque host owner on real VM and native code
 */
#ifndef XIR_PERSISTENT_HOST_CASES_H
#define XIR_PERSISTENT_HOST_CASES_H
typedef struct PersistentTrace {
    XrXirHostInstance *owner;
    XrXirHostCall *call;
    int64_t values[16];
    unsigned count;
} PersistentTrace;
static XrXirOutputStatus persistent_output(void *context,const XrXirOutputGroup *group) {
    PersistentTrace *trace=context;
    CHECK(group->count==1 && group->values[0].type==XR_XIR_I64 && trace->count<16);
    trace->values[trace->count++]=(int64_t)group->values[0].payload;
    XrXirHostCall *empty=NULL;
    CHECK(xr_xir_host_instance_drop(trace->owner)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_host_instance_begin(trace->owner,0,NULL,0,&empty)==XR_XIR_CALL_BUSY && !empty);
    CHECK(xr_xir_host_call_drop(trace->call)==XR_XIR_CALL_BUSY);
    return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceResult persistent_until_pause(XrXirHostCall *call) {
    XrXirInstanceResult result={0};unsigned slices=0;
    do { result=xr_xir_host_call_step_bounded(call,8); CHECK(++slices<10000); }
    while(result.outcome.status==XR_XIR_CALL_READY);
    return result;
}
static void persistent_host_cases(XrXirProgram *program,uint32_t normal,uint32_t current,
    uint32_t reader,uint32_t children) {
    PersistentTrace traces[2]={0}; XrXirHostInstance *owners[2]={0}; XrXirHostCall *calls[2]={0};
    CHECK(xr_xir_host_instance_drop(NULL)==XR_XIR_CALL_READY);
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,persistent_output,&traces[i]};
        CHECK(xr_xir_host_instance_new(program,&config,&owners[i])==XR_XIR_CALL_READY);
        traces[i].owner=owners[i];
        CHECK(xr_xir_host_instance_begin(owners[i],normal,NULL,0,&calls[i])==XR_XIR_CALL_READY);
        traces[i].call=calls[i];
    }
    XrXirHostCall *empty=NULL;
    CHECK(xr_xir_host_instance_begin(owners[0],current,NULL,0,&empty)==XR_XIR_CALL_BUSY && !empty);
    CHECK(xr_xir_host_instance_drop(owners[0])==XR_XIR_CALL_BUSY);
    XrXirInstanceResult first=persistent_until_pause(calls[0]),second=persistent_until_pause(calls[1]);
    CHECK(first.outcome.status==XR_XIR_CALL_SUSPENDED && second.outcome.status==XR_XIR_CALL_SUSPENDED);
    CHECK(traces[0].count==1 && traces[0].values[0]==1 && traces[1].count==1 && traces[1].values[0]==1);
    CHECK(xr_xir_host_call_drop(calls[0])==XR_XIR_CALL_READY); calls[0]=NULL;traces[0].call=NULL;
    CHECK(traces[0].count==2 && traces[0].values[1]==1);
    XrXirCallResult value={0},function={0},text={0};
    CHECK(xr_xir_host_instance_execute(owners[0],current,NULL,0,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.value.type==XR_XIR_I64 && value.value.payload==1); xr_xir_call_result_drop(&value);
    for(unsigned i=0;i<2;++i) {
        CHECK(xr_xir_host_call_resume(calls[1],second.epoch,second.outcome.wake)==XR_XIR_CALL_READY);
        second=persistent_until_pause(calls[1]);
        CHECK(second.outcome.status==(i?XR_XIR_CALL_RETURNED:XR_XIR_CALL_SUSPENDED));
    }
    CHECK(xr_xir_host_call_take(calls[1],&text)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_host_call_drop(calls[1])==XR_XIR_CALL_READY); calls[1]=NULL;traces[1].call=NULL;
    CHECK(xr_xir_host_instance_execute(owners[0],reader,NULL,0,&function)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_host_instance_begin_function(owners[1],&function.value,NULL,0,&empty)==XR_XIR_CALL_BAD_ARGUMENT && !empty);
    CHECK(xr_xir_host_instance_begin_function(owners[0],&function.value,NULL,0,&calls[0])==XR_XIR_CALL_READY);
    CHECK(xr_xir_host_call_resume(calls[0],first.epoch,first.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    first=persistent_until_pause(calls[0]);
    CHECK(first.outcome.status==XR_XIR_CALL_RETURNED && first.outcome.value.payload==1);
    CHECK(xr_xir_host_call_take(calls[0],&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_host_call_drop(calls[0])==XR_XIR_CALL_READY); calls[0]=NULL;
    xr_xir_call_result_drop(&value);
    CHECK(xr_xir_host_instance_begin(owners[0],children,NULL,0,&calls[0])==XR_XIR_CALL_READY);
    traces[0].call=calls[0];
    CHECK(xr_xir_host_call_step_bounded(calls[0],128).outcome.status==XR_XIR_CALL_READY);
    CHECK(xr_xir_host_call_drop(calls[0])==XR_XIR_CALL_READY); calls[0]=NULL;traces[0].call=NULL;
    CHECK(traces[0].count==4 && traces[0].values[2]+traces[0].values[3]==147 && traces[0].values[2]!=traces[0].values[3]);
    CHECK(xr_xir_host_instance_execute(owners[0],current,NULL,0,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.value.payload==1);xr_xir_call_result_drop(&value);
    CHECK(xr_xir_host_instance_drop(owners[0])==XR_XIR_CALL_READY);
    CHECK(xr_xir_host_instance_begin_function(owners[1],&function.value,NULL,0,&empty)==XR_XIR_CALL_BAD_ARGUMENT && !empty);
    CHECK(xr_xir_host_instance_drop(owners[1])==XR_XIR_CALL_READY);
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&text.value,&bytes,&length) && length==11 && !memcmp(bytes,"held-result",11));
    xr_xir_call_result_drop(&function);xr_xir_call_result_drop(&text);
    puts("opaque persistent host: state, callable authority, independent owners, child drain and owned result PASS");
}
#endif // XIR_PERSISTENT_HOST_CASES_H
