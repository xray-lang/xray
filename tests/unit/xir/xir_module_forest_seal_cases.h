/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_forest_seal_cases.h - Fail-closed execution closure publication
 */
#ifndef XIR_MODULE_FOREST_SEAL_CASES_H
#define XIR_MODULE_FOREST_SEAL_CASES_H
#include "xir/xxir_compile_memory.h"
static XrXirStatus module_forest_seal_probe(const XrXirProgramSpec *spec,
    XrCompileResourceLimits limits,size_t ordinal,XrCompileResourceStats *stats,size_t *attempts) {
    CHECK(source_program_compile_fail_at==SIZE_MAX);
    size_t physical=source_program_compile_live,physical_bytes=source_program_compile_bytes;
    source_program_compile_attempts=0;source_program_compile_injected=false;source_program_compile_fail_at=ordinal;
    XrXirCompileContext ctx={0};ctx.limits=xr_xir_compile_default_limits();
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&ctx.resources);
    XrXirProgram *program=NULL;
    XrXirStatus status=xir_compile_resource_status(created);
    XrCompileResourceStats baseline={0};
    if(status==XR_XIR_OK){baseline=consumer_context_stats(&ctx);status=xr_xir_compile_program_seal(&ctx,spec,&program);}
    *attempts=source_program_compile_attempts;
    if(ordinal!=SIZE_MAX)CHECK(source_program_compile_injected && status==XR_XIR_OUT_OF_MEMORY && !program);
    if(status!=XR_XIR_OK)CHECK(!program);
    if(ctx.resources)*stats=consumer_context_stats(&ctx);
    source_program_compile_fail_at=SIZE_MAX;
    xr_xir_compile_program_drop(program);
    if(ctx.resources)consumer_context_ephemeral_free(&ctx,baseline);
    CHECK(source_program_compile_live==physical && source_program_compile_bytes==physical_bytes);
    CHECK(!runtime_live && !runtime_bytes);
    return status;
}
static XrXirProgram *module_forest_seal(const XrXirProgramSpec *spec) {
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrCompileResourceStats stats={0};size_t sites=0;
    CHECK(!runtime_live && !runtime_bytes);
    CHECK(module_forest_seal_probe(spec,limits,SIZE_MAX,&stats,&sites)==XR_XIR_OK);
    for(size_t fail=0;fail<sites;++fail){size_t prefix=0;XrCompileResourceStats failed={0};
        CHECK(module_forest_seal_probe(spec,limits,fail,&failed,&prefix)==XR_XIR_OUT_OF_MEMORY);}
    CHECK(stats.peak_bytes>1);
    for(unsigned below=0;below<2;++below){
        XrCompileResourceLimits probe={XR_COMPILE_MEMORY_FINITE,stats.peak_bytes-below,true};
        XrCompileResourceStats used={0};size_t attempts=0;
        CHECK(module_forest_seal_probe(spec,probe,SIZE_MAX,&used,&attempts)==
            (below?XR_XIR_MEMORY_LIMIT:XR_XIR_OK));
        CHECK(used.peak_bytes<=probe.memory_limit);
    }
    XrXirCompileContext ctx=consumer_context_limits(limits);XrXirProgram *program=NULL;
    XrXirProgram *occupied=(XrXirProgram *)(uintptr_t)1;
    CHECK(xr_xir_compile_program_seal(&ctx,spec,&occupied)==XR_XIR_BAD_STRUCTURE && occupied==(XrXirProgram *)(uintptr_t)1);
    CHECK(xr_xir_compile_program_seal(&ctx,spec,&program)==XR_XIR_OK);
    printf("Module forest seal: %zu compiler allocation failures, held memory exact/minus, both physical domains restored\n",sites);
    return program;
}
#endif // XIR_MODULE_FOREST_SEAL_CASES_H
