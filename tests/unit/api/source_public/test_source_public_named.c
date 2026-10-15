/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_public_named.c - Public-only named calls and owned outcome copies
 */
#include "xray_source_entry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
typedef struct NamedOutput {char bytes[256];size_t length;} NamedOutput;
static XrSourceWriteStatus named_write(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    NamedOutput *output=context;
    CHECK(stream==XR_SOURCE_STDOUT || stream==XR_SOURCE_STDERR);
    if(length>sizeof(output->bytes)-output->length)return XR_SOURCE_WRITE_LIMIT;
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;return XR_SOURCE_WRITE_OK;
}
static XrSourceProgram *named_program(XrSourceCompiler **compiler) {
    XrSourceCompilerOptions options;
    CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=UINT64_C(64)<<20;
    options.live_bytes=UINT64_C(8)<<20;options.work=UINT64_C(128000000);
    CHECK(xray_source_compiler_new(&options,compiler)==XR_SOURCE_OK);
    static const char source[]="const seed:i64=41\nprint(17)\n"
        "fn hidden()->i64{return 99}\n"
        "export fn answer()->i64{return seed+1}\n"
        "export fn parameter(value:i64)->i64{return value}\n"
        "export fn text()->string{return \"a\\0中z\"}\n"
        "export fn truth()->bool{return true}\n"
        "export fn fraction()->f64{return 1.5}\n"
        "export fn values()->Array<i64>{return [10,20]}\n"
        "export fn fail()->i64{assert(false,\"copy\\0中panic\");return 0}\n"
        "export fn paused()->i64{Coro.yield();return seed+1}\n";
    XrSourceText input={NULL,"public-named-entry",source,sizeof(source)-1};
    XrSourceProgram *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_compile_text(*compiler,&input,&program,&diagnostic);
    if(status!=XR_SOURCE_OK && diagnostic) {
        XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);
        fprintf(stderr,"named Source status=%u message=%s\n",(unsigned)status,info.message);
    }
    CHECK(status==XR_SOURCE_OK && program && !diagnostic);return program;
}
static XrSourceCallOptions named_options(NamedOutput *output) {
    XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.write=named_write;options.context=output;return options;
}
static XrSourceResult *named_execute(XrSourceProgram *program,const char *name,const XrSourceCallOptions *options) {
    XrSourceResult *result=NULL;
    XrSourceStatus status=xray_source_execute_named(program,name,strlen(name),options,&result,NULL);
    if(status!=XR_SOURCE_OK)fprintf(stderr,"named %s status=%u\n",name,(unsigned)status);
    CHECK(status==XR_SOURCE_OK && result);return result;
}
static void named_refusals(XrSourceProgram *program,const XrSourceCallOptions *options,NamedOutput *output) {
    XrSourceCall *call=NULL;XrSourceResult *result=NULL;
    XrSourceEvent failure={XR_SOURCE_RUN_THROWN,37,38,39},saved=failure;
    CHECK(xray_source_call_begin_named(program,"hidden",6,options,&call)==XR_SOURCE_NOT_FOUND && !call);
    CHECK(xray_source_call_begin_named(program,"missing",7,options,&call)==XR_SOURCE_NOT_FOUND && !call);
    CHECK(xray_source_call_begin_named(program,"parameter",9,options,&call)==XR_SOURCE_UNSUPPORTED && !call);
    CHECK(xray_source_call_begin_named(program,"answer-extra",6,options,&call)==XR_SOURCE_OK && call);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
    CHECK(xray_source_call_begin_named(program,NULL,6,options,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
    CHECK(xray_source_call_begin_named(program,"answer",0,options,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
    CHECK(xray_source_execute_named(program,"answer\0x",8,options,&result,&failure)==XR_SOURCE_BAD_ARGUMENT && !result);
    CHECK(xray_source_execute_named(program,"hidden",6,options,&result,&failure)==XR_SOURCE_NOT_FOUND && !result);
    CHECK(!memcmp(&failure,&saved,sizeof(saved)) && !output->length);
    result=named_execute(program,"answer",options);
    XrSourceResult *occupied=result;
    CHECK(xray_source_execute_named(program,"answer",6,options,&occupied,&failure)==XR_SOURCE_BAD_ARGUMENT && occupied==result);
    CHECK(xray_source_result_copy(result,&occupied)==XR_SOURCE_BAD_ARGUMENT && occupied==result);
    int64_t integer=-1;int boolean=37;double floating=9.0;
    CHECK(xray_source_result_i64(result,&integer)==XR_SOURCE_OK && integer==42);
    CHECK(xray_source_result_bool(result,&boolean)==XR_SOURCE_INVALID && boolean==37);
    CHECK(xray_source_result_f64(result,&floating)==XR_SOURCE_INVALID && floating==9.0);
    CHECK(output->length==3 && !memcmp(output->bytes,"17\n",3));
    xray_source_result_free(result);
}
static void named_outcomes(int array_display) {
    XrSourceCompiler *compiler=NULL;XrSourceProgram *program=named_program(&compiler);
    xray_source_compiler_free(compiler);compiler=NULL;
    NamedOutput output={0};XrSourceCallOptions options=named_options(&output);
    named_refusals(program,&options,&output);
    XrSourceResult *truth=named_execute(program,"truth",&options);
    int boolean=-1;CHECK(xray_source_result_bool(truth,&boolean)==XR_SOURCE_OK && boolean==1);
    xray_source_result_free(truth);
    XrSourceResult *fraction=named_execute(program,"fraction",&options);
    double floating=0;CHECK(xray_source_result_f64(fraction,&floating)==XR_SOURCE_OK && floating==1.5);
    xray_source_result_free(fraction);
    XrSourceResult *text=named_execute(program,"text",&options),*text_copy=NULL;
    XrSourceResult *array=named_execute(program,"values",&options),*array_copy=NULL;
    XrSourceResult *panic=named_execute(program,"fail",&options),*panic_copy=NULL;
    CHECK(xray_source_result_copy(text,&text_copy)==XR_SOURCE_OK && text_copy);
    CHECK(xray_source_result_copy(array,&array_copy)==XR_SOURCE_OK && array_copy);
    CHECK(xray_source_result_copy(panic,&panic_copy)==XR_SOURCE_OK && panic_copy);
    xray_source_program_free(program);xray_source_result_free(text);
    xray_source_result_free(array);xray_source_result_free(panic);
    const char *bytes=NULL;size_t length=0;
    static const char expected[]="a\0中z",message[]="copy\0中panic";
    CHECK(xray_source_result_bytes(text_copy,&bytes,&length)==XR_SOURCE_OK &&
        length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    CHECK(xray_source_result_bytes(panic_copy,&bytes,&length)==XR_SOURCE_OK &&
        length==sizeof(message)-1 && !memcmp(bytes,message,length));
    XrSourceEvent event;CHECK(xray_source_result_event(panic_copy,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_PANICKED);
    if(array_display) {
        NamedOutput formatted={0};CHECK(xray_source_result_format(array_copy,named_write,&formatted)==XR_SOURCE_OK);
        CHECK(formatted.length==8 && !memcmp(formatted.bytes,"[10, 20]",8));
    }
    xray_source_result_free(text_copy);xray_source_result_free(array_copy);xray_source_result_free(panic_copy);
}
static void named_producer_death(void) {
    XrSourceCompiler *compiler=NULL;XrSourceProgram *program=named_program(&compiler);
    NamedOutput output={0};XrSourceCallOptions options=named_options(&output);XrSourceCall *call=NULL;
    CHECK(xray_source_call_begin_named(program,"paused",6,&options,&call)==XR_SOURCE_OK && call);
    xray_source_program_free(program);xray_source_compiler_free(compiler);
    for(unsigned count=0;count<4096;++count) {
        XrSourceEvent event;CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);
        if(event.state==XR_SOURCE_RUN_READY)continue;
        if(event.state==XR_SOURCE_RUN_SUSPENDED) {
            XrSourceWait wait;CHECK(xray_source_call_wait_request(call,event.epoch,event.wake,&wait,sizeof(wait))==XR_SOURCE_OK);
            CHECK(wait.kind==XR_SOURCE_WAIT_YIELD);
            CHECK(xray_source_call_resume(call,event.epoch,event.wake)==XR_SOURCE_OK);continue;
        }
        CHECK(event.state==XR_SOURCE_RUN_RETURNED);
        XrSourceResult *result=NULL,*copy=NULL;
        CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
        CHECK(xray_source_result_copy(result,&copy)==XR_SOURCE_OK && copy);
        CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
        xray_source_result_free(result);int64_t integer=-1;
        CHECK(xray_source_result_i64(copy,&integer)==XR_SOURCE_OK && integer==42);
        CHECK(output.length==3 && !memcmp(output.bytes,"17\n",3));
        xray_source_result_free(copy);return;
    }
    CHECK(0);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--array-display")));
    named_outcomes(argc==2);named_producer_death();return 0;
}
