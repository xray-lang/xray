/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_aggregate_encode_mixed.c - Actual Array and nullable helper boundaries
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_aggregate_encode_runtime.h"
XR_DATA const XrXirProgramSpec fixture_json_aggregate_program;
XR_DATA const uint32_t fixture_json_aggregate_selection[10];
typedef struct AggregateMixedOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} AggregateMixedOwner;
static unsigned aggregate_mixed_released;
static void aggregate_mixed_release(void *pointer) {
    AggregateMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);xr_compile_resources_free(owner->entries);
    xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner);++aggregate_mixed_released;
}
static XrXirArtifact *aggregate_mixed_lower(const XrXirCompileContext *context,const char *path) {
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
static void aggregate_mixed(unsigned direction,bool faults,const char *path) {
    XrXirCompileContext context=aggregate_runtime_context();XrXirArtifact *artifact=aggregate_mixed_lower(&context,path);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);CHECK(module->function_count==fixture_json_aggregate_program.entry_count);
    uint32_t method=fixture_json_aggregate_selection[7],array=fixture_json_aggregate_selection[8],optional=fixture_json_aggregate_selection[9];
    CHECK(method<module->function_count && array<module->function_count && optional<module->function_count);
    bool calls_array=false,calls_optional=false;
    for(uint32_t i=0;i<module->functions[method].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[method].instructions[i];
        calls_array|=op->op==XR_XIR_CALL && op->immediate==array;calls_optional|=op->op==XR_XIR_CALL && op->immediate==optional;
    }
    CHECK(calls_array && calls_optional);AggregateMixedOwner *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);owner->artifact=artifact;
    CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    for(uint32_t f=0;f<module->function_count;++f) {
        bool helper=f==array || f==optional;
        if((direction==0 && helper) || (direction==1 && !helper))owner->entries[f]=fixture_json_aggregate_program.entries[f];
    }
    CHECK((owner->entries[method].resume==fixture_json_aggregate_program.entries[method].resume)==(direction==1) &&
        (owner->entries[array].resume==fixture_json_aggregate_program.entries[array].resume)==(direction==0) &&
        (owner->entries[optional].resume==fixture_json_aggregate_program.entries[optional].resume)==(direction==0));
    unsigned released=aggregate_mixed_released;
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,module->function_count,
        module->declarations,{owner,aggregate_mixed_release},module->types,xr_xir_compile_program_proof(artifact)};
    XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_program_seal(&context,&spec,&program);
    if(status!=XR_XIR_OK){CHECK(!program);aggregate_mixed_release(owner);xr_compile_resources_release(context.resources);CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);}
    fprintf(stderr,"Aggregate mixed direction%u seal%u\n",direction,(unsigned)status);CHECK(status==XR_XIR_OK);
    aggregate_runtime_faults(program,fixture_json_aggregate_selection,faults);XrXirValue values[6]={{0}};
    aggregate_runtime_results(program,fixture_json_aggregate_selection,values);xr_xir_compile_program_drop(program);
    xr_compile_resources_release(context.resources);CHECK(aggregate_mixed_released==released+1);aggregate_retained(values);
    printf("Aggregate mixed direction%u: actual helper CALLs, six exact JSON outputs, one lease and physical0\n",direction);
}
int main(int argc,char **argv) {
    bool faults=(argc==2 || argc==3) && !strcmp(argv[1],"--runtime-resources"),vm=(argc==2 || argc==3) && !strcmp(argv[1],"--vm-runtime-resources");
    CHECK(argc==1 || faults || vm);const char *path=argc==3?argv[2]:XR_JSON_AGGREGATE_PACKET;
    if(vm)aggregate_mixed(2,true,path);else{aggregate_mixed(0,faults,path);aggregate_mixed(1,faults,path);if(!faults)aggregate_mixed(2,false,path);}return 0;
}
