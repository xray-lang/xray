/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_typed_await.c - Typed child cancellation and uncapturable own cancellation
 */
#include "xray_source_instance.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"typed await %d: %s\n",__LINE__,#c);exit(1); } } while (0)
typedef struct Output { char bytes[4096];size_t length; } Output;
static XrSourceWriteStatus collect(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    Output *output=(Output *)context;
    CHECK(stream==XR_SOURCE_STDOUT || stream==XR_SOURCE_STDERR);
    CHECK(length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;
    return XR_SOURCE_WRITE_OK;
}
static int contains(const Output *output,const char *text) {
    size_t length=strlen(text);
    for (size_t i=0;i<=output->length && length<=output->length-i;++i)
        if (!memcmp(output->bytes+i,text,length)) return 1;
    return 0;
}
static char *read_input(const char *path,size_t *length) {
    FILE *file=NULL;
#if defined(_MSC_VER)
    CHECK(!fopen_s(&file,path,"rb"));
#else
    file=fopen(path,"rb");
#endif
    CHECK(file && !fseek(file,0,SEEK_END));long size=ftell(file);
    CHECK(size>0 && size<65536 && !fseek(file,0,SEEK_SET));
    char *text=(char *)malloc((size_t)size+1);CHECK(text);
    CHECK(fread(text,1,(size_t)size,file)==(size_t)size && !fclose(file));
    *length=(size_t)size;text[*length]=0;return text;
}
static XrSourceInstance *prepare(const char *stdlib,const char *path,Output *output) {
    size_t length=0;char *text=read_input(path,&length);
    XrSourceInput input={0};input.kind=XR_SOURCE_INPUT_TEXT;
    input.value.text.namespace_id="typed-await";input.value.text.bytes=text;input.value.text.length=length;
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=stdlib;
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_build(&input,&options,&artifact,&failure,&diagnostic);
    memset(text,0xa5,length);free(text);
    if (status!=XR_SOURCE_OK && diagnostic) {
        XrSourceDiagnosticReportInfo info={0};CHECK(xray_source_diagnostic_report(diagnostic,&info)==XR_SOURCE_OK);
        for (uint32_t i=0;i<info.roots;++i) {
            XrSourceDiagnosticRoot root={0};CHECK(xray_source_diagnostic_root(diagnostic,i,&root)==XR_SOURCE_OK);
            fprintf(stderr,"Source%u stage%u %s\n",status,failure.stage,root.message);
        }
    }
    CHECK(status==XR_SOURCE_OK && artifact && !diagnostic);
    XrSourceExecutionOptions policy=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK);
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    runtime.write=collect;runtime.context=output;XrSourceInstance *instance=NULL;
    CHECK(xray_source_instance_new(execution,artifact,&runtime,&instance)==XR_SOURCE_OK && instance);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);return instance;
}
static XrSourceInvocation invocation(const char *name) {
    XrSourceInvocation request;CHECK(xray_source_invocation_init(&request,sizeof(request))==XR_SOURCE_OK);
    request.name=name;request.name_length=strlen(name);return request;
}
static XrSourceEvent terminal(XrSourceCall *call) {
    XrSourceEvent event={0};unsigned steps=0;
    do {
        CHECK(++steps<10000 && xray_source_call_poll(call,64,&event)==XR_SOURCE_OK);
        if (event.state==XR_SOURCE_RUN_SUSPENDED) {
            XrSourceWait wait={0};CHECK(xray_source_call_wait_request(call,event.epoch,event.wake,&wait,sizeof(wait))==XR_SOURCE_OK);
            CHECK(wait.kind==XR_SOURCE_WAIT_YIELD);
            CHECK(xray_source_call_resume(call,event.epoch,event.wake)==XR_SOURCE_OK);
        }
    } while (event.state==XR_SOURCE_RUN_READY || event.state==XR_SOURCE_RUN_SUSPENDED);
    return event;
}
static XrSourceResult *execute(XrSourceInstance *instance,const char *name) {
    XrSourceInvocation request=invocation(name);XrSourceCall *call=NULL;
    CHECK(xray_source_instance_begin(instance,&request,&call)==XR_SOURCE_OK && call);
    (void)terminal(call);XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);return result;
}
static void ordinary(const char *stdlib,const char *path) {
    Output output={0};XrSourceInstance *instance=prepare(stdlib,path,&output);
    XrSourceResult *first=execute(instance,"awaitNever");XrSourceEvent event={0};
    CHECK(xray_source_result_event(first,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *owned=NULL;CHECK(xray_source_result_copy(first,&owned)==XR_SOURCE_OK && owned);
    xray_source_result_free(first);output.length=0;
    XrSourceResult *cancelled=execute(instance,"cancelledError");
    CHECK(xray_source_result_event(cancelled,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_THROWN);
    CHECK(output.length==14 && !memcmp(output.bytes,"child-cleanup\n",14));output.length=0;
    XrSourceResult *error=execute(instance,"awaitOne");
    CHECK(xray_source_result_event(error,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_THROWN);
    CHECK(xray_source_instance_drop(&instance,NULL)==XR_SOURCE_OK && !instance);
    const char expected[]="owned \303\251\0\347\225\214";const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_result_bytes(owned,&bytes,&length)==XR_SOURCE_OK && length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    CHECK(xray_source_result_format(cancelled,collect,&output)==XR_SOURCE_OK && contains(&output,"Cancelled.Cancelled"));
    output.length=0;
    CHECK(xray_source_result_format(error,collect,&output)==XR_SOURCE_OK && contains(&output,"A.Bad") && contains(&output,"owned"));
    xray_source_result_free(owned);xray_source_result_free(cancelled);xray_source_result_free(error);
    puts("typed await: Never/A/union, repeated/late cancellation, cleanup before child publication and retired owners PASS");
}
static void own_cancel(const char *stdlib,const char *path,bool independent) {
    Output output={0};XrSourceInstance *instance=prepare(stdlib,path,&output);
    XrSourceResult *initialized=execute(instance,"awaitNever");xray_source_result_free(initialized);output.length=0;
    XrSourceInvocation request=invocation(independent?"independentParent":"ownCancellation");XrSourceCall *call=NULL;
    CHECK(xray_source_instance_begin(instance,&request,&call)==XR_SOURCE_OK && call);
    XrSourceEvent event={0};
    for (unsigned i=0;i<100;++i) CHECK(xray_source_call_poll(call,1,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_READY);
    CHECK(xray_source_call_cancel(call)==XR_SOURCE_OK && xray_source_call_cancel(call)==XR_SOURCE_OK);
    if (independent) {
        for (unsigned i=0;i<100;++i) CHECK(xray_source_call_poll(call,1,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_READY);
        CHECK(contains(&output,"parent-cleanup\n") && !contains(&output,"caught-own"));
        CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
        CHECK(contains(&output,"child-cleanup\n"));
        CHECK(xray_source_instance_drop(&instance,NULL)==XR_SOURCE_OK && !instance);
        puts("root cancellation preserves independent ready child; actual host drop cancels and drains it PASS");return;
    }
    event=terminal(call);CHECK(event.state==XR_SOURCE_RUN_CANCELLED);
    CHECK(contains(&output,"parent-cleanup\n") && !contains(&output,"caught-own"));
    XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
    CHECK(contains(&output,"child-cleanup\n"));
    CHECK(xray_source_instance_drop(&instance,NULL)==XR_SOURCE_OK && !instance);
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_CANCELLED);
    xray_source_result_free(result);puts("own cancellation bypasses typed catches and runs cleanup PASS");
}
int main(int argc,char **argv) {
    CHECK(argc==4);
    if (!strcmp(argv[1],"ordinary")) ordinary(argv[2],argv[3]);
    else {bool independent=!strcmp(argv[1],"host-close");CHECK(independent || !strcmp(argv[1],"own-cancel"));own_cancel(argv[2],argv[3],independent);}
    return 0;
}
