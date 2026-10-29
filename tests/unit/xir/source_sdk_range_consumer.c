/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_sdk_range_consumer.c - Public SDK class-valued ranges after compiler and program retirement
 */
#include "xray_source_ast.h"
#include "xray_source_instance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
static XrSourceValue *range_sdk_call(XrSourceInstance *instance,const char *name,const XrSourceValue *const *args,size_t count) {
    XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.name=name;invocation.name_length=strlen(name);invocation.arguments=args;invocation.argument_count=count;
    XrSourceResult *result=NULL;CHECK(xray_source_instance_execute(instance,&invocation,&result,NULL)==XR_SOURCE_OK && result);
    XrSourceEvent event;CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
    XrSourceValue *value=NULL;CHECK(xray_source_result_value(result,&value)==XR_SOURCE_OK && value);xray_source_result_free(result);return value;
}
static XrSourceValue *range_sdk_scalar(XrSourceExecution *execution,XrSourceValueKind kind,int64_t number) {
    XrSourceScalar scalar={0};scalar.kind=kind;
    if(kind==XR_SOURCE_VALUE_BOOL)scalar.value.boolean=(int)number;else scalar.value.signed_value=number;
    XrSourceValue *value=NULL;CHECK(xray_source_value_new_scalar(execution,&scalar,&value)==XR_SOURCE_OK && value);return value;
}
static void range_sdk_integer(XrSourceValue *value,int64_t expected) {
    XrSourceScalar scalar;CHECK(xray_source_value_scalar(value,&scalar)==XR_SOURCE_OK &&
        scalar.kind==XR_SOURCE_VALUE_I64 && scalar.value.signed_value==expected);xray_source_value_free(value);
}
int main(void) {
    char source[]="export fn make(a:i64,b:i64,closed:bool)->Range{return Range(a,b,closed)}\n"
        "export fn echo(r:Range)->Range{return r}\n"
        "export fn count(r:Range)->i64{return r.count()}\n"
        "export fn length(r:Range)->i64{return len(r)}\n"
        "export fn start(r:Range)->i64{return r.start}\n"
        "export fn indexFirst(r:Range)->i64{return r[0]}\n"
        "export fn indexLast(r:Range)->i64{return r[len(r)-1]}\n"
        "export fn indexNegative(r:Range)->i64{try{return r[-1]}catch panic(p){return p.code}}\n";
    XrSourceCompileOptions options=xray_source_compile_options_default();
    options.stdlib_path=XR_RANGE_VALUE_STDLIB;XrSourceExecution *execution=NULL;
    XrSourceExecutionOptions policy=xray_source_execution_options_default();
    CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK && execution);
    XrSourceText text={NULL,"range-sdk",source,sizeof(source)-1};XrSourceAst *ast=NULL;XrSourceDiagnostic *diagnostic=NULL;
    CHECK(xray_source_parse(&text,&options,&ast,NULL,&diagnostic)==XR_SOURCE_OK && ast && !diagnostic);
    memset(source,0xa5,sizeof(source));XrSourceArtifact *program=NULL;
    XrSourceStatus status=xray_source_build_ast(ast,&options,&program,NULL,&diagnostic);
    if(diagnostic){XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);fprintf(stderr,"Range SDK status%u %s\n",status,info.message);}
    CHECK(status==XR_SOURCE_OK && program && !diagnostic);xray_source_ast_free(ast);
    XrSourceValue *start=range_sdk_scalar(execution,XR_SOURCE_VALUE_I64,INT64_MIN);
    XrSourceValue *end=range_sdk_scalar(execution,XR_SOURCE_VALUE_I64,INT64_MAX);
    XrSourceValue *closed=range_sdk_scalar(execution,XR_SOURCE_VALUE_BOOL,1);
    XrSourceCallOptions call;CHECK(xray_source_call_options_init(&call,sizeof(call))==XR_SOURCE_OK);
    XrSourceInstance *instance=NULL;CHECK(xray_source_instance_new(execution,program,&call,&instance)==XR_SOURCE_OK && instance);
    xray_source_artifact_free(program);xray_source_execution_free(execution);
    const XrSourceValue *args[]={start,end,closed};XrSourceValue *range=range_sdk_call(instance,"make",args,3);
    xray_source_value_free(start);xray_source_value_free(end);xray_source_value_free(closed);
    XrSourceValueKind kind;CHECK(xray_source_value_kind(range,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_CLASS);
    const XrSourceValue *actual[]={range};XrSourceValue *copy=range_sdk_call(instance,"echo",actual,1);
    range_sdk_integer(range_sdk_call(instance,"count",actual,1),INT64_MAX);
    range_sdk_integer(range_sdk_call(instance,"length",actual,1),INT64_MAX);
    range_sdk_integer(range_sdk_call(instance,"start",actual,1),INT64_MIN);
    range_sdk_integer(range_sdk_call(instance,"indexFirst",actual,1),INT64_MIN);
    range_sdk_integer(range_sdk_call(instance,"indexLast",actual,1),-2);
    range_sdk_integer(range_sdk_call(instance,"indexNegative",actual,1),430);
    xray_source_value_free(range);CHECK(xray_source_instance_drop(&instance,NULL)==XR_SOURCE_OK && !instance);
    CHECK(xray_source_value_kind(copy,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_CLASS);
    XrSourceValue *survivor=NULL;CHECK(xray_source_value_copy(copy,&survivor)==XR_SOURCE_OK && survivor);
    xray_source_value_free(copy);xray_source_value_free(survivor);return 0;
}
