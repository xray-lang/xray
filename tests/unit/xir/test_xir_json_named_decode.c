/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_named_decode.c - Source-free generation for actual initializer names
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
#include "xir_json_named_decode_runtime.h"
#include "xir_generated_schema.h"
#include "xir_json_named_decode_source.h"
static XrXirCompileContext named_context(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static void named_source(const XrXirCompileContext *context,const char *text,XrXirLinkageKind kind,
    const XrXirLibraryCatalog *catalog,XrXirSourceResult *output,XrXirStatus expected) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    const char *logical=kind==XR_XIR_LIBRARY?"generator_json_named.xr":"future_json_named.xr";char path[2048];
    int n=snprintf(path,sizeof(path),"%s/%s",XR_JSON_NAMED_WORK,logical);CHECK(n>0 && n<(int)sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_NAMED_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog,NULL};
    XrXirSourceText source={logical,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"Named JSON Source stage%u status%u expected%u %d:%d %s allocated=%llu peak=%llu work=%llu\n",
        (unsigned)kind,(unsigned)status,(unsigned)expected,diagnostic.line,diagnostic.column,diagnostic.message,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    CHECK(status==expected);
    if(expected==XR_XIR_OK)CHECK(output->checked && output->snapshot);
    else CHECK(!output->checked && !output->snapshot && diagnostic.line==4 &&
        strstr(diagnostic.message,"ordinary derive output has invalid logical body data"));
    xr_compile_session_free(session);
}
static uint32_t named_entry(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name) &&
        !memcmp(module->functions[f].name,name,strlen(name))){CHECK(found==UINT32_MAX);found=f;}
    CHECK(found!=UINT32_MAX);return found;
}
static void named_execute(const XrXirCompileContext *context,XrXirCheckedPacket *packet,
    const char *left,const char *right,const char *emit) {
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet->bytes,packet->length,&received,NULL)==XR_XIR_OK);
    memset(packet->bytes,0xa5,packet->length);xr_xir_compile_checked_packet_free(packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t entries[]={named_entry(module,"main"),named_entry(module,"decode"),named_entry(module,"decodeJSON"),
        named_entry(module,"jsonIntegerObject"),UINT32_MAX};
    for(uint32_t f=0;f<module->function_count;++f)if(module->declarations->functions[f].method_kind==XR_XIR_CONSTRUCTOR) {
        CHECK(entries[4]==UINT32_MAX);entries[4]=f;
    }
    CHECK(entries[4]!=UINT32_MAX);bool calls=false,constructs=false;
    for(uint32_t i=0;i<module->functions[entries[2]].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[entries[2]].instructions[i];
        calls|=op->op==XR_XIR_CALL && op->immediate==entries[3];constructs|=op->op==XR_XIR_CALL && op->immediate==entries[4];
        CHECK(op->op!=XR_XIR_STRUCT_NEW && op->op!=XR_XIR_STRUCT_GET && op->op!=XR_XIR_STRUCT_SET && op->op!=XR_XIR_FIELD_PLACE);
    }
    CHECK(calls && constructs);
    if(emit) {
        XrXirCSource source={0};XrXirStatus emitted=xr_xir_compile_emit_c(lowered,"fixture_json_named",8388608,&source);
        XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(context->resources,&spent)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"Named JSON emit status%u allocated=%llu peak=%llu work=%llu\n",(unsigned)emitted,
            (unsigned long long)spent.allocated_bytes,(unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);
        CHECK(emitted==XR_XIR_OK && !strstr(source.text,"xr_json_"));
        for(size_t i=0;i<source.length;++i)if(source.text[i]=='(') {
            size_t next=i+1;while(next<source.length && isspace((unsigned char)source.text[next]))++next;
            CHECK(next==source.length || source.text[next]!='{');
        }
        FILE *file=fopen(emit,"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length);
        CHECK(fprintf(file,"\nconst uint32_t fixture_json_named_selection[5]={%uu,%uu,%uu,%uu,%uu};\n",
            entries[0],entries[1],entries[2],entries[3],entries[4])>0 && !fclose(file));
        xr_xir_compile_c_source_free(&source);
    }
    XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_vm_program_take(&lowered,&program);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"Named JSON Program status%u allocated=%llu peak=%llu work=%llu\n",(unsigned)status,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    CHECK(status==XR_XIR_OK && !lowered);named_oracles(program,entries,left,right);
    char text[256];int n=snprintf(text,sizeof(text),"{\"%s\":0,\"%s\":42}",left,right);CHECK(n>0 && n<(int)sizeof(text));
    XrXirValue retained=named_call(program,entries[1],text,(size_t)n);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context->resources);
    named_expected(&retained,true,42);xr_xir_value_drop(&retained);
}
static int named_cold(const char *path,const char *left,const char *right,const char *emit) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));
    long length=ftell(file);CHECK(length>0 && !fseek(file,0,SEEK_SET));XrXirCompileContext context=named_context();
    XrXirCheckedPacket packet={0};void *bytes=NULL;
    CHECK(xr_compile_resources_alloc(context.resources,(size_t)length,&bytes)==XR_COMPILE_RESOURCE_OK);
    packet.bytes=bytes;packet.length=(size_t)length;
    CHECK(fread(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
    puts("Independent cold runtime-control receiving; original full Source/comptime gate is separate");
    named_execute(&context,&packet,left,right,emit);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);return 0;
}
int main(int argc,char **argv) {
    if(argc==5 && !strcmp(argv[1],"--checked-diagnostic"))return named_cold(argv[2],argv[3],argv[4],NULL);
    if(argc==6 && !strcmp(argv[1],"--checked-native-diagnostic"))return named_cold(argv[2],argv[4],argv[5],argv[3]);
    bool emit=argc==4 && !strcmp(argv[1],"--emit-native");
    bool diagnostic=argc==3 && !strcmp(argv[1],"--write-runtime-diagnostic");CHECK(argc==1 || diagnostic || emit);
    char text[32000];XrXirCompileContext producer_context=named_context();
    int n=snprintf(text,sizeof(text),named_generator,generated_schema_source,XR_XIR_I64,XR_XIR_STRING,XR_XIR_TYPE_NULLABLE,XR_XIR_TYPE_ARRAY,
        XR_XIR_CONST_STRING,XR_XIR_ARRAY_NEW,XR_XIR_CALL,XR_XIR_NULLABLE_IS_SOME,XR_XIR_BRANCH,XR_XIR_NULLABLE_UNWRAP,
        XR_XIR_CONST_INT,XR_XIR_ARRAY_GET,XR_XIR_NULLABLE_SOME,XR_XIR_NULLABLE_NONE,XR_XIR_RETURN,XR_XIR_BOOL+1);
    CHECK(n>0 && n<(int)sizeof(text));size_t used=(size_t)n;
    const char *paths[]={XR_JSON_NUMBER_SOURCE,XR_JSON_STRING_SOURCE,XR_JSON_OBJECT_SOURCE};
    for(unsigned i=0;i<3;++i) {
        FILE *file=fopen(paths[i],"rb");CHECK(file);size_t size=fread(text+used,1,sizeof(text)-used-1,file);
        CHECK(!ferror(file) && feof(file) && !fclose(file));used+=size;CHECK(used<sizeof(text)-1);text[used++]='\n';
    }
    text[used]=0;XrXirSourceResult producer={0};named_source(&producer_context,text,XR_XIR_LIBRARY,NULL,&producer,XR_XIR_OK);
    memset(text,0xa5,sizeof(text));XrXirCheckedPacket original={0};
    CHECK(xr_xir_compile_checked_write(producer.checked,&original,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&producer);
    XrXirCompileContext transport=named_context();XrXirCheckedPacket packet={0};void *owned=NULL;
    CHECK(xr_compile_resources_alloc(transport.resources,original.length,&owned)==XR_COMPILE_RESOURCE_OK);
    packet.bytes=owned;packet.length=original.length;memcpy(packet.bytes,original.bytes,packet.length);
    memset(original.bytes,0xa5,original.length);xr_xir_compile_checked_packet_free(&original);xr_compile_resources_release(producer_context.resources);
    const char *choices[][4]={{"left","right","left","right"},{"right","left","left","right"},{"amount","count","amount","count"}};
    for(unsigned variant=0;variant<4;++variant) {
        unsigned definition=variant?variant-1:0;const char *const *names=choices[definition];
        XrXirCompileContext context=named_context();XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_NAMED_WORK},"generator_json_named.xr"};
        XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
        XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog)==XR_XIR_OK);
        memset(&input,0xa5,sizeof(input));memset(&binding,0xa5,sizeof(binding));
        n=snprintf(text,sizeof(text),named_user,variant?",construct:true":"",names[0],names[1],names[2],names[2],names[3],
            diagnostic?"":"comptime ",names[2],names[3]);CHECK(n>0 && n<(int)sizeof(text));
        XrXirSourceResult user={0};named_source(&context,text,XR_XIR_PROGRAM,catalog,&user,variant?XR_XIR_OK:XR_XIR_BAD_STRUCTURE);
        memset(text,0xa5,sizeof(text));xr_xir_compile_library_catalog_free(catalog);
        if(variant) {
            XrXirCheckedPacket result={0};CHECK(xr_xir_compile_checked_write(user.checked,&result,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&user);
            if(emit && definition==0) {
                FILE *file=fopen(argv[3],"wb");CHECK(file && fwrite(result.bytes,1,result.length,file)==result.length && !fclose(file));
            }
            if(diagnostic) {
                char path[2048];n=snprintf(path,sizeof(path),"%s/variant%u.chk",argv[2],definition);CHECK(n>0 && n<(int)sizeof(path));
                FILE *file=fopen(path,"wb");CHECK(file && fwrite(result.bytes,1,result.length,file)==result.length && !fclose(file));
                xr_xir_compile_checked_packet_free(&result);xr_compile_resources_release(context.resources);
            }else named_execute(&context,&result,names[2],names[3],emit && definition==0?argv[2]:NULL);
        }else {xr_xir_compile_source_result_free(&user);xr_compile_resources_release(context.resources);}
    }
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(transport.resources);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    puts(diagnostic?"Actual source-free g runtime-control packages written; no complete Source/comptime qualification":
        "Actual ordered/reordered/renamed initializer calls, no construct refusal and physical0");return 0;
}
