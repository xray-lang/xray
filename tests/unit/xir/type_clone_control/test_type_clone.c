/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_type_clone.c - Complete Source interruption and owned type snapshots
 */
#include "type_clone_probe.h"
#include "xray_source_ast.h"
#include "xray_source_value.h"
#include "base/xmalloc.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"Type clone %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "clone_fixture.h"
typedef struct Output {char data[16];size_t length;} Output;
static XrSourceWriteStatus write_output(void *opaque,XrSourceOutputStream stream,const char *bytes,size_t count) {
    Output *output=opaque;CHECK(stream==XR_SOURCE_STDOUT&&count<=sizeof(output->data)-output->length);
    memcpy(output->data+output->length,bytes,count);output->length+=count;return XR_SOURCE_WRITE_OK;
}
static void retired_execute(XrSourceArtifact *artifact,unsigned kind) {
    XrSourceExecutionOptions policy=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK);Output output={0};XrSourceCallOptions call_options;
    CHECK(xray_source_call_options_init(&call_options,sizeof(call_options))==XR_SOURCE_OK);call_options.write=write_output;call_options.context=&output;
    XrSourceCall *call=NULL;CHECK(xray_source_artifact_begin(execution,artifact,&call_options,&call)==XR_SOURCE_OK);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);XrSourceEvent event={0};
    for(unsigned i=0;;++i) {CHECK(i<10000&&xray_source_call_poll(call,1024,&event)==XR_SOURCE_OK);if(event.state!=XR_SOURCE_RUN_READY)break;}
    const char *expected=tc_expected(kind);CHECK(event.state==XR_SOURCE_RUN_RETURNED&&output.length==strlen(expected)&&!memcmp(output.data,expected,output.length));
    XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK);CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK&&!call);
    XrSourceValue *value=NULL,*copy=NULL;CHECK(xray_source_result_value(result,&value)==XR_SOURCE_OK);xray_source_result_free(result);
    CHECK(xray_source_value_copy(value,&copy)==XR_SOURCE_OK);xray_source_value_free(value);XrSourceScalar scalar={0};
    CHECK(xray_source_value_scalar(copy,&scalar)==XR_SOURCE_OK&&scalar.kind==XR_SOURCE_VALUE_I64&&scalar.value.signed_value==0);xray_source_value_free(copy);
}
static void release_probe(TcProbe *p) {
    if(p->resources) {XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(p->resources,&stats)==XR_COMPILE_RESOURCE_OK&&stats.held_bytes==p->held_initial);xr_compile_resources_release(p->resources);p->resources=NULL;}
    CHECK(!tc_memory_live()&&!tc_memory_bytes());
}
static uint64_t successful(unsigned kind,char *text,size_t length,XrSourceCompileOptions *options,TcProbe *p) {
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"type-clone",text,length}};XrSourceArtifact *artifact=NULL;
    XrSourceFailure failure={0};XrSourceDiagnostic *report=NULL;tc_probe_begin(p);uint64_t start=xr_time_monotonic_ns();CHECK(start);
    XrSourceStatus status=xray_source_build(&input,options,&artifact,&failure,&report);uint64_t elapsed=xr_time_monotonic_ns()-start;
    if(status!=XR_SOURCE_OK&&report) {XrSourceDiagnosticRoot root={0};CHECK(xray_source_diagnostic_root(report,0,&root)==XR_SOURCE_OK);fprintf(stderr,"Source failure %d:%d %s\n",root.primary.line,root.primary.column,root.message);}
    CHECK(status==XR_SOURCE_OK&&artifact&&!report);p->retired=true;size_t polls=p->polls;tc_probe_end(p);memset(text,'!',length);
    retired_execute(artifact,kind);CHECK(p->polls==polls);release_probe(p);return elapsed;
}
static void source_case(const char *phase,const char *mode) {
    unsigned kind=0;TcPhase selected=TC_METHODS_ZERO;
    if(!strcmp(phase,"parameters")){kind=4;selected=TC_PARAMETERS;}
    else if(!strcmp(phase,"name")){kind=1;selected=TC_NAME;}
    else if(!strcmp(phase,"type-name")){kind=3;selected=TC_NAME;}
    else if(!strcmp(phase,"fields")){kind=1;selected=TC_FIELD_ROWS;}
    else if(!strcmp(phase,"publication"))selected=TC_PUBLICATION;
    else CHECK(!strcmp(phase,"methods"));
    TcMode action=!strcmp(mode,"cancel")?TC_CANCEL:!strcmp(mode,"deadline")?TC_DEADLINE:!strcmp(mode,"oom")?TC_OOM:TC_SUCCESS;
    tc_memory_reset();char *text=xr_malloc(200000);CHECK(text);size_t length=tc_source(kind,text,200000);
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_TYPE_CLONE_STDLIB;
    TcProbe baseline={.phase=selected,.mode=TC_SUCCESS};options.cancel_poll=tc_cancel;options.cancel_context=&baseline;
    uint64_t elapsed=successful(kind,text,length,&options,&baseline);CHECK(baseline.completed&&baseline.calls==1&&baseline.witnesses);
    if(action==TC_SUCCESS){xr_free(text);puts("Complete Source clone baseline and retired execution PASS");return;}
    tc_memory_reset();CHECK(tc_source(kind,text,200000)==length);TcProbe probe={.phase=selected,.mode=action};options.cancel_context=&probe;
    if(action==TC_DEADLINE) {CHECK(elapsed<=UINT64_MAX-UINT64_C(5000000000));options.deadline_mode=XR_SOURCE_DEADLINE_AFTER;options.timeout_ns=elapsed+UINT64_C(5000000000);}
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"type-clone",text,length}};XrSourceArtifact *artifact=NULL;
    XrSourceDiagnostic *report=NULL;XrSourceFailure failure={0};tc_probe_begin(&probe);XrSourceStatus status=xray_source_build(&input,&options,&artifact,&failure,&report);
    XrSourceStatus expected=action==TC_CANCEL?XR_SOURCE_CANCELLED:action==TC_DEADLINE?XR_SOURCE_DEADLINE:XR_SOURCE_OUT_OF_MEMORY;
    fprintf(stderr,"clone %s/%s source%zu status%u stage%u cleanup%u calls%zu witness%zu oom%u held%llu/%llu\n",phase,mode,length,(unsigned)status,(unsigned)failure.stage,(unsigned)failure.cleanup_status,probe.calls,probe.witnesses,(unsigned)probe.oom_seen,(unsigned long long)probe.held_before,(unsigned long long)probe.held_after);
    CHECK(status==expected&&!artifact&&failure.status==expected&&failure.cleanup_status==XR_SOURCE_OK&&failure.stage==XR_SOURCE_STAGE_COMPILE);
    CHECK(probe.completed&&probe.calls==1&&(action==TC_OOM?(probe.oom_seen&&probe.attempts==1):probe.witnesses==1));
    probe.retired=true;tc_probe_end(&probe);xray_source_diagnostic_free(report);release_probe(&probe);
    /* A failed request owns no sticky resource error. Keep the same nondeadline
     * policy/callback options; a deadline recovery is an independent default. */
    CHECK(tc_source(kind,text,200000)==length);probe.retired=false;probe.disabled=true;probe.completed=false;probe.wanted=false;
    if(action==TC_DEADLINE){options=xray_source_compile_options_default();options.stdlib_path=XR_TYPE_CLONE_STDLIB;options.cancel_poll=tc_cancel;options.cancel_context=&probe;}
    successful(kind,text,length,&options,&probe);xr_free(text);CHECK(!tc_memory_live()&&!tc_memory_bytes());
    puts("Whole Source actual child/prefix/publication failure, rollback and fresh-request retired execution PASS");
}
static void write_fixtures(const char *directory) {
    static const char *names[]={"implementation","nominal","wide-parameters","wide-type-name","wide-callable"};char *text=xr_malloc(200000);CHECK(text);
    for(unsigned i=0;i<5;++i) {
        size_t length=tc_source(i,text,200000);char path[4096];int n=snprintf(path,sizeof(path),"%s/%s.xr",directory,names[i]);CHECK(n>0&&(size_t)n<sizeof(path));
        FILE *file=fopen(path,"wb");CHECK(file&&fwrite(text,1,length,file)==length&&!fclose(file));
        n=snprintf(path,sizeof(path),"%s/%s.out",directory,names[i]);CHECK(n>0&&(size_t)n<sizeof(path));file=fopen(path,"wb");CHECK(file);
        const char *expected=tc_expected(i);CHECK(fwrite(expected,1,strlen(expected),file)==strlen(expected)&&!fclose(file));
    }
    xr_free(text);
}
#include "raw_cases.h"
int main(int argc,char **argv) {
    if(argc==3&&!strcmp(argv[1],"--write-fixtures")){write_fixtures(argv[2]);return 0;}
    if(argc==4&&!strcmp(argv[1],"--source")){source_case(argv[2],argv[3]);return 0;}
    CHECK(argc==1);raw_cases();return 0;
}
