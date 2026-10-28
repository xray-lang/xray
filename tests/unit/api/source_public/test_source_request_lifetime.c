/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_request_lifetime.c - Ready artifacts and independent execution ownership
 */
#include "xray_source_ast.h"
#include "xray_source_entry.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while(0)

/* This TU observes the actual compiler allocator. Installed SDK consumers use
 * the canonical archive implementation separately. No runtime budget is changed. */
#define xr_compile_resources_new calibration_resources_new
#include "../../xir/xir_sdk_resource_test.h"
#undef xr_compile_resources_new
#include "os/os_time.h"

/* Observe the actual provider returned to the synchronous public request.
 * This borrows its owner only until build returns and copies no allocator layout. */
static bool calibration_capture;
static XrCompileResources *calibration_owner;
static uint64_t calibration_initial_held;
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(
    const XrCompileResourceLimits *limits,XrCompileResources **output) {
    XrCompileResourceStatus status=calibration_resources_new(limits,output);
    if(status==XR_COMPILE_RESOURCE_OK&&calibration_capture&&!calibration_owner) {
        calibration_owner=*output;
        calibration_initial_held=sdk_stats(*output).held_bytes;
    }
    return status;
}
#include "hir/xhir_items.h"
#include "api/xsource_compile.h"

typedef struct Cancel { int cancelled; size_t polls; } Cancel;
static int cancel_poll(const void *context) {
    Cancel *cancel=(Cancel *)context;++cancel->polls;return cancel->cancelled;
}
static XrSourceExecution *execution_new(void) {
    XrSourceExecutionOptions options=xray_source_execution_options_default();
    XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&options,&execution)==XR_SOURCE_OK && execution);
    return execution;
}
static XrSourceEvent finish(XrSourceCall *call) {
    XrSourceEvent event={0};
    for(size_t i=0;i<4096;++i) {
        CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);
        if(event.state!=XR_SOURCE_RUN_READY)return event;
    }
    CHECK(0);return event;
}
static void independent_requests(void) {
    Cancel cancel={1,0};
    XrSourceCompileOptions options=xray_source_compile_options_default();
    options.stdlib_path=XR_PUBLIC_STDLIB;options.cancel_poll=cancel_poll;options.cancel_context=&cancel;
    char source[]="export fn answer()->i64 { return 42 }\n";
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,
        .value.text={NULL,"request-lifetime",source,sizeof(source)-1}};
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
    CHECK(xray_source_build(&input,&options,&artifact,&failure,&diagnostic)==XR_SOURCE_CANCELLED);
    CHECK(!artifact && failure.status==XR_SOURCE_CANCELLED && failure.cleanup_status==XR_SOURCE_OK);
    xray_source_diagnostic_free(diagnostic);diagnostic=NULL;cancel.cancelled=0;
    CHECK(xray_source_build(&input,&options,&artifact,&failure,&diagnostic)==XR_SOURCE_OK && artifact && !diagnostic);
    XrSourceArtifactInfo info={0};
    CHECK(xray_source_artifact_info(artifact,&info)==XR_SOURCE_OK && info.target==XR_SOURCE_TARGET_VM && info.named_entries==1);
    size_t polls=cancel.polls;cancel.cancelled=1;memset(source,'!',sizeof(source));
    XrSourceExecution *execution=execution_new();XrSourceCallOptions runtime;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    XrSourceInvocation invocation;
    CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.name="answer";invocation.name_length=6;
    XrSourceCall *call=NULL;
    CHECK(xray_source_invoke_begin(execution,artifact,&invocation,&runtime,&call)==XR_SOURCE_OK);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
    CHECK(finish(call).state==XR_SOURCE_RUN_RETURNED);XrSourceResult *result=NULL;
    CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
    int64_t value=0;CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==42);
    CHECK(cancel.polls==polls);xray_source_result_free(result);
}
static void detached_ast_and_result(void) {
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
    char source[]="export fn bytes()->string { return \"A\\0B\" }\n";
    XrSourceText text={NULL,"detached-request-ast",source,sizeof(source)-1};
    XrSourceAst *ast=NULL;XrSourceFailure failure={0};
    CHECK(xray_source_parse(&text,&options,&ast,&failure,NULL)==XR_SOURCE_OK && ast);
    memset(source,'!',sizeof(source));
    XrSourceArtifact *artifact=NULL;
    CHECK(xray_source_build_ast(ast,&options,&artifact,&failure,NULL)==XR_SOURCE_OK && artifact);
    xray_source_ast_free(ast);
    XrSourceExecution *execution=execution_new();XrSourceCallOptions runtime;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.name="bytes";invocation.name_length=5;XrSourceResult *result=NULL;
    CHECK(xray_source_invoke(execution,artifact,&invocation,&runtime,&result,NULL)==XR_SOURCE_OK && result);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
    const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_result_bytes(result,&bytes,&length)==XR_SOURCE_OK && length==3 && !memcmp(bytes,"A\0B",3));
    XrSourceResult *copy=NULL;CHECK(xray_source_result_copy(result,&copy)==XR_SOURCE_OK && copy);
    xray_source_result_free(result);
    CHECK(xray_source_result_bytes(copy,&bytes,&length)==XR_SOURCE_OK && length==3 && !memcmp(bytes,"A\0B",3));
    xray_source_result_free(copy);
}
static void ready_c_artifact(void) {
    XrSourceCompileOptions options=xray_source_compile_options_default();
    options.stdlib_path=XR_PUBLIC_STDLIB;options.target=XR_SOURCE_TARGET_C;
    const char source[]="const answer:i64=42\n";
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,
        .value.text={NULL,"request-c-artifact",source,sizeof(source)-1}};
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};
    CHECK(xray_source_build(&input,&options,&artifact,&failure,NULL)==XR_SOURCE_OK && artifact);
    const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_artifact_c_text(artifact,&bytes,&length)==XR_SOURCE_OK && bytes && length);
    XrSourceExecution *execution=execution_new();XrSourceCallOptions runtime;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    XrSourceResult *result=NULL;
    CHECK(xray_source_artifact_execute(execution,artifact,&runtime,&result,NULL)==XR_SOURCE_BAD_STATE && !result);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
}
static void execution_policy(void) {
    XrSourceExecution *execution=NULL;
    XrSourceExecutionOptions options={XR_SOURCE_MEMORY_FINITE,0};
    CHECK(xray_source_execution_new(&options,&execution)==XR_SOURCE_UNSUPPORTED && !execution);
    options=(XrSourceExecutionOptions){XR_SOURCE_MEMORY_ORDINARY,1};
    CHECK(xray_source_execution_new(&options,&execution)==XR_SOURCE_BAD_ARGUMENT && !execution);
}
typedef struct ArrayOutput { char bytes[16]; size_t size; } ArrayOutput;
static XrSourceWriteStatus array_write(void *context,XrSourceOutputStream stream,const char *bytes,size_t size) {
    ArrayOutput *out=context;
    CHECK(stream==XR_SOURCE_STDOUT && size<=sizeof(out->bytes)-out->size);
    memcpy(out->bytes+out->size,bytes,size);out->size+=size;return XR_SOURCE_WRITE_OK;
}
static void global_array_source(char *source,size_t length,const char *name,const char *expected) {
    Cancel cancel={0,0};XrSourceCompileOptions options=xray_source_compile_options_default();
    options.stdlib_path=XR_PUBLIC_STDLIB;options.cancel_poll=cancel_poll;options.cancel_context=&cancel;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"global-array-slice",source,length}};
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_build(&input,&options,&artifact,&failure,&diagnostic);
    if (status!=XR_SOURCE_OK && diagnostic) {
        XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);
        fprintf(stderr,"Global Array Source %u %d:%d %s\n",(unsigned)status,info.primary.line,info.primary.column,info.message);
    }
    CHECK(status==XR_SOURCE_OK && artifact && !diagnostic && failure.cleanup_status==XR_SOURCE_OK);
    size_t polls=cancel.polls;cancel.cancelled=1;memset(source,'!',length);
    XrSourceExecution *execution=execution_new();XrSourceCallOptions runtime;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    ArrayOutput out={0};runtime.write=array_write;runtime.context=&out;XrSourceCall *call=NULL;
    if (name) {
        XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
        invocation.name=name;invocation.name_length=strlen(name);
        CHECK(xray_source_invoke_begin(execution,artifact,&invocation,&runtime,&call)==XR_SOURCE_OK);
    } else CHECK(xray_source_artifact_begin(execution,artifact,&runtime,&call)==XR_SOURCE_OK);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
    XrSourceEvent event=finish(call);
    if (event.state!=XR_SOURCE_RUN_RETURNED)
        fprintf(stderr,"Global Array runtime state=%u engine=%u output=%zu named=%u\n",
            (unsigned)event.state,event.engine_status,out.size,name!=NULL);
    CHECK(event.state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
    CHECK(cancel.polls==polls && out.size==strlen(expected) && !memcmp(out.bytes,expected,out.size));
    if (name) {
        const char *bytes=NULL;size_t size=0;
        CHECK(xray_source_result_bytes(result,&bytes,&size)==XR_SOURCE_OK && size==11 && !memcmp(bytes,"array-owner",11));
        XrSourceResult *copy=NULL;CHECK(xray_source_result_copy(result,&copy)==XR_SOURCE_OK && copy);
        xray_source_result_free(result);result=copy;
        CHECK(xray_source_result_bytes(result,&bytes,&size)==XR_SOURCE_OK && size==11 && !memcmp(bytes,"array-owner",11));
    } else {
        int64_t value=-1;CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && !value);
    }
    xray_source_result_free(result);
}
static void global_array_lifetime(void) {
    char bytes[]="fn length(bytes:Slice<u8>)->i64 { return len(bytes) }\n"
        "var good:Array<u8>=[65,0,195,169]\nprint(length(good[:]))\n";
    global_array_source(bytes,sizeof(bytes)-1,NULL,"4\n");
    char numbers[]="fn length(bytes:Slice<u8>)->i64 { return len(bytes) }\n"
        "var good:Array<u8>=[65,0,195,169]\nvar values:Array<i64>=[7,9]\n"
        "print(length(good[:]))\nprint(len(values))\n"
        "export fn answer()->string { return \"array-owner\" }\n";
    global_array_source(numbers,sizeof(numbers)-1,"answer","4\n2\n");
}
static void global_string_lifetime(void) {
    char text[]="fn length(v:Slice<u8>)->i64 { return len(v) }\n"
        "var text:string=\"array-\"+\"owner\"\nvar empty:string=\"\"\nvar utf8:string=\"A\\0é\"\n"
        "print(length(text.bytes()))\nprint(length(empty.bytes()))\nprint(length(utf8.bytes()))\n"
        "export fn answer()->string { return text }\n";
    global_array_source(text,sizeof(text)-1,"answer","11\n0\n4\n");
}
/* Report roots are owned independently of parsing, requests and caller text. */
static void module_report_lifetime(bool syntax_first) {
    char source[]="const alpha:i64=1\r\nconst alpha:i64=2\r\nconst beta:i64=3\r\nconst beta:i64=4\r\nprint(9)\r\n";
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
    XrSourceText text={NULL,"two-independent-module-errors",source,sizeof(source)-1};
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text=text};
    XrSourceDiagnostic *report=NULL;XrSourceFailure failure={0};XrSourceArtifact *artifact=NULL;XrSourceAst *ast=NULL;
    XrSourceStatus rejected;
    if(syntax_first) {
        CHECK(xray_source_parse(&text,&options,&ast,&failure,NULL)==XR_SOURCE_OK&&ast);
        rejected=xray_source_build_ast(ast,&options,&artifact,&failure,&report);
    } else rejected=xray_source_build(&input,&options,&artifact,&failure,&report);
    if(rejected!=XR_SOURCE_BAD_ARGUMENT)fprintf(stderr,"module rejection=%u stage=%u syntax=%u\n",(unsigned)rejected,(unsigned)failure.stage,(unsigned)syntax_first);
    if(rejected!=XR_SOURCE_BAD_ARGUMENT&&report) {
        XrSourceDiagnosticReportInfo debug={0};(void)xray_source_diagnostic_report(report,&debug);
        for(uint32_t i=0;i<debug.roots;++i){XrSourceDiagnosticRoot root={0};(void)xray_source_diagnostic_root(report,i,&root);fprintf(stderr,"root %u %s\n",root.code,root.message);}
    }
    CHECK(rejected==XR_SOURCE_BAD_ARGUMENT);
    CHECK(!artifact&&report&&failure.cleanup_status==XR_SOURCE_OK);
    xray_source_ast_free(ast);memset(source,'!',sizeof(source));
    XrSourceDiagnosticReportInfo info={0};CHECK(xray_source_diagnostic_report(report,&info)==XR_SOURCE_OK&&info.roots==2&&!info.complete);
    const char *names[]={"alpha","beta"};
    for(uint32_t i=0;i<info.roots;++i) {
        XrSourceDiagnosticRoot root={0};XrSourceDiagnosticLabel label={0};
        CHECK(xray_source_diagnostic_root(report,i,&root)==XR_SOURCE_OK&&root.code==302&&root.severity==1&&root.labels==1);
        CHECK(!strcmp(root.message,"duplicate declaration name"));
        CHECK(root.primary.line==(int32_t)(2+2*i)&&root.primary.column==7);
        CHECK((root.primary.flags&(XR_SOURCE_LOCATION_BYTES|XR_SOURCE_LOCATION_UTF16|XR_SOURCE_LOCATION_REVISION))==
            (XR_SOURCE_LOCATION_BYTES|XR_SOURCE_LOCATION_UTF16|XR_SOURCE_LOCATION_REVISION));
        CHECK(root.primary.end_byte-root.primary.start_byte==strlen(names[i]));
        CHECK(root.primary.utf16_line==1+2*i&&root.primary.utf16_column==6);
        CHECK(xray_source_diagnostic_label(report,i,0,&label)==XR_SOURCE_OK&&label.location.line==(int32_t)(1+2*i));
        CHECK(!strcmp(label.message,"first declaration is here")&&label.location.utf16_column==6);
        CHECK((label.location.flags&XR_SOURCE_LOCATION_BYTES)&&!memcmp(label.location.revision,root.primary.revision,32));
    }
    XrSourceDiagnosticRoot preserved={.code=12345};
    CHECK(xray_source_diagnostic_root(report,2,&preserved)==XR_SOURCE_BAD_ARGUMENT&&preserved.code==12345);
    XrSourceDiagnosticLabel preserved_label={.message="unchanged"};
    CHECK(xray_source_diagnostic_label(report,0,1,&preserved_label)==XR_SOURCE_BAD_ARGUMENT&&!strcmp(preserved_label.message,"unchanged"));
    xray_source_diagnostic_free(report);
    char corrected[]="const alpha:i64=1\nconst beta:i64=3\nprint(9)\n";
    input.value.text=(XrSourceText){NULL,"corrected-module-errors",corrected,sizeof(corrected)-1};
    CHECK(xray_source_build(&input,&options,&artifact,&failure,NULL)==XR_SOURCE_OK&&artifact);
    memset(corrected,'!',sizeof(corrected));
    XrSourceExecution *execution=execution_new();XrSourceCallOptions runtime;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    ArrayOutput output={0};runtime.write=array_write;runtime.context=&output;XrSourceResult *result=NULL;
    CHECK(xray_source_artifact_execute(execution,artifact,&runtime,&result,NULL)==XR_SOURCE_OK&&result);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
    CHECK(output.size==2&&!memcmp(output.bytes,"9\n",2));xray_source_result_free(result);
}
static XrCompileResourceStatus report_build(XrCompileResources *owner,XrFrontendReport **output) {
    XrHirItems *items=NULL;XrCompileResourceStatus status=xr_hir_items_new(owner,&items);
    static const char text[]="const a=1\nconst a=2\nconst b=3\nconst b=4\n";
    if(status==XR_COMPILE_RESOURCE_OK)status=xr_hir_items_source(items,0,"report.xr",text,sizeof(text)-1,NULL);
    for(uint32_t i=0;status==XR_COMPILE_RESOURCE_OK&&i<4;++i) {
        XrHirItemSpec spec={i<2?"a":"b",0,0,i+1,(int)i+1,7,XR_HIR_ITEM_DECLARATION};
        uint32_t id=0;status=xr_hir_items_reserve(items,&spec,&id);
    }
    if(status==XR_COMPILE_RESOURCE_OK){*output=xr_hir_items_take_report(items);xr_frontend_report_seal(*output,false);}
    xr_hir_items_free(items);return status;
}
static void report_resources(void) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;
    CHECK(!runtime_live&&!runtime_bytes);
    XrCompileResources *owner=sdk_ledger(&sdk_unlimited);XrCompileResourceStats baseline=sdk_stats(owner);
    XrFrontendReport *empty=NULL;char *empty_text=NULL;
    CHECK(xr_frontend_report_new(owner,&empty)==XR_COMPILE_RESOURCE_OK);xr_frontend_report_seal(empty,false);
    CHECK(xr_frontend_report_render(empty,&empty_text)==XR_COMPILE_RESOURCE_OK&&empty_text&&empty_text[0]==0);
    xr_frontend_report_free(empty);CHECK(empty_text[0]==0);xr_compile_resources_free(empty_text);
    CHECK(sdk_stats(owner).held_bytes==baseline.held_bytes);
    XrFrontendReport *report=NULL;CHECK(report_build(owner,&report)==XR_COMPILE_RESOURCE_OK&&report);
    uint64_t peak=sdk_stats(owner).peak_bytes;
    xr_frontend_report_free(report);report=NULL;
    CHECK(sdk_stats(owner).held_bytes==baseline.held_bytes);xr_compile_resources_release(owner);
    CHECK(!runtime_live&&!runtime_bytes);
    for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,delta<0?peak-1:peak+(uint64_t)delta,true};
        owner=sdk_ledger(&limits);baseline=sdk_stats(owner);
        XrCompileResourceStatus status=report_build(owner,&report);
        CHECK(status==(delta<0?XR_COMPILE_RESOURCE_MEMORY_LIMIT:XR_COMPILE_RESOURCE_OK));
        CHECK(delta<0?!report:report!=NULL);xr_frontend_report_free(report);report=NULL;
        CHECK(sdk_stats(owner).held_bytes==baseline.held_bytes);xr_compile_resources_release(owner);
        CHECK(!runtime_live&&!runtime_bytes);
    }
    /* Two real report allocation failures remain resource failures, never roots. */
    owner=sdk_ledger(&sdk_unlimited);baseline=sdk_stats(owner);
    runtime_fail_at=runtime_attempts;
    CHECK(xr_frontend_report_new(owner,&report)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY&&!report&&runtime_attempts>runtime_fail_at);
    runtime_fail_at=SIZE_MAX;CHECK(sdk_stats(owner).held_bytes==baseline.held_bytes);
    CHECK(xr_frontend_report_new(owner,&report)==XR_COMPILE_RESOURCE_OK);
    uint32_t source=0;CHECK(xr_frontend_report_source(report,"report.xr","a",1,NULL,&source)==XR_COMPILE_RESOURCE_OK);
    XrFrontendLocation location=xr_frontend_report_location(report,source,1,1,"a");
    CHECK(xr_frontend_report_add(report,302,"first preserved root",location,NULL,0)==XR_COMPILE_RESOURCE_OK);
    XrFrontendLabel related={"original label",location};
    /* Existing root capacity is live; the next root message succeeds, then its
     * label storage physically fails. That incomplete root must not publish. */
    runtime_fail_at=runtime_attempts+1;
    CHECK(xr_frontend_report_add(report,302,"unpublished root",location,&related,1)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY&&runtime_attempts>runtime_fail_at);
    runtime_fail_at=SIZE_MAX;xr_frontend_report_seal(report,false);
    XrFrontendReportInfo info={0};XrFrontendRoot preserved={0};
    CHECK(xr_frontend_report_info(report,&info)&&info.roots==1&&!info.complete);
    CHECK(xr_frontend_report_root(report,0,&preserved)&&!strcmp(preserved.message,"first preserved root"));
    xr_frontend_report_free(report);report=NULL;CHECK(sdk_stats(owner).held_bytes==baseline.held_bytes);
    xr_compile_resources_release(owner);CHECK(!runtime_live&&!runtime_bytes);
    owner=sdk_ledger(&sdk_unlimited);CHECK(report_build(owner,&report)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(owner);XrSourceDiagnosticRoot root={0};
    CHECK(xray_source_diagnostic_root(report,1,&root)==XR_SOURCE_OK&&root.code==302);
    xr_frontend_report_free(report);CHECK(!runtime_live&&!runtime_bytes);
    printf("Report measured held boundary=%llu, two physical OOM controls, physical=0/0\n",(unsigned long long)peak);
}
static void unicode_report(void) {
    char source[]="/*😀*/ const alpha:i64=1\r\n/*😀*/ const alpha:i64=2\r\n";
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"unicode-module-diagnostic",source,sizeof(source)-1}};
    XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *report=NULL;XrSourceFailure failure={0};
    CHECK(xray_source_build(&input,&options,&artifact,&failure,&report)==XR_SOURCE_BAD_ARGUMENT&&!artifact&&report);
    memset(source,'!',sizeof(source));XrSourceDiagnosticRoot root={0};XrSourceDiagnosticLabel label={0};
    CHECK(xray_source_diagnostic_root(report,0,&root)==XR_SOURCE_OK&&root.primary.column==16&&root.primary.utf16_column==13);
    CHECK(root.primary.line==2&&root.primary.utf16_line==1&&root.primary.end_byte-root.primary.start_byte==5);
    CHECK(xray_source_diagnostic_label(report,0,0,&label)==XR_SOURCE_OK&&label.location.column==16&&label.location.utf16_column==13);
    xray_source_diagnostic_free(report);CHECK(!runtime_live&&!runtime_bytes);
}
typedef struct CancelAt {size_t polls,at;} CancelAt;
static int cancel_at(const void *context) {
    CancelAt *control=(CancelAt *)context;++control->polls;
    return control->at&&control->polls>=control->at;
}
static void report_control(void) {
    static const char text[]="const alpha:i64=1\nconst alpha:i64=2\nconst beta:i64=3\nconst beta:i64=4\nprint(9)\n";
    CancelAt control={0};XrSourceCompileOptions options=xray_source_compile_options_default();
    options.stdlib_path=XR_PUBLIC_STDLIB;options.cancel_poll=cancel_at;options.cancel_context=&control;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"partial-report-cancel",text,sizeof(text)-1}};
    XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *report=NULL;XrSourceFailure failure={0};
    CHECK(xray_source_build(&input,&options,&artifact,&failure,&report)==XR_SOURCE_BAD_ARGUMENT&&!artifact&&report);
    size_t actual_polls=control.polls;CHECK(actual_polls);xray_source_diagnostic_free(report);report=NULL;
    CHECK(!runtime_live&&!runtime_bytes);control=(CancelAt){0,actual_polls};
    CHECK(xray_source_build(&input,&options,&artifact,&failure,&report)==XR_SOURCE_CANCELLED&&!artifact&&report);
    XrSourceDiagnosticReportInfo info={0};
    CHECK(failure.status==XR_SOURCE_CANCELLED&&failure.cleanup_status==XR_SOURCE_OK);
    CHECK(xray_source_diagnostic_report(report,&info)==XR_SOURCE_OK&&info.roots==2&&!info.complete);
    xray_source_diagnostic_free(report);report=NULL;CHECK(!runtime_live&&!runtime_bytes);
    options.cancel_poll=NULL;options.cancel_context=NULL;options.deadline_mode=XR_SOURCE_DEADLINE_AFTER;options.timeout_ns=0;
    CHECK(xray_source_build(&input,&options,&artifact,&failure,&report)==XR_SOURCE_DEADLINE&&!artifact);
    CHECK(failure.status==XR_SOURCE_DEADLINE&&failure.cleanup_status==XR_SOURCE_OK);
    xray_source_diagnostic_free(report);CHECK(!runtime_live&&!runtime_bytes);
}
static void partial_report_lifetime(void) {
    char source[]="go fn() {}()\n";
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"unlocated-go-error",source,sizeof(source)-1}};
    XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *report=NULL;XrSourceFailure failure={0};
    CHECK(xray_source_build(&input,&options,&artifact,&failure,&report)==XR_SOURCE_REJECTED&&!artifact&&report);
    CHECK(failure.cleanup_status==XR_SOURCE_OK);
    memset(source,'!',sizeof(source));
    XrSourceDiagnosticReportInfo info={0};XrSourceDiagnosticRoot root={0};
    CHECK(xray_source_diagnostic_report(report,&info)==XR_SOURCE_OK&&info.roots==1);
    CHECK(xray_source_diagnostic_root(report,0,&root)==XR_SOURCE_OK&&!root.primary.path);
    CHECK((root.primary.flags&XR_SOURCE_LOCATION_LINE)&&root.primary.line==1&&root.primary.column==0);
    CHECK(!(root.primary.flags&(XR_SOURCE_LOCATION_BYTES|XR_SOURCE_LOCATION_UTF16)));
    CHECK(!strcmp(root.message,"go requires an accessible ordinary function declaration"));
    XrCompileResources *owner=sdk_ledger(&sdk_unlimited);
    XrSourceCompileDiagnostic display={.status=XR_SOURCE_COMPILE_REJECTED,.stage=XR_SOURCE_COMPILE_PRODUCT};
    display.source.failure.report=report;report=NULL;
    char *text=NULL;
    CHECK(xr_source_compile_diagnostic_render(&display,owner,&text)==XR_SOURCE_COMPILE_OK&&text);
    xr_source_compile_diagnostic_free(&display);
    CHECK(!display.source.failure.report);
    xr_compile_resources_release(owner);owner=NULL;
    CHECK(!strcmp(text,"line 1:0: error: go requires an accessible ordinary function declaration\n"
        "source build failed (stage=4 status=source-rejected)\n"));
    xr_compile_resources_free(text);
    CHECK(!runtime_live&&!runtime_bytes);

    owner=sdk_ledger(&sdk_unlimited);XrFrontendReport *unknown=NULL;uint32_t file=0;
    CHECK(xr_frontend_report_new(owner,&unknown)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_frontend_report_source(unknown,"unknown.xr",NULL,0,NULL,&file)==XR_COMPILE_RESOURCE_OK);
    XrFrontendLocation location=xr_frontend_report_location(unknown,file,0,0,NULL);
    CHECK(!(location.flags&(XR_FRONTEND_LOCATION_LINE|XR_FRONTEND_LOCATION_BYTES|XR_FRONTEND_LOCATION_UTF16)));
    XrFrontendLabel label={"original unlocated label",location};
    CHECK(xr_frontend_report_add(unknown,0,"first unlocated fact",location,&label,1)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_frontend_report_add(unknown,0,"second unlocated fact",location,NULL,0)==XR_COMPILE_RESOURCE_OK);
    xr_frontend_report_seal(unknown,false);
    display=(XrSourceCompileDiagnostic){.status=XR_SOURCE_COMPILE_REJECTED,.stage=XR_SOURCE_COMPILE_PRODUCT};
    display.source.failure.report=unknown;unknown=NULL;text=NULL;
    CHECK(xr_source_compile_diagnostic_render(&display,owner,&text)==XR_SOURCE_COMPILE_OK&&text);
    xr_source_compile_diagnostic_free(&display);xr_compile_resources_release(owner);
    CHECK(!strcmp(text,"error: first unlocated fact\nnote: original unlocated label\n"
        "error: second unlocated fact\nsource build failed (stage=4 status=source-rejected)\n"));
    xr_compile_resources_free(text);
    CHECK(!runtime_live&&!runtime_bytes);
}
static void module_binding_classes(void) {
    static const struct {const char *text;uint32_t roots;int column,label_column;} cases[]={
        {"const (alpha,_)=(1,2)\nconst (alpha,_)=(3,4)\nconst (beta,_)=(5,6)\nconst (beta,_)=(7,8)\n",2,8,8},
        {"const (alpha,_)=(1,2)\nconst alpha:i64=3\n",1,7,8},
        {"import time\nconst time:i64=3\n",1,7,8}
    };
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
    for(size_t c=0;c<sizeof(cases)/sizeof(*cases);++c) {
        XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"module-binding-classes",cases[c].text,strlen(cases[c].text)}};
        XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *report=NULL;XrSourceFailure failure={0};
        CHECK(xray_source_build(&input,&options,&artifact,&failure,&report)==XR_SOURCE_BAD_ARGUMENT&&!artifact&&report);
        XrSourceDiagnosticReportInfo info={0};CHECK(xray_source_diagnostic_report(report,&info)==XR_SOURCE_OK&&info.roots==cases[c].roots);
        for(uint32_t i=0;i<info.roots;++i) {
            XrSourceDiagnosticRoot root={0};XrSourceDiagnosticLabel label={0};
            CHECK(xray_source_diagnostic_root(report,i,&root)==XR_SOURCE_OK&&root.code==302&&root.primary.line==(int32_t)(2+2*i));
            CHECK(root.primary.column==cases[c].column&&(root.primary.flags&XR_SOURCE_LOCATION_BYTES));
            CHECK(xray_source_diagnostic_label(report,i,0,&label)==XR_SOURCE_OK&&label.location.line==(int32_t)(1+2*i));
            CHECK(label.location.column==cases[c].label_column&&(label.location.flags&XR_SOURCE_LOCATION_BYTES));
        }
        xray_source_diagnostic_free(report);CHECK(!runtime_live&&!runtime_bytes);
    }
    char source[]="type T=i64\nconst T:i64=9\nprint(T)\n";
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"separate-alias-scope",source,sizeof(source)-1}};
    XrSourceArtifact *artifact=NULL;CHECK(xray_source_build(&input,&options,&artifact,NULL,NULL)==XR_SOURCE_OK&&artifact);
    memset(source,'!',sizeof(source));XrSourceExecution *execution=execution_new();XrSourceCallOptions runtime;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);ArrayOutput output={0};
    runtime.write=array_write;runtime.context=&output;XrSourceResult *result=NULL;
    CHECK(xray_source_artifact_execute(execution,artifact,&runtime,&result,NULL)==XR_SOURCE_OK&&result);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
    CHECK(output.size==2&&!memcmp(output.bytes,"9\n",2));xray_source_result_free(result);CHECK(!runtime_live&&!runtime_bytes);
}

