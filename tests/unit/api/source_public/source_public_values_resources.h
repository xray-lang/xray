/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_values_resources.h - Complete owned-value allocation prefixes
 */
#ifndef SOURCE_PUBLIC_VALUES_RESOURCES_H
#define SOURCE_PUBLIC_VALUES_RESOURCES_H
typedef struct ValueOwners {XrSourceCompiler *compiler;XrSourceValue *value,*copy;} ValueOwners;
static XrSourceStatus value_resources(unsigned mode,const XrCompileResourceLimits *limits,
    ValueOwners *owners,XrCompileResourceStats *stats) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=limits->allocated_bytes;
    options.live_bytes=limits->live_bytes;options.work=limits->work;
    XrSourceStatus status=xray_source_compiler_new(&options,&owners->compiler);if(status!=XR_SOURCE_OK)return status;
    const XrSourceScalar scalar={.kind=XR_SOURCE_VALUE_U64,.value.unsigned_value=UINT64_MAX};
    static const char bytes[]="original\0中value";
    status=mode ? xray_source_value_new_string(owners->compiler,bytes,sizeof(bytes)-1,&owners->value) :
        xray_source_value_new_scalar(owners->compiler,&scalar,&owners->value);
    if(status==XR_SOURCE_OK)status=xray_source_value_copy(owners->value,&owners->copy);
    if(status!=XR_SOURCE_OK)return status;
    xray_source_compiler_free(owners->compiler);owners->compiler=NULL;
    xray_source_value_free(owners->value);owners->value=NULL;
    if(stats)CHECK(xr_compile_resources_stats(owners->copy->resources,stats)==XR_COMPILE_RESOURCE_OK);
    if(mode) {
        const char *view=NULL;size_t length=0;
        CHECK(xray_source_value_bytes(owners->copy,&view,&length)==XR_SOURCE_OK &&
            length==sizeof(bytes)-1 && !memcmp(view,bytes,length));
    } else {XrSourceScalar actual;CHECK(xray_source_value_scalar(owners->copy,&actual)==XR_SOURCE_OK &&
        actual.kind==scalar.kind && actual.value.unsigned_value==UINT64_MAX);}
    return XR_SOURCE_OK;
}
static void value_close(ValueOwners *owners) {
    xray_source_compiler_free(owners->compiler);xray_source_value_free(owners->value);xray_source_value_free(owners->copy);
    *owners=(ValueOwners){0};CHECK(!runtime_live && !runtime_bytes);
}
static void value_faults(bool probe) {
    const XrCompileResourceLimits limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    for(unsigned mode=0;mode<2;++mode) {
        size_t count=0;
        for(size_t point=0;!point || (!probe && point<=count);++point) {
            runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;ValueOwners owners={0};
            XrSourceStatus status=value_resources(mode,&limits,&owners,NULL);
            if(!point){CHECK(status==XR_SOURCE_OK && owners.copy);count=runtime_attempts;CHECK(count);}
            else CHECK(status==XR_SOURCE_OUT_OF_MEMORY && !owners.copy && runtime_attempts>runtime_fail_at);
            runtime_fail_at=SIZE_MAX;
            if(owners.value) {
                size_t live=runtime_live,bytes=runtime_bytes;XrCompileResourceStats before,after;
                CHECK(xr_compile_resources_stats(owners.value->resources,&before)==XR_COMPILE_RESOURCE_OK);
                XrSourceValue *retry=NULL;CHECK(xray_source_value_copy(owners.value,&retry)==XR_SOURCE_OK && retry);
                CHECK(xr_compile_resources_stats(owners.value->resources,&after)==XR_COMPILE_RESOURCE_OK &&
                    after.allocated_bytes>before.allocated_bytes && after.work>before.work);
                xray_source_value_free(retry);CHECK(runtime_live==live && runtime_bytes==bytes);
            }
            value_close(&owners);
        }
        printf("public owned value mode=%u sites=%zu swept=%zu; producer death, original ledger retry, physical=0/0\n",
            mode,count,probe?0:count);
    }
}
static void value_budgets(void) {
    const XrCompileResourceLimits generous={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    for(unsigned mode=0;mode<2;++mode) {
        ValueOwners owners={0};XrCompileResourceStats required={0};
        CHECK(value_resources(mode,&generous,&owners,&required)==XR_SOURCE_OK);value_close(&owners);
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
            uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
            CHECK(*bound>1 && *bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
            XrSourceStatus status=value_resources(mode,&limits,&owners,NULL);
            if(delta<0)CHECK((status==XR_SOURCE_BUDGET || status==XR_SOURCE_LIMIT) && !owners.copy);
            else CHECK(status==XR_SOURCE_OK && owners.copy);
            value_close(&owners);
        }
    }
    puts("public scalar/String original cumulative allocation, live and work exact/minus-one/plus-one; physical=0/0");
}
static void value_sticky(void) {
    for(unsigned mode=0;mode<2;++mode) {
        XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
        options.stdlib_path=XR_PUBLIC_STDLIB;ValueOwners owners={0};
        CHECK(xray_source_compiler_new(&options,&owners.compiler)==XR_SOURCE_OK);
        CHECK(xray_source_value_new_string(owners.compiler,"original",8,&owners.value)==XR_SOURCE_OK);
        const XrSourceScalar scalar={.kind=XR_SOURCE_VALUE_I64,.value.signed_value=41};
        runtime_fail_at=runtime_attempts;
        XrSourceStatus status=mode ? xray_source_value_new_string(owners.compiler,"next",4,&owners.copy) :
            xray_source_value_new_scalar(owners.compiler,&scalar,&owners.copy);
        CHECK(status==XR_SOURCE_OUT_OF_MEMORY && !owners.copy && runtime_attempts==runtime_fail_at+1);
        runtime_fail_at=SIZE_MAX;size_t attempts=runtime_attempts;
        CHECK(xray_source_value_new_string(owners.compiler,"retry",5,&owners.copy)==XR_SOURCE_OUT_OF_MEMORY && !owners.copy);
        CHECK(xray_source_value_new_scalar(owners.compiler,&scalar,&owners.copy)==XR_SOURCE_OUT_OF_MEMORY && !owners.copy);
        XrSourceText text={NULL,"latched-constructor","print(1)",8};XrSourceProgram *program=NULL;
        CHECK(xray_source_compile_text(owners.compiler,&text,&program,NULL)==XR_SOURCE_OUT_OF_MEMORY && !program && runtime_attempts==attempts);
        xray_source_compiler_free(owners.compiler);owners.compiler=NULL;
        CHECK(xray_source_value_copy(owners.value,&owners.copy)==XR_SOURCE_OK && owners.copy);
        value_close(&owners);
    }
    puts("public constructors share the real CompileState first OOM; no retry allocations; old owned data remains usable");
}
typedef struct InvokeValues {
    XrSourceCompiler *compiler;
    XrSourceProgram *program;
    XrSourceCall *call;
    XrSourceResult *result;
    XrSourceValue *argument,*value,*copy;
} InvokeValues;
static void invoke_values_prepare(InvokeValues *owners) {
    runtime_fail_at=SIZE_MAX;
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=UINT64_C(64)<<20;
    options.live_bytes=UINT64_C(8)<<20;options.work=UINT64_C(128000000);
    CHECK(xray_source_compiler_new(&options,&owners->compiler)==XR_SOURCE_OK);
    static const char source[]="export fn echo(value:string)->string{Coro.yield();return value}\n";
    XrSourceText text={NULL,"typed-invocation-faults",source,sizeof(source)-1};
    CHECK(xray_source_compile_text(owners->compiler,&text,&owners->program,NULL)==XR_SOURCE_OK);
    static const char bytes[]="original\0中value";
    CHECK(xray_source_value_new_string(owners->compiler,bytes,sizeof(bytes)-1,&owners->argument)==XR_SOURCE_OK);
}
static XrSourceStatus invoke_values_drive(InvokeValues *owners) {
    XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);options.write=discard;
    const XrSourceValue *arguments[]={owners->argument};XrSourceInvocation invocation;
    CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.name="echo";invocation.name_length=4;invocation.arguments=arguments;invocation.argument_count=1;
    XrSourceStatus status=xray_source_invoke_begin(owners->program,&invocation,&options,&owners->call);
    if(status!=XR_SOURCE_OK)return status;
    xray_source_value_free(owners->argument);owners->argument=NULL;
    xray_source_program_free(owners->program);owners->program=NULL;
    xray_source_compiler_free(owners->compiler);owners->compiler=NULL;
    bool complete=false;
    for(unsigned steps=0;steps<4096;++steps) {
        XrSourceEvent event;status=xray_source_call_poll(owners->call,8,&event);if(status!=XR_SOURCE_OK)return status;
        if(event.state==XR_SOURCE_RUN_READY)continue;
        if(event.state==XR_SOURCE_RUN_SUSPENDED) {
            CHECK(xray_source_call_resume(owners->call,event.epoch,event.wake)==XR_SOURCE_OK);continue;
        }
        if(event.state==XR_SOURCE_RUN_OUT_OF_MEMORY)return XR_SOURCE_OUT_OF_MEMORY;
        CHECK(event.state==XR_SOURCE_RUN_RETURNED);complete=true;break;
    }
    CHECK(complete);status=xray_source_call_take(owners->call,&owners->result);if(status!=XR_SOURCE_OK)return status;
    status=xray_source_result_value(owners->result,&owners->value);if(status!=XR_SOURCE_OK)return status;
    status=xray_source_value_copy(owners->value,&owners->copy);if(status!=XR_SOURCE_OK)return status;
    const char *bytes=NULL;size_t length=0;static const char expected[]="original\0中value";
    CHECK(xray_source_value_bytes(owners->copy,&bytes,&length)==XR_SOURCE_OK &&
        length==sizeof(expected)-1 && !memcmp(bytes,expected,length));return XR_SOURCE_OK;
}
static void invoke_values_faults(bool probe) {
    size_t count=0;
    for(size_t point=0;!point || (!probe && point<=count);++point) {
        InvokeValues owners={0};invoke_values_prepare(&owners);
        runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;
        XrSourceStatus status=invoke_values_drive(&owners);
        if(!point){CHECK(status==XR_SOURCE_OK && owners.copy);count=runtime_attempts;CHECK(count);}
        else CHECK(status==XR_SOURCE_OUT_OF_MEMORY && !owners.copy && runtime_attempts>runtime_fail_at);
        runtime_fail_at=SIZE_MAX;
        if(owners.result && !owners.value) {
            CHECK(xray_source_result_value(owners.result,&owners.value)==XR_SOURCE_OK && owners.value);
        }
        CHECK(xray_source_call_drop(&owners.call,NULL)==XR_SOURCE_OK);
        xray_source_result_free(owners.result);xray_source_value_free(owners.argument);
        xray_source_value_free(owners.value);xray_source_value_free(owners.copy);
        xray_source_program_free(owners.program);xray_source_compiler_free(owners.compiler);
        CHECK(!runtime_live && !runtime_bytes);
    }
    printf("public typed invocation sites=%zu swept=%zu; argument/Program/Compiler death, yield, actual result/value copies, physical=0/0\n",
        count,probe?0:count);
}
static void invocation_name_guards(void) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;InvokeValues owners={0};
    CHECK(xray_source_compiler_new(&options,&owners.compiler)==XR_SOURCE_OK);
    static const char source[]="export fn identity<T>(value:T)->T{return value}\n"
        "export fn force()->i64{return identity<i64>(41)}\n";
    XrSourceText text={NULL,"invocation-source-names",source,sizeof(source)-1};
    CHECK(xray_source_compile_text(owners.compiler,&text,&owners.program,NULL)==XR_SOURCE_OK);
    const XrSourceScalar scalar={.kind=XR_SOURCE_VALUE_I64,.value.signed_value=41};
    CHECK(xray_source_value_new_scalar(owners.compiler,&scalar,&owners.argument)==XR_SOURCE_OK);
    XrXirSourceProductPacketView packet={0};XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_source_product_packet(owners.program->product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(xr_xir_compile_source_product_context(owners.program->product),
        packet.bytes,packet.length,&closed,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(closed);unsigned matched=0;
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    const XrSourceValue *arguments[]={owners.argument};XrSourceInvocation invocation;
    CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.arguments=arguments;invocation.argument_count=1;
    for(uint32_t i=0;i<module->function_count;++i) {
        const XrXirFunction *function=&module->functions[i];
        if(function->name_length<9 || memcmp(function->name,"identity$",9))continue;
        CHECK(!module->declarations->functions[i].exported);
        ++matched;invocation.name=function->name;invocation.name_length=function->name_length;
        XrSourceStatus status=xray_source_invoke_begin(owners.program,&invocation,&runtime,&owners.call);
        fprintf(stderr,"actual closed specialization %.*s public begin=%u\n",(int)function->name_length,function->name,(unsigned)status);
        CHECK(status==XR_SOURCE_NOT_FOUND && !owners.call);
    }
    CHECK(matched==1);invocation.name="identity";invocation.name_length=8;
    CHECK(xray_source_invoke_begin(owners.program,&invocation,&runtime,&owners.call)==XR_SOURCE_NOT_FOUND && !owners.call);
    xr_xir_compile_artifact_free(closed);xray_source_value_free(owners.argument);
    xray_source_program_free(owners.program);xray_source_compiler_free(owners.compiler);
    CHECK(!runtime_live && !runtime_bytes);
}
#endif // SOURCE_PUBLIC_VALUES_RESOURCES_H
