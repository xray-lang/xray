/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_module_state_class_native.c - Execute actual generated native class initialization Programs
 *
 * KEY CONCEPT:
 *   Finite complete Programs preserve real ownership and physical cleanup.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_module_state_class_owner.h"
#include "xir_module_state_class_cases.h"
#define STATE_JOIN_(a,b) a##b
#define STATE_JOIN(a,b) STATE_JOIN_(a,b)
XR_DATA const XrXirProgramSpec STATE_JOIN(XR_STATE_CLASS_PREFIX,_program);
static XrXirStatus state_class_seal_probe(const XrCompileResourceLimits *caps,size_t fail_at,
    XrCompileResourceStats *stats,size_t *points) {
    CHECK(!runtime_live && !source_program_compile_live);source_program_compile_attempts=0;
    source_program_compile_fail_at=fail_at;source_program_compile_injected=false;
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};XrXirProgram *program=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(caps,&context.resources);
    XrXirStatus status=created==XR_COMPILE_RESOURCE_OK?
        xr_xir_compile_program_seal(&context,&STATE_JOIN(XR_STATE_CLASS_PREFIX,_program),&program):
        (created==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET);
    CHECK(status==XR_XIR_OK?program!=NULL:program==NULL);
    *points=source_program_compile_attempts;source_program_compile_fail_at=SIZE_MAX;
    xr_xir_compile_program_drop(program);
    if(context.resources) {
        CHECK(xr_compile_resources_stats(context.resources,stats)==XR_COMPILE_RESOURCE_OK);
        CHECK(stats->live_bytes==sizeof(XrCompileResources));xr_compile_resources_release(context.resources);
    }
    CHECK(!runtime_live && !runtime_bytes && !source_program_compile_live && !source_program_compile_bytes);
    return status;
}
static void state_class_seal_scan(void) {
    const XrCompileResourceLimits caps={UINT64_C(32)*1024*1024,UINT64_C(8)*1024*1024,64000000};
    XrCompileResourceStats baseline={0};size_t points=0;
    CHECK(state_class_seal_probe(&caps,SIZE_MAX,&baseline,&points)==XR_XIR_OK);size_t total=points;
    for(size_t point=0;point<total;++point) {
        XrCompileResourceStats current={0};size_t attempts=0;
        CHECK(state_class_seal_probe(&caps,point,&current,&attempts)==XR_XIR_OUT_OF_MEMORY);
        CHECK(source_program_compile_injected && attempts>point);
    }
    for(uint32_t axis=0;axis<3;++axis) {
        XrCompileResourceLimits exact=caps;
        if(axis==0)exact.allocated_bytes=baseline.allocated_bytes;
        else if(axis==1)exact.live_bytes=baseline.peak_bytes;else exact.work=baseline.work;
        XrCompileResourceStats current={0};size_t attempts=0;
        CHECK(state_class_seal_probe(&exact,SIZE_MAX,&current,&attempts)==XR_XIR_OK);
        if(axis==0)--exact.allocated_bytes;else if(axis==1)--exact.live_bytes;else --exact.work;
        CHECK(state_class_seal_probe(&exact,SIZE_MAX,&current,&attempts)==XR_XIR_BUDGET);
        CHECK(!source_program_compile_injected);
    }
    printf("class state %u native seal: every %zu actual allocation ordinal; exact/minus1 allocated=%llu live=%llu work=%llu; physical0/0\n",
        XR_STATE_CLASS_SCENARIO,total,(unsigned long long)baseline.allocated_bytes,
        (unsigned long long)baseline.peak_bytes,(unsigned long long)baseline.work);
    source_program_owners_free();
}
int main(void) {
    const XrXirProgramSpec *spec=&STATE_JOIN(XR_STATE_CLASS_PREFIX,_program);
    for(uint32_t order=0;order<2;++order) {
        const XrXirCompileContext *context=source_program_owner(UINT64_C(32)*1024*1024,64000000);
        for(uint32_t abi=4;abi<=6;++abi) {
            XrXirProgramSpec stale=*spec;stale.abi_version=abi;XrXirProgram *out=NULL;
            CHECK(xr_xir_compile_program_seal(context,&stale,&out)==XR_XIR_BAD_LAYOUT && !out);
        }
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,spec,&program)==XR_XIR_OK);
        state_class_pair(program,XR_STATE_CLASS_SCENARIO,order);source_program_owners_free();
    }
    const XrXirCompileContext *context=source_program_owner(UINT64_C(32)*1024*1024,64000000);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,spec,&program)==XR_XIR_OK);
    state_class_runtime_scan(program,XR_STATE_CLASS_SCENARIO);source_program_owners_free();
    state_class_seal_scan();xr_free(runtime_owned);return 0;
}
