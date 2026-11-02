/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_go_closure.c - Real closure environments across a Task boundary
 *
 * KEY CONCEPT:
 *   Cold metadata and execution outlive Source; a type never grants a target.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include "base/xresource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"go closure %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static uint64_t held(XrCompileResources *resources) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
static void execute(XrXirProgram *program,const uint32_t ids[2]) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    for(uint32_t n=0;n<2;++n) {
        CHECK(xr_xir_instance_start(instance,ids[n],NULL,0)==XR_XIR_CALL_READY);
        XrXirCallStatus status=XR_XIR_CALL_READY;
        for(uint32_t step=0;status==XR_XIR_CALL_READY;++step) {
            CHECK(step<100000);status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
        }
        if(status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"entry %u status %u\n",ids[n],status);
        CHECK(status==XR_XIR_CALL_RETURNED);
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        CHECK(result.type==XR_XIR_I64 && result.payload==(n?42u:0u));xr_xir_value_drop(&result);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
#if defined(XR_GO_CLOSURE_NATIVE)
XR_DATA const XrXirProgramSpec go_closure_program;
XR_DATA const uint32_t go_closure_ids[2];
#else
static XrXirStatus check(const XrXirCompileContext *context,const char *text,XrXirSourceResult *result,
    XrCompileRequestControl *control) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    if(control)CHECK(xr_compile_session_bind_request(session,control)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_GO_CLOSURE_FIXTURES};
    XrXirSourceRequest request={session,XR_GO_CLOSURE_FIXTURES "/closure.xr",&authority,context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=text?xr_xir_compile_source_check_text(&request,&(XrXirSourceText){"closure.xr",text,strlen(text)},result,&diagnostic,NULL):
        xr_xir_compile_source_check(&request,result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static bool cancelled(const void *pointer) {++*(unsigned *)pointer;return true;}
static void rejections(const XrXirCompileContext *context) {
    const char *cases[]={
        "fn bad(f:fn()->i64)->i64{return try! await go f()}",
        "fn bad()->i64{var value=42;return try! await go fn()->i64{return value}()}",
        "fn bad<T>(value:T)->T{return try! await go fn()->T{return value}()}",
        "fn bad()->i64{return try! await go fn(value:i64)->i64{return value}()}",
        "fn bad()->i64{return try! await go fn(value:i64)->i64{return value}(true)}",
        "fn bad()->i64{var value=42;return try! await go fn(v:ref i64)->i64{return v}(ref value)}",
        "class Box{value:i64=42}\nfn bad()->i64{const box=Box();return try! await go fn()->i64{return box.value}()}",
        "fn bad()->i64{var values=[42];const view:Slice<i64>=values[:];return try! await go fn()->i64{return view[0]}()}",
        "var state=42\nfn bad()->i64{return try! await go fn()->i64{return state}()}",
        "enum Failure{Bad}\nfn bad()->i64{const task=go fn() throws(Failure)->i64{throw Failure.Bad}();return await task}",
        "import \"./producer\" as producer\nfn bad()->i64{return try! await go producer.secret()}",
        "enum Failure{Bad}\nenum Other{Bad}\nfn bad(){const task:Task<i64,Other>=go fn() throws(Failure)->i64{throw Failure.Bad}()}"
    };
    for(size_t c=0;c<sizeof(cases)/sizeof(*cases);++c) {
        uint64_t before=held(context->resources);XrXirSourceResult result={0};
        fprintf(stderr,"negative %zu: ",c);
        XrXirStatus status=check(context,cases[c],&result,NULL);
        CHECK(status==(c==10?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE) && !result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result);CHECK(held(context->resources)==before);
    }
    for(unsigned mode=0;mode<2;++mode) {
        unsigned calls=0;XrCompileRequestOptions options={.cancel_poll=cancelled,.cancel_context=&calls};
        if(mode)options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};
        XrCompileRequestControl control={0};CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        uint64_t before=held(context->resources);XrXirSourceResult result={0};
        CHECK(check(context,NULL,&result,&control)==(mode?XR_XIR_DEADLINE:XR_XIR_CANCELLED));
        CHECK(!result.checked && !result.snapshot && calls==(mode?0u:1u));
        xr_xir_compile_source_result_free(&result);CHECK(held(context->resources)==before);
    }
    XrXirSourceResult recovered={0};uint64_t before=held(context->resources);
    CHECK(check(context,NULL,&recovered,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&recovered);
    CHECK(held(context->resources)==before);
}
#endif
int main(int argc,char **argv) {
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    uint64_t before=held(context.resources);XrXirProgram *program=NULL;
#if defined(XR_GO_CLOSURE_NATIVE)
    CHECK(argc==1);(void)argv;
    CHECK(xr_xir_compile_program_seal(&context,&go_closure_program,&program)==XR_XIR_OK);
    execute(program,go_closure_ids);
#else
    CHECK(argc==1 || argc==3);bool emit=argc==3 && !strcmp(argv[1],"--emit");CHECK(argc==1 || emit);
    XrXirSourceResult source={0};CHECK(check(&context,NULL,&source,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrXirArtifact *cold=NULL,*closed=NULL,*code=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(code);
    uint32_t ids[2]={module->declarations->entry_function,UINT32_MAX};
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==4 && !memcmp(module->functions[f].name,"main",4)) {CHECK(ids[1]==UINT32_MAX);ids[1]=f;}
    CHECK(ids[1]!=UINT32_MAX);
    if(emit) {
        XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(code,"go_closure",16777216,&generated)==XR_XIR_OK);
        FILE *file=fopen(argv[2],"wb");CHECK(file && !strstr(generated.text,"({"));
        CHECK(fwrite(generated.text,1,generated.length,file)==generated.length);
        CHECK(fprintf(file,"\nconst uint32_t go_closure_ids[2]={%u,%u};\n",ids[0],ids[1])>0 && !fclose(file));
        xr_xir_compile_c_source_free(&generated);xr_xir_compile_artifact_free(code);
    } else {
        CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK && !code);execute(program,ids);
        xr_xir_compile_program_drop(program);program=NULL;rejections(&context);
    }
#endif
    xr_xir_compile_program_drop(program);CHECK(held(context.resources)==before);
    xr_compile_resources_release(context.resources);puts("GO actual target, captures and retired metadata PASS");return 0;
}
