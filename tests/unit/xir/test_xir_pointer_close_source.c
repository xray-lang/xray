/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_pointer_close_source.c - Retained close owners through the public API
 *
 * KEY CONCEPT:
 *   Real page-provider refusal is independent of an owned terminal outcome.
 *   Retrying close neither reruns the program nor repeats successful unmaps.
 */
#include "xray_source_entry.h"
#include "xray_source_instance.h"
#include "xray_source_repl.h"
#include "xir/xxir_pointer.h"
#include "xir/xxir_call.h"
#include "xir/xxir_value_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"close %d: %s\n",__LINE__,#c);exit(1);}} while (0)
XR_FUNC void pointer_fail_next_unmap(void);
XR_FUNC void pointer_fail_unmap_after(unsigned);
XR_FUNC XrXirDomain *pointer_instance_observed_domain(void);
XR_FUNC uint64_t pointer_instance_observed_frame(void);
XR_FUNC void pointer_instance_observer_drop(void);
typedef struct CloseCapture {
    XrXirTypeArena *arena;
    XrXirValue pages[2];
    unsigned writes;
    bool partial;
} CloseCapture;
static void pages_open(CloseCapture *capture) {
    XrXirDomain *domain=pointer_instance_observed_domain();CHECK(domain);
    XrXirType type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_page_allocate(domain,capture->arena,type,1,3,&capture->pages[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_page_allocate(domain,capture->arena,type,17,3,&capture->pages[1])==XR_XIR_VALUE_OK);
    if (capture->partial) pointer_fail_unmap_after(1);else pointer_fail_next_unmap();
}
static XrSourceWriteStatus capture_write(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    CloseCapture *capture=context;CHECK(stream==XR_SOURCE_STDOUT && length==2 && !memcmp(bytes,"9\n",2));
    CHECK(++capture->writes==1);pages_open(capture);return XR_SOURCE_WRITE_OK;
}
static XrSourceArtifact *artifact_new(void) {
    static const char source[]="export fn answer()->i64{print(9);return 42}\n";
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,
        .value.text={NULL,"pointer-close-owner",source,sizeof(source)-1}};
    XrSourceCompileOptions options=xray_source_compile_options_default();
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_build(&input,&options,&artifact,&failure,&diagnostic);
    if (status!=XR_SOURCE_OK) fprintf(stderr,"close Source status=%u stage=%u\n",status,failure.stage);
    CHECK(status==XR_SOURCE_OK && artifact);xray_source_diagnostic_free(diagnostic);return artifact;
}
static void one_shot(XrSourceExecution *execution,XrSourceArtifact *artifact,CloseCapture *capture) {
    XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.write=capture_write;options.context=capture;
    XrSourceInvocation invocation;
    CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.name="answer";invocation.name_length=6;
    XrSourceResult *result=NULL;XrSourceCall *pending=NULL;XrSourceEvent failure={0},cleanup={0};
    XrSourceCompletion completion={XRAY_SOURCE_API_VERSION,sizeof(completion),&result,&pending,&failure,&cleanup};
    XrSourceCallOptions old=options;old.api_version=1;
    CHECK(xray_source_invoke(execution,artifact,&invocation,&old,(XrSourceCompletion *)(uintptr_t)1)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(xray_source_invoke(execution,artifact,&invocation,&options,&completion)==XR_SOURCE_IO);
    CHECK(pending && result && capture->writes==1 && failure.state==XR_SOURCE_RUN_RETURNED &&
        cleanup.state==XR_SOURCE_RUN_CLEANUP_FAILED && cleanup.engine_status==XR_XIR_CALL_CLOSE_FAILED);
    int64_t value=0;CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==42);
    XrSourceEvent untouched={.epoch=123};XrSourceResult *other=NULL;
    CHECK(xray_source_call_poll(pending,1,&untouched)==XR_SOURCE_BAD_STATE && untouched.epoch==123);
    CHECK(xray_source_call_cancel(pending)==XR_SOURCE_BAD_STATE &&
        xray_source_call_resume(pending,0,0)==XR_SOURCE_BAD_STATE &&
        xray_source_call_take(pending,&other)==XR_SOURCE_BAD_STATE && !other);
    XrXirDomain *domain=pointer_instance_observed_domain();
    uint64_t before=xr_xir_domain_stats(domain).live_bytes;
    if (capture->partial) {
        uint8_t byte=77;
        CHECK(xr_xir_pointer_store(&capture->pages[0],0,13)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_pointer_load(&capture->pages[1],0,&byte)==XR_XIR_VALUE_BAD_ARGUMENT && byte==77);
    }
    pointer_fail_next_unmap();XrSourceCall *same=pending;
    CHECK(xray_source_call_drop(&pending,&cleanup)==XR_SOURCE_IO && pending==same &&
        xr_xir_domain_stats(domain).live_bytes==before && capture->writes==1);
    CHECK(xray_source_call_drop(&pending,&cleanup)==XR_SOURCE_OK && !pending && capture->writes==1);
    CHECK(cleanup.engine_status==XR_XIR_CALL_READY);
    xray_source_result_free(result);
    xr_xir_value_drop(&capture->pages[0]);xr_xir_value_drop(&capture->pages[1]);
    CHECK(xr_xir_domain_stats(domain).live_bytes==pointer_instance_observed_frame());
    pointer_instance_observer_drop();
}
static void repl_generation(XrSourceExecution *execution,CloseCapture *capture) {
    XrSourceCompileOptions compile=xray_source_compile_options_default();
    XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.write=capture_write;options.context=capture;
    XrSourceRepl *owner=NULL;
    CHECK(xray_source_repl_new(&compile,execution,"close-generations",&options,&owner)==XR_SOURCE_OK);
    capture->partial=true;capture->writes=0;pages_open(capture);
    const char *sources[]={"var keep:i64=7\n","var next:i64=keep+1\n"};
    for (unsigned i=0;i<2;++i) {
        XrSourceText text={NULL,"close-generations",sources[i],strlen(sources[i])};
        XrSourceDiagnostic *diagnostic=NULL;
        CHECK(xray_source_repl_begin(owner,&text,&diagnostic)==XR_SOURCE_OK);
        xray_source_diagnostic_free(diagnostic);
        XrSourceEvent observed={0};
        do {CHECK(xray_source_repl_poll(owner,256,&observed)==XR_SOURCE_OK);}
        while(observed.state==XR_SOURCE_RUN_READY);
        CHECK(observed.state==XR_SOURCE_RUN_RETURNED);
        XrSourceResult *result=NULL;CHECK(xray_source_repl_take(owner,&result)==XR_SOURCE_OK && result);
        xray_source_result_free(result);
    }
    XrSourceRepl *same=owner;XrSourceEvent cleanup={0};
    CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_IO && owner==same &&
        cleanup.engine_status==XR_XIR_CALL_CLOSE_FAILED && capture->writes==0);
    XrSourceText text={NULL,"close-generations","print(9)\n",9};
    CHECK(xray_source_repl_begin(owner,&text,NULL)==XR_SOURCE_BAD_STATE);
    XrXirDomain *domain=pointer_instance_observed_domain();
    uint64_t held=xr_xir_domain_stats(domain).live_bytes;
    pointer_fail_next_unmap();
    CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_IO && owner==same &&
        xr_xir_domain_stats(domain).live_bytes==held);
    CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && !owner && capture->writes==0);
    xr_xir_value_drop(&capture->pages[0]);xr_xir_value_drop(&capture->pages[1]);
    CHECK(xr_xir_domain_stats(domain).live_bytes==pointer_instance_observed_frame());
    pointer_instance_observer_drop();
}
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats frame={0};CHECK(xr_compile_resources_stats(context.resources,&frame)==XR_COMPILE_RESOURCE_OK);
    const XrXirTypeNode node={.kind=XR_XIR_TYPE_MUT_PTR,.element=XR_XIR_U8};
    XrXirTypes types={&node,1,NULL,NULL};CloseCapture capture={0};
    CHECK(xr_xir_compile_type_arena_new(&context,&types,&capture.arena)==XR_XIR_VALUE_OK);
    XrSourceExecutionOptions options=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&options,&execution)==XR_SOURCE_OK);
    XrSourceArtifact *artifact=artifact_new();
    one_shot(execution,artifact,&capture);
    capture.writes=0;capture.partial=true;one_shot(execution,artifact,&capture);
    repl_generation(execution,&capture);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
    xr_xir_compile_type_arena_drop(capture.arena);XrCompileResourceStats final={0};
    CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK && final.held_bytes==frame.held_bytes);
    xr_compile_resources_release(context.resources);puts("Source close owner result/partial/retry PASS");return 0;
}
