/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_scope_source.c - Owned lexical groups across real Source exits
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
#define CHECK(c) do {if(!(c)){fprintf(stderr,"task scope %d: %s\n",__LINE__,#c);exit(1);}}while(0)
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
#if defined(XR_TASK_SCOPE_SOURCE_NATIVE)
XR_DATA const XrXirProgramSpec task_scope_source_program;
XR_DATA const uint32_t task_scope_source_ids[2];
#else
static XrXirStatus check(const XrXirCompileContext *context,const char *text,XrXirSourceResult *result,
    XrCompileRequestControl *control) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    if(control)CHECK(xr_compile_session_bind_request(session,control)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_TASK_SCOPE_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,XR_TASK_SCOPE_SOURCE_FIXTURES "/scope.xr",&authority,context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=text?xr_xir_compile_source_check_text(&request,&(XrXirSourceText){"scope.xr",text,strlen(text)},result,&diagnostic,NULL):
        xr_xir_compile_source_check(&request,result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static bool cancelled(const void *pointer) {++*(unsigned *)pointer;return true;}
static void rejections(const XrXirCompileContext *context) {
    const struct {const char *text;XrXirStatus status;} cases[]={
        {"scope {}",XR_XIR_BAD_STRUCTURE},
        {"scope {fn worker(){}}",XR_XIR_BAD_STRUCTURE},
        {"fn bad(f:fn()){scope {go f()}}",XR_XIR_BAD_TYPE},
        {"fn bad(){var x=42;scope {go fn(){print(x)}()}}",XR_XIR_BAD_TYPE},
        {"enum ErrorCode{Bad} fn bad(){scope {go fn() throws(ErrorCode){throw ErrorCode.Bad}()}}",XR_XIR_UNSUPPORTED}
    };
    for(size_t c=0;c<sizeof(cases)/sizeof(*cases);++c) {
        uint64_t before=held(context->resources);XrXirSourceResult result={0};
        XrXirStatus status=check(context,cases[c].text,&result,NULL);
        CHECK(status==cases[c].status && !result.checked && !result.snapshot);
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
#if defined(XR_TASK_SCOPE_SOURCE_NATIVE)
    CHECK(argc==1);(void)argv;
    CHECK(xr_xir_compile_program_seal(&context,&task_scope_source_program,&program)==XR_XIR_OK);
    execute(program,task_scope_source_ids);
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
        XrXirCSource generated={0};CHECK(xr_xir_compile_emit_c(code,"task_scope_source",16777216,&generated)==XR_XIR_OK);
        FILE *file=fopen(argv[2],"wb");CHECK(file && !strstr(generated.text,"({"));
        CHECK(fwrite(generated.text,1,generated.length,file)==generated.length);
        CHECK(fprintf(file,"\nconst uint32_t task_scope_source_ids[2]={%u,%u};\n",ids[0],ids[1])>0 && !fclose(file));
        xr_xir_compile_c_source_free(&generated);xr_xir_compile_artifact_free(code);
    } else {
        CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK && !code);execute(program,ids);
        xr_xir_compile_program_drop(program);program=NULL;rejections(&context);
    }
#endif
    xr_xir_compile_program_drop(program);CHECK(held(context.resources)==before);
    xr_compile_resources_release(context.resources);puts("Lexical groups, pending exits and retired metadata PASS");return 0;
}
