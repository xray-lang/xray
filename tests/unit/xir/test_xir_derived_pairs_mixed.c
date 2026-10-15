/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_derived_pairs_mixed.c - Both actual native and VM cross-entry calls
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_derived_pairs_source.h"
XR_DATA const XrXirProgramSpec fixture_pairs_program;
XR_DATA const uint32_t fixture_pairs_selection[2];
typedef struct DerivedPairsMixed {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} DerivedPairsMixed;
static unsigned derived_pairs_mixed_released;
static void derived_pairs_mixed_release(void *pointer) {
    DerivedPairsMixed *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner);++derived_pairs_mixed_released;
}
static void derived_pairs_mixed(unsigned direction,bool inject) {
    XrXirCompileContext context={0};XrXirArtifact *artifact=derived_pairs_lower(&context);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->function_count==fixture_pairs_program.entry_count);
    uint32_t entries[2]={derived_pairs_function(module,"checksum"),derived_pairs_function(module,"table")};
    CHECK(entries[0]==fixture_pairs_selection[0] && entries[1]==fixture_pairs_selection[1]);
    const XrXirFunction *checksum=&module->functions[entries[0]];bool call=false;
    for(uint32_t i=0;i<checksum->instruction_count;++i)
        call|=checksum->instructions[i].op==XR_XIR_CALL && checksum->instructions[i].immediate==entries[1];
    CHECK(call);
    DerivedPairsMixed *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->artifact=artifact;
    CHECK(xr_compile_resources_calloc(context.resources,module->function_count,sizeof(*owner->entries),(void **)&owner->entries)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources,module->function_count,sizeof(*owner->bindings),(void **)&owner->bindings)==XR_COMPILE_RESOURCE_OK);
    for(uint32_t f=0;f<module->function_count;++f)
        CHECK(xr_xir_compile_vm_bind(artifact,f,&owner->bindings[f],&owner->entries[f])==XR_XIR_OK);
    uint32_t selected=entries[direction];owner->entries[selected]=fixture_pairs_program.entries[selected];
    CHECK(owner->entries[entries[direction]].resume==fixture_pairs_program.entries[entries[direction]].resume);
    CHECK(owner->entries[entries[1-direction]].resume!=fixture_pairs_program.entries[entries[1-direction]].resume);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        owner->entries,module->function_count,module->declarations,{owner,derived_pairs_mixed_release},module->types,xr_xir_compile_program_proof(artifact)};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    size_t sites=derived_pairs_runtime_failures(program,entries,inject);
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    CHECK(derived_pairs_mixed_released==direction+1 && !runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("Ordinary mixed constant table direction=%u checksum32 / ordered2,3; real cross-entry CALL; runtime sites=%zu; full FI=%u; lease released once; physical0\n",direction,sites,inject);
}

int main(int argc,char **argv) {
    bool inject=argc==2 && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || inject);
    derived_pairs_mixed(0,inject);derived_pairs_mixed(1,inject);return 0;
}
