/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_aggregate_encode.c - Real Array and nullable field encoding
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
#include "xir_json_aggregate_encode_source.h"
static XrXirCompileContext aggregate_context(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();return context;
}
#include "xir_json_aggregate_encode_negative.h"
static void aggregate_source(const XrXirCompileContext *context,const XrXirLibraryCatalog *catalog,
    const char *text,XrXirLinkageKind kind,XrXirSourceResult *output) {
    const char *logical=kind==XR_XIR_LIBRARY?"json_aggregates.xr":"future_json_aggregates.xr";char path[2048];
    int n=snprintf(path,sizeof(path),"%s/%s",XR_JSON_AGGREGATE_WORK,logical);CHECK(n>0 && n<(int)sizeof(path));
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_AGGREGATE_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog,NULL};
    XrXirSourceText input={logical,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,&diagnostic,NULL);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"Aggregate JSON Source%u status%u %d:%d %s allocated=%llu peak=%llu work=%llu\n",(unsigned)kind,(unsigned)status,
        diagnostic.line,diagnostic.column,diagnostic.message,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    CHECK(status==XR_XIR_OK && output->checked && output->snapshot);xr_compile_session_free(session);
}
static uint32_t aggregate_entry(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name) &&
        !memcmp(module->functions[f].name,name,strlen(name))){CHECK(found==UINT32_MAX);found=f;}
    CHECK(found!=UINT32_MAX);return found;
}
static XrXirValue aggregate_call(XrXirProgram *program,uint32_t entry) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,16384).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return value;
}
int main(int argc,char **argv) {
    bool negative=argc==2 && !strcmp(argv[1],"--negative");CHECK(argc==1 || negative);
    char source[24000];int n=snprintf(source,sizeof(source),aggregate_generator_format,generated_schema_source,
        XR_XIR_CONST_STRING,XR_XIR_STRUCT_GET,XR_XIR_TO_STRING,XR_XIR_CONCAT_STRING,XR_XIR_RETURN,XR_XIR_CALL,
        XR_XIR_I64,XR_XIR_TYPE_ARRAY,XR_XIR_TYPE_NULLABLE,XR_XIR_STRING);CHECK(n>0 && n<(int)sizeof(source));
    FILE *helper=fopen(XR_JSON_AGGREGATE_SOURCE,"rb");CHECK(helper);size_t count=fread(source+n,1,sizeof(source)-(size_t)n-1,helper);
    CHECK(!ferror(helper) && feof(helper) && !fclose(helper));source[n+count]=0;
    XrXirCompileContext producer=aggregate_context(),transport=aggregate_context();XrXirSourceResult result={0};
    aggregate_source(&producer,NULL,source,XR_XIR_LIBRARY,&result);memset(source,0xa5,sizeof(source));
    XrXirCheckedPacket original={0},packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&original,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);void *owned=NULL;
    CHECK(xr_compile_resources_alloc(transport.resources,original.length,&owned)==XR_COMPILE_RESOURCE_OK);
    packet.bytes=owned;packet.length=original.length;memcpy(packet.bytes,original.bytes,packet.length);
    memset(original.bytes,0xa5,original.length);xr_xir_compile_checked_packet_free(&original);xr_compile_resources_release(producer.resources);
    if(negative) {
        for(unsigned variant=0;variant<4;++variant)aggregate_reject(&packet,variant);
        memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(transport.resources);
        CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
        puts("Actual missing private READ and unsupported Array/nullable element types rejected with empty outputs and physical0");return 0;
    }
    XrXirCompileContext receiver=aggregate_context();XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_AGGREGATE_WORK},"json_aggregates.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&receiver,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(transport.resources);
    char future[sizeof(aggregate_user_source)];memcpy(future,aggregate_user_source,sizeof(future));
    aggregate_source(&receiver,catalog,future,XR_XIR_PROGRAM,&result);memset(future,0xa5,sizeof(future));xr_xir_compile_library_catalog_free(catalog);
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&result);
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(&receiver,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t entries[7];
    uint32_t generated=aggregate_entry(module,"encodeJSON"),array=aggregate_entry(module,"jsonIntegerArray"),optional=aggregate_entry(module,"jsonOptionalInteger");
    unsigned reads=0,calls=0;bool actual_array=false,actual_optional=false;
    for(uint32_t i=0;i<module->functions[generated].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[generated].instructions[i];
        if(op->op==XR_XIR_STRUCT_GET){CHECK(op->immediate==1 || op->immediate==2);++reads;}
        if(op->op==XR_XIR_CALL){CHECK(op->immediate==array || op->immediate==optional);++calls;actual_array|=op->immediate==array;actual_optional|=op->immediate==optional;}
        CHECK(op->op!=XR_XIR_STRUCT_NEW);
    }
    CHECK(reads==2 && calls==2 && actual_array && actual_optional);
    for(unsigned i=0;i<6;++i){char name[24];CHECK(snprintf(name,sizeof(name),"case%u",i)>0);entries[i]=aggregate_entry(module,name);}
    entries[6]=aggregate_entry(module,"main");XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_vm_program_take(&lowered,&program);
    XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(receiver.resources,&spent)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"Aggregate JSON Program%u allocated=%llu peak=%llu work=%llu\n",(unsigned)status,(unsigned long long)spent.allocated_bytes,
        (unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);CHECK(status==XR_XIR_OK && !lowered);
    XrXirValue values[6]={{0}};for(unsigned i=0;i<6;++i)values[i]=aggregate_call(program,entries[i]);
    XrXirValue answer=aggregate_call(program,entries[6]);CHECK(answer.type==XR_XIR_I64 && !answer.reserved && answer.payload==42);xr_xir_value_drop(&answer);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(receiver.resources);
    const char *expected[]={"{\"values\":[],\"optional\":null}","{\"values\":[1,-2,42],\"optional\":7}",
        "{\"values\":[-9223372036854775808,9223372036854775807],\"optional\":-9223372036854775808}",
        "{\"values\":[0,0,0],\"optional\":0}","{\"values\":[7],\"optional\":null}",NULL};
    char longer[2048];size_t used=(size_t)snprintf(longer,sizeof(longer),"{\"values\":[");
    for(int i=0;i<64;++i){n=snprintf(longer+used,sizeof(longer)-used,"%s%d",i?",":"",i-32);CHECK(n>0 && (size_t)n<sizeof(longer)-used);used+=(size_t)n;}
    CHECK(snprintf(longer+used,sizeof(longer)-used,"],\"optional\":9223372036854775807}")>0);expected[5]=longer;
    for(unsigned i=0;i<6;++i){const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&values[i],&bytes,&length) && length==strlen(expected[i]) && !memcmp(bytes,expected[i],length));xr_xir_value_drop(&values[i]);}
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Actual Array<i64>/nullable i64 JSON field READ and ordinary helper CALLs, six independent exact outputs, comptime42 and final String lifetime/physical0");return 0;
}
