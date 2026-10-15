/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_derived_squares_mixed.c - Both actual native and VM cross-entry calls
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_derived_squares_source.h"
XR_DATA const XrXirProgramSpec fixture_squares_program;
XR_DATA const uint32_t fixture_squares_selection[2];
typedef struct SquaresMixed {XrXirArtifact *artifact;XrXirCallEntry *entries;XrXirVmBinding *bindings;} SquaresMixed;
static unsigned derived_squares_mixed_released;
static void derived_squares_mixed_release(void *pointer) {
    SquaresMixed *owner=pointer;xr_xir_compile_artifact_free(owner->artifact);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner);++derived_squares_mixed_released;
}
static void derived_squares_mixed(unsigned direction,bool inject) {
    XrXirCompileContext context={0};XrXirArtifact *artifact=derived_squares_lower(&context);
    XrXirDomain *reader=NULL;CHECK(xr_xir_compile_domain_new(context.resources,UINT64_MAX,&reader)==XR_XIR_VALUE_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module->function_count==fixture_squares_program.entry_count);
    uint32_t entries[2]={derived_squares_function(module,"checksum"),derived_squares_function(module,"table")};
    CHECK(entries[0]==fixture_squares_selection[0] && entries[1]==fixture_squares_selection[1]);
    const XrXirFunction *checksum=&module->functions[entries[0]];bool call=false;
    for(uint32_t i=0;i<checksum->instruction_count;++i)
        call|=checksum->instructions[i].op==XR_XIR_CALL && checksum->instructions[i].immediate==entries[1];
    CHECK(call);
    SquaresMixed *owner=NULL;
    CHECK(xr_compile_resources_calloc(context.resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->artifact=artifact;
    CHECK(xr_compile_resources_calloc(context.resources,module->function_count,sizeof(*owner->entries),(void **)&owner->entries)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources,module->function_count,sizeof(*owner->bindings),(void **)&owner->bindings)==XR_COMPILE_RESOURCE_OK);
    for(uint32_t f=0;f<module->function_count;++f)
        CHECK(xr_xir_compile_vm_bind(artifact,f,&owner->bindings[f],&owner->entries[f])==XR_XIR_OK);
    uint32_t selected=entries[direction];owner->entries[selected]=fixture_squares_program.entries[selected];
    CHECK(owner->entries[entries[direction]].resume==fixture_squares_program.entries[entries[direction]].resume);
    CHECK(owner->entries[entries[1-direction]].resume!=fixture_squares_program.entries[entries[1-direction]].resume);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        owner->entries,module->function_count,module->declarations,{owner,derived_squares_mixed_release},module->types,xr_xir_compile_program_proof(artifact)};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&spec,&program)==XR_XIR_OK);
    size_t sites=derived_squares_runtime_failures(program,entries,reader,inject);
    xr_xir_compile_program_drop(program);xr_xir_domain_drop(reader);xr_compile_resources_release(context.resources);
    CHECK(derived_squares_mixed_released==direction+1 && !runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("Ordinary mixed constant table direction=%u checksum140 / 0,1,4,9,16,25,36,49/capacity8; real cross-entry CALL; runtime sites=%zu; full FI=%u; lease released once; physical0\n",direction,sites,inject);
}

int main(int argc,char **argv) {
    bool inject=argc==2 && !strcmp(argv[1],"--runtime-resources");CHECK(argc==1 || inject);
    derived_squares_mixed(0,inject);derived_squares_mixed(1,inject);return 0;
}
