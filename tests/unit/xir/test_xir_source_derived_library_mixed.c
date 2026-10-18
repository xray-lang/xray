/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derived_library_mixed.c - Cold library private READ boundaries
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_derived_library_runtime.h"
XR_DATA const XrXirProgramSpec fixture_derived_library_program;
XR_DATA const uint32_t fixture_derived_library_selection[3];
typedef struct ColdMixedOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} ColdMixedOwner;
static unsigned cold_mixed_released;
static void cold_mixed_release(void *pointer) {
    ColdMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);xr_compile_resources_free(owner->entries);
    xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner);++cold_mixed_released;
}
static XrXirArtifact *cold_mixed_lower(const XrXirCompileContext *context,const char *path) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);
    CHECK(length>0 && length<=67108864 && !fseek(file,0,SEEK_SET));void *bytes=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,(size_t)length,&bytes)==XR_COMPILE_RESOURCE_OK);
    CHECK(fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,bytes,(size_t)length,&checked,NULL)==XR_XIR_OK);
    memset(bytes,0xa5,(size_t)length);xr_compile_resources_free(bytes);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);return lowered;
}
static void cold_mixed(unsigned direction,bool faults,const char *path) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrXirArtifact *artifact=cold_mixed_lower(&context,path);const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->function_count==fixture_derived_library_program.entry_count);
    uint32_t caller=fixture_derived_library_selection[XR_DERIVED_LIBRARY_GENERATOR_ONLY?0:1],method=fixture_derived_library_selection[2];
    const char *method_name=XR_DERIVED_LIBRARY_GENERATOR_ONLY?"parameterNames":"measure";
    CHECK(caller<module->function_count && method<module->function_count && module->functions[method].name_length==strlen(method_name) &&
        !memcmp(module->functions[method].name,method_name,strlen(method_name)));bool calls=false;
    for(uint32_t i=0;i<module->functions[caller].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[caller].instructions[i];calls|=op->op==XR_XIR_CALL && op->immediate==method;
    }
    CHECK(calls);ColdMixedOwner *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);owner->artifact=artifact;
    CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    if(!direction)owner->entries[method]=fixture_derived_library_program.entries[method];
    else if(direction==1)for(uint32_t f=0;f<module->function_count;++f)if(f!=method)owner->entries[f]=fixture_derived_library_program.entries[f];
    CHECK((owner->entries[caller].resume==fixture_derived_library_program.entries[caller].resume)==(direction==1) &&
        (owner->entries[method].resume==fixture_derived_library_program.entries[method].resume)==(direction==0));
    unsigned released=cold_mixed_released;
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,module->function_count,
        module->declarations,{owner,cold_mixed_release},module->types,xr_xir_compile_program_proof(artifact)};
    XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_program_seal(&context,&spec,&program);
    if(status!=XR_XIR_OK){CHECK(!program);cold_mixed_release(owner);xr_compile_resources_release(context.resources);CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);}
    fprintf(stderr,"Cold library mixed direction%u seal%u\n",direction,(unsigned)status);CHECK(status==XR_XIR_OK);
    cold_runtime_faults(program,fixture_derived_library_selection,faults);
    XrXirValue kept=cold_runtime_results(program,fixture_derived_library_selection);xr_xir_compile_program_drop(program);
    xr_compile_resources_release(context.resources);CHECK(cold_mixed_released==released+1);cold_runtime_retained(&kept);
    printf("Cold library mixed direction%u: actual %s CALL, main42, last String, one lease and physical0\n",direction,method_name);
}
int main(int argc,char **argv) {
    bool faults=(argc==2 || argc==3) && !strcmp(argv[1],"--runtime-resources"),vm=(argc==2 || argc==3) && !strcmp(argv[1],"--vm-runtime-resources");CHECK(argc==1 || faults || vm);
    const char *path=argc==3?argv[2]:XR_DERIVED_LIBRARY_PACKET;
    if(vm)cold_mixed(2,true,path);else{cold_mixed(0,faults,path);cold_mixed(1,faults,path);if(!faults)cold_mixed(2,false,path);}return 0;
}
