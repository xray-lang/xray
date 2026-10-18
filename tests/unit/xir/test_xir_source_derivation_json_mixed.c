/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_derivation_json_mixed.c - Real JSON calls across both engines
 */
#include "base/xmalloc.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_derivation_json_runtime.h"
XR_DATA const XrXirProgramSpec fixture_json_program;
XR_DATA const uint32_t fixture_json_selection[4];
typedef struct JsonMixedOwner {
    XrXirArtifact *artifact;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} JsonMixedOwner;
static unsigned json_mixed_released;
static void json_mixed_release(void *pointer) {
    JsonMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner);++json_mixed_released;
}
static XrXirArtifact *json_mixed_lower(const XrXirCompileContext *context,const char *path) {
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
static void json_mixed(unsigned direction,bool faults,const char *path) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    unsigned released=json_mixed_released;XrXirArtifact *artifact=json_mixed_lower(&context,path);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);CHECK(module->function_count==fixture_json_program.entry_count);
    uint32_t caller=fixture_json_selection[1],method=fixture_json_selection[2],quote=fixture_json_selection[3];
    CHECK(caller<module->function_count && method<module->function_count && quote<module->function_count);
    CHECK(module->functions[caller].name_length==6 && !memcmp(module->functions[caller].name,"encode",6));
    CHECK(module->functions[method].name_length==10 && !memcmp(module->functions[method].name,"encodeJSON",10));
    CHECK(module->functions[quote].name_length==9 && !memcmp(module->functions[quote].name,"jsonQuote",9));
    bool calls=false;
    for(uint32_t i=0;i<module->functions[caller].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[caller].instructions[i];
        calls|=op->op==XR_XIR_CALL && op->immediate==method;
    }
    CHECK(calls);calls=false;
    for(uint32_t i=0;i<module->functions[method].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[method].instructions[i];
        calls|=op->op==XR_XIR_CALL && op->immediate==quote;
    }
    CHECK(calls);JsonMixedOwner *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->artifact=artifact;CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    if(direction<2) {
        uint32_t selected=direction?method:caller,other=direction?caller:method;
        owner->entries[selected]=fixture_json_program.entries[selected];
        CHECK(owner->entries[selected].resume==fixture_json_program.entries[selected].resume &&
            owner->entries[other].resume!=fixture_json_program.entries[other].resume);
        if(!direction) owner->entries[quote]=fixture_json_program.entries[quote];
        CHECK((owner->entries[quote].resume==fixture_json_program.entries[quote].resume)==!direction);
    } else CHECK(owner->entries[caller].resume!=fixture_json_program.entries[caller].resume &&
        owner->entries[method].resume!=fixture_json_program.entries[method].resume);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        owner->entries,module->function_count,module->declarations,{owner,json_mixed_release},module->types,
        xr_xir_compile_program_proof(artifact)};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    if(!faults) json_quote_oracles(program,fixture_json_selection[3]);
    if(!faults) json_runtime_entered_oom(program,caller);
    size_t sites=json_runtime_failures(program,caller,true,faults);XrXirValue text=json_runtime_take(program,caller);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    CHECK(json_mixed_released==released+1);json_runtime_expected(&text,true);xr_xir_value_drop(&text);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("ordinary JSON mixed direction%u: real CALL/string oracle, sites=%zu full FI=%u, one lease/result after producer death/physical0\n",direction,sites,faults);
}
int main(int argc,char **argv) {
    bool vm=(argc==2 || argc==3) && !strcmp(argv[1],"--vm-runtime-resources");
    bool faults=(argc==2 || argc==3) && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || faults || vm);
    const char *path=argc==3?argv[2]:XR_SOURCE_DERIVATION_JSON_PACKET;
    if(vm) json_mixed(2,true,path);else {json_mixed(0,faults,path);json_mixed(1,faults,path);}return 0;
}
