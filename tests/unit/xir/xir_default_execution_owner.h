/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_default_execution_owner.h - One bounded ledger for default graph consumers
 *
 * KEY CONCEPT:
 *   Each whole graph retains its compiler owner through the final escaped value.
 */
#ifndef XIR_DEFAULT_EXECUTION_OWNER_H
#define XIR_DEFAULT_EXECUTION_OWNER_H
#include "xir/xxir_compile_context.h"
typedef XrXirStatus (*DefaultBuild)(const XrXirCompileContext *, unsigned, XrXirProgram **);
static inline XrCompileResourceLimits default_limits(void) {
    return (XrCompileResourceLimits){33554432,8388608,64000000};
}
static inline XrXirCompileContext default_context(XrCompileResourceLimits limits) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static inline XrCompileResourceStats default_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static inline void default_owner_free(XrXirCompileContext *context,XrCompileResourceStats baseline) {
    XrCompileResourceStats stats=default_stats(context);
    CHECK(stats.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context->resources);*context=(XrXirCompileContext){0};
    instance_compile_zero();CHECK(!runtime_live && !runtime_bytes && !runtime_owned);
}
static inline void default_pipeline_report(const XrXirCompileContext *context,const char *name) {
    XrCompileResourceStats stats=default_stats(context);
    printf("default graph %s compiler allocated=%llu live=%llu peak=%llu work=%llu attempts=%zu\n",
        name,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.live_bytes,
        (unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,instance_compile_attempts);
}
static inline void default_compiler_faults(DefaultBuild build,unsigned variant) {
    XrCompileResourceStats exact={0};size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        instance_compile_attempts=0;instance_compile_fail_at=0;instance_compile_injected=false;
        if(!pass) {
            XrCompileResources *rejected=NULL;XrCompileResourceLimits limits=default_limits();
            CHECK(xr_compile_resources_new(&limits,&rejected)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !rejected);
            CHECK(instance_compile_injected);instance_compile_zero();
        }
        instance_compile_fail_at=SIZE_MAX;
        XrXirCompileContext context=default_context(default_limits());
        XrCompileResourceStats baseline=default_stats(&context);XrXirProgram *program=NULL;
        instance_compile_attempts=0;instance_compile_fail_at=pass?pass-1:SIZE_MAX;instance_compile_injected=false;
        XrXirStatus status=build(&context,variant,&program);
        if(!pass) {CHECK(status==XR_XIR_OK && program);sites=instance_compile_attempts;CHECK(sites>0 && sites<20000);exact=default_stats(&context);}
        else {if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"default compiler ordinal%zu status%u\n",pass-1,status);
            CHECK(status==XR_XIR_OUT_OF_MEMORY && !program && instance_compile_injected);}
        instance_compile_fail_at=SIZE_MAX;xr_xir_compile_program_drop(program);default_owner_free(&context,baseline);
    }
    for(unsigned axis=0;axis<4;++axis) {
        XrCompileResourceLimits limits={exact.allocated_bytes,exact.peak_bytes,exact.work};
        if(axis==1)--limits.allocated_bytes;if(axis==2)--limits.live_bytes;if(axis==3)--limits.work;
        XrXirCompileContext context=default_context(limits);XrCompileResourceStats baseline=default_stats(&context);
        XrXirProgram *program=NULL;XrXirStatus status=build(&context,variant,&program);
        CHECK(axis?(status==XR_XIR_BUDGET && !program):(status==XR_XIR_OK && program));
        xr_xir_compile_program_drop(program);default_owner_free(&context,baseline);
    }
    printf("default graph compiler variant%u: %zu actual OOM; exact/three minus-one; both physical0\n",variant,sites);
}
#endif // XIR_DEFAULT_EXECUTION_OWNER_H
