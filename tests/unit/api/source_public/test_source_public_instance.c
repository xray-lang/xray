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
int main(void) {
    static const char source[]="fn initial()->i64{print(90);return 40}\nvar state=initial()\n"
        "export fn read()->i64{return state}\nexport fn next(value:i64)->i64{state+=value;return state}\n"
        "export fn reader()->fn(i64)->i64{return fn(value:i64)->i64{return state+value}}\n"
        "export fn paused()->i64{state+=1;defer{print(state)};Coro.yield();state+=10;return state}\n"
        "export fn emit()->i64{print(7);return state}\nfn hidden()->i64{return 99}\n"
        "export fn twoLines()->i64{print(7);print(8);return state}\n";
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;
    XrSourceCompiler *compiler=NULL;CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK);
    XrSourceText text={NULL,"public-persistent-instance",source,sizeof(source)-1};
    XrSourceProgram *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_compile_text(compiler,&text,&program,&diagnostic);
    if(diagnostic) {XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);fprintf(stderr,"%u: %s\n",(unsigned)status,info.message);}
    CHECK(status==XR_SOURCE_OK && program && !diagnostic);
    XrSourceScalar scalar={.kind=XR_SOURCE_VALUE_I64,.value.signed_value=2};
    XrSourceValue *two=NULL,*wrong=NULL;
    CHECK(xray_source_value_new_scalar(compiler,&scalar,&two)==XR_SOURCE_OK);
    CHECK(xray_source_value_new_string(compiler,"wrong",5,&wrong)==XR_SOURCE_OK);
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    runtime.output_byte_limit=6;runtime.write=collect;
    Output outputs[3]={0};XrSourceInstance *owners[3]={0};
    for(unsigned i=0;i<3;++i) {
        runtime.context=&outputs[i];
        CHECK(xray_source_instance_new(program,&runtime,&owners[i])==XR_SOURCE_OK);
        outputs[i].owner=owners[i];
    }
    Output one_shot={0};runtime.output_byte_limit=5;runtime.context=&one_shot;
    XrSourceInvocation two_lines=invocation("twoLines",NULL,0);XrSourceResult *limited=NULL;
    /* The initial module line spends three bytes, leaving only one full line. */
    CHECK(xray_source_invoke(program,&two_lines,&runtime,&limited,NULL)==XR_SOURCE_LIMIT && !limited);
    CHECK(one_shot.length==5 && !memcmp(one_shot.bytes,"90\n7\n",5));
    xray_source_program_free(program);xray_source_compiler_free(compiler);
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
    return 0;
}
