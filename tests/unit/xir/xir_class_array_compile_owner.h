/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_array_compile_owner.h - One bounded ledger for class Array consumers
 *
 * KEY CONCEPT:
 *   Each whole graph retains its compiler owner through the final escaped value.
 */
#ifndef XIR_CLASS_ARRAY_COMPILE_OWNER_H
#define XIR_CLASS_ARRAY_COMPILE_OWNER_H
#include "xir/xxir_compile_context.h"
typedef XrXirStatus (*ClassArrayBuild)(const XrXirCompileContext *, unsigned, XrXirProgram **);
static inline XrCompileResourceLimits class_array_limits(void) {
    return (XrCompileResourceLimits){33554432,8388608,64000000};
}
static inline XrXirCompileContext class_array_context(XrCompileResourceLimits limits) {
    XrXirCompileContext context={0};
    C(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static inline XrCompileResourceStats class_array_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    C(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static inline void class_array_owner_free(XrXirCompileContext *context,XrCompileResourceStats baseline) {
    XrCompileResourceStats stats=class_array_stats(context);
    C(stats.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context->resources);*context=(XrXirCompileContext){0};
    instance_compile_zero();C(!runtime_live && !runtime_bytes && !runtime_owned);
}
static inline void class_array_pipeline_report(const XrXirCompileContext *context,const char *name) {
    XrCompileResourceStats stats=class_array_stats(context);
    printf("class Array %s compiler allocated=%llu live=%llu peak=%llu work=%llu attempts=%zu\n",
        name,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.live_bytes,
        (unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,instance_compile_attempts);
}
static inline void class_array_compiler_faults(ClassArrayBuild build,unsigned variant) {
    XrCompileResourceStats exact={0};size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        instance_compile_attempts=0;instance_compile_fail_at=0;instance_compile_injected=false;
        if(!pass) {
            XrCompileResources *rejected=NULL;XrCompileResourceLimits limits=class_array_limits();
            C(xr_compile_resources_new(&limits,&rejected)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !rejected);
            C(instance_compile_injected);instance_compile_zero();
        }
        instance_compile_fail_at=SIZE_MAX;
        XrXirCompileContext context=class_array_context(class_array_limits());
        XrCompileResourceStats baseline=class_array_stats(&context);XrXirProgram *program=NULL;
        instance_compile_attempts=0;instance_compile_fail_at=pass?pass-1:SIZE_MAX;instance_compile_injected=false;
        XrXirStatus status=build(&context,variant,&program);
        if(!pass) {C(status==XR_XIR_OK && program);sites=instance_compile_attempts;C(sites>0 && sites<20000);exact=class_array_stats(&context);}
        else {if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"class compiler ordinal%zu status%u\n",pass-1,status);
            C(status==XR_XIR_OUT_OF_MEMORY && !program && instance_compile_injected);}
        instance_compile_fail_at=SIZE_MAX;xr_xir_compile_program_drop(program);class_array_owner_free(&context,baseline);
    }
    for(unsigned axis=0;axis<4;++axis) {
        XrCompileResourceLimits limits={exact.allocated_bytes,exact.peak_bytes,exact.work};
        if(axis==1)--limits.allocated_bytes;if(axis==2)--limits.live_bytes;if(axis==3)--limits.work;
        XrXirCompileContext context=class_array_context(limits);XrCompileResourceStats baseline=class_array_stats(&context);
        XrXirProgram *program=NULL;XrXirStatus status=build(&context,variant,&program);
        C(axis?(status==XR_XIR_BUDGET && !program):(status==XR_XIR_OK && program));
        xr_xir_compile_program_drop(program);class_array_owner_free(&context,baseline);
    }
    printf("class Array compiler variant%u: %zu actual OOM; exact/three minus-one; both physical0\n",variant,sites);
}
#endif // XIR_CLASS_ARRAY_COMPILE_OWNER_H
