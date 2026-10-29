/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cast_source.c - Public scalar cast consumers after producer retirement
 */
#include "xray_source_ast.h"
#include "xray_source_value.h"
#include "cast_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)

typedef struct CompileControl {size_t polls;int retired;} CompileControl;
static int cancel(const void *opaque) {
    CompileControl *control=(CompileControl *)opaque;CHECK(!control->retired);++control->polls;return 0;
}
typedef struct Output {char bytes[sizeof(cast_expected)];size_t length;} Output;
static XrSourceWriteStatus capture(void *opaque,XrSourceOutputStream stream,const char *bytes,size_t count) {
    Output *output=(Output *)opaque;CHECK(stream==XR_SOURCE_STDOUT && count<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,count);output->length+=count;return XR_SOURCE_WRITE_OK;
}
static void report_failure(const XrSourceDiagnostic *diagnostic) {
    if(!diagnostic)return;XrSourceDiagnosticReportInfo info={0};
    CHECK(xray_source_diagnostic_report(diagnostic,&info)==XR_SOURCE_OK);
    for(uint32_t i=0;i<info.roots;++i) {
        XrSourceDiagnosticRoot root={0};CHECK(xray_source_diagnostic_root(diagnostic,i,&root)==XR_SOURCE_OK);
        fprintf(stderr,"%d:%d: %s\n",root.primary.line,root.primary.column,root.message);
    }
}
static void retired_source(const char *stdlib) {
    char *source=(char *)malloc(sizeof(cast_source));CHECK(source);memcpy(source,cast_source,sizeof(cast_source));
    CompileControl control={0};XrSourceCompileOptions options=xray_source_compile_options_default();
    options.stdlib_path=stdlib;options.cancel_poll=cancel;options.cancel_context=&control;
    XrSourceText text={NULL,"hir-cast",source,sizeof(cast_source)-1};
    XrSourceAst *ast=NULL;XrSourceDiagnostic *diagnostic=NULL;XrSourceFailure failure={0};
    CHECK(xray_source_parse(&text,&options,&ast,&failure,&diagnostic)==XR_SOURCE_OK && ast && !diagnostic);
    memset(source,'!',sizeof(cast_source));free(source);source=NULL;memset(&text,0xa5,sizeof(text));
    XrSourceArtifact *artifact=NULL;
    XrSourceStatus status=xray_source_build_ast(ast,&options,&artifact,&failure,&diagnostic);
    if(status!=XR_SOURCE_OK)report_failure(diagnostic);
    CHECK(status==XR_SOURCE_OK && artifact && !diagnostic && control.polls);
    xray_source_ast_free(ast);ast=NULL;control.retired=1;size_t polls=control.polls;memset(&options,0xa5,sizeof(options));
    XrSourceExecutionOptions policy=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK && execution);
    Output output={0};XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    runtime.write=capture;runtime.context=&output;XrSourceCall *call=NULL;
    CHECK(xray_source_artifact_begin(execution,artifact,&runtime,&call)==XR_SOURCE_OK && call);
    xray_source_artifact_free(artifact);artifact=NULL;xray_source_execution_free(execution);execution=NULL;
    XrSourceEvent event={0};
    for(size_t i=0;;++i) {
        CHECK(i<10000 && xray_source_call_poll(call,1024,&event)==XR_SOURCE_OK);
        if(event.state!=XR_SOURCE_RUN_READY)break;
    }
    CHECK(event.state==XR_SOURCE_RUN_RETURNED && control.polls==polls);
    CHECK(output.length==sizeof(cast_expected)-1 && !memcmp(output.bytes,cast_expected,output.length));
    XrSourceResult *result=NULL;XrSourceValue *value=NULL,*copy=NULL;
    CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
    CHECK(xray_source_result_value(result,&value)==XR_SOURCE_OK && value);xray_source_result_free(result);
    CHECK(xray_source_value_copy(value,&copy)==XR_SOURCE_OK && copy);xray_source_value_free(value);
    XrSourceScalar scalar={0};CHECK(xray_source_value_scalar(copy,&scalar)==XR_SOURCE_OK &&
        scalar.kind==XR_SOURCE_VALUE_I64 && scalar.value.signed_value==0);
    xray_source_value_free(copy);CHECK(control.polls==polls);
}
static void refusals(const char *stdlib) {
    for(size_t i=0;i<sizeof(cast_refusals)/sizeof(cast_refusals[0]);++i) {
        XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=stdlib;
        XrSourceText text={NULL,"hir-cast-refusal",cast_refusals[i].source,strlen(cast_refusals[i].source)};
        XrSourceInput input={0};input.kind=XR_SOURCE_INPUT_TEXT;input.value.text=text;XrSourceArtifact *artifact=NULL;
        XrSourceDiagnostic *diagnostic=NULL;XrSourceFailure failure={0};
        CHECK(xray_source_build(&input,&options,&artifact,&failure,&diagnostic)==cast_refusals[i].status && !artifact && diagnostic);
        memset(&options,0xa5,sizeof(options));XrSourceDiagnosticReportInfo report={0};XrSourceDiagnosticRoot root={0};
        CHECK(xray_source_diagnostic_report(diagnostic,&report)==XR_SOURCE_OK && report.roots==1);
        CHECK(xray_source_diagnostic_root(diagnostic,0,&root)==XR_SOURCE_OK && root.primary.line==cast_refusals[i].line);
        CHECK(root.message && !strcmp(root.message,cast_refusals[i].message));
        xray_source_diagnostic_free(diagnostic);
    }
}
static XrSourceWriteStatus capture_format(void *opaque,XrSourceOutputStream stream,const char *bytes,size_t count) {
    Output *output=(Output *)opaque;CHECK((stream==XR_SOURCE_STDOUT || stream==XR_SOURCE_STDERR) && count<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,count);output->length+=count;return XR_SOURCE_WRITE_OK;
}
static void panics(const char *stdlib) {
    for(size_t i=0;i<sizeof(cast_panics)/sizeof(cast_panics[0]);++i) {
        XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=stdlib;
        XrSourceText text={NULL,"hir-cast-panic",cast_panics[i].source,strlen(cast_panics[i].source)};
        XrSourceInput input={0};input.kind=XR_SOURCE_INPUT_TEXT;input.value.text=text;
        XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *diagnostic=NULL;XrSourceFailure failure={0};
        CHECK(xray_source_build(&input,&options,&artifact,&failure,&diagnostic)==XR_SOURCE_OK && artifact && !diagnostic);
        memset(&options,0xa5,sizeof(options));memset(&input,0xa5,sizeof(input));
        XrSourceExecutionOptions policy=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
        CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK && execution);
        Output output={0};XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
        runtime.write=capture;runtime.context=&output;XrSourceCall *call=NULL;
        CHECK(xray_source_artifact_begin(execution,artifact,&runtime,&call)==XR_SOURCE_OK && call);
        xray_source_artifact_free(artifact);xray_source_execution_free(execution);XrSourceEvent event={0};
        for(size_t step=0;;++step) {
            CHECK(step<10000 && xray_source_call_poll(call,1024,&event)==XR_SOURCE_OK);
            if(event.state!=XR_SOURCE_RUN_READY)break;
        }
        CHECK(event.state==XR_SOURCE_RUN_PANICKED && event.engine_status==14 && !output.length);
        XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
        CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
        CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_PANICKED);
        XrSourceValue *value=NULL;CHECK(xray_source_result_value(result,&value)==XR_SOURCE_BAD_STATE && !value);
        CHECK(xray_source_result_format(result,capture_format,&output)==XR_SOURCE_OK && output.length);
        const char expected_panic[]="[Uncaught Panic] E0422: numeric conversion is out of range\n";
        CHECK(output.length==sizeof(expected_panic)-1 && !memcmp(output.bytes,expected_panic,output.length));
        printf("%s state%u engine%u: ",cast_panics[i].name,(unsigned)event.state,event.engine_status);
        CHECK(fwrite(output.bytes,1,output.length,stdout)==output.length);putchar('\n');xray_source_result_free(result);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==2);retired_source(argv[1]);refusals(argv[1]);panics(argv[1]);
    puts("Public cast Source SDK: all numeric pairs and scalar text, original refusals/panics and producer retirement PASS");return 0;
}
