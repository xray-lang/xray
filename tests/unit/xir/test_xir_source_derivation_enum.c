/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derivation_enum.c - Ordinary enum consumer after producer death
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_derivation_enum_runtime.h"
#include "xir_generated_schema.h"
#include "xir_source_derivation_enum_source.h"
static void enum_source(const XrXirCompileContext *context,const char *text,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,XrXirSourceResult *output) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    const char *logical=kind==XR_XIR_LIBRARY?"generator_enum.xr":"future_enum.xr";
    char path[2048];int n=snprintf(path,sizeof(path),"%s/%s",XR_SOURCE_DERIVATION_ENUM_WORK,logical);
    CHECK(n>0 && n<(int)sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_ENUM_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog,NULL};
    XrXirSourceText source={logical,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"ordinary enum Source status%u %d:%d %s\n",
        (unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && output->checked && output->snapshot);xr_compile_session_free(session);
    memset(path,0xa5,sizeof(path));
}
#include "xir_source_derivation_enum_negative.h"
static uint32_t enum_function(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;size_t length=strlen(name);
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==length && !memcmp(module->functions[f].name,name,length)) {
            CHECK(found==UINT32_MAX);found=f;
        }
    CHECK(found!=UINT32_MAX);return found;
}
static void enum_execute(const XrXirCompileContext *context,XrXirSourceResult *source,const char *emit,const char *packet_path,bool faults) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);
    if(packet_path) {
        FILE *file=fopen(packet_path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
    }
    xr_xir_compile_source_result_free(source);XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t entries[]={enum_function(module,"first"),enum_function(module,"payload"),
        enum_function(module,"last"),enum_function(module,"main")};
    if(emit) {
        uint32_t method=enum_function(module,"caseName");XrXirCSource output={0};
        CHECK(xr_xir_compile_emit_c(lowered,"fixture_enum",8388608,&output)==XR_XIR_OK);
        for(size_t i=0;i<output.length;++i) if(output.text[i]=='(') {
            size_t next=i+1;while(next<output.length && isspace((unsigned char)output.text[next]))++next;
            CHECK(next==output.length || output.text[next]!='{');
        }
        FILE *file=fopen(emit,"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length);
        CHECK(fprintf(file,"\nconst uint32_t fixture_enum_selection[5]={%uu,%uu,%uu,%uu,%uu};\n",
            entries[0],entries[1],entries[2],entries[3],method)>0 && !fclose(file));xr_xir_compile_c_source_free(&output);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirValue values[4];
    for(unsigned i=0;i<4;++i) {
        size_t sites=enum_runtime_failures(program,entries[i],i,faults);
        printf("ordinary enum VM case%u: sites=%zu full runtime FI=%u\n",i,sites,faults);
        values[i]=enum_runtime_take(program,entries[i]);
    }
    xr_xir_compile_program_drop(program);
    for(unsigned i=0;i<4;++i) {enum_runtime_expected(&values[i],i);xr_xir_value_drop(&values[i]);}
}
int main(int argc,char **argv) {
    bool emit=argc==4 && !strcmp(argv[1],"--emit-native"),faults=argc==2 && !strcmp(argv[1],"--runtime-resources");
    CHECK(argc==1 || emit || faults);
    if(!emit && !faults) enum_negative_cases();
    const XrCompileResourceLimits caps={67108864,8388608,128000000};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    char text[18000];int n=snprintf(text,sizeof(text),enum_generator_format,generated_schema_source,
        XR_XIR_NOMINAL_ENUM,XR_XIR_ENUM_TAG,XR_XIR_CONST_INT,XR_XIR_EQ_INT,XR_XIR_BRANCH,
        XR_XIR_CONST_STRING,XR_XIR_RETURN,XR_XIR_I64+1,XR_XIR_BOOL+1);
    CHECK(n>0 && n<(int)sizeof(text));XrXirSourceResult producer={0};
    enum_source(&context,text,XR_XIR_LIBRARY,NULL,&producer);memset(text,0xa5,sizeof(text));
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&producer);
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_ENUM_WORK},"generator_enum.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    memset(&binding,0xa5,sizeof(binding));memset(&input,0xa5,sizeof(input));XrXirSourceResult user={0};
    enum_source(&context,enum_user_source,XR_XIR_PROGRAM,catalog,&user);xr_xir_compile_library_catalog_free(catalog);
    enum_execute(&context,&user,emit?argv[2]:NULL,emit?argv[3]:NULL,faults);CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
    printf("Ordinary enum generator: future variants/real CFG, payload and both empty cases, comptime+independent VM oracle, producer death/physical0; allocated=%llu peak=%llu work=%llu\n",
        (unsigned long long)after.allocated_bytes,(unsigned long long)after.peak_bytes,(unsigned long long)after.work);return 0;
}
