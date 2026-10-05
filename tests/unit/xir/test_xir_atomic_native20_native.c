/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_native20_native.c - Independent original Atomic native expectations
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_atomic_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_atomic_native20_cases.h"
XR_DATA const XrXirProgramSpec atomic_native20_program;
XR_DATA const uint32_t atomic_native20_selection[18];
static XrXirStatus atomic_native_compiler_probe(XrCompileResourceLimits caps,XrCompileResourceStats *stats) {
    AtomicCompileOwner owner;XrCompileResourceStatus opened=atomic_compile_owner_new(caps,&owner);
    if(opened!=XR_COMPILE_RESOURCE_OK) {
        atomic_compile_owner_free(&owner);
        return opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    }
    XrXirProgram *program=NULL;
    XrXirStatus status=xr_xir_compile_program_seal(&owner.context,&atomic_native20_program,&program);
    if(status!=XR_XIR_OK)CHECK(!program);
    else {
        CHECK(program);XrXirProgram *occupied=program;
        XrCompileResourceStats before=atomic_compile_stats(&owner);
        size_t attempts=atomic_program_compile_attempts,live=atomic_program_compile_live,bytes=atomic_program_compile_bytes;
        CHECK(xr_xir_compile_program_seal(&owner.context,&atomic_native20_program,&occupied)==XR_XIR_BAD_STRUCTURE);
        CHECK(occupied==program);
        XrCompileResourceStats after=atomic_compile_stats(&owner);
        CHECK(before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes &&
            before.peak_bytes==after.peak_bytes && before.work==after.work);
        CHECK(attempts==atomic_program_compile_attempts && live==atomic_program_compile_live && bytes==atomic_program_compile_bytes);
    }
    xr_xir_compile_program_drop(program);
    if(stats)*stats=atomic_compile_stats(&owner);
    CHECK(!runtime_live && !runtime_bytes);atomic_compile_owner_free(&owner);return status;
}
static void atomic_native_compiler_faults(void) {
    atomic_program_compile_attempts=0;XrCompileResourceStats stats={0};
    CHECK(atomic_native_compiler_probe(atomic_compile_caps(),&stats)==XR_XIR_OK);
    size_t sites=atomic_program_compile_attempts;CHECK(sites && sites<20000);
    printf("Atomic20 native seal baseline sites=%zu allocated=%llu peak=%llu work=%llu\n",sites,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    for(size_t ordinal=0;ordinal<sites;++ordinal) {
        atomic_program_compile_attempts=0;atomic_program_compile_fail_at=ordinal;atomic_program_compile_injected=false;
        XrXirStatus status=atomic_native_compiler_probe(atomic_compile_caps(),NULL);
        atomic_program_compile_fail_at=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"Atomic20 native seal ordinal=%zu status=%u injected=%u\n",ordinal,status,atomic_program_compile_injected);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && atomic_program_compile_injected && atomic_program_compile_attempts>ordinal);
        CHECK(!atomic_program_compile_live && !atomic_program_compile_bytes && !runtime_live && !runtime_bytes);
    }
    for(unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits exact=atomic_compile_caps();
        if(!axis)exact.allocated_bytes=stats.allocated_bytes;
        else if(axis==1)exact.live_bytes=stats.peak_bytes;else exact.work=stats.work;
        CHECK(atomic_native_compiler_probe(exact,NULL)==XR_XIR_OK);
        if(!axis)--exact.allocated_bytes;else if(axis==1)--exact.live_bytes;else --exact.work;
        CHECK(atomic_native_compiler_probe(exact,NULL)==XR_XIR_BUDGET);
    }
    printf("Atomic20 native seal all %zu actual OOM ordinals and 3 exact/minus1 axes; occupied output preserved; physical=0/0\n",sites);
    AtomicCompileOwner owner;CHECK(atomic_compile_owner_new(atomic_compile_caps(),&owner)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&owner.context,&atomic_native20_program,&program)==XR_XIR_OK);
    for(unsigned n=0;n<2;++n)atomic_runtime_faults(program,atomic_native20_selection[n],n);
    xr_xir_compile_program_drop(program);atomic_compile_owner_free(&owner);
    printf("Atomic20 native runtime actual faults i64=%zu bool=%zu; original 127 windows; physical0\n",atomic_runtime_sites[0],atomic_runtime_sites[1]);

}

int atomic_native20_run(unsigned mode,bool compiler) {
    CHECK(mode<20);if(compiler){atomic_native_compiler_faults();return 0;}
    AtomicCompileOwner owner;CHECK(atomic_compile_owner_new(atomic_compile_caps(),&owner)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&owner.context,&atomic_native20_program,&program)==XR_XIR_OK);
    for(unsigned n=0;n<18;++n)CHECK(atomic_native20_selection[n]<atomic_native20_program.entry_count);
    int code=0;if(mode<18)code=atomic_normal(program,atomic_native20_selection[mode],mode);
    else{atomic_runtime_faults(program,atomic_native20_selection[mode-18],mode-18);xr_xir_compile_program_drop(program);}
    CHECK(!runtime_live && !runtime_bytes);atomic_compile_owner_free(&owner);return code;
}
