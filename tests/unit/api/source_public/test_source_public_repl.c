/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_public_repl.c - Public-only incremental ownership and publication
 */
#include "xray_source_repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
static const char namespace_id[]="public-real-repl";
typedef struct Trace { XrSourceRepl *owner;char bytes[128];size_t count;unsigned callbacks; } Trace;
static XrSourceWriteStatus collect(void *context,XrSourceOutputStream stream,const char *bytes,size_t count) {
    Trace *trace=context;CHECK(stream==XR_SOURCE_STDOUT && count<=sizeof(trace->bytes)-trace->count);
    if(trace->owner) {
        XrSourceRepl *owner=trace->owner;XrSourceEvent event={.engine_status=777};
        XrSourceResult *result=NULL;XrSourceValue *value=NULL;
        XrSourceText text={NULL,namespace_id,"const reentry=1\n",16};
        CHECK(xray_source_repl_drop(&owner,&event)==XR_SOURCE_BUSY && owner==trace->owner && event.engine_status==777);
        CHECK(xray_source_repl_begin(owner,&text,NULL)==XR_SOURCE_BUSY);
        CHECK(xray_source_repl_poll(owner,1,&event)==XR_SOURCE_BUSY && event.engine_status==777);
        CHECK(xray_source_repl_read(owner,"value",&value)==XR_SOURCE_BUSY && !value);
        CHECK(xray_source_repl_take(owner,&result)==XR_SOURCE_BUSY && !result);
        CHECK(xray_source_repl_cancel(owner)==XR_SOURCE_BUSY);
    }
    memcpy(trace->bytes+trace->count,bytes,count);trace->count+=count;++trace->callbacks;return XR_SOURCE_WRITE_OK;
}
static XrSourceRepl *open_repl(Trace *trace,size_t output_limit) {
    XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    CHECK(xray_source_compiler_options_init(&compiler,sizeof(compiler))==XR_SOURCE_OK);
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    compiler.stdlib_path=XR_PUBLIC_STDLIB;runtime.write=collect;runtime.context=trace;runtime.output_byte_limit=output_limit;
    XrSourceRepl *owner=NULL;
    CHECK(xray_source_repl_new(&compiler,"",&runtime,&owner)!=XR_SOURCE_OK && !owner);
    CHECK(xray_source_repl_new(&compiler,namespace_id,&runtime,&owner)==XR_SOURCE_OK && owner);
    CHECK(xray_source_repl_new(&compiler,namespace_id,&runtime,&owner)==XR_SOURCE_BAD_ARGUMENT && owner);
    trace->owner=owner;return owner;
}
static XrSourceStatus begin(XrSourceRepl *owner,const char *source,XrSourceDiagnostic **diagnostic) {
    XrSourceText text={NULL,namespace_id,source,strlen(source)};
    return xray_source_repl_begin(owner,&text,diagnostic);
}
static XrSourceEvent pause_or_finish(XrSourceRepl *owner) {
    XrSourceEvent event={0};unsigned polls=0;
    do {CHECK(xray_source_repl_poll(owner,8,&event)==XR_SOURCE_OK && ++polls<100000);}
    while(event.state==XR_SOURCE_RUN_READY);return event;
}
static XrSourceResult *take(XrSourceRepl *owner,XrSourceRunState expected) {
    XrSourceResult *result=NULL;XrSourceEvent event={0};
    CHECK(xray_source_repl_take(owner,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==expected);
    XrSourceResult *empty=NULL;CHECK(xray_source_repl_take(owner,&empty)==XR_SOURCE_BAD_STATE && !empty);
    return result;
}
static void execute(XrSourceRepl *owner,const char *source) {
    XrSourceDiagnostic *diagnostic=NULL;XrSourceStatus status=begin(owner,source,&diagnostic);
    if(diagnostic) {XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);fprintf(stderr,"%u: %s\n",(unsigned)status,info.message);}
    CHECK(status==XR_SOURCE_OK && !diagnostic);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_RETURNED);
    xray_source_result_free(take(owner,XR_SOURCE_RUN_RETURNED));
}
static XrSourceValue *read_value(XrSourceRepl *owner,const char *name) {
    XrSourceValue *value=NULL;CHECK(xray_source_repl_read(owner,name,&value)==XR_SOURCE_OK && value);return value;
}
static void integer(XrSourceRepl *owner,const char *name,int64_t expected) {
    XrSourceValue *value=read_value(owner,name);XrSourceScalar scalar={0};
    CHECK(xray_source_value_scalar(value,&scalar)==XR_SOURCE_OK && scalar.kind==XR_SOURCE_VALUE_I64 && scalar.value.signed_value==expected);
    CHECK(xray_source_repl_read(owner,name,&value)==XR_SOURCE_BAD_ARGUMENT);
    xray_source_value_free(value);
}
static void history(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,3);
    XrSourceText wrong={NULL,"foreign-session","var never=1\n",12};
    CHECK(xray_source_repl_begin(owner,&wrong,NULL)==XR_SOURCE_BAD_ARGUMENT);
    wrong.namespace_id=NULL;CHECK(xray_source_repl_begin(owner,&wrong,NULL)==XR_SOURCE_BAD_ARGUMENT);
    wrong.namespace_id=namespace_id;wrong.absolute_path="C:/not-a-memory-input.xr";
    CHECK(xray_source_repl_begin(owner,&wrong,NULL)==XR_SOURCE_BAD_ARGUMENT);
    char first[]="var value=40\nfn next()->i64{value+=1;return value}\nconst old=next\nprint(17)\n";
    CHECK(begin(owner,first,NULL)==XR_SOURCE_OK);memset(first,'!',sizeof(first));
    XrSourceValue *empty=NULL;CHECK(xray_source_repl_read(owner,"value",&empty)==XR_SOURCE_BUSY && !empty);
    CHECK(begin(owner,"const pending=99\n",NULL)==XR_SOURCE_BUSY);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_RETURNED);
    CHECK(begin(owner,"const untaken=99\n",NULL)==XR_SOURCE_BUSY);
    XrSourceResult *survivor=take(owner,XR_SOURCE_RUN_RETURNED);
    XrSourceValue *function=read_value(owner,"old");
    execute(owner,"const first=next()\n");integer(owner,"first",41);
    execute(owner,"var value=\"shadow\"\nconst second=old()\n");integer(owner,"second",42);
    XrSourceValue *shadow=read_value(owner,"value");const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_value_bytes(shadow,&bytes,&length)==XR_SOURCE_OK && length==6 && !memcmp(bytes,"shadow",6));
    XrSourceDiagnostic *diagnostic=NULL;
    CHECK(begin(owner,"var invisible=missingDefinition\n",&diagnostic)==XR_SOURCE_REJECTED && diagnostic);
    CHECK(xray_source_repl_read(owner,"invisible",&empty)!=XR_SOURCE_OK && !empty);
    CHECK(begin(owner,"const abandoned=old()\nassert(false)\n",NULL)==XR_SOURCE_OK);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_PANICKED);
    xray_source_result_free(take(owner,XR_SOURCE_RUN_PANICKED));
    CHECK(xray_source_repl_read(owner,"abandoned",&empty)!=XR_SOURCE_OK && !empty);
    execute(owner,"const after=old()\n");integer(owner,"after",44);
    CHECK(trace.count==3 && !memcmp(trace.bytes,"17\n",3) && trace.callbacks);
    CHECK(begin(owner,"const notPublished=old()\nprint(99)\n",NULL)==XR_SOURCE_OK);
    XrSourceEvent limited=pause_or_finish(owner);
    CHECK(limited.state==XR_SOURCE_RUN_CLEANUP_FAILED);
    XrSourceResult *limit_result=take(owner,XR_SOURCE_RUN_LIMIT);XrSourceEvent original_limit={0};
    CHECK(xray_source_result_event(limit_result,&original_limit)==XR_SOURCE_OK &&
        original_limit.engine_status==limited.engine_status);
    xray_source_result_free(limit_result);
    /* Runtime LIMIT remains fatal during actual candidate retirement. The
     * owner preserves the original outcome and cannot grant a fresh allowance. */
    CHECK(xray_source_repl_read(owner,"notPublished",&empty)==XR_SOURCE_BAD_STATE && !empty);
    CHECK(begin(owner,"const retry=old()\n",NULL)==XR_SOURCE_BAD_STATE);
    XrSourceEvent cleanup={0};CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && !owner && cleanup.state==XR_SOURCE_RUN_READY);
    trace.owner=NULL;int64_t zero=9;CHECK(xray_source_result_i64(survivor,&zero)==XR_SOURCE_OK && zero==0);
    XrSourceValueKind kind;CHECK(xray_source_value_kind(function,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_CALLABLE);
    CHECK(xray_source_value_bytes(shadow,&bytes,&length)==XR_SOURCE_OK && length==6 && !memcmp(bytes,"shadow",6));
    XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK && info.message && *info.message);
    xray_source_diagnostic_free(diagnostic);xray_source_value_free(shadow);xray_source_value_free(function);xray_source_result_free(survivor);
    CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
}
static void cancellation(void) {
    Trace trace={0};XrSourceRepl *owner=open_repl(&trace,128);execute(owner,"var state=0\n");
    XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus prepared=begin(owner,"state+=1\ndefer{state+=10}\nCoro.yield()\nstate+=100\n",&diagnostic);
    CHECK(prepared==XR_SOURCE_REJECTED && diagnostic);
    XrSourceDiagnosticInfo invalid;CHECK(xray_source_diagnostic_info(diagnostic,&invalid)==XR_SOURCE_OK &&
        strstr(invalid.message,"enclosing real block"));
    xray_source_diagnostic_free(diagnostic);diagnostic=NULL;integer(owner,"state",0);
    prepared=begin(owner,"{state+=1\ndefer{state+=10}\nCoro.yield()\nstate+=100}\n",&diagnostic);
    if(diagnostic){XrSourceDiagnosticInfo info;CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);fprintf(stderr,"cancel block %u at%d: %s\n",(unsigned)prepared,info.line,info.message);}
    CHECK(prepared==XR_SOURCE_OK && !diagnostic);
    XrSourceEvent suspended=pause_or_finish(owner);CHECK(suspended.state==XR_SOURCE_RUN_SUSPENDED);
    XrSourceWait wait={0};CHECK(xray_source_repl_wait_request(owner,suspended.epoch,suspended.wake,&wait,sizeof(wait))==XR_SOURCE_OK && wait.kind==XR_SOURCE_WAIT_YIELD);
    CHECK(xray_source_repl_resume(owner,suspended.epoch,suspended.wake+1)!=XR_SOURCE_OK);
    CHECK(xray_source_repl_cancel(owner)==XR_SOURCE_OK);
    CHECK(pause_or_finish(owner).state==XR_SOURCE_RUN_CANCELLED);
    xray_source_result_free(take(owner,XR_SOURCE_RUN_CANCELLED));integer(owner,"state",11);
    CHECK(xray_source_repl_resume(owner,suspended.epoch,suspended.wake)==XR_SOURCE_BAD_STATE);
    execute(owner,"const afterCancel=state\n");integer(owner,"afterCancel",11);
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(&owner,&cleanup)==XR_SOURCE_OK && cleanup.state==XR_SOURCE_RUN_READY);
}
int main(void) {
    history();cancellation();
    puts("public Source REPL: real Compiler/Catalog, exact AST, paired publication, old closures, rollback, cumulative output, owned survivors, reentry and cancel/defer PASS");return 0;
}
