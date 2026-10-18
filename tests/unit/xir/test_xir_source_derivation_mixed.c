/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derivation_mixed.c - Both actual derived method call directions
 */
#include "base/xmalloc.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}} while(0)
#include "xir_source_derivation_runtime.h"
XR_DATA const XrXirProgramSpec fixture_derive_program;
XR_DATA const uint32_t fixture_derive_selection[2];
typedef struct DeriveMixedOwner {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} DeriveMixedOwner;
static unsigned derive_mixed_released;
static void derive_mixed_release(void *pointer) {
    DeriveMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner);++derive_mixed_released;
}
static XrXirArtifact *derive_mixed_lower(const XrXirCompileContext *context,const char *path) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));
    long length=ftell(file);CHECK(length>0 && !fseek(file,0,SEEK_SET));void *bytes=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,(size_t)length,&bytes)==XR_COMPILE_RESOURCE_OK);
    CHECK(fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,bytes,(size_t)length,&checked,NULL)==XR_XIR_OK);
    memset(bytes,0xa5,(size_t)length);xr_compile_resources_free(bytes);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    return lowered;
}
static void derive_mixed(unsigned direction,bool faults,const char *path) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    unsigned released=derive_mixed_released;
    XrXirArtifact *artifact=derive_mixed_lower(&context,path);const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->function_count==fixture_derive_program.entry_count);
    uint32_t caller=fixture_derive_selection[0],method=fixture_derive_selection[1];
    CHECK(caller<module->function_count && method<module->function_count);
    CHECK(module->functions[caller].name_length==4 && !memcmp(module->functions[caller].name,"main",4));
    CHECK(module->functions[method].name_length==7 && !memcmp(module->functions[method].name,"measure",7));
    bool calls=false;
    for (uint32_t i=0;i<module->functions[caller].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[caller].instructions[i];
        calls|=op->op==XR_XIR_CALL && op->immediate==method;
    }
    CHECK(calls);DeriveMixedOwner *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->artifact=artifact;
    CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    if (direction<2) {
        uint32_t selected=direction?method:caller,other=direction?caller:method;
        owner->entries[selected]=fixture_derive_program.entries[selected];
        CHECK(owner->entries[selected].resume==fixture_derive_program.entries[selected].resume &&
            owner->entries[other].resume!=fixture_derive_program.entries[other].resume);
    } else CHECK(owner->entries[caller].resume!=fixture_derive_program.entries[caller].resume &&
        owner->entries[method].resume!=fixture_derive_program.entries[method].resume);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        owner->entries,module->function_count,module->declarations,{owner,derive_mixed_release},module->types,
        xr_xir_compile_program_proof(artifact)};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    size_t sites=derive_runtime_failures(program,caller,faults);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    CHECK(derive_mixed_released==released+1 && !runtime_live && !runtime_bytes &&
        !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("ordinary derived method mixed direction=%u real CALL42; sites=%zu full FI=%u; one lease/physical0\n",direction,sites,faults);
}
int main(int argc,char **argv) {
    bool vm=(argc==2 || argc==3) && !strcmp(argv[1],"--vm-runtime-resources");
    bool faults=(argc==2 || argc==3) && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || faults || vm);
    const char *path=argc==3?argv[2]:XR_SOURCE_DERIVATION_PACKET;
    if (vm) derive_mixed(2,true,path);
    else {derive_mixed(0,faults,path);derive_mixed(1,faults,path);}return 0;
}
