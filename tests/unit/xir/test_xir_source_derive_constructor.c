/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derive_constructor.c - Explicit generated construction authority
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_generated_schema.h"
static const char constructor_generator[]=
    "%s"
    "type MethodInfo=(string,i64,i64,i64,i64)\n"
    "type CalleeInfo=(string,string,i64,i64,i64,i64)\n"
    "type DeriveContext=(Array<MethodInfo>,Array<CalleeInfo>,Array<i64>,i64,Array<string>)\n"
    "export fn generate(info:TypeInfo,context:DeriveContext)->Generated {\n"
    " const emptyFunctions:Array<FunctionRow>=[];const emptyParameters:Array<i64>=[]\n"
    " const emptyBlocks:Array<BlockRow>=[];const emptyCode:Array<InstructionRow>=[]\n"
    " const literals:Array<string>=[];const arguments:Array<i64>=[]\n"
    " if(context.3<0 || len(context.0)!=1){return (0,emptyFunctions,emptyParameters,emptyBlocks,emptyCode,emptyParameters,literals,arguments)}\n"
    " const method=context.0[0];const ctor=context.1[context.3]\n"
    " if(method.3!=0 || method.4!=0 || ctor.4!=1 || info.1[context.2[ctor.3]].1!=%u){\n"
    "  return (0,emptyFunctions,emptyParameters,emptyBlocks,emptyCode,emptyParameters,literals,arguments)\n }\n"
    " const code:Array<InstructionRow>=[(%u,-%u,0,0,0,0,-7,0,0,-1),\n"
    "  (%u,method.1,0,1,0,0,%s,0,0,-1),(%u,-1,2,0,0,0,0,0,0,-1)]\n"
    " const functions:Array<FunctionRow>=[(method.0,0,0,method.1,0,1,0,1,0,3,0,1,0,0)]\n"
    " const parameters:Array<i64>=[info.0];const blocks:Array<BlockRow>=[(0,3,0,0)];const operands:Array<i64>=[1]\n"
    " return (1,functions,parameters,blocks,code,operands,literals,arguments)\n}\n";
static const char constructor_user[]=
    "import {generate} from \"./constructor_generator\"\n"
    "interface Recreate<T> { recreate()->T }\n"
    "@derive(Recreate<Future>,generate%s)\n"
    "struct Future {private const amount:i64\n"
    " private init(value:i64){this.amount=(value<0)?0:value}\n"
    " static seed(value:i64)->Future{return Future(value)}\n"
    " observed()->i64{return this.amount} }\n"
    "export fn dynamic()->i64{return Future.seed(19).recreate().observed()}\n"
    "export fn main()->i64 {const folded=comptime Future.seed(27).recreate().observed();return (folded==0)?42:0}\n";
static void constructor_source(const XrXirCompileContext *context,const char *text,XrXirLinkageKind kind,
    const XrXirLibraryCatalog *catalog,XrXirSourceResult *output,XrXirStatus expected) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    const char *logical=kind==XR_XIR_LIBRARY?"constructor_generator.xr":"future_constructor.xr";char path[2048];
    int n=snprintf(path,sizeof(path),"%s/%s",XR_SOURCE_CONSTRUCTOR_WORK,logical);CHECK(n>0 && n<(int)sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_CONSTRUCTOR_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog,NULL};
    XrXirSourceText source={logical,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
    fprintf(stderr,"Source constructor expected%u status%u %d:%d %s\n",expected,(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==expected);
    if(expected==XR_XIR_OK)CHECK(output->checked && output->snapshot);
    else {
        CHECK(!output->checked && !output->snapshot);
        if(expected==XR_XIR_BAD_STRUCTURE)CHECK(diagnostic.line==4 &&
            strstr(diagnostic.message,"ordinary derive output has invalid logical body data"));
        if(expected==XR_XIR_BAD_TYPE)CHECK(diagnostic.line==3 &&
            strstr(diagnostic.message,"derived recreate failed checking for Future at instruction 1"));
    }
    xr_compile_session_free(session);
}
static XrXirCompileContext constructor_context(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();return context;
}
static uint32_t constructor_entry(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name) &&
        !memcmp(module->functions[f].name,name,strlen(name))){CHECK(found==UINT32_MAX);found=f;}
    CHECK(found!=UINT32_MAX);return found;
}
static void constructor_run(XrXirSourceResult *user) {
    XrXirArtifact *closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_specialize(user->checked,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(user);const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t entries[]={constructor_entry(module,"main"),constructor_entry(module,"dynamic")};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entries[i],NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        CHECK(result.type==XR_XIR_I64 && !result.reserved && result.payload==(i?0:42));xr_xir_value_drop(&result);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
}
static void constructor_cases(bool bypass) {
    XrXirCompileContext producer_context=constructor_context();char source[12000];
    int n=snprintf(source,sizeof(source),constructor_generator,generated_schema_source,XR_XIR_I64,
        XR_XIR_CONST_INT,XR_XIR_I64+1,bypass?XR_XIR_STRUCT_NEW:XR_XIR_CALL,bypass?"0":"-ctor.2-1",XR_XIR_RETURN);
    CHECK(n>0 && n<(int)sizeof(source));
    XrXirSourceResult producer={0};constructor_source(&producer_context,source,XR_XIR_LIBRARY,NULL,&producer,XR_XIR_OK);memset(source,0xa5,sizeof(source));
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&producer);
    XrXirCompileContext transport=constructor_context();XrXirCheckedPacket delivery={0};void *owned=NULL;
    CHECK(xr_compile_resources_alloc(transport.resources,packet.length,&owned)==XR_COMPILE_RESOURCE_OK);
    delivery.bytes=owned;delivery.length=packet.length;memcpy(delivery.bytes,packet.bytes,packet.length);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(producer_context.resources);
    packet=delivery;
    for(unsigned authorized=bypass?1:0;authorized<2;++authorized) {
        XrXirCompileContext context=constructor_context();XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_CONSTRUCTOR_WORK},"constructor_generator.xr"};
        XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
        XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog)==XR_XIR_OK);
        memset(&input,0xa5,sizeof(input));memset(&binding,0xa5,sizeof(binding));
        n=snprintf(source,sizeof(source),constructor_user,authorized?",construct:true":"");CHECK(n>0 && n<(int)sizeof(source));
        XrXirStatus expected=bypass?XR_XIR_BAD_TYPE:(authorized?XR_XIR_OK:XR_XIR_BAD_STRUCTURE);
        XrXirSourceResult user={0};constructor_source(&context,source,XR_XIR_PROGRAM,catalog,&user,expected);
        memset(source,0xa5,sizeof(source));xr_xir_compile_library_catalog_free(catalog);
        if(expected==XR_XIR_OK)constructor_run(&user);else xr_xir_compile_source_result_free(&user);
        xr_compile_resources_release(context.resources);
    }
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(transport.resources);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
}
int main(void) {
    constructor_cases(false);constructor_cases(true);
    puts("Source-derived private constructor: explicit authority, initializer clamp, direct construction refusal, generic requirement, producer death, comptime42/VM0 and physical0");return 0;
}
