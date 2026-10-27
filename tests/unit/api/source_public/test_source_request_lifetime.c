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
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while(0)

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
        XrSourceDiagnosticInfo info={0};CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);
        fprintf(stderr,"Global Array Source %u %d:%d %s\n",(unsigned)status,info.line,info.column,info.message);
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
int main(void) {
    independent_requests();detached_ast_and_result();ready_c_artifact();execution_policy();
    global_array_lifetime();global_string_lifetime();
    puts("Independent Source requests, ready artifacts, detached AST and result ownership passed");return 0;
}
