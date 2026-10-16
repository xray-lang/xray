/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_copy_bytes_mixed.c - All bytes cases across both engines
 */
#include "base/xmalloc.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_string_copy_bytes_runtime.h"
XR_DATA const XrXirProgramSpec fixture_bytes_program;
XR_DATA const uint32_t fixture_bytes_selection[5];
typedef struct ByteCopyMixedOwner {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} ByteCopyMixedOwner;
static unsigned bytes_mixed_released;
static void bytes_mixed_release(void *pointer) {
    ByteCopyMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner);++bytes_mixed_released;
}
static XrXirArtifact *bytes_mixed_lower(const XrXirCompileContext *context,const char *path) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));
    long length=ftell(file);CHECK(length>0 && !fseek(file,0,SEEK_SET));void *bytes=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,(size_t)length,&bytes)==XR_COMPILE_RESOURCE_OK);
    CHECK(fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,bytes,(size_t)length,&checked,NULL)==XR_XIR_OK);
    memset(bytes,0xa5,(size_t)length);xr_compile_resources_free(bytes);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);return lowered;
}
static void bytes_mixed(unsigned direction,unsigned which,bool faults,const char *path) {
    CHECK(which<4);XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    unsigned released=bytes_mixed_released;XrXirArtifact *artifact=bytes_mixed_lower(&context,path);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);CHECK(module->function_count==fixture_bytes_program.entry_count);
    uint32_t caller=fixture_bytes_selection[which],method=fixture_bytes_selection[4];const char *names[]={"run","folded","empty","isolated"};
    CHECK(caller<module->function_count && method<module->function_count);
    CHECK(module->functions[caller].name_length==strlen(names[which]) &&
        !memcmp(module->functions[caller].name,names[which],strlen(names[which])));
    CHECK(module->functions[method].name_length==4 && !memcmp(module->functions[method].name,"copy",4));
    bool calls=false;
    for(uint32_t i=0;i<module->functions[caller].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[caller].instructions[i];
        calls|=op->op==XR_XIR_CALL && op->immediate==method;
    }
    CHECK(which==0?calls:!calls);ByteCopyMixedOwner *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->artifact=artifact;CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    if(direction<2) {
        uint32_t selected=direction?method:caller,other=direction?caller:method;
        owner->entries[selected]=fixture_bytes_program.entries[selected];
        CHECK(owner->entries[selected].resume==fixture_bytes_program.entries[selected].resume &&
            owner->entries[other].resume!=fixture_bytes_program.entries[other].resume);
    } else CHECK(owner->entries[caller].resume!=fixture_bytes_program.entries[caller].resume &&
        owner->entries[method].resume!=fixture_bytes_program.entries[method].resume);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        owner->entries,module->function_count,module->declarations,{owner,bytes_mixed_release},module->types,
        xr_xir_compile_program_proof(artifact)};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    size_t sites=byte_runtime_failures(program,caller,which==2,faults);XrXirValue value=byte_runtime_take(program,caller,which==2);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    CHECK(bytes_mixed_released==released+1);byte_value(&value,which==2);xr_xir_value_drop(&value);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("ordinary bytes mixed direction%u case%u: real CALL, sites=%zu full FI=%u, one lease/result after producer death/physical0\n",direction,which,sites,faults);
}
int main(int argc,char **argv) {
    bool vm=(argc==2 || argc==3) && !strcmp(argv[1],"--vm-runtime-resources");
    bool faults=(argc==2 || argc==3) && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || faults || vm);
    const char *path=argc==3?argv[2]:XR_BYTE_COPY_PACKET;
    if(vm){for(unsigned which=0;which<4;++which)bytes_mixed(2,which,true,path);}
    else {bytes_mixed(0,0,faults,path);bytes_mixed(1,0,faults,path);}
    return 0;
}
