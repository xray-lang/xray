/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_math_source.c - Complete math source, retired packets and paired execution
 *
 * KEY CONCEPT:
 *   Original contract assertions remain in the input. Authentic private leaves
 *   share VM, generated C and CTFE while scalar range faults retain their channel.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_native_callable.h"
#include "toolchain/xcompiler_session.h"
#include "base/xresource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"math Source %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#if !defined(XR_MATH_NATIVE)
#include "math_source_memory.h"
#endif
static XrXirOutputStatus output(void *unused,const XrXirOutputGroup *group) {
    (void)unused;
    if(!group || group->stream!=XR_XIR_STDOUT || group->count!=1 || !group->line)return XR_XIR_OUTPUT_BAD_ARGUMENT;
    const char *bytes=NULL;size_t length=0;
    if(!xr_xir_string_view(group->values,&bytes,&length))return XR_XIR_OUTPUT_BAD_ARGUMENT;
    return fwrite(bytes,1,length,stdout)==length && fputc('\n',stdout)!=EOF?XR_XIR_OUTPUT_OK:XR_XIR_OUTPUT_ERROR;
}
static void execute(XrXirProgram *program,const uint32_t ids[8]) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,output,NULL};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    for(uint32_t n=0;n<8;++n) {
        CHECK(xr_xir_instance_start(instance,ids[n],NULL,0)==XR_XIR_CALL_READY);
        XrXirCallStatus status=XR_XIR_CALL_READY;
        for(uint32_t step=0;status==XR_XIR_CALL_READY;++step) {
            CHECK(step<500000);status=xr_xir_instance_poll_bounded(instance,16).outcome.status;
        }
        if(status!=((n>=3&&n<7)?XR_XIR_CALL_NUMERIC_RANGE:XR_XIR_CALL_RETURNED))
            fprintf(stderr,"math entry%u actual%u\n",n,status);
        CHECK(status==((n>=3&&n<7)?XR_XIR_CALL_NUMERIC_RANGE:XR_XIR_CALL_RETURNED));
        if(n<3||n==7) {
            XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            CHECK(value.type==(uint32_t)(n==2?XR_XIR_F64:XR_XIR_I64));
            CHECK((uint64_t)value.payload==(n==2?UINT64_C(0x8000000000000000):n?42u:0u));xr_xir_value_drop(&value);
        }
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
#if defined(XR_MATH_NATIVE)
XR_DATA const XrXirProgramSpec math_source_program;
XR_DATA const uint32_t math_source_ids[8];
#else
static uint32_t named(const XrXirModule *module,const char *name) {
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(name)&&!memcmp(module->functions[f].name,name,strlen(name)))return f;
    CHECK(false);return UINT32_MAX;
}
static XrXirStatus check(const XrXirCompileContext *context,const char *text,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"math-source",XR_MATH_ROOT};
    XrXirSourceRequest request={session,XR_MATH_ROOT "/tests/fixtures/xir_math/driver.xr",&authority,
        context,XR_MATH_ROOT "/stdlib",NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={"tests/fixtures/xir_math/driver.xr",text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"math check%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static size_t append_file(char *text,size_t used,size_t capacity,const char *path) {
    FILE *file=fopen(path,"rb");CHECK(file);size_t length=fread(text+used,1,capacity-used-1,file);
    CHECK(length&&feof(file)&&!ferror(file)&&!fclose(file));text[used+length]=0;return used+length;
}
#include "math_source_cold.h"
static bool math_cancel(const void *unused) {(void)unused;return true;}
static void math_source_resources(const XrXirCompileContext *context,const char *text) {
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(unsigned mode=0;mode<2;++mode) {
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
        XrCompileRequestControl control={0};XrCompileRequestOptions options={.cancel_poll=math_cancel};
        if(mode) {
            CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
            CHECK(xr_compile_session_bind_request(session,&control)==XR_COMPILE_RESOURCE_OK);
        } else math_fail_next=true;
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"math-source",XR_MATH_ROOT};
        XrXirSourceRequest request={session,XR_MATH_ROOT "/tests/fixtures/xir_math/driver.xr",&authority,
            context,XR_MATH_ROOT "/stdlib",NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceText input={"tests/fixtures/xir_math/driver.xr",text,strlen(text)};XrXirSourceResult result={0};
        XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,NULL,NULL);
        CHECK(status==(mode?XR_XIR_CANCELLED:XR_XIR_OUT_OF_MEMORY)&&!result.checked&&!result.snapshot);
        CHECK(!math_fail_next&&math_allocation_failures==1);
        xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(before.held_bytes==after.held_bytes);
    }
    XrXirSourceResult restored={0};CHECK(check(context,text,&restored)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&restored);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.held_bytes==after.held_bytes);
}

