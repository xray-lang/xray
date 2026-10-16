/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_regex_precompile_mixed.c - All regex cases across both engines
 */
#include "base/xmalloc.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_regex_precompile_runtime.h"
XR_DATA const XrXirProgramSpec fixture_regex_program;
XR_DATA const uint32_t fixture_regex_selection[4];
typedef struct RegexMixedOwner {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} RegexMixedOwner;
static unsigned regex_mixed_released;
static void regex_mixed_release(void *pointer) {
    RegexMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner);++regex_mixed_released;
}
static XrXirArtifact *regex_mixed_lower(const XrXirCompileContext *context,const char *path) {
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
static void regex_mixed(unsigned direction,unsigned which,bool faults,const char *path) {
    CHECK(which<3);XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    unsigned released=regex_mixed_released;XrXirArtifact *artifact=regex_mixed_lower(&context,path);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);CHECK(module->function_count==fixture_regex_program.entry_count);
    uint32_t caller=fixture_regex_selection[which],method=fixture_regex_selection[3];const char *names[]={"matrix","program","answer"};
    CHECK(caller<module->function_count && method<module->function_count);
    CHECK(module->functions[caller].name_length==strlen(names[which]) &&
        !memcmp(module->functions[caller].name,names[which],strlen(names[which])));
    CHECK(module->functions[method].name_length==12 && !memcmp(module->functions[method].name,"matchesRegex",12));
    bool calls=false;
    for(uint32_t i=0;i<module->functions[caller].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[caller].instructions[i];
        calls|=op->op==XR_XIR_CALL && op->immediate==method;
    }
    CHECK(which==0?calls:!calls);RegexMixedOwner *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->artifact=artifact;CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    if(direction<2) {
        uint32_t selected=direction?method:caller,other=direction?caller:method;
        owner->entries[selected]=fixture_regex_program.entries[selected];
        CHECK(owner->entries[selected].resume==fixture_regex_program.entries[selected].resume &&
            owner->entries[other].resume!=fixture_regex_program.entries[other].resume);
    } else CHECK(owner->entries[caller].resume!=fixture_regex_program.entries[caller].resume &&
        owner->entries[method].resume!=fixture_regex_program.entries[method].resume);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        owner->entries,module->function_count,module->declarations,{owner,regex_mixed_release},module->types,
        xr_xir_compile_program_proof(artifact)};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    size_t sites=regex_runtime_failures(program,caller,which,faults);XrXirValue value=regex_runtime_take(program,caller);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    CHECK(regex_mixed_released==released+1);regex_value(&value,which);xr_xir_value_drop(&value);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("ordinary regex mixed direction%u case%u: real CALL, sites=%zu full FI=%u, one lease/result after producer death/physical0\n",direction,which,sites,faults);
}
int main(int argc,char **argv) {
    bool vm=(argc==2 || argc==3) && !strcmp(argv[1],"--vm-runtime-resources");
    bool faults=(argc==2 || argc==3) && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || faults || vm);
    const char *path=argc==3?argv[2]:XR_REGEX_PRECOMPILE_PACKET;
    if(vm){for(unsigned which=0;which<3;++which)regex_mixed(2,which,true,path);}
    else {regex_mixed(0,0,faults,path);regex_mixed(1,0,faults,path);}
    return 0;
}
