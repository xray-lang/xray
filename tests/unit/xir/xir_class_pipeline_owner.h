/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_pipeline_owner.h - One finite owner for complete class producers
 *
 * KEY CONCEPT:
 *   Compiler metadata and runtime storage retain and refund their actual owners.
 */
#ifndef XIR_CLASS_PIPELINE_OWNER_H
#define XIR_CLASS_PIPELINE_OWNER_H
#include "xir_source_program_compile_owner.h"
typedef struct ClassPipelineOwner {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
    size_t blocks,bytes;
} ClassPipelineOwner;
typedef XrXirStatus (*ClassPipelineOperation)(const XrXirCompileContext *,void *);
static const XrCompileResourceLimits class_pipeline_limits={33554432,8388608,64000000};
static inline XrCompileResourceStats class_pipeline_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static inline XrXirStatus class_pipeline_new(ClassPipelineOwner *owner,const XrCompileResourceLimits *limits) {
    CHECK(!owner->context.resources && source_program_compile_fail_at==SIZE_MAX);
    owner->blocks=source_program_compile_live;owner->bytes=source_program_compile_bytes;
    XrCompileResourceStatus status=xr_compile_resources_new(limits,&owner->context.resources);
    if(status!=XR_COMPILE_RESOURCE_OK)return status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    owner->context.limits=xr_xir_compile_default_limits();owner->baseline=class_pipeline_stats(&owner->context);return XR_XIR_OK;
}
static inline void class_pipeline_drop(ClassPipelineOwner *owner) {
    CHECK(source_program_compile_fail_at==SIZE_MAX);
    if(owner->context.resources){CHECK(class_pipeline_stats(&owner->context).live_bytes==owner->baseline.live_bytes);xr_compile_resources_release(owner->context.resources);}
    CHECK(source_program_compile_live==owner->blocks && source_program_compile_bytes==owner->bytes);*owner=(ClassPipelineOwner){0};
}
static inline void class_pipeline_release_producer(ClassPipelineOwner *owner) {
    /* Owned arenas and Programs retain this ledger after its producer handle dies. */
    CHECK(owner->context.resources && source_program_compile_fail_at==SIZE_MAX);
    xr_compile_resources_release(owner->context.resources);*owner=(ClassPipelineOwner){0};
}
static inline void class_pipeline_faults(const char *name,ClassPipelineOperation operation,void *fixture) {
    size_t before_blocks=source_program_compile_live,before_bytes=source_program_compile_bytes;
    XrCompileResources *rejected=NULL;source_program_compile_attempts=0;source_program_compile_fail_at=0;source_program_compile_injected=false;
    CHECK(xr_compile_resources_new(&class_pipeline_limits,&rejected)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !rejected && source_program_compile_injected);
    source_program_compile_fail_at=SIZE_MAX;CHECK(source_program_compile_live==before_blocks && source_program_compile_bytes==before_bytes);
    ClassPipelineOwner owner={0};CHECK(class_pipeline_new(&owner,&class_pipeline_limits)==XR_XIR_OK);
    source_program_compile_attempts=0;CHECK(operation(&owner.context,fixture)==XR_XIR_OK);
    size_t sites=source_program_compile_attempts;CHECK(sites);XrCompileResourceStats exact=class_pipeline_stats(&owner.context);class_pipeline_drop(&owner);
    for(size_t site=0;site<sites;++site){
        CHECK(class_pipeline_new(&owner,&class_pipeline_limits)==XR_XIR_OK);
        source_program_compile_attempts=0;source_program_compile_fail_at=site;source_program_compile_injected=false;
        XrXirStatus status=operation(&owner.context,fixture);source_program_compile_fail_at=SIZE_MAX;
        if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"%s compiler ordinal%zu/%zu status%u\n",name,site,sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected);class_pipeline_drop(&owner);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned less=0;less<2;++less){
        XrCompileResourceLimits limits=class_pipeline_limits;
        uint64_t value=axis==0?exact.allocated_bytes:axis==1?exact.peak_bytes:exact.work;CHECK(value);
        uint64_t *cap=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;*cap=value-less;
        XrXirStatus status=class_pipeline_new(&owner,&limits);if(status==XR_XIR_OK)status=operation(&owner.context,fixture);
        CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK));class_pipeline_drop(&owner);
    }
    printf("%s compilerOOM=%zu allocated=%llu peak=%llu work=%llu threeaxes/physicalbaseline PASS\n",name,sites,(unsigned long long)exact.allocated_bytes,(unsigned long long)exact.peak_bytes,(unsigned long long)exact.work);
}
static inline void class_pipeline_final_zero(void) {
    CHECK(!source_program_compile_live && !source_program_compile_bytes);source_program_owners_free();
}
#endif // XIR_CLASS_PIPELINE_OWNER_H