static void controls(const XrXirCompileContext *context,const XrXirArtifact *code) {
    const XrXirModule *module=xr_xir_compile_artifact_module(code);uint32_t found=0;
    for(uint32_t f=0;f<module->function_count;++f) {
        uint32_t id=module->declarations->functions[f].native_callable;if(!xir_math_native_id(id))continue;
        CHECK(xir_native_callable_verify(module,f)==XR_XIR_OK);++found;
        XrXirFunction changed=module->functions[f];changed.result=XR_XIR_STRING;
        CHECK(!xir_math_native_signature(id,changed.parameters,changed.parameter_count,changed.result,changed.error_type));
        XrXirSourceModule wrong=module->declarations->modules[module->declarations->functions[f].module];
        wrong.name="math";wrong.name_length=4;
        XrXirDeclarations declarations=*module->declarations;XrXirFunctionIdentity identity={0};
        identity.module=0;declarations.modules=&wrong;declarations.module_count=1;declarations.functions=&identity;
        CHECK(!xir_math_native_scope(&declarations,0,1));
    }
    CHECK(found==28);
    const XrXirTarget leaf_target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
    for(uint32_t f=0;f<module->function_count;++f) {
        uint32_t id=module->declarations->functions[f].native_callable;if(!xir_math_native_id(id))continue;
        const XrXirFunction *function=module->functions+f;XrXirValue arguments[2]={{0}},result={0};
        for(uint32_t p=0;p<function->parameter_count;++p)arguments[p].type=function->parameters[p];
        XrXirCtfeCallRequest request={.evaluation={.artifact=code,.function=f,.target=leaf_target,
            .arguments=function->parameter_count?arguments:NULL,.argument_count=function->parameter_count,
            .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},
            .call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
        memcpy(request.evaluation.checked_identity,code->checked_identity,32);
        XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&request,&result);
        if(status!=(xir_math_native_random(id)?XR_XIR_CTFE_NOT_SUPPORTED:XR_XIR_CTFE_OK))
            fprintf(stderr,"math native CTFE id%u status%u\n",id,status);
        CHECK(status==(xir_math_native_random(id)?XR_XIR_CTFE_NOT_SUPPORTED:XR_XIR_CTFE_OK));
        if(!xir_math_native_random(id))CHECK(result.type==(uint32_t)function->result&&!result.reserved);
        else CHECK(!result.type&&!result.reserved&&!result.payload);
        xr_xir_value_drop(&result);
        if(id==XR_NATIVE_CALLABLE_MATH_FLOOR || id==XR_NATIVE_CALLABLE_MATH_CEIL ||
            id==XR_NATIVE_CALLABLE_MATH_ROUND || id==XR_NATIVE_CALLABLE_MATH_TRUNC) {
            uint64_t bits=id==XR_NATIVE_CALLABLE_MATH_FLOOR?UINT64_C(0x7ff0000000000000):
                id==XR_NATIVE_CALLABLE_MATH_CEIL?UINT64_C(0x7ff8000000000000):
                id==XR_NATIVE_CALLABLE_MATH_ROUND?UINT64_C(0x43e0000000000000):UINT64_C(0xc3e0000000000001);
            memcpy(&arguments[0].payload,&bits,sizeof(bits));
            XrXirCtfeStatus ranged=xr_xir_compile_ctfe_calls(&request,&result);
            if(ranged!=XR_XIR_CTFE_NUMERIC_RANGE)fprintf(stderr,"math range CTFE id%u status%u\n",id,ranged);
            CHECK(ranged==XR_XIR_CTFE_NUMERIC_RANGE);
            CHECK(!result.type&&!result.reserved&&!result.payload);
        }
    }
    static const char *const rejected[]={
        "__sqrt(4.0)","import math;math.sqrt(\"bad\")", "import math;math.pow(2.0)",
        "import math;const bad:string=math.sqrt(4.0)","import math;math.random(1)",
        "import math;math.randomInt(1.5,2)","import math;math.__sqrt(4.0)"};
    for(uint32_t c=0;c<sizeof(rejected)/sizeof(*rejected);++c) {
        XrXirSourceResult result={0};CHECK(check(context,rejected[c],&result)==(c==0?XR_XIR_BAD_VALUE:c==6?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE)&&!result.checked&&!result.snapshot);
        xr_xir_compile_source_result_free(&result);
    }
    XrXirSourceResult shadow={0};CHECK(check(context,"fn __sqrt(x:f64)->f64{return x};assert(__sqrt(42.0)==42.0)",&shadow)==XR_XIR_OK);
    module=xr_xir_compile_artifact_module(shadow.checked);
    CHECK(!module->declarations->functions[named(module,"__sqrt")].native_callable);xr_xir_compile_source_result_free(&shadow);
    const XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};module=xr_xir_compile_artifact_module(code);
    for(uint32_t n=0;n<3;++n) {
        XrXirCtfeCallRequest request={.evaluation={.artifact=code,.function=named(module,n==2?"caughtMathRange":n?"compileRandom":"compilePure"),
            .target=target,.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},
            .call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
        memcpy(request.evaluation.checked_identity,code->checked_identity,32);XrXirValue result={0};
        XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&request,&result);
        if(status!=(n==1?XR_XIR_CTFE_NOT_SUPPORTED:XR_XIR_CTFE_OK))fprintf(stderr,"math CTFE%u status%u\n",n,status);
        CHECK(status==(n==1?XR_XIR_CTFE_NOT_SUPPORTED:XR_XIR_CTFE_OK));
        if(!n)CHECK(result.type==XR_XIR_F64&&result.payload==UINT64_C(0x4031000000000000));
        else if(n==2)CHECK(result.type==XR_XIR_I64&&result.payload==42);
        else CHECK(!result.type&&!result.reserved&&!result.payload);
        xr_xir_value_drop(&result);
    }
}
#endif
int main(int argc,char **argv) {
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);XrXirProgram *program=NULL;
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
#if defined(XR_MATH_NATIVE)
    CHECK(argc==1);(void)argv;CHECK(xr_xir_compile_program_seal(&context,&math_source_program,&program)==XR_XIR_OK);
    execute(program,math_source_ids);xr_xir_compile_program_drop(program);
