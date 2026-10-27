/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_request_empty_enum.c - Arena-owned CTFE enum through a ready artifact
 */
#include "xray_source_entry.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
int main(void) {
    static const char source[]=
        "enum Flag { Off, On }\n"
        "fn choose()->Flag { return Flag.On }\n"
        "export fn answer()->i64 { const flag=comptime choose();"
        " return match(flag){Flag.Off -> 0, Flag.On -> 42} }\n";
    XrSourceCompileOptions options=xray_source_compile_options_default();
    options.stdlib_path=XR_PUBLIC_STDLIB;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,
        .value.text={NULL,"arena-only-enum",source,sizeof(source)-1}};
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_build(&input,&options,&artifact,&failure,&diagnostic);
    if (diagnostic) {
        XrSourceDiagnosticInfo info={0};
        CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);
        fprintf(stderr,"compile status=%u: %s\n",(unsigned)status,info.message);
    }
    CHECK(status==XR_SOURCE_OK && artifact && !diagnostic);
    XrSourceExecutionOptions policy=xray_source_execution_options_default();
    XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK && execution);
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.name="answer";invocation.name_length=6;
    XrSourceCall *call=NULL;
    CHECK(xray_source_invoke_begin(execution,artifact,&invocation,&runtime,&call)==XR_SOURCE_OK && call);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);
    XrSourceEvent event={0};
    for (unsigned polls=0;polls<4096;++polls) {
        CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);
        if (event.state!=XR_SOURCE_RUN_READY) break;
    }
    CHECK(event.state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *result=NULL;
    CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
    int64_t answer=0;CHECK(xray_source_result_i64(result,&answer)==XR_SOURCE_OK && answer==42);
    XrSourceResult *copy=NULL;CHECK(xray_source_result_copy(result,&copy)==XR_SOURCE_OK && copy);
    xray_source_result_free(result);
    CHECK(xray_source_result_i64(copy,&answer)==XR_SOURCE_OK && answer==42);
    xray_source_result_free(copy);
    puts("Arena-only empty enum CTFE, ready artifact and producer-independent result PASS");
    return 0;
}
