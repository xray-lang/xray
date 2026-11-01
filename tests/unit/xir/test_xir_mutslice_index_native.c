/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * The native gate executes the real emitted mutable range operations. */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Native MutSlice %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "mutslice_index_fixture.h"
#include "mutslice_index_runtime.h"
XR_DATA const XrXirProgramSpec mutslice_index_program;
XR_DATA const uint32_t mutslice_index_ids[MUTSLICE_INDEX_ENTRIES];
static void mutslice_program_refusals(const XrXirCompileContext *context) {
    const XrXirProgramSpec *original=&mutslice_index_program;
    XrXirCallEntry *entries=xr_malloc((size_t)original->entry_count*sizeof(*entries));CHECK(entries);
    memcpy(entries,original->entries,(size_t)original->entry_count*sizeof(*entries));
    XrXirProgramSpec bad=*original;bad.entries=entries;unsigned tested=0;
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(uint32_t t=0;t<original->types->count;++t) {
        if(original->types->nodes[t].kind!=XR_XIR_TYPE_MUT_SLICE)continue;
        XrXirType type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);uint32_t f=mutslice_index_ids[0];
        XrXirCallEntry saved=entries[f];XrXirProgram *rejected=NULL;
        entries[f].result=type;
        CHECK(xr_xir_compile_program_seal(context,&bad,&rejected)==XR_XIR_BAD_STRUCTURE&&!rejected);
        entries[f]=saved;entries[f].parameters=&type;entries[f].parameter_count=1;
        CHECK(xr_xir_compile_program_seal(context,&bad,&rejected)==XR_XIR_BAD_STRUCTURE&&!rejected);
        entries[f]=saved;++tested;
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
    }
    CHECK(tested==2);xr_free(entries);
}
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    mutslice_program_refusals(&context);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&mutslice_index_program,&program)==XR_XIR_OK);
    mutslice_run(program,mutslice_index_ids);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live&&!source_fixture_compile_bytes);puts("MutSlice generated C/retirement PASS");return 0;
}
