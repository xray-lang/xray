/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_entry_oracles.c - Independent entry ownership and interactive expectations
 *
 * KEY CONCEPT:
 *   Public-only calls exercise real owners; the allocator companion runs the
 *   same cases and requires physical zero after every complete owner lifetime.
 */
#include "xray_source_instance.h"
#include "xray_source_repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#if ENTRY_ORACLE_TRACKED
#include "xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "execution/xr_xir_host_execution.c"
#include "api/xsource_api.c"
#endif
typedef struct OracleOutput {char bytes[256];size_t length;XrSourceCall *call;unsigned cleanup_callbacks;} OracleOutput;
static XrSourceWriteStatus oracle_output(void *context,XrSourceOutputStream stream,const char *bytes,size_t size) {
    OracleOutput *out=context;CHECK(stream==XR_SOURCE_STDOUT && size<=sizeof(out->bytes)-out->length);
    memcpy(out->bytes+out->length,bytes,size);out->length+=size;
    if(out->call) {
        XrSourceEvent event={XR_SOURCE_RUN_PANICKED,31,32,33},before=event;XrSourceCall *call=out->call;
        CHECK(xray_source_call_cancel(call)==XR_SOURCE_BUSY);
        CHECK(xray_source_call_drop(&call,&event)==XR_SOURCE_BUSY && call==out->call && !memcmp(&event,&before,sizeof(event)));
        ++out->cleanup_callbacks;
    }
    return XR_SOURCE_WRITE_OK;
}
static XrSourceCompilerOptions oracle_compiler_options(void) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_ORACLE_STDLIB;return options;
}
static XrSourceCallOptions oracle_runtime(OracleOutput *out) {
    XrSourceCallOptions options;CHECK(xray_source_call_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.write=oracle_output;options.context=out;return options;
}
static XrSourceInvocation oracle_invocation(const char *name) {
    XrSourceInvocation request;CHECK(xray_source_invocation_init(&request,sizeof(request))==XR_SOURCE_OK);
    request.name=name;request.name_length=strlen(name);return request;
}
static XrSourceProgram *oracle_compile(const char *source,XrSourceCompiler **compiler) {
    XrSourceCompilerOptions options=oracle_compiler_options();
    CHECK(xray_source_compiler_new(&options,compiler)==XR_SOURCE_OK);
    const XrSourceText text={NULL,"entry-oracle",source,strlen(source)};
    XrSourceProgram *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_compile_text(*compiler,&text,&program,&diagnostic);
    if(diagnostic){XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);fprintf(stderr,"source %u: %s\n",(unsigned)status,info.message);}
    CHECK(status==XR_SOURCE_OK && program && !diagnostic);return program;
}
static XrSourceEvent oracle_poll(XrSourceCall *call) {
    XrSourceEvent event={0};
    for(unsigned i=0;i<10000;++i) {
        CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);
        if(event.state!=XR_SOURCE_RUN_READY)return event;
    }
    CHECK(0);return event;
}
static void oracle_i64(XrSourceInstance *owner,const char *name,int64_t expected) {
    XrSourceInvocation request=oracle_invocation(name);XrSourceResult *result=NULL;int64_t value=0;
    CHECK(xray_source_instance_execute(owner,&request,&result,NULL)==XR_SOURCE_OK);
    CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==expected);xray_source_result_free(result);
}
static void oracle_C15(void) {
    for(unsigned phase=0;phase<4;++phase) {
        XrSourceCompiler *compiler=NULL;XrSourceProgram *program=oracle_compile(
            "export fn paused()->i64{defer{print(\"cleanup\")};Coro.yield();return 42}\n",&compiler);
        OracleOutput out={0};XrSourceCallOptions runtime=oracle_runtime(&out);XrSourceInvocation request=oracle_invocation("paused");
        XrSourceCall *call=NULL;CHECK(xray_source_invoke_begin(program,&request,&runtime,&call)==XR_SOURCE_OK);
        xray_source_program_free(program);xray_source_compiler_free(compiler);out.call=phase==3?call:NULL;
        XrSourceEvent suspension={0};
        if(phase) {suspension=oracle_poll(call);CHECK(suspension.state==XR_SOURCE_RUN_SUSPENDED);}
        if(phase==2) {
            CHECK(xray_source_call_resume(call,suspension.epoch,suspension.wake)==XR_SOURCE_OK);
            CHECK(oracle_poll(call).state==XR_SOURCE_RUN_RETURNED);
            XrSourceStatus late=xray_source_call_cancel(call);CHECK(late!=XR_SOURCE_OK);
        } else {
            CHECK(xray_source_call_cancel(call)==XR_SOURCE_OK);
            CHECK(oracle_poll(call).state==XR_SOURCE_RUN_CANCELLED);
        }
        XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK);
        XrSourceEvent event;CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK &&
            event.state==(phase==2?XR_SOURCE_RUN_RETURNED:XR_SOURCE_RUN_CANCELLED));
        if(phase)CHECK(xray_source_call_resume(call,suspension.epoch,suspension.wake)==XR_SOURCE_BAD_STATE);
        CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);out.call=NULL;
        if(phase){CHECK(out.length==8 && !memcmp(out.bytes,"cleanup\n",8));}
        else CHECK(!out.length);
        if(phase==3)CHECK(out.cleanup_callbacks==1);
        xray_source_result_free(result);
    }
    puts("C15 cancellation before execution/yielded/after ready/during cleanup, stale wake refusal; owned results retained PASS");
}
static void oracle_C17(void) {
    XrSourceCompiler *compiler=NULL;XrSourceProgram *program=oracle_compile(
        "var count=0\nexport fn next()->i64{count+=1;return count}\n"
        "fn answer()->i64{return 42}\nexport fn callback()->fn()->i64{return answer}\n"
        "export fn text()->string{return \"A\\0B\"}\n",&compiler);
    OracleOutput out={0};XrSourceCallOptions runtime=oracle_runtime(&out);XrSourceInstance *a=NULL,*b=NULL;
    CHECK(xray_source_instance_new(program,&runtime,&a)==XR_SOURCE_OK && xray_source_instance_new(program,&runtime,&b)==XR_SOURCE_OK);
    xray_source_program_free(program);xray_source_compiler_free(compiler);
    oracle_i64(a,"next",1);oracle_i64(a,"next",2);oracle_i64(b,"next",1);
    XrSourceInvocation request=oracle_invocation("text");XrSourceResult *text=NULL,*function_result=NULL;
    CHECK(xray_source_instance_execute(a,&request,&text,NULL)==XR_SOURCE_OK);
    request=oracle_invocation("callback");CHECK(xray_source_instance_execute(a,&request,&function_result,NULL)==XR_SOURCE_OK);
    XrSourceValue *function=NULL;CHECK(xray_source_result_value(function_result,&function)==XR_SOURCE_OK);xray_source_result_free(function_result);
    CHECK(xray_source_instance_drop(&a,NULL)==XR_SOURCE_OK && !a);
    const char *bytes=NULL;size_t size=0;
    CHECK(xray_source_result_bytes(text,&bytes,&size)==XR_SOURCE_OK && size==3 && !memcmp(bytes,"A\0B",3));
    XrSourceCall *call=NULL;
    CHECK(xray_source_instance_begin_function(b,function,NULL,0,&call)==XR_SOURCE_BAD_ARGUMENT && !call);
    xray_source_value_free(function);CHECK(xray_source_instance_drop(&b,NULL)==XR_SOURCE_OK && !b);
    CHECK(xray_source_result_bytes(text,&bytes,&size)==XR_SOURCE_OK && size==3 && !memcmp(bytes,"A\0B",3));
    xray_source_result_free(text);puts("C17 owned A-NUL-B after producer/Instance death; escaped callable rejects foreign owner; state2/1 PASS");
}
static XrSourceRepl *oracle_repl(OracleOutput *out) {
    XrSourceCompilerOptions compiler=oracle_compiler_options();XrSourceCallOptions runtime=oracle_runtime(out);XrSourceRepl *owner=NULL;
    CHECK(xray_source_repl_new(&compiler,"entry-oracle",&runtime,&owner)==XR_SOURCE_OK);return owner;
}
static XrSourceRunState oracle_input(XrSourceRepl *owner,const char *source) {
    const XrSourceText text={NULL,"entry-oracle",source,strlen(source)};XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_repl_begin(owner,&text,&diagnostic);
    if(status!=XR_SOURCE_OK){xray_source_diagnostic_free(diagnostic);return XR_SOURCE_RUN_API_ERROR;}
    XrSourceEvent event={0};
    for(unsigned i=0;i<10000;++i){CHECK(xray_source_repl_poll(owner,16,&event)==XR_SOURCE_OK);if(event.state!=XR_SOURCE_RUN_READY)break;}
    CHECK(event.state!=XR_SOURCE_RUN_READY && event.state!=XR_SOURCE_RUN_SUSPENDED);
    XrSourceResult *result=NULL;CHECK(xray_source_repl_take(owner,&result)==XR_SOURCE_OK);xray_source_result_free(result);return event.state;
}
static void oracle_repl_integer(XrSourceRepl *owner,const char *name,int64_t expected) {
    XrSourceValue *value=NULL;XrSourceScalar actual={0};
    CHECK(xray_source_repl_read(owner,name,&value)==XR_SOURCE_OK && xray_source_value_scalar(value,&actual)==XR_SOURCE_OK);
    CHECK(actual.kind==XR_SOURCE_VALUE_I64 && actual.value.signed_value==expected);xray_source_value_free(value);
}
static void oracle_close_repl(XrSourceRepl **owner) {
    XrSourceEvent cleanup;CHECK(xray_source_repl_drop(owner,&cleanup)==XR_SOURCE_OK && !*owner && cleanup.state==XR_SOURCE_RUN_READY);
}
static void oracle_absent(XrSourceRepl *owner,const char *name) {
    const XrSourceReplBindingInfo *rows=NULL;size_t count=0;
    CHECK(xray_source_repl_bindings(owner,&rows,&count)==XR_SOURCE_OK);
    for(size_t i=0;i<count;++i)CHECK(strcmp(rows[i].name,name));
    XrSourceValue *value=NULL;XrSourceStatus status=xray_source_repl_read(owner,name,&value);
    CHECK(status!=XR_SOURCE_OK && !value);
}
static void oracle_C19(void) {
    OracleOutput out={0};XrSourceRepl *owner=oracle_repl(&out);
    const char *inputs[]={"fn value() -> i64 { return 40 }\n","fn before() -> i64 { return value() + 2 }\n",
        "fn value() -> string { return \"later\" }\n","var old = before()\n","var latest = value()\n"};
    for(unsigned i=0;i<5;++i)CHECK(oracle_input(owner,inputs[i])==XR_SOURCE_RUN_RETURNED);
    oracle_repl_integer(owner,"old",42);XrSourceValue *value=NULL;const char *bytes=NULL;size_t size=0;
    CHECK(xray_source_repl_read(owner,"latest",&value)==XR_SOURCE_OK);oracle_close_repl(&owner);
    CHECK(xray_source_value_bytes(value,&bytes,&size)==XR_SOURCE_OK && size==5 && !memcmp(bytes,"later",5));
    xray_source_value_free(value);puts("C19 old i64=42/latest string=later from original five inputs PASS");
}
static void oracle_C20(void) {
    OracleOutput out={0};XrSourceRepl *owner=oracle_repl(&out);
    CHECK(oracle_input(owner,"fn value() -> i64 { return 5 }\n")==XR_SOURCE_RUN_RETURNED);
    CHECK(oracle_input(owner,"fn value() -> string { return 1 }\n")==XR_SOURCE_RUN_API_ERROR);
    CHECK(oracle_input(owner,"var answer = value()\n")==XR_SOURCE_RUN_RETURNED);oracle_repl_integer(owner,"answer",5);
    const XrSourceReplBindingInfo *rows=NULL;size_t count=0;
    CHECK(xray_source_repl_bindings(owner,&rows,&count)==XR_SOURCE_OK && count==2);
    unsigned first=0,third=0;
    for(size_t i=0;i<count;++i){if(rows[i].generation==1)++first;else if(rows[i].generation==3)++third;}
    CHECK(first==1 && third==1);oracle_close_repl(&owner);
    puts("C20 original3 attempts: actual published births1/3 only, rejected2 absent, answer5 PASS");
}
static void oracle_C21(void) {
    OracleOutput out={0};XrSourceRepl *owner=oracle_repl(&out);
    CHECK(oracle_input(owner,"fn value()->i64{return 5}\n")==XR_SOURCE_RUN_RETURNED);
    CHECK(oracle_input(owner,"fn value()->i64{return 99}\nvar zero=0\nvar crash=1/zero\n")==XR_SOURCE_RUN_PANICKED);
    CHECK(oracle_input(owner,"var answer=value()\n")==XR_SOURCE_RUN_RETURNED);oracle_repl_integer(owner,"answer",5);oracle_close_repl(&owner);
    owner=oracle_repl(&out);CHECK(oracle_input(owner,"var x=11\n")==XR_SOURCE_RUN_RETURNED);
    CHECK(oracle_input(owner,"x+=1\nvar transient=9\nvar zero=0\nvar crash=1/zero\n")==XR_SOURCE_RUN_PANICKED);
    oracle_repl_integer(owner,"x",12);oracle_absent(owner,"transient");oracle_close_repl(&owner);
    owner=oracle_repl(&out);
    CHECK(oracle_input(owner,"fn apply(f:fn(i64)->i64,x:i64)->i64{return f(x)}\nfn original(x:i64)->i64{return x+1}\n")==XR_SOURCE_RUN_RETURNED);
    CHECK(oracle_input(owner,"fn original(x:i64)->i64{return x*2}\nvar transient=apply(original,10)\nvar zero=0\nvar crash=1/zero\n")==XR_SOURCE_RUN_PANICKED);
    CHECK(oracle_input(owner,"var answer=apply(original,10)\n")==XR_SOURCE_RUN_RETURNED);oracle_repl_integer(owner,"answer",11);
    oracle_absent(owner,"transient");oracle_close_repl(&owner);
    puts("C21 separate sessions: old5, side effect12, old callable11, no transient publication PASS");
}
static void oracle_C22(void) {
    OracleOutput out={0};XrSourceRepl *owner=oracle_repl(&out);
    CHECK(oracle_input(owner,"print(\"init\")\nfn identity<T>(x:T)->T{return x}\nvar first=identity(41)\n")==XR_SOURCE_RUN_RETURNED);
    CHECK(oracle_input(owner,"fn later()->i64{return identity(42)}\n")==XR_SOURCE_RUN_RETURNED);
    CHECK(oracle_input(owner,"var answer=later()\n")==XR_SOURCE_RUN_RETURNED);
    CHECK(oracle_input(owner,"class Point { x:i64; y:i64\ninit(x:i64,y:i64){this.x=x;this.y=y}}\n")==XR_SOURCE_RUN_RETURNED);
    CHECK(oracle_input(owner,"var px=Point(7,8).x\n")==XR_SOURCE_RUN_RETURNED);
    oracle_repl_integer(owner,"first",41);oracle_repl_integer(owner,"answer",42);oracle_repl_integer(owner,"px",7);
    CHECK(out.length==5 && !memcmp(out.bytes,"init\n",5));oracle_close_repl(&owner);
    puts("C22 one initialization, cross-input generic41/42 and Point(7,8).x=7 PASS");
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    if(!strcmp(argv[1],"C15"))oracle_C15();else if(!strcmp(argv[1],"C17"))oracle_C17();
    else if(!strcmp(argv[1],"C19"))oracle_C19();else if(!strcmp(argv[1],"C20"))oracle_C20();
    else if(!strcmp(argv[1],"C21"))oracle_C21();else if(!strcmp(argv[1],"C22"))oracle_C22();else CHECK(0);
#if ENTRY_ORACLE_TRACKED
    CHECK(!runtime_live && !runtime_bytes);puts("All actual compiler/runtime owners released, physical=0/0");
#endif
    return 0;
}