#else
    if(argc==3&&!strcmp(argv[1],"--previous122")) {
        unsigned char packet[8192];FILE *file=fopen(argv[2],"rb");CHECK(file);
        size_t length=fread(packet,1,sizeof(packet),file);CHECK(length&&feof(file)&&!ferror(file)&&!fclose(file));
        CHECK(packet[8]==41&&packet[12]==122&&XR_XIR_CHECKED_CONTRACT==123);
        XrXirArtifact *rejected=NULL;
        CHECK(xr_xir_compile_checked_read(&context,packet,length,&rejected,NULL)==XR_XIR_BAD_STRUCTURE&&!rejected);
        XrCompileResourceStats restored={0};CHECK(xr_compile_resources_stats(context.resources,&restored)==XR_COMPILE_RESOURCE_OK);
        CHECK(restored.held_bytes==baseline.held_bytes);
        xr_compile_resources_release(context.resources);return 0;
    }
    CHECK(argc==1||(argc==3&&!strcmp(argv[1],"--emit")));char text[32768]={0};
    size_t length=append_file(text,0,sizeof(text),XR_MATH_ROOT "/tests/stdlib/contracts/math/probes/current.xr");
    length=append_file(text,length,sizeof(text),XR_MATH_ROOT "/tests/fixtures/xir_math/driver.xr");CHECK(length);
    math_source_resources(&context,text);
    XrXirSourceResult warm={0};CHECK(check(&context,text,&warm)==XR_XIR_OK);xr_xir_compile_source_result_free(&warm);
    XrXirSourceResult source=math_cold_source(&context,text);memset(text,'!',sizeof(text));
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *cold=NULL,*closed=NULL,*code=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(code);
    uint32_t ids[8]={module->declarations->entry_function,named(module,"main"),named(module,"negativeZero"),
        named(module,"badFloor"),named(module,"badCeil"),named(module,"badRound"),named(module,"badTrunc"),named(module,"caughtMathRange")};
    if(argc==3) {
        XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(code,"math_source",16777216,&generated)==XR_XIR_OK);
        FILE *file=fopen(argv[2],"wb");CHECK(file&&fwrite(generated.text,1,generated.length,file)==generated.length);
        CHECK(fprintf(file,"\nconst uint32_t math_source_ids[8]={%u,%u,%u,%u,%u,%u,%u,%u};\n",
            ids[0],ids[1],ids[2],ids[3],ids[4],ids[5],ids[6],ids[7])>0&&!fclose(file));
        xr_xir_compile_c_source_free(&generated);xr_xir_compile_artifact_free(code);
    } else {
        controls(&context,code);CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK&&!code);
        execute(program,ids);xr_xir_compile_program_drop(program);
    }
#endif
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"math held baseline=%llu actual=%llu\n",(unsigned long long)baseline.held_bytes,(unsigned long long)stats.held_bytes);
    CHECK(stats.held_bytes==baseline.held_bytes);
    xr_compile_resources_release(context.resources);return 0;
}
