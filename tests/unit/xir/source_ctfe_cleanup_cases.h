/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_ctfe_cleanup_cases.h - Public requests with actual comptime cleanup
 */
#ifndef SOURCE_CTFE_CLEANUP_CASES_H
#define SOURCE_CTFE_CLEANUP_CASES_H
#include "xray_source_entry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct CtfeCleanupOutput {char bytes[256];size_t length;} CtfeCleanupOutput;
static XrSourceWriteStatus ctfe_cleanup_capture(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    CtfeCleanupOutput *output=context;CHECK(stream==XR_SOURCE_STDOUT && length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;return XR_SOURCE_WRITE_OK;
}
static void source_ctfe_cleanup_cases(const char *stdlib,const char *path) {
    FILE *file=fopen(path,"rb");CHECK(file);char source[16384];
    size_t length=fread(source,1,sizeof(source)-256,file);
    CHECK(length && feof(file) && !ferror(file) && fclose(file)==0);
    char original[sizeof(source)];memcpy(original,source,length);
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=stdlib;
    const char *failed[]={"\nconst denied = comptime ctfeDouble()\n","\nconst denied = comptime ctfeDouble446()\n"};
    for(size_t i=0;i<sizeof(failed)/sizeof(failed[0]);++i) {
        size_t tail=strlen(failed[i]);memcpy(source+length,failed[i],tail);
        XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"ctfe-double-failure",source,length+tail}};
        XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
        CHECK(xray_source_build(&input,&options,&artifact,&failure,&diagnostic)==XR_SOURCE_REJECTED && !artifact && diagnostic);
        memset(source,'!',sizeof(source));
        CHECK(failure.status==XR_SOURCE_REJECTED && failure.stage==XR_SOURCE_STAGE_COMPILE && failure.cleanup_status==XR_SOURCE_OK);
        XrSourceDiagnosticReportInfo report={0};XrSourceDiagnosticRoot root={0};
        CHECK(xray_source_diagnostic_report(diagnostic,&report)==XR_SOURCE_OK && report.roots==1);
        CHECK(xray_source_diagnostic_root(diagnostic,0,&root)==XR_SOURCE_OK);
        CHECK(root.primary.line>0 && strstr(root.message,"panic escaped defer cleanup"));
        xray_source_diagnostic_free(diagnostic);
        memcpy(source,original,length);
        input.value.text.length=length;input.value.text.namespace_id="ctfe-cleanup-retry";failure=(XrSourceFailure){0};
        CHECK(xray_source_build(&input,&options,&artifact,&failure,NULL)==XR_SOURCE_OK && artifact);
        memset(source,'!',sizeof(source));
        XrSourceExecutionOptions policy=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
        CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK);
        XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
        invocation.name="foldedBoth";invocation.name_length=10;
        CtfeCleanupOutput output={0};XrSourceCallOptions runtime;
        CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
        runtime.write=ctfe_cleanup_capture;runtime.context=&output;
        XrSourceResult *result=NULL;XrSourceEvent event={0};
        CHECK(xray_source_invoke(execution,artifact,&invocation,&runtime,&result,&event)==XR_SOURCE_OK && result);
        xray_source_execution_free(execution);xray_source_artifact_free(artifact);
        const char expected_output[]="true\ntrue\n420\ntrue\n420\n42\ntrue\n446\n42\ntry! failed: Failure.Bad(\"new \303\251\000\347\225\214\", 31)\n42\n42\n42\n42\n42\n";
        CHECK(output.length==sizeof(expected_output)-1 && !memcmp(output.bytes,expected_output,output.length));
        CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
        XrSourceValue *value=NULL;CHECK(xray_source_result_value(result,&value)==XR_SOURCE_OK && value);
        xray_source_result_free(result);
        const char expected[]="try! failed: Failure.Bad(\"new \303\251\000\347\225\214\", 31)";
        const char *bytes=NULL;size_t count=0;CHECK(xray_source_value_bytes(value,&bytes,&count)==XR_SOURCE_OK);
        CHECK(count==sizeof(expected)-1 && !memcmp(bytes,expected,count));xray_source_value_free(value);
        memcpy(source,original,length);
    }
    memset(source,'!',sizeof(source));
}
#endif // SOURCE_CTFE_CLEANUP_CASES_H
