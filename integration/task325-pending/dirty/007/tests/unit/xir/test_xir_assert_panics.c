/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_assert_panics.c - Typed callback outcomes and exact owner consumption
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_effects.h"
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)

#include "xir_assert_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_effects.c"
#include "xir/xxir_specialize.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
#include "xir_assert_panics_inbox_gates.inc.c"
static void panics_write(const char *path,const char *source) {
    FILE *file=fopen(path,"wb");CHECK(file);
    size_t length=strlen(source);CHECK(fwrite(source,1,length,file)==length && fclose(file)==0);
}
#include "xir_assert_panics_source_oom.inc.c"
static XrXirSourceResult panics_source(const char *directory,const char *path,const char *source,bool valid) {
    panics_write(path,source);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(assert_compile_context->resources,&session)==XR_COMPILER_SESSION_OK);CHECK(session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
    XrXirSourceRequest request={session,path,&authority,assert_compile_context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    xr_compile_session_free(session);
    if ((status==XR_XIR_OK)!=valid) fprintf(stderr,"check=%u %d:%d %s\n%s\n",status,diagnostic.line,diagnostic.column,diagnostic.message,source);
    CHECK((status==XR_XIR_OK)==valid && (result.checked!=NULL)==valid && (result.snapshot!=NULL)==valid);
    return result;
}
static uint32_t panics_find(const XrXirModule *module,const char *name) {
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))) return f;
    CHECK(false);return UINT32_MAX;
}
static void panics_role_rejections(const char *directory,const char *path) {
    const char *reject[]={
        "assertPanics<()>(fn(){})",
        "assertPanics<i64>(fn()->i64{return 1})",
        "assertPanics(fn(value:i64){})",
        "assertPanics(1)",
        "assertPanics(fn(){},null)",
        "assertPanics(fn(){},1)",
        "fn ordinary<T>(value:T){};ordinary<()>(())",
        "fn assertPanics(action:fn()->R,msg:string=\"\"){}",
        "fn unused<T>(value:T){assertPanics(value)}",
        "fn unused(){assertPanics(fn(){},null)}"
    };
    for (uint32_t i=0;i<sizeof(reject)/sizeof(reject[0]);++i) {
        XrXirSourceResult result=panics_source(directory,path,reject[i],false);xr_xir_compile_source_result_free(&result);
        CHECK(!runtime_live && !runtime_bytes);
    }
    const char *ordinary="fn assertPanics<T>(action:fn()->T,msg:string=\"ordinary\"){};assertPanics<i64>(fn()->i64{return 1})";
    XrXirSourceResult result=panics_source(directory,path,ordinary,true);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
    CHECK(view && view->module_count==1);
    for (uint32_t d=0;d<view->declaration_count;++d) CHECK(!view->declarations[d].native_identity);
    xr_xir_compile_source_result_free(&result);CHECK(!runtime_live && !runtime_bytes);
}
static void panics_query_roles(const XrXirSourceResult *result) {
    const XrXirModule *module=xr_xir_compile_artifact_module(result->checked);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result->snapshot);
    CHECK(view && view->complete && view->module_count==2 && module->defaults && module->defaults->count==3);
    uint32_t owner=panics_find(module,"assertPanics");
    CHECK(module->generics && module->generics[owner].parameter_count==1 && module->generics[owner].parameter_kinds &&
        module->generics[owner].parameter_kinds[0]==XR_XIR_BINDER_RESULT_VARIABLE);
    const XrXirDefaultBinding *binding=&module->defaults->records[1];
    CHECK(binding->owner==owner && binding->ordinal==1 && binding->owner_kind==XR_XIR_DEFAULT_PARAMETER);
    CHECK(!module->declarations->functions[binding->function].exported &&
        module->generics[binding->function].parameter_kinds && module->generics[binding->function].parameter_kinds[0]==XR_XIR_BINDER_RESULT_VARIABLE);
    uint32_t found=0;
    for (uint32_t d=0;d<view->declaration_count;++d) {
        const XrXirSourceDeclaration *decl=&view->declarations[d];
        if (decl->native_identity==XR_CORE_BUILTIN_ASSERT_PANICS) {
            CHECK(decl->kind==XR_XIR_SOURCE_INTRINSIC && decl->generic_parameter_count==1 &&
                decl->type_parameter_kinds && decl->type_parameter_kinds[0]==XR_XIR_BINDER_RESULT_VARIABLE);
            ++found;
        }
    }
    CHECK(found==1);
}
#include "xir_assert_panics_execution.inc.c"
#include "xir_assert_panics_packet.inc.c"
#include "xir_assert_panics_authority.inc.c"
#include "xir_assert_panics_pipeline_oom.inc.c"
#include "xir_assert_panics_resource.inc.c"
int main(int argc,char **argv) {
    assert_compile_begin();
    CHECK(argc==3 || argc==4);char path[2048];CHECK(snprintf(path,sizeof(path),"%s/panics.xr",argv[1])>0);
    if (argc==4) {
        panics_source_oom(argv[1],path);panics_pipeline_oom(argv[1],path);
        puts("Panics Source/pipeline/runtime actual OOM and exact budget PASS");assert_compile_end();return 0;
    }
    panics_inbox_gates();panics_role_rejections(argv[1],path);panics_packet_gates();
    panics_authority(argv[1],path);panics_execution(argv[1],path,argv[2]);panics_resource(argv[1],path);
    const char *source=
        "export fn expected()->i64 {assertPanics(fn(){var zero=0;var ignored=1/zero});return 7}\n"
        "export fn unitNormal()->string {try{assertPanics(fn(){},\"unit\")}catch panic(p){return p.message};return \"bad\"}\n"
        "export fn integerNormal()->i64 {try{assertPanics(fn()->i64{return 17})}catch panic(p){return p.code};return -1}\n"
        "export fn ownedNormal()->string {try{assertPanics(fn()->string{return \"owned-return\"},\"string\")}catch panic(p){return p.message};return \"bad\"}\n"
        "fn unused<T>(action:fn()->T){assertPanics(action)}\n";
    XrXirSourceResult result=panics_source(argv[1],path,source,true);panics_query_roles(&result);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);
    XrXirArtifact *read=NULL,*closed=NULL,*lowered=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_checked_read(assert_compile_context,packet.bytes,packet.length,&read,&diagnostic)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    XrXirStatus status=xr_xir_compile_specialize(read,&closed,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"specialize=%u f=%u b=%u i=%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);xr_xir_compile_artifact_free(read);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,&diagnostic)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const char *names[]={"expected","unitNormal","integerNormal","ownedNormal"};uint32_t entries[4];
    for (uint32_t i=0;i<4;++i) entries[i]=panics_find(xr_xir_compile_artifact_module(lowered),names[i]);
    XrXirCSource output={0};CHECK(xr_xir_compile_emit_c(lowered,"panics_checked",16777216,&output)==XR_XIR_OK);
    FILE *generated=fopen(argv[2],"wb");CHECK(generated);
    CHECK(fwrite(output.text,1,output.length,generated)==output.length);
    CHECK(fputs("\nconst uint32_t panics_checked_functions[4]={",generated)>=0);
    for (uint32_t i=0;i<4;++i) CHECK(fprintf(generated,"%s%uu",i ? "," : "",entries[i])>0);
    CHECK(fputs("};\n",generated)>=0 && fclose(generated)==0);xr_xir_compile_c_source_free(&output);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
    XrXirInstance *instance=NULL;XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    for (uint32_t i=0;i<4;++i) {
        CHECK(xr_xir_instance_start(instance,entries[i],NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance, UINT64_MAX);
        CHECK(polled.outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        if (i==0 || i==2) CHECK(value.type==XR_XIR_I64 && value.payload==(i==0 ? 7 : 445));
        else {const char *bytes=NULL;size_t length=0;const char *expected=i==1 ? "unit" : "string";
            CHECK(xr_xir_string_view(&value,&bytes,&length) && length==strlen(expected) && !memcmp(bytes,expected,length));}
        xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);CHECK(!runtime_live && !runtime_bytes);
    puts("Typed action Source/default/roles and owned inbox physical gates PASS");assert_compile_end();return 0;
}
