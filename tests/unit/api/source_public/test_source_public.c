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
#include <threads.h>
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
static void compiler_layout_guards(void) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    CHECK(options.api_version==2 && options.wait_size==sizeof(XrSourceWait));
    XrSourceCompilerOptions saved=options;XrSourceCompiler *compiler=NULL;
    CHECK(xray_source_compiler_options_init(&options,sizeof(options)-1)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(options.api_version==saved.api_version && options.struct_size==saved.struct_size && options.wait_size==saved.wait_size &&
        options.allocated_bytes==saved.allocated_bytes && options.live_bytes==saved.live_bytes && options.work==saved.work && options.stdlib_path==saved.stdlib_path);
    options.api_version=1;CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_BAD_ARGUMENT && !compiler);
    options=saved;--options.wait_size;CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_BAD_ARGUMENT && !compiler);
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
static XrSourceEvent drain_waiting(XrSourceCall *call) {
    for(size_t step=0;step<4096;++step) {
        XrSourceEvent event;CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);
        if(event.state==XR_SOURCE_RUN_READY)continue;
        if(event.state!=XR_SOURCE_RUN_SUSPENDED)return event;
        XrSourceWait wait={0};CHECK(xray_source_call_wait_request(call,event.epoch,event.wake,&wait,sizeof(wait))==XR_SOURCE_OK);
        CHECK(wait.kind==XR_SOURCE_WAIT_YIELD || wait.kind==XR_SOURCE_WAIT_TIMER);
        if(wait.kind==XR_SOURCE_WAIT_TIMER) {
            struct timespec delay={(time_t)(wait.after_ms/1000u),(long)((wait.after_ms%1000u)*UINT64_C(1000000))};
            struct timespec remaining;
            int slept;
            do {slept=thrd_sleep(&delay,&remaining);CHECK(slept==0 || slept==-1);if(slept==-1)delay=remaining;}while(slept==-1);
        }
        CHECK(xray_source_call_resume(call,event.epoch,event.wake)==XR_SOURCE_OK);
    }
    CHECK(0);return (XrSourceEvent){0};
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
    XrSourceWait request={XR_SOURCE_WAIT_TASK,77,&event,88,99};
    CHECK(xray_source_call_wait_request(call,event.epoch,event.wake,&request,sizeof(request)-1)==XR_SOURCE_BAD_ARGUMENT);
    CHECK(xray_source_call_wait_request(call,event.epoch+1,event.wake,&request,sizeof(request))==XR_SOURCE_BAD_STATE);
    CHECK(request.kind==XR_SOURCE_WAIT_TASK && request.after_ms==77 && request.subject==&event && request.generation==88 && request.ticket==99);
    CHECK(xray_source_call_wait_request(call,event.epoch,event.wake,&request,sizeof(request))==XR_SOURCE_OK);
    CHECK(request.kind==XR_SOURCE_WAIT_TIMER && request.after_ms==5 && !request.subject && !request.generation && !request.ticket);
    CHECK(xray_source_call_resume(call,event.epoch,event.wake+1)==XR_SOURCE_BAD_STATE);
    CHECK(xray_source_call_cancel(call)==XR_SOURCE_OK);CHECK(finish(call).state==XR_SOURCE_RUN_CANCELLED);
    XrSourceResult *cancelled=NULL;CHECK(xray_source_call_take(call,&cancelled)==XR_SOURCE_OK);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK);xray_source_result_free(cancelled);
    XrSourceResult *result=NULL;CHECK(xray_source_execute(program,&options,&result,NULL)==XR_SOURCE_OK);
    xray_source_program_free(program);CHECK(out.size==6 && !memcmp(out.data,"awake\n",6));xray_source_result_free(result);
}
static void task_timer_protocol(void) {
    const char source[]="import time\nfn worker() -> i64 { time.sleep(5); return 42 }\n"
        "fn main() -> i64 { return await go worker() }\nprint(main())\n";
    XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,source);
    XrSourceProgram *synchronous=compile(compiler,source);xray_source_compiler_free(compiler);
    Bytes out={0};XrSourceCallOptions options=options_for(&out);XrSourceCall *call=NULL;
    CHECK(xray_source_call_begin(program,&options,&call)==XR_SOURCE_OK);xray_source_program_free(program);
    XrSourceEvent event=finish(call);CHECK(event.state==XR_SOURCE_RUN_SUSPENDED && event.wake && !out.size);
    XrSourceWait wait={0};CHECK(xray_source_call_wait_request(call,event.epoch,event.wake,&wait,sizeof(wait))==XR_SOURCE_OK);
    CHECK(wait.kind==XR_SOURCE_WAIT_TIMER && wait.after_ms==5 && !wait.subject && !wait.generation && !wait.ticket);
    CHECK(xray_source_call_cancel(call)==XR_SOURCE_OK);CHECK(drain_waiting(call).state==XR_SOURCE_RUN_CANCELLED);
    XrSourceResult *cancelled=NULL;CHECK(xray_source_call_take(call,&cancelled)==XR_SOURCE_OK);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK);xray_source_result_free(cancelled);
    XrSourceResult *result=NULL;CHECK(xray_source_execute(synchronous,&options,&result,NULL)==XR_SOURCE_OK);
    xray_source_program_free(synchronous);CHECK(out.size==3 && !memcmp(out.data,"42\n",3));
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);xray_source_result_free(result);
}
static void task_scalar_protocol(void) {
    static const struct {const char *type,*literal,*bytes;size_t length;} cases[]={
        {"i8","-128","-128\n",sizeof("-128\n")-1},
        {"i16","-32768","-32768\n",sizeof("-32768\n")-1},
        {"i32","-2147483648","-2147483648\n",sizeof("-2147483648\n")-1},
        {"u8","255","255\n",sizeof("255\n")-1},
        {"u16","65535","65535\n",sizeof("65535\n")-1},
        {"u32","4294967295","4294967295\n",sizeof("4294967295\n")-1},
        {"u64","18446744073709551615","18446744073709551615\n",sizeof("18446744073709551615\n")-1},
        {"f32","1.5","1.5\n",sizeof("1.5\n")-1},
        {"f64","1.5","1.5\n",sizeof("1.5\n")-1},
        {"rune","'x'","x\n",sizeof("x\n")-1},
        {"i64","-42","-42\n",sizeof("-42\n")-1},
        {"bool","true","true\n",sizeof("true\n")-1},
        {"string","\"a\\0中z\"","a\0中z\n",sizeof("a\0中z\n")-1},
        {NULL,NULL,"17\n",sizeof("17\n")-1}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        char source[1024];unsigned outputs=cases[i].type?2u:1u;
        int length=cases[i].type?snprintf(source,sizeof(source),
            "fn worker()->%s { Coro.yield(); return %s }\n"
            "fn calculate()->%s { const task:Task<%s> = go worker(); const first=await task; print(first); return await task }\n"
            "print(calculate())\n",cases[i].type,cases[i].literal,cases[i].type,cases[i].type):
            snprintf(source,sizeof(source),"fn worker() { Coro.yield() }\n"
                "fn calculate() { const task:Task<null> = go worker(); await task; await task; print(17) }\ncalculate()\n");
        CHECK(length>0 && (size_t)length<sizeof(source));
        XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,source);
        memset(source,'!',sizeof(source));xray_source_compiler_free(compiler);
        Bytes out={0};XrSourceCallOptions options=options_for(&out);XrSourceCall *call=NULL;
        CHECK(xray_source_call_begin(program,&options,&call)==XR_SOURCE_OK);xray_source_program_free(program);
        CHECK(drain_waiting(call).state==XR_SOURCE_RUN_RETURNED);
        XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK);
        CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK);
        CHECK(out.size==outputs*cases[i].length && out.stream==XR_SOURCE_STDOUT);
        for(unsigned copy=0;copy<outputs;++copy)
            CHECK(!memcmp(out.data+copy*cases[i].length,cases[i].bytes,cases[i].length));
        int64_t value=-1;CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==0);
        xray_source_result_free(result);
    }
    puts("public-only primitive Task14, repeated await, exact numeric/NUL/UTF8 bytes and producer death PASS");
}

