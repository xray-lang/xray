/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_strings_mixed.c - Ordinary string tokens across both engines
 */
#include "base/xmalloc.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_strings_runtime.h"
XR_DATA const XrXirProgramSpec fixture_json_strings_program;
XR_DATA const uint32_t fixture_json_strings_selection[4];
typedef struct JsonStringsMixedOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} JsonStringsMixedOwner;
static unsigned strings_released;
static void strings_mixed_release(void *pointer) {
    JsonStringsMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner);++strings_released;
}
static XrXirArtifact *strings_mixed_lower(const XrXirCompileContext *context,const char *path) {
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
static void strings_mixed(unsigned direction,const char *path) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    unsigned released=strings_released;XrXirArtifact *artifact=strings_mixed_lower(&context,path);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);CHECK(module->function_count==fixture_json_strings_program.entry_count);
    const char *names[]={"main","decode","jsonQuote","jsonUnquote"};
    for(unsigned i=0;i<4;++i) {uint32_t f=fixture_json_strings_selection[i];
        CHECK(f<module->function_count && module->functions[f].name_length==strlen(names[i]) && !memcmp(module->functions[f].name,names[i],strlen(names[i])));}
    uint32_t caller=fixture_json_strings_selection[1],callee=fixture_json_strings_selection[3];bool calls=false;
    for(uint32_t i=0;i<module->functions[caller].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[caller].instructions[i];calls|=op->op==XR_XIR_CALL && op->immediate==callee;
    }
    CHECK(calls);JsonStringsMixedOwner *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);owner->artifact=artifact;
    CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    if(direction<2) {
        uint32_t selected=direction?callee:caller,other=direction?caller:callee;
        owner->entries[selected]=fixture_json_strings_program.entries[selected];
        CHECK(owner->entries[selected].resume==fixture_json_strings_program.entries[selected].resume && owner->entries[other].resume!=fixture_json_strings_program.entries[other].resume);
        if(!direction)owner->entries[fixture_json_strings_selection[2]]=fixture_json_strings_program.entries[fixture_json_strings_selection[2]];
    }
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,module->function_count,
        module->declarations,{owner,strings_mixed_release},module->types,xr_xir_compile_program_proof(artifact)};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    json_strings_oracles(program,fixture_json_strings_selection);const char input[]="\"\\u6c49\\ud83d\\ude00\"";
    XrXirValue retained=json_strings_call(program,caller,input,sizeof(input)-1);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);CHECK(strings_released==released+1);
    json_strings_expected(&retained,"汉😀",strlen("汉😀"));xr_xir_value_drop(&retained);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("Ordinary JSON tokens mixed direction%u/real CALL/one lease/retained nullable/physical0\n",direction);
}
int main(int argc,char **argv) {
    bool vm=(argc==2 || argc==3) && !strcmp(argv[1],"--vm-runtime-resources");
    json_strings_faults=vm || ((argc==2 || argc==3) && !strcmp(argv[1],"--runtime-resources"));CHECK(argc==1 || json_strings_faults);
    const char *path=argc==3?argv[2]:XR_JSON_STRINGS_PACKET;
    if(vm)strings_mixed(2,path);else{strings_mixed(0,path);strings_mixed(1,path);}return 0;
}
