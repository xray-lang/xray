/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_optional_chain.c - Real source, cold artifact and optional field execution
 *
 * KEY CONCEPT:
 *   Ordinary field authority and all ownership checks run before either backend.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Optional chain %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "optional_chain_fixture.h"
#include "optional_chain_runtime.h"
static XrXirStatus optional_check(const XrXirCompileContext *context,const char *text,
    XrCompileRequestControl *control,XrXirSourceResult *output) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    if(control)CHECK(xr_compile_session_bind_request(session,control)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"optional-chain",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText input={.text=text,.length=strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Optional Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static bool optional_cancel(const void *data){unsigned *calls=(unsigned *)data;++*calls;return true;}
static void optional_refusals(const XrXirCompileContext *context) {
    const char *const inputs[]={
        "type I={value:i64};type R={inner:I?};var r:R?=null;var bad=r?.inner.value\n",
        "type I={value:i64};var r:I?=null;var bad=(r?.value).missing\n",
        "type I={value:i64};var r:I?=null;var bad=r?.missing\n",
        "var values:Array<i64>?=null;var bad=values?[0]\n",
        "class C{private value:i64;init(){this.value=1}};var c:C?=null;var bad=c?.value\n",
        "type R={value:i64};var r:R?=null;r?.value=2\n",
        "class C{init(){};consume(v:i64)->i64{return v}};var c:C?=null;var bad=c?.consume(1/0)\n"
    };
    const XrXirStatus expected[]={XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,XR_XIR_BAD_TYPE,
        XR_XIR_BAD_TYPE,XR_XIR_BAD_STRUCTURE,XR_XIR_BAD_TYPE};
    _Static_assert(sizeof(inputs)/sizeof(*inputs)==sizeof(expected)/sizeof(*expected),"each refusal has an actual typed boundary");
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(unsigned i=0;i<sizeof(inputs)/sizeof(*inputs);++i) {
        XrXirSourceResult output={0};XrXirStatus status=optional_check(context,inputs[i],NULL,&output);
        CHECK(status==expected[i] && !output.checked && !output.snapshot);xr_xir_compile_source_result_free(&output);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    }
    for(unsigned i=0;i<2;++i) {
        unsigned calls=0;XrCompileRequestControl control={0};XrCompileRequestOptions options={.cancel_poll=optional_cancel,.cancel_context=&calls};
        if(i)options=(XrCompileRequestOptions){.deadline_mode=XR_COMPILE_DEADLINE_AFTER};
        CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
        XrXirSourceResult output={0};CHECK(optional_check(context,optional_chain_source,&control,&output)==(i?XR_XIR_DEADLINE:XR_XIR_CANCELLED));
        CHECK(!output.checked && !output.snapshot && calls==(i?0u:1u));xr_xir_compile_source_result_free(&output);
    }
    XrXirCompileContext small=*context;small.limits.instructions=1;XrXirSourceResult output={0};
    CHECK(optional_check(&small,optional_chain_source,NULL,&output)==XR_XIR_BUDGET && !output.checked && !output.snapshot);
    xr_xir_compile_source_result_free(&output);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==1||argc==2);XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    optional_refusals(&context);XrXirSourceResult source={0};CHECK(optional_check(&context,optional_chain_source,NULL,&source)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t ids[2]={UINT32_MAX,UINT32_MAX};
    const char *names[]={"test","escape"};
    for(uint32_t f=0;f<module->function_count;++f)for(unsigned n=0;n<2;++n)
        if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n])))ids[n]=f;
    CHECK(ids[0]!=UINT32_MAX && ids[1]!=UINT32_MAX);
    if(argc==2) {
        XrXirCSource code={0};CHECK(xr_xir_compile_emit_c(lowered,"optional_chain",8388608,&code)==XR_XIR_OK);
        FILE *file=fopen(argv[1],"wb");CHECK(file && fwrite(code.text,1,code.length,file)==code.length);
        CHECK(fprintf(file,"\nconst uint32_t optional_chain_ids[2]={%uu,%uu};\n",ids[0],ids[1])>0 && !fclose(file));
        xr_xir_compile_c_source_free(&code);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    optional_run(program,ids);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);puts("Optional fields/cold/VM/retirement PASS");return 0;
}