static void task_constructed_protocol(void) {
    static const struct {const char *source,*bytes;size_t length;} cases[]={
        {"fn worker()->Array<i64> { Coro.yield(); return [10,20] }\n"
         "fn calculate() { const task:Task<Array<i64>> = go worker(); var first=await task; first[0]=99; assert(first[0]==99); const next=await task; print(next[0]); print(next[1]) }\ncalculate()\n",
         "10\n20\n",sizeof("10\n20\n")-1},
        {"fn worker()->Array<Array<string>> { Coro.yield(); return [[\"owned\",\"array\"],[\"typed\"]] }\n"
         "fn calculate() { const task:Task<Array<Array<string>>> = go worker(); var first=await task; first[0][0]=\"changed\"; assert(first[0][0]==\"changed\"); const next=await task; print(next[0][0]); print(next[0][1]); print(next[1][0]) }\ncalculate()\n",
         "owned\narray\ntyped\n",sizeof("owned\narray\ntyped\n")-1},
        {"fn worker()->i64? { Coro.yield(); return 42 }\n"
         "fn calculate() { const task:Task<i64?> = go worker(); const first=await task; assert(first! ==42); const next=await task; print(next!) }\ncalculate()\n",
         "42\n",sizeof("42\n")-1},
        {"fn worker()->Array<i64>? { Coro.yield(); return [10,20] }\n"
         "fn calculate() { const task:Task<Array<i64>?> = go worker(); const first=await task; assert(first![0]==10); const next=await task; print(next![1]) }\ncalculate()\n",
         "20\n",sizeof("20\n")-1},
        {"fn worker()->(i64,string) { Coro.yield(); return (42,\"a\\0中z\") }\n"
         "fn calculate() { const task:Task<(i64,string)> = go worker(); const first=await task; assert(first.0==42); const next=await task; print(next.0); print(next.1) }\ncalculate()\n",
         "42\na\0中z\n",sizeof("42\na\0中z\n")-1},
        {"enum Choice { Empty, Selected { values:Array<i64> } }\n"
         "fn worker()->Choice { Coro.yield(); return Choice.Selected{values:[10,20]} }\n"
         "fn calculate() { const task:Task<Choice> = go worker(); const first=await task; print(match(first){Choice.Empty -> 0, Choice.Selected{values} -> values[0]}); const next=await task; print(match(next){Choice.Empty -> 0, Choice.Selected{values} -> values[1]}) }\ncalculate()\n",
         "10\n20\n",sizeof("10\n20\n")-1},
        {"fn worker()->Atomic<i64> { Coro.yield(); return Atomic<i64>(42) }\n"
         "fn calculate() { const task:Task<Atomic<i64>> = go worker(); const first=await task; print(first.fetchAdd(2)); const next=await task; print(next.load()) }\ncalculate()\n",
         "42\n44\n",sizeof("42\n44\n")-1},
        {"fn leaf()->i64 { Coro.yield(); return 42 }\nfn worker()->Task<i64> { Coro.yield(); return go leaf() }\n"
         "fn calculate() { const task:Task<Task<i64>> = go worker(); const first=await task; const next=await task; print(await first); print(await next) }\ncalculate()\n",
         "42\n42\n",sizeof("42\n42\n")-1}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        char source[2048];size_t length=strlen(cases[i].source);CHECK(length<sizeof(source));
        memcpy(source,cases[i].source,length+1);
        XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,source);
        memset(source,'!',sizeof(source));xray_source_compiler_free(compiler);
        Bytes out={0};XrSourceCallOptions options=options_for(&out);XrSourceCall *call=NULL;
        CHECK(xray_source_call_begin(program,&options,&call)==XR_SOURCE_OK);xray_source_program_free(program);
        CHECK(drain_waiting(call).state==XR_SOURCE_RUN_RETURNED);
        XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK);
        CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK);
        CHECK(out.size==cases[i].length && out.stream==XR_SOURCE_STDOUT && !memcmp(out.data,cases[i].bytes,out.size));
        int64_t value=-1;CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==0);
        xray_source_result_free(result);
    }
    puts("public-only constructed Task8, Array copy isolation, shared Atomic, repeated await and exact owned bytes PASS");
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
int main(void) {compiler_layout_guards();detached_call();escaped_panic();cancellation_and_budget();timer_protocol();task_timer_protocol();task_scalar_protocol();task_constructed_protocol();reentry();located_diagnostic();puts("public header compiler/Program/Call/result ownership, exact waits and byte semantics PASS");return 0;}
