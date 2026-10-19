/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_json_named_mixed.c - Ordinary string tokens across both engines
 */
#include "base/xmalloc.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_json_typed_decode_runtime.h"
XR_DATA const XrXirProgramSpec fixture_json_typed_program;
XR_DATA const uint32_t fixture_json_typed_selection[8];
typedef struct JsonNamedMixedOwner {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} JsonNamedMixedOwner;
static unsigned named_released;
static void named_mixed_release(void *pointer) {
    JsonNamedMixedOwner *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);xr_compile_resources_free(owner);++named_released;
}
static XrXirArtifact *named_mixed_lower(const XrXirCompileContext *context,const char *path) {
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
static void named_mixed(unsigned direction,const char *path,const JsonTypedKeys *keys) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    unsigned released=named_released;XrXirArtifact *artifact=named_mixed_lower(&context,path);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);CHECK(module->function_count==fixture_json_typed_program.entry_count);
    const char *names[]={"main","decode","decodeJSON","jsonScalarObject"};
    for(unsigned i=0;i<4;++i) {uint32_t f=fixture_json_typed_selection[i];
        CHECK(f<module->function_count && module->functions[f].name_length==strlen(names[i]) && !memcmp(module->functions[f].name,names[i],strlen(names[i])));}
    uint32_t caller=fixture_json_typed_selection[2],callee=fixture_json_typed_selection[3],ctor=fixture_json_typed_selection[4];bool calls=false,constructs=false;
    for(uint32_t i=0;i<module->functions[caller].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[caller].instructions[i];calls|=op->op==XR_XIR_CALL && op->immediate==callee;constructs|=op->op==XR_XIR_CALL && op->immediate==ctor;
    }
    CHECK(calls && constructs && ctor<module->function_count && module->declarations->functions[ctor].method_kind==XR_XIR_CONSTRUCTOR);JsonNamedMixedOwner *owner=NULL;
    for(unsigned p=0;p<3;++p) {
        uint32_t parser=fixture_json_typed_selection[5+p];bool found=false;CHECK(parser<module->function_count);
        for(uint32_t i=0;i<module->functions[caller].instruction_count;++i) {
            const XrXirInstruction *op=&module->functions[caller].instructions[i];found|=op->op==XR_XIR_CALL && op->immediate==parser;
        }
        CHECK(found);
    }
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);owner->artifact=artifact;
    CHECK(xr_xir_compile_vm_bind_table(artifact,&owner->bindings,&owner->entries)==XR_XIR_OK);
    if(direction<2) {
        uint32_t selected=direction?callee:caller,other=direction?caller:callee;
        owner->entries[selected]=fixture_json_typed_program.entries[selected];
        CHECK(owner->entries[selected].resume==fixture_json_typed_program.entries[selected].resume && owner->entries[other].resume!=fixture_json_typed_program.entries[other].resume);
        if(direction)owner->entries[ctor]=fixture_json_typed_program.entries[ctor];
        if(direction)for(unsigned p=0;p<3;++p) {
            uint32_t parser=fixture_json_typed_selection[5+p];owner->entries[parser]=fixture_json_typed_program.entries[parser];
        }
    }
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,module->function_count,
        module->declarations,{owner,named_mixed_release},module->types,xr_xir_compile_program_proof(artifact)};
    XrXirProgram *program=NULL;XrXirStatus sealed=xr_xir_compile_program_seal(&context,&spec,&program);
    XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(context.resources,&spent)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"Typed JSON mixed direction%u seal%u allocated=%llu peak=%llu work=%llu\n",direction,(unsigned)sealed,
        (unsigned long long)spent.allocated_bytes,(unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);
    if(sealed!=XR_XIR_OK) {
        CHECK(!program);named_mixed_release(owner);xr_compile_resources_release(context.resources);
        CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
        puts("Failed mixed typed seal cleaned to physical0; no retry or budget reset");
    }
    CHECK(sealed==XR_XIR_OK);
    named_oracles(program,fixture_json_typed_selection,keys);char text[256];
    int n=snprintf(text,sizeof(text),"{\"%s\":\"ok\",\"%s\":true,\"%s\":42}",keys->label,keys->active,keys->count);CHECK(n>0 && n<(int)sizeof(text));
    XrXirValue retained=named_call(program,fixture_json_typed_selection[1],text,(size_t)n);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);CHECK(named_released==released+1);
    named_expected(&retained,(JsonTypedExpected){true,"ok",2,true,42});xr_xir_value_drop(&retained);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("Ordinary JSON tokens mixed direction%u/real CALL/one lease/retained nullable/physical0\n",direction);
}
int main(int argc,char **argv) {
    bool vm=argc==6 && !strcmp(argv[1],"--vm");CHECK(argc==1 || argc==5 || vm);
    unsigned start=vm?2u:1u;const char *path=argc==1?XR_JSON_TYPED_PACKET:argv[start];
    JsonTypedKeys keys={"label","active","count"};
    if(argc!=1)keys=(JsonTypedKeys){argv[start+1],argv[start+2],argv[start+3]};
    if(vm)named_mixed(2,path,&keys);else {named_mixed(0,path,&keys);named_mixed(1,path,&keys);}return 0;
}
