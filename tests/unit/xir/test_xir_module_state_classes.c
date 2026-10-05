/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_module_state_classes.c - Compile and fault-scan complete class initialization Programs
 *
 * KEY CONCEPT:
 *   Finite complete Programs preserve real ownership and physical cleanup.
 */

#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_module_state_class_owner.h"
#include "xir_module_state_class_fixture.h"
#include "xir_module_state_class_cases.h"
static XrXirStatus state_class_pipeline(const XrXirCompileContext *context,uint32_t scenario,
    XrXirProgram **program,const char *path) {
    XrXirArtifact *artifact=NULL;XrXirStatus status=state_class_build(context,scenario,&artifact);
    XrXirCSource source={0};char prefix[32];CHECK(snprintf(prefix,sizeof(prefix),"state_class_%u",scenario)>0);
    if(status==XR_XIR_OK)status=xr_xir_compile_emit_c(artifact,prefix,1024*1024,&source);
    CHECK(status==XR_XIR_OK || (!source.text && !source.length));
    if(status==XR_XIR_OK && path) {
        CHECK(!strstr(source.text,"({"));FILE *file=fopen(path,"wb");CHECK(file);
        CHECK(fwrite(source.text,1,source.length,file)==source.length && !fclose(file));
    }
    xr_xir_compile_c_source_free(&source);
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&artifact,program);
    CHECK(status==XR_XIR_OK || !*program);
    xr_xir_compile_artifact_free(artifact);return status;
}
static XrXirStatus state_class_probe(uint32_t scenario,const XrCompileResourceLimits *caps,
    size_t fail_at,XrCompileResourceStats *stats,size_t *points) {
    CHECK(!runtime_live && !source_program_compile_live);source_program_compile_attempts=0;
    source_program_compile_fail_at=fail_at;source_program_compile_injected=false;
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};XrXirProgram *program=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(caps,&context.resources);
    XrXirStatus status=created==XR_COMPILE_RESOURCE_OK?state_class_pipeline(&context,scenario,&program,NULL):
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
static void state_class_compiler_scan(uint32_t scenario) {
    const XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,128000000};
    XrCompileResourceStats stats={0};size_t points=0;
    CHECK(state_class_probe(scenario,&caps,SIZE_MAX,&stats,&points)==XR_XIR_OK);size_t total=points;
    for(size_t point=0;point<total;++point) {
        XrCompileResourceStats fault={0};size_t attempts=0;
        CHECK(state_class_probe(scenario,&caps,point,&fault,&attempts)==XR_XIR_OUT_OF_MEMORY);
        CHECK(source_program_compile_injected && attempts>point);
    }
    for(uint32_t axis=0;axis<3;++axis) {
        XrCompileResourceLimits exact=caps;
        if(axis==0)exact.allocated_bytes=stats.allocated_bytes;
        else if(axis==1)exact.live_bytes=stats.peak_bytes;else exact.work=stats.work;
        XrCompileResourceStats current={0};size_t attempts=0;
        CHECK(state_class_probe(scenario,&exact,SIZE_MAX,&current,&attempts)==XR_XIR_OK);
        if(axis==0)--exact.allocated_bytes;else if(axis==1)--exact.live_bytes;else --exact.work;
        CHECK(state_class_probe(scenario,&exact,SIZE_MAX,&current,&attempts)==XR_XIR_BUDGET);
        CHECK(!source_program_compile_injected);
    }
    printf("class state %u compiler: every %zu actual allocation ordinal; exact/minus1 allocated=%llu live=%llu work=%llu; physical0/0\n",
        scenario,total,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
}
static void state_class_rejections(uint32_t scenario) {
    const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,128000000);
    for(uint32_t variant=1;variant<=2;++variant) {
        StateClassFixture f;state_class_fixture(&f,scenario,variant);XrXirArtifact *out=NULL;
        CHECK(xr_xir_compile_check(context,&f.module,&out,NULL)==(variant==1?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE) && !out);
    }
    source_program_owners_free();
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==3);uint32_t first=argc==3?(uint32_t)strtoul(argv[2],NULL,10):5;
    CHECK(first==5 || first==8);
    for(uint32_t scenario=first;;scenario=8) {
        state_class_rejections(scenario);
        for(uint32_t order=0;order<2;++order) {
            const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,128000000);
            XrXirProgram *program=NULL;CHECK(state_class_pipeline(context,scenario,&program,argc==3&&order==0?argv[1]:NULL)==XR_XIR_OK);
            state_class_pair(program,scenario,order);source_program_owners_free();
        }
        if(argc==1) {
            const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,128000000);
            XrXirProgram *program=NULL;CHECK(state_class_pipeline(context,scenario,&program,NULL)==XR_XIR_OK);
            state_class_runtime_scan(program,scenario);source_program_owners_free();
            state_class_compiler_scan(scenario);source_program_owners_free();
        }
        if(argc==3 || scenario==8)break;
    }
    xr_free(runtime_owned);return 0;
}
