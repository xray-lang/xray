/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_public_instance.c - Public headers and real persistent ownership
 */
#include "xray_source_instance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
typedef struct Output {
    XrSourceInstance *owner;
    XrSourceCall *call;
    char bytes[64];
    size_t length;
} Output;
static XrSourceWriteStatus collect(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    Output *out=context;CHECK(stream==XR_SOURCE_STDOUT && length<=sizeof(out->bytes)-out->length);
    XrSourceInstance *owner=out->owner;XrSourceCall *call=NULL;
    XrSourceEvent event={0},before=event;
    if(owner) {
        CHECK(xray_source_instance_drop(&owner,&event)==XR_SOURCE_BUSY && owner==out->owner && !memcmp(&event,&before,sizeof(event)));
        CHECK(xray_source_instance_begin_main(owner,&call)==XR_SOURCE_BUSY && !call);
    }
    if(out->call) {
        call=out->call;
        CHECK(xray_source_call_drop(&call,&event)==XR_SOURCE_BUSY && call==out->call);
        CHECK(xray_source_call_poll(call,1,&event)==XR_SOURCE_BUSY);
    }
    memcpy(out->bytes+out->length,bytes,length);out->length+=length;return XR_SOURCE_WRITE_OK;
}
static XrSourceWriteStatus formatted_failure(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    Output *out=context;CHECK(stream==XR_SOURCE_STDERR && length<=sizeof(out->bytes)-out->length);
    memcpy(out->bytes+out->length,bytes,length);out->length+=length;return XR_SOURCE_WRITE_OK;
}typedef struct RefusedOutput {XrSourceWriteStatus status;unsigned calls;} RefusedOutput;
static XrSourceWriteStatus refuse_output(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    RefusedOutput *out=context;CHECK(stream==XR_SOURCE_STDOUT && length==3 && !memcmp(bytes,"90\n",3));
    ++out->calls;return out->status;
}
static XrSourceInvocation invocation(const char *name,const XrSourceValue *const *arguments,size_t count) {
    XrSourceInvocation request;CHECK(xray_source_invocation_init(&request,sizeof(request))==XR_SOURCE_OK);
    request.name=name;request.name_length=strlen(name);request.arguments=arguments;request.argument_count=count;return request;
}
static XrSourceResult *execute(XrSourceInstance *owner,const XrSourceInvocation *request) {
    XrSourceResult *result=NULL;XrSourceStatus status=xray_source_instance_execute(owner,request,&result,NULL);
    if(status!=XR_SOURCE_OK)fprintf(stderr,"execute %.*s status=%u\n",(int)request->name_length,request->name,(unsigned)status);
    CHECK(status==XR_SOURCE_OK && result);return result;
}
static XrSourceEvent until_pause(XrSourceCall *call) {
    XrSourceEvent event;unsigned steps=0;
    do {CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK && ++steps<10000);}
    while(event.state==XR_SOURCE_RUN_READY);return event;
}
static void returned_i64(XrSourceResult *result,int64_t expected) {
    XrSourceEvent event;int64_t actual=0;
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
    CHECK(xray_source_result_i64(result,&actual)==XR_SOURCE_OK && actual==expected);
    xray_source_result_free(result);
}
typedef struct CountedOutput {
    const char *piece;
    size_t piece_bytes,bytes,calls;
} CountedOutput;
static XrSourceWriteStatus count_output(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    CountedOutput *out=context;CHECK(stream==XR_SOURCE_STDOUT);
    if(!out->calls) CHECK(length==3 && !memcmp(bytes,"90\n",3));
    else CHECK(length==out->piece_bytes+1 && !memcmp(bytes,out->piece,out->piece_bytes) && bytes[length-1]=='\n');
    CHECK(length<=SIZE_MAX-out->bytes);out->bytes+=length;++out->calls;return XR_SOURCE_WRITE_OK;
}
static void default_output(XrSourceExecution *execution,XrSourceArtifact *program) {
    char piece[4096];memset(piece,'x',sizeof(piece));
    XrSourceValue *argument=NULL;CHECK(xray_source_value_new_string(execution,piece,sizeof(piece),&argument)==XR_SOURCE_OK);
    const XrSourceValue *arguments[]={argument};XrSourceInvocation request=invocation("manyLines",arguments,1);
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    CHECK(runtime.output_byte_limit==SIZE_MAX);
    CountedOutput out={.piece=piece,.piece_bytes=sizeof(piece)};runtime.write=count_output;runtime.context=&out;
    XrSourceInstance *owner=NULL;CHECK(xray_source_instance_new(execution,program,&runtime,&owner)==XR_SOURCE_OK);
    returned_i64(execute(owner,&request),40);
    const size_t expected=3+257*(sizeof(piece)+1);
    CHECK(out.calls==258 && out.bytes==expected && out.bytes>1048576);
    CHECK(xray_source_instance_drop(&owner,NULL)==XR_SOURCE_OK && !owner);
    out.bytes=out.calls=0;XrSourceResult *result=NULL;
    CHECK(xray_source_invoke(execution,program,&request,&runtime,&result,NULL)==XR_SOURCE_OK && result);
    returned_i64(result,40);CHECK(out.calls==258 && out.bytes==expected);
    xray_source_value_free(argument);
    puts("public default output: persistent and one-shot cumulative bytes exceed 1 MiB PASS");
}
static void completed_child(void) {
    static const char source[]="fn divide(n:i64)->i64{return 1/n}\n"
        "fn values()->Iterator<i64>{defer{const ignored=divide(0)};yield 7}\n"
        "export fn answer()->Iterator<i64>{const it=values();print(it.next());return it}\n"
        "export fn again()->i64{return 42}\n";
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"completed-child",source,sizeof(source)-1}};
    XrSourceArtifact *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
    CHECK(xray_source_build(&input,&options,&program,NULL,&diagnostic)==XR_SOURCE_OK && program && !diagnostic);
    XrSourceExecutionOptions policy=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK);
    XrSourceResult *retained[2]={0};
    for(unsigned persistent=0;persistent<2;++persistent) {
        Output out={0};XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
        runtime.write=collect;runtime.context=&out;XrSourceInvocation request=invocation("answer",NULL,0);
        XrSourceInstance *owner=NULL;
        if(persistent) {CHECK(xray_source_instance_new(execution,program,&runtime,&owner)==XR_SOURCE_OK);out.owner=owner;}
        XrSourceResult *result=NULL;XrSourceEvent failure={0},event={0};
        XrSourceStatus status=persistent?xray_source_instance_execute(owner,&request,&result,&failure):
            xray_source_invoke(execution,program,&request,&runtime,&result,&failure);
        if(result)CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK);
        if(status!=XR_SOURCE_OK || !result || event.state!=XR_SOURCE_RUN_CLEANUP_FAILED)
            fprintf(stderr,"completed child persistent=%u status=%u result=%p event=%u engine=%u failure=%u bytes=%zu\n",
                persistent,(unsigned)status,(void *)result,(unsigned)event.state,(unsigned)event.engine_status,
                (unsigned)failure.engine_status,out.length);
        CHECK(status==XR_SOURCE_OK && result && event.state==XR_SOURCE_RUN_CLEANUP_FAILED);
        CHECK(out.length==2 && !memcmp(out.bytes,"7\n",2));
        CHECK(xray_source_result_copy(result,&retained[persistent])==XR_SOURCE_OK);xray_source_result_free(result);result=NULL;
        CHECK(xray_source_instance_drop(&owner,NULL)==XR_SOURCE_OK && !owner);out.owner=NULL;
        request=invocation("again",NULL,0);CHECK(xray_source_invoke(execution,program,&request,&runtime,&result,NULL)==XR_SOURCE_OK);
        returned_i64(result,42);
    }
    xray_source_artifact_free(program);xray_source_execution_free(execution);
    static const char expected[]="[Fatal Cleanup] E0420: division by zero\n[Prior Cancellation]\n";
    for(unsigned i=0;i<2;++i) {
        Output formatted={0};CHECK(xray_source_result_format(retained[i],formatted_failure,&formatted)==XR_SOURCE_OK);
        CHECK(formatted.length==sizeof(expected)-1 && !memcmp(formatted.bytes,expected,formatted.length));
        xray_source_result_free(retained[i]);
    }
    puts("public completed Iterator: late owned fatal, primary/prior, retired results and new host request PASS");
}
static void initializing_child(void) {
    static const char source[]="import { values } from \"./_cleanup_initialized_iterator\"\n"
        "enum InitFailure{Failed{stream:Iterator<i64>}}\n"
        "fn fail()throws(InitFailure)->i64{const it=values(0);print(it.next());throw InitFailure.Failed{stream:it}}\n"
        "const neverPublished=try fail()\nexport fn answer()->i64{return 42}\n";
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={__FILE__,NULL,source,sizeof(source)-1}};
    XrSourceArtifact *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus compiled=xray_source_build(&input,&options,&program,NULL,&diagnostic);
    if(diagnostic) {XrSourceDiagnosticRoot info={0};CHECK(xray_source_diagnostic_root(diagnostic,0,&info)==XR_SOURCE_OK);
        fprintf(stderr,"initializing child compile=%u: %s\n",(unsigned)compiled,info.message);}
    CHECK(compiled==XR_SOURCE_OK && program && !diagnostic);
    XrSourceExecutionOptions policy=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK);
    Output out={0};XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    runtime.write=collect;runtime.context=&out;XrSourceInvocation request=invocation("answer",NULL,0);
    XrSourceResult *result=NULL;XrSourceEvent failure={0},event={0};
    XrSourceStatus status=xray_source_invoke(execution,program,&request,&runtime,&result,&failure);
    if(result)CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK);
    fprintf(stderr,"initializing child status=%u result=%p event=%u engine=%u failure=%u bytes=%zu\n",
        (unsigned)status,(void *)result,(unsigned)event.state,(unsigned)event.engine_status,(unsigned)failure.engine_status,out.length);
    CHECK(status==XR_SOURCE_OK && result && event.state==XR_SOURCE_RUN_CLEANUP_FAILED);
    CHECK(out.length==2 && !memcmp(out.bytes,"7\n",2));
    xray_source_artifact_free(program);xray_source_execution_free(execution);
    Output formatted={0};CHECK(xray_source_result_format(result,formatted_failure,&formatted)==XR_SOURCE_OK);
    static const char expected[]="[Fatal Cleanup] E0420: division by zero\n[Prior Cancellation]\n";
    CHECK(formatted.length==sizeof(expected)-1 && !memcmp(formatted.bytes,expected,formatted.length));xray_source_result_free(result);
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--completed-child")){completed_child();return 0;}
    if(argc==2 && !strcmp(argv[1],"--initializing-child")){initializing_child();return 0;}
    CHECK(argc==1);
    static const char source[]="fn initial()->i64{print(90);return 40}\nvar state=initial()\n"
        "export fn read()->i64{return state}\nexport fn next(value:i64)->i64{state+=value;return state}\n"
        "export fn reader()->fn(i64)->i64{return fn(value:i64)->i64{return state+value}}\n"
        "export fn paused()->i64{state+=1;defer{print(state)};Coro.yield();state+=10;return state}\n"
        "export fn emit()->i64{print(7);return state}\nfn hidden()->i64{return 99}\n"
        "export fn twoLines()->i64{print(7);print(8);return state}\n"
        "export fn manyLines(value:string)->i64{var n:i64=0;while(n<257){print(value);n+=1};return state}\n";
    XrSourceCompileOptions options=xray_source_compile_options_default();
    options.stdlib_path=XR_PUBLIC_STDLIB;
    XrSourceExecutionOptions policy=xray_source_execution_options_default();
    XrSourceExecution *execution=NULL;CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK);
    XrSourceText text={NULL,"public-persistent-instance",source,sizeof(source)-1};
    XrSourceArtifact *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text=text};
    XrSourceStatus status=xray_source_build(&input,&options,&program,NULL,&diagnostic);
    if(diagnostic) {XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);fprintf(stderr,"%u: %s\n",(unsigned)status,info.message);}
    CHECK(status==XR_SOURCE_OK && program && !diagnostic);
    default_output(execution,program);
    XrSourceScalar scalar={.kind=XR_SOURCE_VALUE_I64,.value.signed_value=2};
    XrSourceValue *two=NULL,*wrong=NULL;
    CHECK(xray_source_value_new_scalar(execution,&scalar,&two)==XR_SOURCE_OK);
    CHECK(xray_source_value_new_string(execution,"wrong",5,&wrong)==XR_SOURCE_OK);
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    runtime.output_byte_limit=6;runtime.write=collect;
    Output outputs[3]={0};XrSourceInstance *owners[3]={0};
    for(unsigned i=0;i<3;++i) {
        runtime.context=&outputs[i];
        CHECK(xray_source_instance_new(execution,program,&runtime,&owners[i])==XR_SOURCE_OK);
        outputs[i].owner=owners[i];
    }
    Output one_shot={0};runtime.output_byte_limit=5;runtime.context=&one_shot;
    XrSourceInvocation two_lines=invocation("twoLines",NULL,0);XrSourceResult *limited=NULL;
    /* The initial module line spends three bytes, leaving only one full line. */
    XrSourceEvent limited_failure={0},limited_event={0};
    status=xray_source_invoke(execution,program,&two_lines,&runtime,&limited,&limited_failure);
    if(status!=XR_SOURCE_LIMIT || limited) {
        if(limited)CHECK(xray_source_result_event(limited,&limited_event)==XR_SOURCE_OK);
        fprintf(stderr,"one-shot output status=%u result=%p event=%u engine=%u failure=%u bytes=%zu hex=",
            (unsigned)status,(void *)limited,(unsigned)limited_event.state,(unsigned)limited_event.engine_status,
            (unsigned)limited_failure.engine_status,one_shot.length);
        for(size_t i=0;i<one_shot.length;++i)fprintf(stderr,"%02x",(unsigned char)one_shot.bytes[i]);
        fputc('\n',stderr);
    }
    CHECK(status==XR_SOURCE_LIMIT && !limited);
    CHECK(one_shot.length==5 && !memcmp(one_shot.bytes,"90\n7\n",5));
    const XrSourceWriteStatus refused[]={XR_SOURCE_WRITE_OOM,XR_SOURCE_WRITE_LIMIT,XR_SOURCE_WRITE_IO};
    const XrSourceStatus rejected[]={XR_SOURCE_OUT_OF_MEMORY,XR_SOURCE_LIMIT,XR_SOURCE_IO};
    for(unsigned i=0;i<3;++i) {
        RefusedOutput out={refused[i],0};runtime.write=refuse_output;runtime.context=&out;
        CHECK(xray_source_invoke(execution,program,&two_lines,&runtime,&limited,&limited_failure)==rejected[i] && !limited);
        CHECK(out.calls==1);
    }
    xray_source_artifact_free(program);xray_source_execution_free(execution);
    const XrSourceValue *arguments[]={two};
    XrSourceInvocation next=invocation("next",arguments,1),read=invocation("read",NULL,0),reader=invocation("reader",NULL,0);
    returned_i64(execute(owners[0],&next),42);returned_i64(execute(owners[1],&read),40);
    CHECK(outputs[0].length==3 && !memcmp(outputs[0].bytes,"90\n",3) && outputs[1].length==3);
    XrSourceCall *call=NULL;XrSourceInvocation paused=invocation("paused",NULL,0);
    CHECK(xray_source_instance_begin_main(owners[2],&call)==XR_SOURCE_OK);
    CHECK(xray_source_call_cancel(call)==XR_SOURCE_OK);
    CHECK(until_pause(call).state==XR_SOURCE_RUN_CANCELLED);
    XrSourceResult *cancelled=NULL;CHECK(xray_source_call_take(call,&cancelled)==XR_SOURCE_OK);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call && !outputs[2].length);
    CHECK(xray_source_instance_begin(owners[2],&read,&call)==XR_SOURCE_EXECUTION_FAILED && !call);
    XrSourceEvent cancelled_event;CHECK(xray_source_result_event(cancelled,&cancelled_event)==XR_SOURCE_OK && cancelled_event.state==XR_SOURCE_RUN_CANCELLED);
    xray_source_result_free(cancelled);
    CHECK(xray_source_instance_begin(owners[0],&paused,&call)==XR_SOURCE_OK);outputs[0].call=call;
    XrSourceEvent suspension=until_pause(call);CHECK(suspension.state==XR_SOURCE_RUN_SUSPENDED);
    XrSourceCall *empty=NULL;XrSourceInstance *before=owners[0];XrSourceResult *result=NULL;
    CHECK(xray_source_instance_drop(&owners[0],NULL)==XR_SOURCE_BUSY && owners[0]==before);
    CHECK(xray_source_instance_begin(owners[0],&read,&empty)==XR_SOURCE_BUSY && !empty);
    CHECK(xray_source_instance_execute(owners[0],&read,&result,NULL)==XR_SOURCE_BUSY && !result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);outputs[0].call=NULL;
    CHECK(outputs[0].length==6 && !memcmp(outputs[0].bytes,"90\n43\n",6));
    returned_i64(execute(owners[0],&read),43);
    result=execute(owners[0],&reader);XrSourceValue *function=NULL,*copy=NULL;
    CHECK(xray_source_result_value(result,&function)==XR_SOURCE_OK);xray_source_result_free(result);result=NULL;
    CHECK(xray_source_value_copy(function,&copy)==XR_SOURCE_OK);xray_source_value_free(function);
    CHECK(xray_source_instance_begin_function(owners[1],copy,arguments,1,&empty)==XR_SOURCE_BAD_ARGUMENT && !empty);
    CHECK(xray_source_instance_begin_function(owners[0],two,arguments,1,&empty)==XR_SOURCE_BAD_ARGUMENT && !empty);
    const XrSourceValue *bad_arguments[]={wrong};
    CHECK(xray_source_instance_begin_function(owners[0],copy,bad_arguments,1,&empty)==XR_SOURCE_BAD_ARGUMENT && !empty);
    CHECK(xray_source_instance_begin_function(owners[0],copy,arguments,1,&call)==XR_SOURCE_OK);
    xray_source_value_free(two);xray_source_value_free(wrong);xray_source_value_free(copy);
    CHECK(xray_source_call_resume(call,suspension.epoch,suspension.wake)==XR_SOURCE_BAD_STATE);
    CHECK(until_pause(call).state==XR_SOURCE_RUN_RETURNED);
    CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);returned_i64(result,45);result=NULL;
    CHECK(xray_source_instance_begin_main(owners[0],&call)==XR_SOURCE_OK);
    CHECK(until_pause(call).state==XR_SOURCE_RUN_RETURNED);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call && outputs[0].length==6);
    XrSourceInvocation hidden=invocation("hidden",NULL,0);
    CHECK(xray_source_instance_begin(owners[0],&hidden,&empty)==XR_SOURCE_NOT_FOUND && !empty);
    XrSourceInvocation emit=invocation("emit",NULL,0);XrSourceEvent failure;
    status=xray_source_instance_execute(owners[0],&emit,&result,&failure);
    if(status!=XR_SOURCE_LIMIT || result || outputs[0].length!=6)
        fprintf(stderr,"persistent output status=%u result=%d bytes=%zu\n",(unsigned)status,result!=NULL,outputs[0].length);
    CHECK(status==XR_SOURCE_LIMIT && !result && outputs[0].length==6);
    CHECK(xray_source_instance_begin(owners[0],&read,&empty)==XR_SOURCE_BAD_STATE && !empty);
    returned_i64(execute(owners[1],&read),40);
    for(unsigned i=0;i<3;++i)CHECK(xray_source_instance_drop(&owners[i],NULL)==XR_SOURCE_OK && !owners[i]);
    CHECK(xray_source_instance_drop(&owners[0],NULL)==XR_SOURCE_OK);
    puts("public persistent Instance: producer death, state, callable arguments, authority, reentry, cancellation and cumulative output PASS");
    completed_child();
    initializing_child();
    return 0;
}
