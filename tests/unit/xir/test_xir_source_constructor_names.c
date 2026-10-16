/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_constructor_names.c - Current ordered initialization inputs
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
#include "xir_source_constructor_names_source.h"
static XrXirCompileContext names_context(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();return context;
}
static void names_source(const XrXirCompileContext *context,const char *text,XrXirLinkageKind kind,
    const XrXirLibraryCatalog *catalog,XrXirSourceResult *output,XrXirStatus expected) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    const char *logical=kind==XR_XIR_LIBRARY?"constructor_names.xr":"future_constructor_names.xr";char path[2048];
    int n=snprintf(path,sizeof(path),"%s/%s",XR_CTOR_NAMES_WORK,logical);CHECK(n>0 && n<(int)sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CTOR_NAMES_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog,NULL};
    XrXirSourceText source={logical,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
    fprintf(stderr,"Constructor names Source%u expected%u %d:%d %s\n",(unsigned)status,(unsigned)expected,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==expected);
    if(expected==XR_XIR_OK)CHECK(output->checked && output->snapshot);
    else CHECK(!output->checked && !output->snapshot && diagnostic.line==4 &&
        !strcmp(diagnostic.message,"derive startup failed"));
    xr_compile_session_free(session);
}
static uint32_t names_entry(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name) &&
        !memcmp(module->functions[f].name,name,strlen(name))){CHECK(found==UINT32_MAX);found=f;}
    CHECK(found!=UINT32_MAX);return found;
}
static XrXirValue names_execute(XrXirSourceResult *source) {
    XrXirArtifact *closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_specialize(source->checked,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(source);const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t entries[]={names_entry(module,"run"),names_entry(module,"main")};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirValue result={0};
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entries[i],NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        if(!i)result=value;else{CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==42);xr_xir_value_drop(&value);}
    }
    xr_xir_compile_program_drop(program);return result;
}
int main(void) {
    XrXirCompileContext producer_context=names_context();char text[12000];
    int n=snprintf(text,sizeof(text),names_generator,generated_schema_source,XR_XIR_CONST_STRING,XR_XIR_RETURN);CHECK(n>0 && n<(int)sizeof(text));
    XrXirSourceResult producer={0};names_source(&producer_context,text,XR_XIR_LIBRARY,NULL,&producer,XR_XIR_OK);memset(text,0xa5,sizeof(text));
    XrXirCheckedPacket original={0};CHECK(xr_xir_compile_checked_write(producer.checked,&original,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&producer);
    XrXirCompileContext transport=names_context();XrXirCheckedPacket packet={0};void *owned=NULL;
    CHECK(xr_compile_resources_alloc(transport.resources,original.length,&owned)==XR_COMPILE_RESOURCE_OK);
    packet.bytes=owned;packet.length=original.length;memcpy(packet.bytes,original.bytes,packet.length);
    memset(original.bytes,0xa5,original.length);xr_xir_compile_checked_packet_free(&original);xr_compile_resources_release(producer_context.resources);
    const struct {const char *left,*right,*expected;bool authorized,legacy;} cases[]={
        {"left","right","left:right",true,false},{"right","left","right:left",true,false},
        {"renamedLeft","renamedRight","renamedLeft:renamedRight",true,false},
        {"hiddenLeft","hiddenRight","none",false,false},{"left","right","unused",true,true}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrXirCompileContext context=names_context();XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CTOR_NAMES_WORK},"constructor_names.xr"};
        XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
        XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog)==XR_XIR_OK);
        memset(&input,0xa5,sizeof(input));memset(&binding,0xa5,sizeof(binding));
        n=snprintf(text,sizeof(text),names_user,cases[i].legacy?"legacy":"generate",cases[i].authorized?",construct:true":"",
            cases[i].left,cases[i].right,cases[i].left,cases[i].right,cases[i].expected);CHECK(n>0 && n<(int)sizeof(text));
        XrXirSourceResult user={0};names_source(&context,text,XR_XIR_PROGRAM,catalog,&user,cases[i].legacy?XR_XIR_BAD_TYPE:XR_XIR_OK);
        memset(text,0xa5,sizeof(text));xr_xir_compile_library_catalog_free(catalog);XrXirValue result={0};
        if(!cases[i].legacy)result=names_execute(&user);else xr_xir_compile_source_result_free(&user);
        xr_compile_resources_release(context.resources);
        if(!cases[i].legacy){const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&result,&bytes,&length) &&
            length==strlen(cases[i].expected) && !memcmp(bytes,cases[i].expected,length));xr_xir_value_drop(&result);}
    }
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(transport.resources);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Constructor names: actual two-i64 order/reorder/rename, no-construct empty names, old context refusal, producer/Source/Program/ledger death and physical0");return 0;
}
