/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_public.c - Consumer compiled against the public header alone
 */
#include "xray_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while(0)
typedef struct Bytes { char data[512];size_t size;XrSourceOutputStream stream; } Bytes;
static XrSourceWriteStatus write_bytes(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    Bytes *out=context;if(length>sizeof(out->data)-out->size)return XR_SOURCE_WRITE_IO;
    memcpy(out->data+out->size,bytes,length);out->size+=length;out->stream=stream;return XR_SOURCE_WRITE_OK;
}
static XrSourceCompiler *compiler_new(void) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=UINT64_C(64)<<20;options.live_bytes=UINT64_C(8)<<20;options.work=UINT64_C(128000000);
    XrSourceCompiler *compiler=NULL;CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK);return compiler;
}
static XrSourceProgram *compile(XrSourceCompiler *compiler,const char *source) {
    XrSourceText text={NULL,"public-source-entry",source,strlen(source)};XrSourceProgram *program=NULL;
    XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_compile_text(compiler,&text,&program,&diagnostic);
    if(status!=XR_SOURCE_OK && diagnostic) {XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);fprintf(stderr,"compile status=%d %s\n",status,info.message);}
    CHECK(status==XR_SOURCE_OK && program && !diagnostic);return program;
}
static XrSourceCallOptions options_for(Bytes *out) {
    XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.write=write_bytes;options.context=out;return options;
}
static XrSourceEvent finish(XrSourceCall *call) {
    XrSourceEvent event={0};size_t steps=0;
    do {CHECK(++steps<4096);CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);}
    while(event.state==XR_SOURCE_RUN_READY);
    return event;
}
static void detached_call(void) {
    XrSourceCompiler *compiler=compiler_new();char source[]="print(\"A\\0B\")\n";
    XrSourceProgram *program=compile(compiler,source);memset(source,'!',sizeof(source));xray_source_compiler_free(compiler);
    XrSourceProgramInfo info;CHECK(xray_source_program_info(program,&info)==XR_SOURCE_OK && info.modules==1);
    Bytes out={0};XrSourceCallOptions options=options_for(&out);XrSourceCall *call=NULL;
    CHECK(xray_source_call_begin(program,&options,&call)==XR_SOURCE_OK);xray_source_program_free(program);
    CHECK(finish(call).state==XR_SOURCE_RUN_RETURNED);
    XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK);
    XrSourceResult *again=NULL;CHECK(xray_source_call_take(call,&again)==XR_SOURCE_BAD_STATE && !again);
    XrSourceEvent cleanup;CHECK(xray_source_call_drop(&call,&cleanup)==XR_SOURCE_OK && !call && cleanup.state==XR_SOURCE_RUN_READY);
    int64_t value=-1;CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==0);
    CHECK(out.size==4 && !memcmp(out.data,"A\0B\n",4));xray_source_result_free(result);
}
static void escaped_panic(void) {
    XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,"assert(false, \"a\\0中z\")\n");
    xray_source_compiler_free(compiler);Bytes out={0};XrSourceCallOptions options=options_for(&out);
    XrSourceResult *result=NULL;XrSourceEvent failure={0};
    CHECK(xray_source_execute(program,&options,&result,&failure)==XR_SOURCE_OK);xray_source_program_free(program);
    XrSourceEvent event;CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_PANICKED);
    const char *bytes=NULL;size_t length=0;CHECK(xray_source_result_bytes(result,&bytes,&length)==XR_SOURCE_OK);
    const char expected[]="a\0\xe4\xb8\xad" "z";CHECK(length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    CHECK(xray_source_result_format(result,write_bytes,&out)==XR_SOURCE_OK);
    const char formatted[]="[Uncaught Panic] E0445: a\0\xe4\xb8\xad" "z\n";
    CHECK(out.stream==XR_SOURCE_STDERR && out.size==sizeof(formatted)-1 && !memcmp(out.data,formatted,out.size));
    xray_source_result_free(result);
}
static void cancellation_and_budget(void) {
    XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,"var n:i64 = 0\nwhile (true) { n += 1 }\n");
    xray_source_compiler_free(compiler);Bytes out={0};XrSourceCallOptions options=options_for(&out);
    XrSourceCall *call=NULL;CHECK(xray_source_call_begin(program,&options,&call)==XR_SOURCE_OK);
    XrSourceEvent event;CHECK(xray_source_call_poll(call,1,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_READY);
    CHECK(xray_source_call_cancel(call)==XR_SOURCE_OK);CHECK(finish(call).state==XR_SOURCE_RUN_CANCELLED);
    XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK);CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK);
    xray_source_result_free(result);options.work_limit=64;
    CHECK(xray_source_call_begin(program,&options,&call)==XR_SOURCE_OK);xray_source_program_free(program);
    CHECK(finish(call).state==XR_SOURCE_RUN_LIMIT);result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK);xray_source_result_free(result);
}
static void timer_protocol(void) {
    XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,"import time\ntime.sleep(5)\nprint(\"awake\")\n");
    xray_source_compiler_free(compiler);Bytes out={0};XrSourceCallOptions options=options_for(&out);XrSourceCall *call=NULL;
    CHECK(xray_source_call_begin(program,&options,&call)==XR_SOURCE_OK);
    XrSourceEvent event=finish(call);CHECK(event.state==XR_SOURCE_RUN_SUSPENDED && event.wake && !out.size);
    XrSourceWait request={XR_SOURCE_WAIT_TASK,77};
    CHECK(xray_source_call_wait_request(call,event.epoch+1,event.wake,&request)==XR_SOURCE_BAD_STATE);
    CHECK(request.kind==XR_SOURCE_WAIT_TASK && request.after_ms==77);
    CHECK(xray_source_call_wait_request(call,event.epoch,event.wake,&request)==XR_SOURCE_OK);
    CHECK(request.kind==XR_SOURCE_WAIT_TIMER && request.after_ms==5);
    CHECK(xray_source_call_resume(call,event.epoch,event.wake+1)==XR_SOURCE_BAD_STATE);
    CHECK(xray_source_call_cancel(call)==XR_SOURCE_OK);CHECK(finish(call).state==XR_SOURCE_RUN_CANCELLED);
    XrSourceResult *cancelled=NULL;CHECK(xray_source_call_take(call,&cancelled)==XR_SOURCE_OK);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK);xray_source_result_free(cancelled);
    XrSourceResult *result=NULL;CHECK(xray_source_execute(program,&options,&result,NULL)==XR_SOURCE_OK);
    xray_source_program_free(program);CHECK(out.size==6 && !memcmp(out.data,"awake\n",6));xray_source_result_free(result);
}
typedef struct Reentry {XrSourceCall *call;unsigned callbacks;} Reentry;
static XrSourceWriteStatus reentrant_write(void *context,XrSourceOutputStream stream,const char *bytes,size_t size) {
    Reentry *guard=context;CHECK(stream==XR_SOURCE_STDOUT && size==3 && !memcmp(bytes,"42\n",3));
    XrSourceEvent event={XR_SOURCE_RUN_PANICKED,99,77,55};XrSourceResult *result=NULL;
    CHECK(xray_source_call_poll(guard->call,1,&event)==XR_SOURCE_BUSY);
    CHECK(event.engine_status==99 && event.epoch==77 && event.wake==55);
    CHECK(xray_source_call_take(guard->call,&result)==XR_SOURCE_BUSY && !result);
    CHECK(xray_source_call_cancel(guard->call)==XR_SOURCE_BUSY);
    XrSourceCall *original=guard->call;
    CHECK(xray_source_call_drop(&guard->call,&event)==XR_SOURCE_BUSY && guard->call==original);
    ++guard->callbacks;return XR_SOURCE_WRITE_OK;
}
static void reentry(void) {
    XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,"print(42)\n");
    xray_source_compiler_free(compiler);Reentry guard={0};XrSourceCallOptions options;
    CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);options.write=reentrant_write;options.context=&guard;
    CHECK(xray_source_call_begin(program,&options,&guard.call)==XR_SOURCE_OK);xray_source_program_free(program);
    CHECK(finish(guard.call).state==XR_SOURCE_RUN_RETURNED && guard.callbacks==1);
    XrSourceResult *result=NULL;CHECK(xray_source_call_take(guard.call,&result)==XR_SOURCE_OK);
    CHECK(xray_source_call_drop(&guard.call,NULL)==XR_SOURCE_OK);xray_source_result_free(result);
}
static void located_diagnostic(void) {
    XrSourceCompiler *compiler=compiler_new();const char source[]="var value = missing_symbol\n";
    XrSourceText text={XR_PUBLIC_FIXTURE "/unsaved-public.xr",NULL,source,sizeof(source)-1};
    XrSourceProgram *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
    CHECK(xray_source_compile_text(compiler,&text,&program,&diagnostic)==XR_SOURCE_REJECTED && !program && diagnostic);
    xray_source_compiler_free(compiler);XrSourceDiagnosticInfo info;
    CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK && info.line==1 && info.column==13);
    CHECK(info.path && strstr(info.path,"unsaved-public.xr") && strstr(info.message,"initialized value"));xray_source_diagnostic_free(diagnostic);
}
int main(void) {detached_call();escaped_panic();cancellation_and_budget();timer_protocol();reentry();located_diagnostic();puts("public header compiler/Program/Call/result ownership and byte semantics PASS");return 0;}
