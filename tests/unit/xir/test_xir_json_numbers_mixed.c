/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_numbers_mixed.c - Ordinary string tokens across both engines
 */
#include "base/xmalloc.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_numbers_runtime.h"
XR_DATA const XrXirProgramSpec fixture_json_numbers_program;
XR_DATA const uint32_t fixture_json_numbers_selection[4];
typedef struct JsonNumbersMixedOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} JsonNumbersMixedOwner;
static unsigned numbers_released;
static void numbers_mixed_release(void *pointer) {
    JsonNumbersMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner);++numbers_released;
}
static XrXirArtifact *numbers_mixed_lower(const XrXirCompileContext *context,const char *path) {
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
static void numbers_mixed(unsigned direction,const char *path) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    unsigned released=numbers_released;XrXirArtifact *artifact=numbers_mixed_lower(&context,path);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);CHECK(module->function_count==fixture_json_numbers_program.entry_count);
    const char *names[]={"main","inspect","jsonNumberToken","jsonInteger"};
    for(unsigned i=0;i<4;++i) {uint32_t f=fixture_json_numbers_selection[i];
        CHECK(f<module->function_count && module->functions[f].name_length==strlen(names[i]) && !memcmp(module->functions[f].name,names[i],strlen(names[i])));}
    uint32_t caller=fixture_json_numbers_selection[1],callee=fixture_json_numbers_selection[3];bool calls=false,token_call=false;
    for(uint32_t i=0;i<module->functions[caller].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[caller].instructions[i];calls|=op->op==XR_XIR_CALL && op->immediate==callee;token_call|=op->op==XR_XIR_CALL && op->immediate==fixture_json_numbers_selection[2];
    }
    CHECK(calls && token_call);JsonNumbersMixedOwner *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);owner->artifact=artifact;
    CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    if(direction<2) {
        uint32_t selected=direction?callee:caller,other=direction?caller:callee;
        owner->entries[selected]=fixture_json_numbers_program.entries[selected];
        CHECK(owner->entries[selected].resume==fixture_json_numbers_program.entries[selected].resume && owner->entries[other].resume!=fixture_json_numbers_program.entries[other].resume);
        if(direction)owner->entries[fixture_json_numbers_selection[2]]=fixture_json_numbers_program.entries[fixture_json_numbers_selection[2]];
    }
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,module->function_count,
        module->declarations,{owner,numbers_mixed_release},module->types,xr_xir_compile_program_proof(artifact)};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    json_numbers_oracles(program,fixture_json_numbers_selection);const char input[]="-9223372036854775808";
    XrXirValue retained=json_numbers_call(program,caller,input,sizeof(input)-1);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);CHECK(numbers_released==released+1);
    json_numbers_expected(&retained,(JsonNumberExpected){true,true,INT64_MIN});xr_xir_value_drop(&retained);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("Ordinary JSON tokens mixed direction%u/real CALL/one lease/retained tuple/physical0\n",direction);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2);const char *path=argc==2?argv[1]:XR_JSON_NUMBERS_PACKET;
    numbers_mixed(0,path);numbers_mixed(1,path);return 0;
}
