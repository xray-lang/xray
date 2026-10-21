/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_syntax.c - Ordinary JSON tokens from a cold checked library
 */
#include "base/xmalloc.h"
#include "base/xresource_limits.h"
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_syntax_runtime.h"
static const char syntax_consumer[]=
    "import {jsonValid} from \"./json_syntax\"\n"
    "export fn inspect(text:string)->bool {return jsonValid(text)}\n"
    "export fn main()->i64 {\n"
    " const token=comptime jsonValid(\"{\\\"label\\\":\\\"\\\\u6c49\\\\ud83d\\\\ude00\\\",\\\"items\\\":[null,true,false,-12.5e+4,{},[]]}\")\n"
    " return token?42:0\n"
    "}\n";
static const char syntax_runtime_consumer[]=
    "import {jsonValid} from \"./json_syntax\"\n"
    "export fn inspect(text:string)->bool {return jsonValid(text)}\n"
    "export fn main()->i64 {\n"
    " const token=jsonValid(\"{\\\"label\\\":\\\"\\\\u6c49\\\\ud83d\\\\ude00\\\",\\\"items\\\":[null,true,false,-12.5e+4,{},[]]}\")\n"
    " return token?42:0\n"
    "}\n";
static void syntax_source(const XrXirCompileContext *context,const char *text,XrXirLinkageKind kind,
    const XrXirLibraryCatalog *catalog,XrXirSourceResult *output) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    const char *logical=kind==XR_XIR_LIBRARY?"json_syntax.xr":"future_json_syntax.xr";char path[2048];
    int n=snprintf(path,sizeof(path),"%s/%s",XR_JSON_SYNTAX_WORK,logical);CHECK(n>0 && n<(int)sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_SYNTAX_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog};
    XrXirSourceText source={logical,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"JSON syntax stage%u status%u allocated=%llu peak=%llu work=%llu\n",(unsigned)kind,(unsigned)status,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    if(status!=XR_XIR_OK)fprintf(stderr,"Ordinary JSON syntax Source%u %d:%d %s\n",(unsigned)status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && output->checked && output->snapshot);xr_compile_session_free(session);
}
static uint32_t syntax_entry(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))) {
            CHECK(found==UINT32_MAX);found=f;
        }
    CHECK(found!=UINT32_MAX);return found;
}
/* Consumes the packet and the caller resource reference on every checked path. */
static int syntax_execute(const XrXirCompileContext *context,XrXirCheckedPacket *packet,
    const char *emit_path,bool diagnostic) {
    bool emit=emit_path!=NULL;
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet->bytes,packet->length,&received,NULL)==XR_XIR_OK);
    memset(packet->bytes,0xa5,packet->length);xr_xir_compile_checked_packet_free(packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);const char *names[]={"main","inspect","jsonValid","jsonStringEnd"};
    uint32_t entries[4];for(unsigned i=0;i<4;++i)entries[i]=syntax_entry(module,names[i]);
    if(emit) {
        XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(lowered,"fixture_json_syntax",8388608,&source)==XR_XIR_OK);
        CHECK(!strstr(source.text,"xr_json_"));
        for(size_t i=0;i<source.length;++i)if(source.text[i]=='(') {
            size_t next=i+1;while(next<source.length && isspace((unsigned char)source.text[next]))++next;
            CHECK(next==source.length || source.text[next]!='{');
        }
        FILE *file=fopen(emit_path,"wb");CHECK(file && fwrite(source.text,1,source.length,file)==source.length);
        CHECK(fprintf(file,"\nconst uint32_t fixture_json_syntax_selection[4]={%uu,%uu,%uu,%uu};\n",entries[0],entries[1],entries[2],entries[3])>0 && !fclose(file));
        xr_xir_compile_c_source_free(&source);
    }
    XrXirProgram *program=NULL;XrXirStatus taken=xr_xir_compile_vm_program_take(&lowered,&program);
    XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(context->resources,&spent)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"JSON syntax Program take%u output%u lowered%u allocated=%llu peak=%llu work=%llu\n",(unsigned)taken,
        program!=NULL,lowered!=NULL,(unsigned long long)spent.allocated_bytes,(unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);
    if(taken!=XR_XIR_OK) {
        CHECK(!program && lowered);xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context->resources);
        CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    CHECK(taken==XR_XIR_OK && !lowered);
    if(diagnostic)puts("Private runtime diagnostic only: the original Source comptime consumer remains a separate failing gate");
    json_syntax_oracles(program,entries);const char retained_input[]="[0]";
    XrXirValue retained=json_syntax_call(program,entries[1],retained_input,sizeof(retained_input)-1);
    xr_xir_compile_program_drop(program);XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context->resources);json_syntax_expected(&retained,true);xr_xir_value_drop(&retained);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("Ordinary JSON syntax received Program/VM/scalar result after producer and Program death/physical0: allocated=%llu peak=%llu work=%llu\n",
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);return 0;
}
static int syntax_cold_diagnostic(const char *path,const char *emit) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));
    long length=ftell(file);CHECK(length>0 && !fseek(file,0,SEEK_SET));
    XrXirCompileContext context={0};const XrCompileResourceLimits caps=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirCheckedPacket packet={0};void *bytes=NULL;
    CHECK(xr_compile_resources_alloc(context.resources,(size_t)length,&bytes)==XR_COMPILE_RESOURCE_OK);
    packet.bytes=bytes;packet.length=(size_t)length;
    CHECK(fread(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
    puts("Independent cold receiver of the actual runtime-control Checked packet; no Source comptime qualification");
    return syntax_execute(&context,&packet,emit,true);
}
int main(int argc,char **argv) {
    if(argc==4 && !strcmp(argv[1],"--checked-runtime-diagnostic"))return syntax_cold_diagnostic(argv[2],argv[3]);
    if(argc==3 && !strcmp(argv[1],"--checked-vm-diagnostic"))return syntax_cold_diagnostic(argv[2],NULL);
    bool diagnostic=argc>=2 && (!strcmp(argv[1],"--runtime-diagnostic") || !strcmp(argv[1],"--emit-runtime-diagnostic"));
    bool emit=argc==4 && (!strcmp(argv[1],"--emit-native") || !strcmp(argv[1],"--emit-runtime-diagnostic"));
    CHECK(argc==1 || emit || (argc==2 && diagnostic));
    char bytes[24000];size_t used=0;const char *paths[]={XR_JSON_STRING_SOURCE,XR_JSON_NUMBER_SOURCE,XR_JSON_SYNTAX_SOURCE};
    for(unsigned i=0;i<3;++i) {
        FILE *file=fopen(paths[i],"rb");CHECK(file);size_t n=fread(bytes+used,1,sizeof(bytes)-used-1,file);
        CHECK(!ferror(file) && feof(file) && !fclose(file));used+=n;CHECK(used<sizeof(bytes)-1);bytes[used++]='\n';
    }
    bytes[used]=0;XrXirCompileContext context={0};const XrCompileResourceLimits caps=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirSourceResult producer={0};syntax_source(&context,bytes,XR_XIR_LIBRARY,NULL,&producer);memset(bytes,0xa5,sizeof(bytes));
    XrXirCheckedPacket library={0};CHECK(xr_xir_compile_checked_write(producer.checked,&library,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_module(producer.checked)->declarations->module_count==1);xr_xir_compile_source_result_free(&producer);
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_JSON_SYNTAX_WORK},"json_syntax.xr"};
    XrXirLibraryInput input={library.bytes,library.length,{0},&binding,1};xr_sha256(library.bytes,library.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus catalog_status=xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog);
    if(catalog_status!=XR_XIR_OK)fprintf(stderr,"JSON syntax catalog status%u\n",(unsigned)catalog_status);
    CHECK(catalog_status==XR_XIR_OK);
    memset(library.bytes,0xa5,library.length);xr_xir_compile_checked_packet_free(&library);memset(&binding,0xa5,sizeof(binding));
    memset(&input,0xa5,sizeof(input));XrXirSourceResult user={0};
    syntax_source(&context,diagnostic?syntax_runtime_consumer:syntax_consumer,XR_XIR_PROGRAM,catalog,&user);
    xr_xir_compile_library_catalog_free(catalog);XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(user.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&user);
    if(emit){FILE *file=fopen(argv[3],"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}
    return syntax_execute(&context,&packet,emit?argv[2]:NULL,diagnostic);
}