typedef struct Calibration {
    size_t polls,at,hit,physical_live,physical_bytes;
    uint64_t started,previous,max_gap_ns,held,peak,timeout_ns;
    bool wait_deadline,cancel;
} Calibration;
static int calibration_poll(const void *context) {
    Calibration *control=(Calibration *)context;
    uint64_t now=xr_time_monotonic_ns();CHECK(now);
    if(control->previous&&now-control->previous>control->max_gap_ns)
        control->max_gap_ns=now-control->previous;
    control->previous=now;++control->polls;
    if(calibration_owner) {
        uint64_t peak=sdk_stats(calibration_owner).peak_bytes;
        if(peak>control->peak)control->peak=peak;
    }
    if(!control->at||control->polls!=control->at)return 0;
    CHECK(calibration_owner);
    control->held=sdk_stats(calibration_owner).held_bytes;
    control->physical_live=runtime_live;control->physical_bytes=runtime_bytes;
    CHECK(control->held>calibration_initial_held&&control->physical_live>1);
    ++control->hit;
    /* The deadline remains the real request's monotonic deadline. The callback
     * never substitutes a clock or returns cancellation in the deadline case. */
    if(control->wait_deadline) {
        uint64_t wait_started=now;
        do {xr_time_sleep_ns(1000000);now=xr_time_monotonic_ns();CHECK(now);}
        while(now-wait_started<=control->timeout_ns);
    }
    return control->cancel;
}
static XrSourceStatus calibration_build(const XrSourceInput *input,
    const XrSourceCompileOptions *options,Calibration *control,
    XrSourceArtifact **artifact,XrSourceFailure *failure,XrSourceDiagnostic **report) {
    CHECK(!calibration_capture&&!calibration_owner);
    calibration_capture=true;control->started=xr_time_monotonic_ns();CHECK(control->started);
    XrSourceStatus status=xray_source_build(input,options,artifact,failure,report);
    calibration_capture=false;calibration_owner=NULL;
    return status;
}
static void calibration_execute(XrSourceArtifact *artifact,char *source,size_t length,Calibration *control) {
    size_t polls=control->polls;
    memset(source,'!',length);control->at=1;control->cancel=true;
    XrSourceExecution *execution=execution_new();XrSourceCallOptions runtime;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.name="answer";invocation.name_length=6;XrSourceCall *call=NULL;
    CHECK(xray_source_invoke_begin(execution,artifact,&invocation,&runtime,&call)==XR_SOURCE_OK&&call);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
    CHECK(finish(call).state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK&&result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK&&!call);
    int64_t value=0;CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK&&value==42);
    CHECK(control->polls==polls);xray_source_result_free(result);
}
static void calibration_failure(const XrSourceInput *input,XrSourceCompileOptions *options,
    Calibration *control,XrSourceStatus expected,XrSourceStage stage) {
    XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *report=NULL;XrSourceFailure failure={0};
    XrSourceStatus status=calibration_build(input,options,control,&artifact,&failure,&report);
    printf("Control bytes=%zu at=%zu polls=%zu hit=%zu status=%u stage=%u held=%llu physical=%zu/%zu\n",
        input->value.text.length,control->at,control->polls,control->hit,(unsigned)status,(unsigned)failure.stage,
        (unsigned long long)control->held,control->physical_live,control->physical_bytes);
    CHECK(status==expected&&!artifact&&control->hit==1);
    CHECK(failure.status==expected&&failure.cleanup_status==XR_SOURCE_OK&&failure.stage==stage);
    CHECK(report);XrSourceDiagnosticReportInfo info={0};XrSourceDiagnosticRoot root={0};
    CHECK(xray_source_diagnostic_report(report,&info)==XR_SOURCE_OK&&info.roots&&!info.complete);
    CHECK(xray_source_diagnostic_root(report,0,&root)==XR_SOURCE_OK&&root.message&&root.message[0]);
    xray_source_diagnostic_free(report);CHECK(!runtime_live&&!runtime_bytes);
}
static void compilation_controls(void) {
    static const size_t padding[]={4096,16384,65536};
    static const char suffix[]="*/\nexport fn answer()->i64 { return 42 }\n";
    for(size_t i=0;i<sizeof(padding)/sizeof(*padding);++i) {
        size_t length=padding[i]+sizeof(suffix)-1;
        char *source=sdk_fixture_malloc(length+1);CHECK(source);
        memset(source,' ',padding[i]);source[0]='/';source[1]='*';
        memcpy(source+padding[i],suffix,sizeof(suffix));
        XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,
            .value.text={NULL,"mid-request-calibration",source,length}};
        Calibration control={0};XrSourceCompileOptions options=xray_source_compile_options_default();
        options.stdlib_path=XR_PUBLIC_STDLIB;options.profile_allocations=1;
        options.cancel_poll=calibration_poll;options.cancel_context=&control;
        XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};
        CHECK(calibration_build(&input,&options,&control,&artifact,&failure,NULL)==XR_SOURCE_OK&&artifact);
        uint64_t elapsed=xr_time_monotonic_ns()-control.started;size_t total=control.polls;
        CHECK(total>4&&control.peak);uint64_t peak=control.peak;
        xray_source_artifact_free(artifact);artifact=NULL;
        CHECK(!runtime_live&&!runtime_bytes);
        printf("Calibration bytes=%zu polls=%zu elapsed_ns=%llu max_callback_gap_ns=%llu\n",
            length,total,(unsigned long long)elapsed,(unsigned long long)control.max_gap_ns);
        /* The finite owner uses this run's observed held peak, not a public
         * quota or a frozen allocator-size expectation. */
        options.memory_mode=XR_SOURCE_MEMORY_FINITE;options.memory_limit=peak;
        /* The public COMPILE stage is intentionally not a claim about an
         * internal parser/copy/lower phase. No poll ordinal is a phase identity. */
        control=(Calibration){.at=total/2,.cancel=true};
        calibration_failure(&input,&options,&control,XR_SOURCE_CANCELLED,XR_SOURCE_STAGE_COMPILE);
        control=(Calibration){.at=total,.cancel=true};
        calibration_failure(&input,&options,&control,XR_SOURCE_CANCELLED,XR_SOURCE_STAGE_PUBLISH);
        if(i==1) {
            control=(Calibration){.at=total/2,.wait_deadline=true,.timeout_ns=5000000000ULL};
            options.deadline_mode=XR_SOURCE_DEADLINE_AFTER;options.timeout_ns=control.timeout_ns;
            calibration_failure(&input,&options,&control,XR_SOURCE_DEADLINE,XR_SOURCE_STAGE_COMPILE);
            /* This poll checked a live deadline before entering the callback.
             * Returning cancellation wins even though the deadline then elapsed;
             * this demonstrates poll precedence, not chronological ordering. */
            control=(Calibration){.at=total/2,.wait_deadline=true,.cancel=true,.timeout_ns=5000000000ULL};
            calibration_failure(&input,&options,&control,XR_SOURCE_CANCELLED,XR_SOURCE_STAGE_COMPILE);
        }
        control=(Calibration){0};
        CHECK(calibration_build(&input,&options,&control,&artifact,&failure,NULL)==XR_SOURCE_OK&&artifact);
        CHECK(failure.status==XR_SOURCE_OK&&failure.cleanup_status==XR_SOURCE_OK);
        calibration_execute(artifact,source,length,&control);
        sdk_fixture_free(source);CHECK(!runtime_live&&!runtime_bytes);
    }
}

int main(void) {
    compilation_controls();
    report_resources();report_control();unicode_report();module_binding_classes();partial_report_lifetime();
    module_report_lifetime(false);module_report_lifetime(true);
    independent_requests();detached_ast_and_result();ready_c_artifact();execution_policy();
    global_array_lifetime();global_string_lifetime();
    CHECK(!runtime_live&&!runtime_bytes);
    puts("Independent Source requests, ready artifacts, detached AST and result ownership passed");return 0;
}
