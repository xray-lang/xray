/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_generated_native.c - Source-backed Array execution and ownership qualification
 *
 * KEY CONCEPT:
 *   Generated current-ABI native entries retain the original Array workload expectations.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while(0)
#include "xir_array_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_array_generated_cases.h"
XR_DATA const XrXirProgramSpec array_generated_program;
XR_DATA const uint32_t array_generated_selection[11];
/* The generated ProgramSpec has a separate receiving seal owner. Every
 * actual compiler allocation, including the ledger itself, is replayed. */
static XrXirStatus array_native_compiler_probe(XrCompileResourceLimits caps,XrCompileResourceStats *stats) {
    ArrayCompileOwner owner;XrCompileResourceStatus opened=array_compile_owner_new(caps,&owner);
    if(opened!=XR_COMPILE_RESOURCE_OK) {
        array_compile_owner_free(&owner);
        return opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    }
    XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_program_seal(&owner.context,&array_generated_program,&program);
    if(status!=XR_XIR_OK)CHECK(!program);
    else {
        CHECK(program);XrXirProgram *occupied=program;
        XrCompileResourceStats before=array_compile_stats(&owner);
        size_t attempts=array_program_compile_attempts,live=array_program_compile_live,bytes=array_program_compile_bytes;
        CHECK(xr_xir_compile_program_seal(&owner.context,&array_generated_program,&occupied)==XR_XIR_BAD_STRUCTURE);
        CHECK(occupied==program);
        XrCompileResourceStats after=array_compile_stats(&owner);
        CHECK(before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes &&
            before.peak_bytes==after.peak_bytes && before.work==after.work);
        CHECK(attempts==array_program_compile_attempts && live==array_program_compile_live && bytes==array_program_compile_bytes);
    }
    xr_xir_compile_program_drop(program);
    if(stats)*stats=array_compile_stats(&owner);
    CHECK(!runtime_live && !runtime_bytes);array_compile_owner_free(&owner);return status;
}
static void array_native_compiler_faults(void) {
    array_program_compile_attempts=0;XrCompileResourceStats stats={0};
    CHECK(array_native_compiler_probe(array_compile_caps(),&stats)==XR_XIR_OK);
    size_t sites=array_program_compile_attempts;CHECK(sites && sites<20000);
    printf("Array native seal baseline sites=%zu allocated=%llu peak=%llu work=%llu\n",sites,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    for(size_t ordinal=0;ordinal<sites;++ordinal) {
        array_program_compile_attempts=0;array_program_compile_fail_at=ordinal;array_program_compile_injected=false;
        XrXirStatus status=array_native_compiler_probe(array_compile_caps(),NULL);
        array_program_compile_fail_at=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"Array native seal ordinal=%zu status=%u injected=%u\n",ordinal,status,array_program_compile_injected);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && array_program_compile_injected && array_program_compile_attempts>ordinal);
        CHECK(!array_program_compile_live && !array_program_compile_bytes && !runtime_live && !runtime_bytes);
    }
    for(unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits exact=array_compile_caps();
        if(!axis)exact.allocated_bytes=stats.allocated_bytes;
        else if(axis==1)exact.live_bytes=stats.peak_bytes;else exact.work=stats.work;
        CHECK(array_native_compiler_probe(exact,NULL)==XR_XIR_OK);
        if(!axis)--exact.allocated_bytes;else if(axis==1)--exact.live_bytes;else --exact.work;
        CHECK(array_native_compiler_probe(exact,NULL)==XR_XIR_BUDGET);
    }
    printf("Array native seal all %zu actual OOM ordinals and 3 exact/minus1 axes; occupied output preserved; physical=0/0\n",sites);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--compiler")));
    if(argc==2) {array_native_compiler_faults();return 0;}
    ArrayCompileOwner owner;CHECK(array_compile_owner_new(array_compile_caps(),&owner)==XR_COMPILE_RESOURCE_OK);
    ArrayProgramEntries entries;memcpy(entries.entries,array_generated_selection,sizeof(entries.entries));
    for(unsigned i=0;i<ARRAY_ENTRY_COUNT;++i)CHECK(entries.entries[i]<array_generated_program.entry_count);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&array_generated_program,&program)==XR_XIR_OK && program);
    int status=array_case(program,entries,XR_ARRAY_CASE);
    CHECK(!runtime_live && !runtime_bytes);array_compile_owner_free(&owner);return status;
}
