#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_allocation_context.h - One finite owner per complete allocation probe
 */
#ifndef XIR_SOURCE_ALLOCATION_CONTEXT_H
#define XIR_SOURCE_ALLOCATION_CONTEXT_H
static XrCompileResourceStats allocation_last_stats;
static XrCompileResourceLimits allocation_limits(void) {
    return (XrCompileResourceLimits){UINT64_C(64)*1024*1024,
        UINT64_C(8)*1024*1024,UINT64_C(128000000)};
}
static XrXirCompileContext allocation_context(XrCompileResourceLimits limits) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static void allocation_context_close(XrXirCompileContext *context) {
    xr_compile_resources_release(context->resources);context->resources=NULL;
}
static XrXirStatus allocation_snapshot_copy(const XrXirSourceView *view,
    const XrCompileResourceLimits *limits,XrXirSourceSnapshot **output) {
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStatus created=xr_compile_resources_new(limits,&context.resources);
    if(created!=XR_COMPILE_RESOURCE_OK)return xir_compile_resource_status(created);
    XrXirStatus status=xir_fixture_snapshot_copy(&context,view,output);
    CHECK(xr_compile_resources_stats(context.resources,&allocation_last_stats)==XR_COMPILE_RESOURCE_OK);
    allocation_context_close(&context);return status;
}
static void allocation_snapshot_boundaries(const XrXirSourceView *view,XrCompileResourceStats required) {
    size_t baseline=source_fixture_compile_live;size_t before=source_fixture_compile_attempts;
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *bound=(uint64_t)((int64_t)*bound+delta);
        XrXirSourceSnapshot *snapshot=NULL;
        CHECK(allocation_snapshot_copy(view,&limits,&snapshot)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK((snapshot!=NULL)==(delta>=0));xr_xir_compile_source_snapshot_free(snapshot);
        CHECK(source_fixture_compile_live==baseline);
    }
    source_fixture_compile_attempts=before;
}
static XrXirStatus allocation_source_check_limits(const XrXirSourceRequest *request,
    const XrCompileResourceLimits *limits,XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    XrCompilerSession *session=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(limits,&context.resources);
    XrXirStatus status=xir_compile_resource_status(made);
    if(status==XR_XIR_OK){
        XrCompilerSessionStatus created=xr_compile_session_new(context.resources,&session);
        status=created==XR_COMPILER_SESSION_OK?XR_XIR_OK:
            created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
        if(status==XR_XIR_OK){
            XrXirSourceRequest owned=*request;owned.context=&context;owned.session=session;
            status=xr_xir_compile_source_check(&owned,output,diagnostic,NULL);
        }
        xr_compile_session_free(session);
        CHECK(xr_compile_resources_stats(context.resources,&allocation_last_stats)==XR_COMPILE_RESOURCE_OK);
        allocation_context_close(&context);
    }
    if(diagnostic && status!=XR_XIR_OK && !diagnostic->status){
        diagnostic->status=status;strcpy(diagnostic->message,"source allocation failed");
    }
    return status;
}
static XrXirStatus allocation_source_check(const XrXirSourceRequest *request,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    XrCompileResourceLimits limits=allocation_limits();
    return allocation_source_check_limits(request,&limits,output,diagnostic);
}
static bool allocation_private_context(SourceContext *context,XrCompileResourceLimits limits) {
    XrCompileResourceStatus status=xr_compile_resources_new(&limits,&context->compile.resources);
    context->compile.limits=xr_xir_compile_default_limits();
    context->remaining_blocks=context->compile.limits.blocks;
    context->remaining_instructions=context->compile.limits.instructions;
    if(status!=XR_COMPILE_RESOURCE_OK)context->diagnostic.status=xir_compile_resource_status(status);
    return status==XR_COMPILE_RESOURCE_OK;
}
static void allocation_private_close(SourceContext *context) {
    while(context->memory){SourceMemory *next=context->memory->next;
        xr_compile_resources_free(context->memory);context->memory=next;}
    allocation_context_close(&context->compile);
}
#endif
