/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_nullable_print.c - Source-created nullable strings reach ordinary output
 */
#include "xray_source_entry.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "nullable_string_print_expected.h"
typedef struct Output { char bytes[4096]; size_t size, calls; XrSourceWriteStatus refuse; } Output;
typedef struct Cancel { int cancelled; size_t polls; } Cancel;
static int cancelled(const void *context) { Cancel *c=(Cancel *)context; ++c->polls; return c->cancelled; }
static XrSourceWriteStatus write_bytes(void *context,XrSourceOutputStream stream,const char *text,size_t size) {
    Output *out=context; CHECK(stream==XR_SOURCE_STDOUT && size<=sizeof(out->bytes)-out->size); ++out->calls;
    if (out->refuse!=XR_SOURCE_WRITE_OK) return out->refuse;
    memcpy(out->bytes+out->size,text,size);out->size+=size;return XR_SOURCE_WRITE_OK;
}
static XrSourceCompileOptions options(void) {
    XrSourceCompileOptions o=xray_source_compile_options_default();o.stdlib_path=XR_PUBLIC_STDLIB;return o;
}
static char *read_source(size_t *length) {
    FILE *file=fopen(XR_NULLABLE_PRINT_SOURCE,"rb");CHECK(file);
    CHECK(!fseek(file,0,SEEK_END));long size=ftell(file);CHECK(size>0 && !fseek(file,0,SEEK_SET));
    char *text=xr_malloc((size_t)size+1);CHECK(text);
    CHECK(fread(text,1,(size_t)size,file)==(size_t)size && !fclose(file));text[size]=0;*length=(size_t)size;return text;
}
static XrSourceStatus build(const char *text,size_t length,const XrSourceCompileOptions *o,XrSourceArtifact **artifact) {
    XrSourceInput in={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"nullable-string-print",text,length}};
    XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_build(&in,o,artifact,&failure,&diagnostic);
    if(status!=XR_SOURCE_OK && diagnostic) {
        XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);
        fprintf(stderr,"Source %u %d:%d %s\n",(unsigned)status,info.primary.line,info.primary.column,info.message);
    }
    CHECK(failure.status==status && failure.cleanup_status==XR_SOURCE_OK);
    xray_source_diagnostic_free(diagnostic);return status;
}
static XrSourceEvent finish(XrSourceCall *call) {
    XrSourceEvent event={0};
    for(size_t i=0;i<4096;++i) {
        CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);
        if(event.state!=XR_SOURCE_RUN_READY)return event;
    }
    CHECK(0);return event;
}
static void run_ready(XrSourceArtifact *artifact,Output *out,const char *name,XrSourceRunState expected) {
    XrSourceExecutionOptions o=xray_source_execution_options_default();XrSourceExecution *owner=NULL;
    CHECK(xray_source_execution_new(&o,&owner)==XR_SOURCE_OK);
    XrSourceCallOptions call_options;CHECK(xray_source_call_options_init(&call_options,sizeof(call_options))==XR_SOURCE_OK);
    call_options.write=write_bytes;call_options.context=out;
    XrSourceCall *call=NULL;
    if(name) {
        XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
        invocation.name=name;invocation.name_length=strlen(name);
        CHECK(xray_source_invoke_begin(owner,artifact,&invocation,&call_options,&call)==XR_SOURCE_OK);
    } else CHECK(xray_source_artifact_begin(owner,artifact,&call_options,&call)==XR_SOURCE_OK);
    xray_source_artifact_free(artifact);xray_source_execution_free(owner);
    XrSourceEvent event=finish(call);CHECK(event.state==expected);
    XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==expected);
    if(name) {
        const char *bytes=NULL;size_t length=0;
        CHECK(xray_source_result_bytes(result,&bytes,&length)==XR_SOURCE_OK && length==12 && !memcmp(bytes,"owned-result",12));
    }
    xray_source_result_free(result);
}
static void vm_and_lifetime(void) {
    size_t size=0;char *text=read_source(&size);XrSourceArtifact *artifact=NULL;Cancel cancel={0};
    XrSourceCompileOptions o=options();o.cancel_poll=cancelled;o.cancel_context=&cancel;
    CHECK(build(text,size,&o,&artifact)==XR_SOURCE_OK && artifact);
    size_t polls=cancel.polls;cancel.cancelled=1;memset(text,'!',size);xr_free(text);
    Output out={0};run_ready(artifact,&out,NULL,XR_SOURCE_RUN_RETURNED);
    CHECK(cancel.polls==polls && out.size==sizeof(nullable_print_expected)-1 &&
        !memcmp(out.bytes,nullable_print_expected,out.size));
    const char named[]="export fn owned()->string { const text:string?=\"owned\"+\"-result\"; print(text); return text! }\n";
    o=options();artifact=NULL;CHECK(build(named,sizeof(named)-1,&o,&artifact)==XR_SOURCE_OK);
    out=(Output){0};run_ready(artifact,&out,"owned",XR_SOURCE_RUN_RETURNED);
    CHECK(out.size==13 && !memcmp(out.bytes,"owned-result\n",13));
}
static void exact_rejections(void) {
    const char *inputs[]={"const text:i64?=null\nprint(text)\n", "const text:Array<string>?=null\nprint(text)\n"};
    XrSourceCompileOptions o=options();
    for(size_t i=0;i<sizeof(inputs)/sizeof(*inputs);++i) {
        XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *diagnostic=NULL;XrSourceFailure failure={0};
        XrSourceInput in={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"nullable-print-reject",inputs[i],strlen(inputs[i])}};
        XrSourceStatus status=xray_source_build(&in,&o,&artifact,&failure,&diagnostic);
        CHECK(status==XR_SOURCE_REJECTED && !artifact && diagnostic && failure.status==status);
        XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);
        CHECK(!strcmp(info.message,"print requires an admitted display type"));xray_source_diagnostic_free(diagnostic);
    }
}
static void refusal_without_publication(void) {
    size_t length=0;char *source=read_source(&length);XrSourceArtifact *artifact=NULL;
    XrSourceCompileOptions o=options();o.memory_mode=XR_SOURCE_MEMORY_FINITE;o.memory_limit=0;
    CHECK(build(source,length,&o,&artifact)==XR_SOURCE_MEMORY_LIMIT && !artifact);
    for(unsigned i=0;i<2;++i) {
        o=options();CHECK(build(source,length,&o,&artifact)==XR_SOURCE_OK && artifact);
        Output out={.refuse=i?XR_SOURCE_WRITE_LIMIT:XR_SOURCE_WRITE_OOM};
        run_ready(artifact,&out,NULL,i?XR_SOURCE_RUN_LIMIT:XR_SOURCE_RUN_OUT_OF_MEMORY);artifact=NULL;
        CHECK(out.calls==1 && !out.size);
    }
    xr_free(source);
}
static void emit(const char *path) {
    size_t length=0;char *source=read_source(&length);XrSourceCompileOptions o=options();o.target=XR_SOURCE_TARGET_C;
    XrSourceArtifact *artifact=NULL;CHECK(build(source,length,&o,&artifact)==XR_SOURCE_OK);
    memset(source,'!',length);xr_free(source);
    const char *text=NULL;size_t size=0;CHECK(xray_source_artifact_c_text(artifact,&text,&size)==XR_SOURCE_OK && size);
    FILE *file=fopen(path,"wb");CHECK(file && fwrite(text,1,size,file)==size && !fclose(file));
    xray_source_artifact_free(artifact);
}
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--emit")) { emit(argv[2]);return 0; }
    CHECK(argc==1);vm_and_lifetime();exact_rejections();refusal_without_publication();
    puts("Source nullable String print: exact output, snapshots, single evaluation, ownership and refusal PASS");return 0;
}
