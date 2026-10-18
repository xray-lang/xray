/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_instance_resources.h - Actual persistent public ownership cuts
 */
#ifndef SOURCE_PUBLIC_INSTANCE_RESOURCES_H
#define SOURCE_PUBLIC_INSTANCE_RESOURCES_H
typedef struct PublicInstanceOwners {
    XrSourceCompiler *compiler;
    XrSourceProgram *program;
    XrSourceInstance *instance;
    XrSourceCall *call;
    XrSourceResult *result;
    XrSourceValue *argument,*function,*value,*independent;
} PublicInstanceOwners;
static void public_instance_close(PublicInstanceOwners *owners) {
    CHECK(xray_source_call_drop(&owners->call,NULL)==XR_SOURCE_OK);
    CHECK(xray_source_instance_drop(&owners->instance,NULL)==XR_SOURCE_OK);
    xray_source_result_free(owners->result);xray_source_value_free(owners->argument);
    xray_source_value_free(owners->function);xray_source_value_free(owners->value);
    xray_source_value_free(owners->independent);xray_source_program_free(owners->program);
    xray_source_compiler_free(owners->compiler);*owners=(PublicInstanceOwners){0};
    CHECK(!runtime_live && !runtime_bytes);
}
static XrSourceStatus public_instance_pause(XrSourceCall *call,XrSourceEvent *event) {
    unsigned steps=0;XrSourceStatus status;
    do {status=xray_source_call_poll(call,8,event);CHECK(++steps<10000);}
    while(status==XR_SOURCE_OK && event->state==XR_SOURCE_RUN_READY);
    if(status!=XR_SOURCE_OK)return status;
    if(event->state==XR_SOURCE_RUN_OUT_OF_MEMORY)return XR_SOURCE_OUT_OF_MEMORY;
    if(event->state==XR_SOURCE_RUN_LIMIT)return XR_SOURCE_LIMIT;
    CHECK(event->state==XR_SOURCE_RUN_RETURNED || event->state==XR_SOURCE_RUN_SUSPENDED);
    return XR_SOURCE_OK;
}
static XrSourceStatus public_instance_drive(const XrCompileResourceLimits *limits,PublicInstanceOwners *owners) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=limits->allocated_bytes;
    options.live_bytes=limits->live_bytes;options.work=limits->work;
    XrSourceStatus status=xray_source_compiler_new(&options,&owners->compiler);if(status!=XR_SOURCE_OK)return status;
    static const char source[]="var state=40\n"
        "export fn paused()->i64{state+=1;defer{print(state)};Coro.yield();return state}\n"
        "export fn reader()->fn(i64)->i64{return fn(value:i64)->i64{return state+value}}\n";
    XrSourceText text={NULL,"public-instance-prefix",source,sizeof(source)-1};
    status=xray_source_compile_text(owners->compiler,&text,&owners->program,NULL);if(status!=XR_SOURCE_OK)return status;
    XrSourceScalar scalar={.kind=XR_SOURCE_VALUE_I64,.value.signed_value=2};
    status=xray_source_value_new_scalar(owners->compiler,&scalar,&owners->argument);if(status!=XR_SOURCE_OK)return status;
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);runtime.write=discard;
    status=xray_source_instance_new(owners->program,&runtime,&owners->instance);if(status!=XR_SOURCE_OK)return status;
    xray_source_program_free(owners->program);owners->program=NULL;
    xray_source_compiler_free(owners->compiler);owners->compiler=NULL;
    XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.name="paused";invocation.name_length=6;
    status=xray_source_instance_begin(owners->instance,&invocation,&owners->call);if(status!=XR_SOURCE_OK)return status;
    XrSourceEvent event;
    status=public_instance_pause(owners->call,&event);if(status!=XR_SOURCE_OK)return status;
    CHECK(event.state==XR_SOURCE_RUN_SUSPENDED);
    CHECK(xray_source_call_drop(&owners->call,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_READY);
    invocation.name="reader";invocation.name_length=6;
    status=xray_source_instance_execute(owners->instance,&invocation,&owners->result,NULL);if(status!=XR_SOURCE_OK)return status;
    status=xray_source_result_value(owners->result,&owners->function);if(status!=XR_SOURCE_OK)return status;
    xray_source_result_free(owners->result);owners->result=NULL;
    const XrSourceValue *arguments[]={owners->argument};
    status=xray_source_instance_begin_function(owners->instance,owners->function,arguments,1,&owners->call);
    if(status!=XR_SOURCE_OK)return status;
    xray_source_value_free(owners->argument);owners->argument=NULL;
    xray_source_value_free(owners->function);owners->function=NULL;
    status=public_instance_pause(owners->call,&event);if(status!=XR_SOURCE_OK)return status;
    CHECK(event.state==XR_SOURCE_RUN_RETURNED);
    status=xray_source_call_take(owners->call,&owners->result);if(status!=XR_SOURCE_OK)return status;
    CHECK(xray_source_call_drop(&owners->call,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_READY);
    status=xray_source_result_value(owners->result,&owners->value);if(status!=XR_SOURCE_OK)return status;
    xray_source_result_free(owners->result);owners->result=NULL;
    CHECK(xray_source_instance_drop(&owners->instance,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_READY);
    status=xray_source_value_copy(owners->value,&owners->independent);if(status!=XR_SOURCE_OK)return status;
    xray_source_value_free(owners->value);owners->value=NULL;
    CHECK(xray_source_value_scalar(owners->independent,&scalar)==XR_SOURCE_OK &&
        scalar.kind==XR_SOURCE_VALUE_I64 && scalar.value.signed_value==43);
    return XR_SOURCE_OK;
}
static void public_instance_faults(bool probe) {
    const XrCompileResourceLimits limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    size_t sites=0;
    for(size_t point=0;!point || (!probe && point<=sites);++point) {
        PublicInstanceOwners owners={0};runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;
        XrSourceStatus status=public_instance_drive(&limits,&owners);
        if(!point){CHECK(status==XR_SOURCE_OK && owners.independent);sites=runtime_attempts;CHECK(sites>0);}
        else {
            if(status!=XR_SOURCE_OUT_OF_MEMORY || owners.independent)
                fprintf(stderr,"public Instance FI point=%zu/%zu status=%u\n",point,sites,(unsigned)status);
            CHECK(status==XR_SOURCE_OUT_OF_MEMORY && !owners.independent && runtime_attempts>runtime_fail_at);
        }
        runtime_fail_at=SIZE_MAX;public_instance_close(&owners);
    }
    printf("public persistent Instance actual sites=%zu swept=%zu; producer/argument/function death, cancel/restart, real callable and physical=0/0\n",sites,probe?0:sites);
}
static void public_instance_budgets(void) {
    const XrCompileResourceLimits generous={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    PublicInstanceOwners owners={0};XrCompileResourceStats required={0};runtime_fail_at=SIZE_MAX;
    CHECK(public_instance_drive(&generous,&owners)==XR_SOURCE_OK);
    CHECK(xr_compile_resources_stats(owners.independent->resources,&required)==XR_COMPILE_RESOURCE_OK);
    public_instance_close(&owners);
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound>1 && *bound<UINT64_MAX);*bound=delta<0?*bound-1:*bound+(unsigned)delta;
        XrSourceStatus status=public_instance_drive(&limits,&owners);
        if(delta<0) {
            if(status!=XR_SOURCE_BUDGET || owners.independent)
                fprintf(stderr,"public Instance budget axis=%u delta=%d status=%u\n",axis,delta,(unsigned)status);
            CHECK(status==XR_SOURCE_BUDGET && !owners.independent);
        } else CHECK(status==XR_SOURCE_OK && owners.independent);
        public_instance_close(&owners);
    }
    printf("public persistent Instance original compiler axes allocated=%llu peak=%llu work=%llu; exact/minus-one/plus-one physical=0/0\n",
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
}
#endif // SOURCE_PUBLIC_INSTANCE_RESOURCES_H
