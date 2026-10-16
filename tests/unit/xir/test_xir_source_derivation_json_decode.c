/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derivation_json_decode.c - Ordinary decoder and authorized init
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_nullable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_generated_schema.h"
#include "xir_source_derivation_json_decode_source.h"
static XrXirCompileContext decode_context(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();return context;
}
static void decode_source(const XrXirCompileContext *context,const char *text,XrXirLinkageKind kind,
    const XrXirLibraryCatalog *catalog,XrXirSourceResult *output,XrXirStatus expected) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    const char *logical=kind==XR_XIR_LIBRARY?"generator_json_decode.xr":"future_json_decode.xr";char path[2048];
    int n=snprintf(path,sizeof(path),"%s/%s",XR_JSON_DECODE_WORK,logical);CHECK(n>0 && n<(int)sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_DECODE_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog,NULL};
    XrXirSourceText source={logical,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"JSON decode Source stage%u status%u %d:%d %s allocated=%llu peak=%llu work=%llu\n",(unsigned)kind,(unsigned)status,
        diagnostic.line,diagnostic.column,diagnostic.message,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    CHECK(status==expected);
    if(expected==XR_XIR_OK)CHECK(output->checked && output->snapshot);
    else CHECK(!output->checked && !output->snapshot && diagnostic.line==4 &&
        strstr(diagnostic.message,"ordinary derive output has invalid logical body data"));
    xr_compile_session_free(session);
}
static uint32_t decode_entry(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name) &&
        !memcmp(module->functions[f].name,name,strlen(name))){CHECK(found==UINT32_MAX);found=f;}
    CHECK(found!=UINT32_MAX);return found;
}
static XrXirValue decode_call(XrXirProgram *program,uint32_t entry,const char *text,size_t length) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValue argument={0},result={0};CHECK(xr_xir_string_new(domain,text,length,&argument)==XR_XIR_VALUE_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,&argument,1)==XR_XIR_CALL_READY);xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);
    CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return result;
}
static void decode_expected(const XrXirValue *value,bool expected,int64_t number) {
    bool some=false;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(value,&some,&payload) && some==expected);
    if(some)CHECK(payload && payload->type==XR_XIR_I64 && !payload->reserved && payload->payload==number);else CHECK(!payload);
}
static void decode_execute(const XrXirCompileContext *context,XrXirSourceResult *user) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(user->checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(user);XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t main=decode_entry(module,"main"),decode=decode_entry(module,"decode");
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    const struct {const char *text;bool some;int64_t number;} cases[]={
        {"0",true,0},{"-0",true,0},{"42",true,42},{"-7",true,0},{"9223372036854775807",true,INT64_MAX},
        {"-9223372036854775808",true,0},{"9223372036854775808",false,0},{"-9223372036854775809",false,0},
        {"0.0",false,0},{"1e0",false,0},{"01",false,0},{"-01",false,0},{"+1",false,0},{"",false,0},
        {"null",false,0},{"true",false,0},{" 1",false,0},{"1 ",false,0},{"\"1\"",false,0}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrXirValue value=decode_call(program,decode,cases[i].text,strlen(cases[i].text));
        decode_expected(&value,cases[i].some,cases[i].number);xr_xir_value_drop(&value);
    }
    const char nul[]={'1',0,'2'};XrXirValue value=decode_call(program,decode,nul,sizeof(nul));decode_expected(&value,false,0);xr_xir_value_drop(&value);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,main,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);value=decode_call(program,decode,"42",2);
    xr_xir_compile_program_drop(program);decode_expected(&value,true,42);xr_xir_value_drop(&value);
    puts("Ordinary derived JSON integer decoder:20 independent cases, exact init clamp/None/comptime42, input Domain and Program death");
}
int main(void) {
    XrXirCompileContext producer_context=decode_context();char text[18000];
    int n=snprintf(text,sizeof(text),json_decode_generator,generated_schema_source,XR_XIR_I64,XR_XIR_STRING,XR_XIR_TYPE_NULLABLE,
        XR_XIR_CALL,XR_XIR_NULLABLE_IS_SOME,XR_XIR_BRANCH,XR_XIR_NULLABLE_UNWRAP,XR_XIR_NULLABLE_SOME,XR_XIR_NULLABLE_NONE,XR_XIR_RETURN,XR_XIR_BOOL+1,XR_XIR_I64+1);
    CHECK(n>0 && n<(int)sizeof(text));FILE *file=fopen(XR_JSON_NUMBER_SOURCE,"rb");CHECK(file);
    size_t size=fread(text+n,1,sizeof(text)-(size_t)n-1,file);CHECK(!ferror(file) && feof(file) && !fclose(file));text[n+size]=0;
    XrXirSourceResult producer={0};decode_source(&producer_context,text,XR_XIR_LIBRARY,NULL,&producer,XR_XIR_OK);memset(text,0xa5,sizeof(text));
    XrXirCheckedPacket original={0};CHECK(xr_xir_compile_checked_write(producer.checked,&original,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&producer);
    XrXirCompileContext transport=decode_context();XrXirCheckedPacket packet={0};void *owned=NULL;
    CHECK(xr_compile_resources_alloc(transport.resources,original.length,&owned)==XR_COMPILE_RESOURCE_OK);
    packet.bytes=owned;packet.length=original.length;memcpy(packet.bytes,original.bytes,packet.length);
    memset(original.bytes,0xa5,original.length);xr_xir_compile_checked_packet_free(&original);xr_compile_resources_release(producer_context.resources);
    for(unsigned authorized=0;authorized<2;++authorized) {
        XrXirCompileContext context=decode_context();XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_DECODE_WORK},"generator_json_decode.xr"};
        XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
        XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog)==XR_XIR_OK);
        memset(&input,0xa5,sizeof(input));memset(&binding,0xa5,sizeof(binding));
        n=snprintf(text,sizeof(text),json_decode_user,authorized?",construct:true":"");CHECK(n>0 && n<(int)sizeof(text));
        XrXirSourceResult user={0};decode_source(&context,text,XR_XIR_PROGRAM,catalog,&user,authorized?XR_XIR_OK:XR_XIR_BAD_STRUCTURE);
        memset(text,0xa5,sizeof(text));xr_xir_compile_library_catalog_free(catalog);
        if(authorized)decode_execute(&context,&user);else xr_xir_compile_source_result_free(&user);
        xr_compile_resources_release(context.resources);
    }
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(transport.resources);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Source-free ordinary JSON decode derives a future one-i64 initializer; constructor authority required; physical0");return 0;
}
