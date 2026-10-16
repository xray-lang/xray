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
#include "xir/xxir_emit_c.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_derivation_json_decode_runtime.h"
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
static XrXirValue decode_execute(const XrXirCompileContext *context,XrXirSourceResult *user,const char *emit,const char *packet_path) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(user->checked,&packet,NULL)==XR_XIR_OK);
    if(packet_path){FILE *file=fopen(packet_path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}
    xr_xir_compile_source_result_free(user);XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t entries[5]={decode_entry(module,"main"),decode_entry(module,"decode"),decode_entry(module,"decodeJSON"),decode_entry(module,"jsonInteger"),UINT32_MAX};
    for(uint32_t f=0;f<module->function_count;++f)if(module->declarations->functions[f].method_kind==XR_XIR_CONSTRUCTOR) {
        CHECK(entries[4]==UINT32_MAX);entries[4]=f;
    }
    CHECK(entries[4]!=UINT32_MAX);
    if(emit) {
        XrXirCSource source={0};XrXirStatus status=xr_xir_compile_emit_c(lowered,"fixture_json_decode",8388608,&source);
        XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(context->resources,&spent)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"JSON decode emit status%u allocated=%llu peak=%llu work=%llu\n",(unsigned)status,
            (unsigned long long)spent.allocated_bytes,(unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);
        CHECK(status==XR_XIR_OK && !strstr(source.text,"xr_json_"));
        for(size_t i=0;i<source.length;++i)if(source.text[i]=='(') {
            size_t next=i+1;while(next<source.length && isspace((unsigned char)source.text[next]))++next;
            CHECK(next==source.length || source.text[next]!='{');
        }
        FILE *file=fopen(emit,"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length);
        CHECK(fprintf(file,"\nconst uint32_t fixture_json_decode_selection[5]={%uu,%uu,%uu,%uu,%uu};\n",entries[0],entries[1],entries[2],entries[3],entries[4])>0 && !fclose(file));
        xr_xir_compile_c_source_free(&source);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    decode_oracles(program,entries);XrXirValue value=decode_call(program,entries[1],"42",2);
    xr_xir_compile_program_drop(program);return value;
}
int main(int argc,char **argv) {
    bool emit=argc==4 && !strcmp(argv[1],"--emit-native");CHECK(argc==1 || emit);
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
        XrXirValue retained={0};
        if(authorized)retained=decode_execute(&context,&user,emit?argv[2]:NULL,emit?argv[3]:NULL);
        else xr_xir_compile_source_result_free(&user);
        XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(context.resources,&spent)==XR_COMPILE_RESOURCE_OK);
        if(authorized)printf("JSON decode complete pipeline allocated=%llu peak=%llu work=%llu\n",
            (unsigned long long)spent.allocated_bytes,(unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);
        xr_compile_resources_release(context.resources);
        if(authorized){decode_expected(&retained,true,42);xr_xir_value_drop(&retained);}
    }
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(transport.resources);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Source-free ordinary JSON decode derives a future one-i64 initializer; constructor authority required; physical0");return 0;
}
