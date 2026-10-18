/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_projection_resources.h - Real owned projection allocation cuts
 */
#ifndef SOURCE_PUBLIC_PROJECTION_RESOURCES_H
#define SOURCE_PUBLIC_PROJECTION_RESOURCES_H
typedef struct ProjectionOwners {
    XrSourceCompiler *compiler;
    XrSourceProgram *program;
    XrSourceResult *result;
    XrSourceValue *root,*parts[6];
} ProjectionOwners;
static XrSourceStatus projection_prepare(ProjectionOwners *owners,const XrCompileResourceLimits *limits) {
    runtime_fail_at=SIZE_MAX;
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=limits->allocated_bytes;
    options.live_bytes=limits->live_bytes;options.work=limits->work;
    XrSourceStatus status=xray_source_compiler_new(&options,&owners->compiler);if(status!=XR_SOURCE_OK)return status;
    static const char source[]="fn intrinsic(divisor:i64)->PanicInfo{try{print(10/divisor)}catch panic(p){return p};return intrinsic(divisor)}\n"
        "export fn data()->(string,(),i64?,PanicInfo){return (\"a\\0中\",(),41,intrinsic(0))}\n";
    XrSourceText text={NULL,"projection-prefix",source,sizeof(source)-1};
    status=xray_source_compile_text(owners->compiler,&text,&owners->program,NULL);if(status!=XR_SOURCE_OK)return status;
    XrSourceInvocation request;CHECK(xray_source_invocation_init(&request,sizeof(request))==XR_SOURCE_OK);
    request.name="data";request.name_length=4;XrSourceCallOptions runtime;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);runtime.write=discard;
    status=xray_source_invoke(owners->program,&request,&runtime,&owners->result,NULL);if(status!=XR_SOURCE_OK)return status;
    XrSourceEvent event;CHECK(xray_source_result_event(owners->result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
    status=xray_source_result_value(owners->result,&owners->root);if(status!=XR_SOURCE_OK)return status;
    xray_source_result_free(owners->result);owners->result=NULL;
    xray_source_program_free(owners->program);owners->program=NULL;
    xray_source_compiler_free(owners->compiler);owners->compiler=NULL;return XR_SOURCE_OK;
}
static XrSourceStatus projection_drive(ProjectionOwners *owners) {
    size_t count=0;CHECK(xray_source_value_tuple_count(owners->root,&count)==XR_SOURCE_OK && count==4);
    for(size_t i=0;i<4;++i) {
        XrSourceStatus status=xray_source_value_tuple_get(owners->root,i,&owners->parts[i]);
        if(status!=XR_SOURCE_OK)return status;
    }
    xray_source_value_free(owners->root);owners->root=NULL;
    int some=37;XrSourceStatus status=xray_source_value_nullable(owners->parts[2],&some,&owners->parts[4]);
    if(status!=XR_SOURCE_OK){CHECK(some==37 && !owners->parts[4]);return status;}
    CHECK(some==1);xray_source_value_free(owners->parts[2]);owners->parts[2]=NULL;
    int64_t code=0;CHECK(xray_source_value_panic_code(owners->parts[3],&code)==XR_SOURCE_OK && code==420);
    status=xray_source_value_panic_message(owners->parts[3],&owners->parts[5]);if(status!=XR_SOURCE_OK)return status;
    xray_source_value_free(owners->parts[3]);owners->parts[3]=NULL;
    const char *bytes=NULL;size_t length=0;static const char expected[]="a\0中";
    CHECK(xray_source_value_bytes(owners->parts[0],&bytes,&length)==XR_SOURCE_OK &&
        length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    CHECK(xray_source_value_bytes(owners->parts[5],&bytes,&length)==XR_SOURCE_OK && length==16 && !memcmp(bytes,"division by zero",16));
    XrSourceScalar scalar;CHECK(xray_source_value_scalar(owners->parts[4],&scalar)==XR_SOURCE_OK &&
        scalar.kind==XR_SOURCE_VALUE_I64 && scalar.value.signed_value==41);
    XrSourceValueKind kind;CHECK(xray_source_value_kind(owners->parts[1],&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_UNIT);
    return XR_SOURCE_OK;
}
static void projection_close(ProjectionOwners *owners) {
    xray_source_value_free(owners->root);for(size_t i=0;i<6;++i)xray_source_value_free(owners->parts[i]);
    xray_source_result_free(owners->result);xray_source_program_free(owners->program);xray_source_compiler_free(owners->compiler);
    *owners=(ProjectionOwners){0};CHECK(!runtime_live && !runtime_bytes);
}
static void projection_faults(bool probe) {
    const XrCompileResourceLimits limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    size_t count=0;
    for(size_t point=0;!point || (!probe && point<=count);++point) {
        ProjectionOwners owners={0};CHECK(projection_prepare(&owners,&limits)==XR_SOURCE_OK);
        runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;
        XrSourceStatus status=projection_drive(&owners);
        if(!point){CHECK(status==XR_SOURCE_OK && owners.parts[5]);count=runtime_attempts;CHECK(count);}
        else CHECK(status==XR_SOURCE_OUT_OF_MEMORY && !owners.parts[5] && runtime_attempts>runtime_fail_at);
        runtime_fail_at=SIZE_MAX;projection_close(&owners);
    }
    printf("public immutable projections actual sites=%zu swept=%zu; source/producer/parents dead, partial owners released, physical=0/0\n",
        count,probe?0:count);
}
static void projection_budgets(void) {
    const XrCompileResourceLimits generous={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    ProjectionOwners owners={0};XrCompileResourceStats required={0};
    CHECK(projection_prepare(&owners,&generous)==XR_SOURCE_OK && projection_drive(&owners)==XR_SOURCE_OK);
    CHECK(xr_compile_resources_stats(owners.parts[5]->resources,&required)==XR_COMPILE_RESOURCE_OK);projection_close(&owners);
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound>1 && *bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        XrSourceStatus status=projection_prepare(&owners,&limits);
        if(status==XR_SOURCE_OK)status=projection_drive(&owners);
        if(delta<0) {
            if(status!=XR_SOURCE_BUDGET || owners.parts[5])
                fprintf(stderr,"projection budget axis=%u delta=%d status=%u\n",axis,delta,(unsigned)status);
            CHECK(status==XR_SOURCE_BUDGET && !owners.parts[5]);
        } else CHECK(status==XR_SOURCE_OK && owners.parts[5]);
        projection_close(&owners);
    }
    printf("public projection original compiler axes allocated=%llu peak=%llu work=%llu; exact/minus-one/plus-one, physical=0/0\n",
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
}
#endif // SOURCE_PUBLIC_PROJECTION_RESOURCES_H
