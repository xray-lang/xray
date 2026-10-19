/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derivation_json.c - Source-free ordinary JSON encoding
 *
 * KEY CONCEPT:
 *   A real ordinary generator iterates filtered future fields and emits a
 *   checked string-producing method. Runtime output has an independent oracle.
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_derivation_json_runtime.h"
#include "xir_generated_schema.h"
#include "xir_source_derivation_json_source.h"
#include "xir_source_derivation_json_resources.h"
static void json_source(const XrXirCompileContext *context,const char *text,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,XrXirSourceResult *output,XrXirStatus expected) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    const char *logical=kind==XR_XIR_LIBRARY?"generator_json.xr":"future_json.xr";
    char path[2048];int n=snprintf(path,sizeof(path),"%s/%s",XR_SOURCE_DERIVATION_JSON_WORK,logical);
    CHECK(n>0 && n<(int)sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_JSON_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog,NULL};
    XrXirSourceText source={logical,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
    if(status!=expected)fprintf(stderr,"ordinary JSON Source status%u %d:%d %s\n",
        (unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==expected);
    if(expected==XR_XIR_OK) CHECK(output->checked && output->snapshot);
    else CHECK(!output->checked && !output->snapshot && diagnostic.line==4 &&
        diagnostic.column>0 && strstr(diagnostic.message,"Future.left"));
    xr_compile_session_free(session);
    memset(path,0xa5,sizeof(path));
}
static void json_refuse_ungranted_public(const XrXirCheckedPacket *packet) {
    const XrXirCompileContext *context=source_fixture_source_owner(67108864,128000000);
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_JSON_WORK},"generator_json.xr"};
    XrXirLibraryInput input={packet->bytes,packet->length,{0},&binding,1};xr_sha256(packet->bytes,packet->length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_OK);
    const char *read=strstr(json_user_source,"read:[\"left\",\"secret\"");CHECK(read);read+=6;
    CHECK(!strncmp(read,"\"left\",",7));char negative[4096];
    int n=snprintf(negative,sizeof(negative),"%.*s%s",(int)(read-json_user_source),json_user_source,read+7);
    CHECK(n>0 && n<(int)sizeof(negative));XrXirSourceResult output={0};
    json_source(context,negative,XR_XIR_PROGRAM,catalog,&output,XR_XIR_BAD_TYPE);
    xr_xir_compile_source_result_free(&output);xr_xir_compile_library_catalog_free(catalog);
    source_fixture_source_owner_close(context);
}
static uint32_t json_function(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;size_t length=strlen(name);
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==length && !memcmp(module->functions[f].name,name,length)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    CHECK(found!=UINT32_MAX);return found;
}
static void json_execute(const XrXirCompileContext *context,XrXirSourceResult *source,const char *emit,const char *packet_path,bool faults) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);
    if (packet_path) {
        FILE *file=fopen(packet_path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
    }
    xr_xir_compile_source_result_free(source);XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t main=json_function(module,"main"),encode=json_function(module,"encode");
    if (emit) {
        uint32_t method=json_function(module,"encodeJSON");XrXirCSource output={0};
        CHECK(xr_xir_compile_emit_c(lowered,"fixture_json",8388608,&output)==XR_XIR_OK);
        for (size_t i=0;i<output.length;++i) if (output.text[i]=='(') {
            size_t next=i+1;while(next<output.length && isspace((unsigned char)output.text[next]))++next;
            CHECK(next==output.length || output.text[next]!='{');
        }
        FILE *file=fopen(emit,"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length);
        uint32_t quote=json_function(module,"jsonQuote");
        CHECK(fprintf(file,"\nconst uint32_t fixture_json_selection[4]={%uu,%uu,%uu,%uu};\n",main,encode,method,quote)>0 && !fclose(file));
        xr_xir_compile_c_source_free(&output);
    }
    uint32_t quote=json_function(module,"jsonQuote");
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    if(!faults) json_quote_oracles(program,quote);
    if(!faults) json_runtime_entered_oom(program,encode);
    size_t constant_sites=json_runtime_failures(program,main,false,faults),text_sites=json_runtime_failures(program,encode,true,faults);
    XrXirValue constant=json_runtime_take(program,main),text=json_runtime_take(program,encode);
    CHECK(constant.type==XR_XIR_I64 && !constant.reserved && constant.payload==42);xr_xir_value_drop(&constant);
    xr_xir_compile_program_drop(program);
    json_runtime_expected(&text,true);xr_xir_value_drop(&text);
    printf("ordinary JSON VM census: constant=%zu string=%zu; full runtime FI=%u\n",constant_sites,text_sites,faults);
}
int main(int argc,char **argv) {
    if (argc==7 && !strcmp(argv[1],"--compiler-fi")) {json_compiler_partition(argv);return 0;}
    bool generator_packet=argc==3 && !strcmp(argv[1],"--emit-generator");
    bool compiler=argc==2 && (!strcmp(argv[1],"--compiler-controls") || !strcmp(argv[1],"--compiler-resources"));
    bool emit=argc==4 && !strcmp(argv[1],"--emit-native"),faults=argc==2 && !strcmp(argv[1],"--runtime-resources");
    CHECK(argc==1 || emit || faults || compiler || generator_packet);
    XrCompileResourceLimits caps={67108864,8388608,128000000};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    char text[18000];int n=snprintf(text,sizeof(text),json_generator_format,generated_schema_source,
        XR_XIR_CONST_STRING,XR_XIR_STRUCT_GET,XR_XIR_TO_STRING,XR_XIR_CONCAT_STRING,XR_XIR_RETURN,XR_XIR_CALL,
        XR_XIR_I64,XR_XIR_BOOL,XR_XIR_STRING);
    CHECK(n>0 && n<(int)sizeof(text));FILE *helper=fopen(XR_JSON_ENCODING_SOURCE,"rb");CHECK(helper);
    size_t count=fread(text+n,1,sizeof(text)-(size_t)n-1,helper);CHECK(!ferror(helper) && feof(helper) && !fclose(helper));
    text[(size_t)n+count]=0;XrXirSourceResult producer={0};
    json_source(&context,text,XR_XIR_LIBRARY,NULL,&producer,XR_XIR_OK);memset(text,0xa5,sizeof(text));
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    const XrXirModule *library=xr_xir_compile_artifact_module(producer.checked);
    fprintf(stderr,"Ordinary JSON generator modules=%u\n",library->declarations->module_count);
    for(uint32_t m=0;m<library->declarations->module_count;++m) {
        const XrXirSourceModule *module=&library->declarations->modules[m];
        fprintf(stderr,"JSON module%u name%.*s\n",m,(int)module->name_length,module->name);
    }
    xr_xir_compile_source_result_free(&producer);
    if (generator_packet || compiler) {
        if (generator_packet) {
            FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
        } else {
            char path[2048];n=snprintf(path,sizeof(path),"%s/future_json.xr",XR_SOURCE_DERIVATION_JSON_WORK);
            CHECK(n>0 && n<(int)sizeof(path));json_compiler_resources(&packet,path,!strcmp(argv[1],"--compiler-resources"));
        }
        xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);return 0;
    }
    if (!emit && !faults) json_refuse_ungranted_public(&packet);
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_JSON_WORK},"generator_json.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;XrXirStatus catalog_status=xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog);
    fprintf(stderr,"Ordinary JSON catalog status%u\n",(unsigned)catalog_status);CHECK(catalog_status==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    memset(&binding,0xa5,sizeof(binding));memset(&input,0xa5,sizeof(input));XrXirSourceResult user={0};
    json_source(&context,json_user_source,XR_XIR_PROGRAM,catalog,&user,XR_XIR_OK);xr_xir_compile_library_catalog_free(catalog);
    json_execute(&context,&user,emit?argv[2]:NULL,emit?argv[3]:NULL,faults);CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
    printf("Ordinary JSON scalar-record encoder: dynamic future fields, explicit private READ, hidden string omitted, bool/escaped UTF8/comptime+VM oracle, producer death, physical0; allocated=%llu peak=%llu work=%llu\n",
        (unsigned long long)after.allocated_bytes,(unsigned long long)after.peak_bytes,(unsigned long long)after.work);return 0;
}
