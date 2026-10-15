/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_public_faults.c - Complete public compiler and runtime allocation cuts
 */
#include "xray_source_entry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "execution/xr_xir_host_execution.c"
#include "api/xsource_api.c"
static XrSourceWriteStatus discard(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    (void)context;CHECK(stream==XR_SOURCE_STDOUT || stream==XR_SOURCE_STDERR);CHECK(bytes || !length);return XR_SOURCE_WRITE_OK;
}
static XrSourceStatus drive(unsigned mode,XrSourceCompiler **compiler,XrSourceProgram **program,
    XrSourceDiagnostic **diagnostic,XrSourceCall **call,XrSourceResult **result) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=UINT64_C(64)<<20;options.live_bytes=UINT64_C(8)<<20;options.work=UINT64_C(128000000);
    XrSourceStatus status=xray_source_compiler_new(&options,compiler);if(status!=XR_SOURCE_OK)return status;
    const char *source=mode==6?"export fn answer()->Array<Array<string>>{return [[\"owned\\0中\"],[\"next\"]]}\n":
        mode==5?"export fn answer()->string{return \"owned\\0中value\"}\n":
        mode==0?"print(42)\n":mode==1?"assert(false, \"a\\0中z\")\n":
        mode==2?"fn worker() -> i64 { return 42 }\nfn main() -> i64 { return await go worker() }\nprint(main())\n":
        mode==3?"fn worker()->f32 { Coro.yield(); return 1.5 }\n"
        "fn calculate()->f32 { const task:Task<f32> = go worker(); const first=await task; print(first); return await task }\nprint(calculate())\n":
        "fn worker()->Array<i64> { Coro.yield(); return [10,20] }\n"
        "fn calculate() { const task:Task<Array<i64>> = go worker(); var first=await task; first[0]=99; assert(first[0]==99); const next=await task; assert(next[0]==10); print(next[1]) }\ncalculate()\n";
    XrSourceText text={NULL,"public-faults",source,strlen(source)};
    status=xray_source_compile_text(*compiler,&text,program,diagnostic);if(status!=XR_SOURCE_OK)return status;
    xray_source_compiler_free(*compiler);*compiler=NULL;
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);runtime.write=discard;
    if(mode==5 || mode==6) {
        XrSourceResult *original=NULL;
        status=xray_source_execute_named(*program,"answer",6,&runtime,&original,NULL);
        xray_source_program_free(*program);*program=NULL;
        if(status==XR_SOURCE_OK)status=xray_source_result_copy(original,result);
        if(status==XR_SOURCE_OK && mode==6 && (*result)->value.status==XR_XIR_CALL_RETURNED) {
            size_t attempts=runtime_attempts,live=runtime_live,physical=runtime_bytes;
            XrSourceStatus formatted=xray_source_result_format(*result,discard,NULL);
            CHECK(formatted==XR_SOURCE_OK && runtime_attempts==attempts && runtime_live==live && runtime_bytes==physical);
        }
        xray_source_result_free(original);return status;
    }
    if(mode!=1)return xray_source_execute(*program,&runtime,result,NULL);
    status=xray_source_call_begin(*program,&runtime,call);if(status!=XR_SOURCE_OK)return status;
    xray_source_program_free(*program);*program=NULL;
    XrSourceEvent event;size_t steps=0;
    do {CHECK(++steps<4096);status=xray_source_call_poll(*call,8,&event);if(status!=XR_SOURCE_OK)return status;}
    while(event.state==XR_SOURCE_RUN_READY);
    return xray_source_call_take(*call,result);
}
static void allocation_faults(unsigned begin,unsigned end,bool probe) {
    for(unsigned mode=begin;mode<end;++mode) {
        size_t count=0;
        for(size_t point=0;!point || (!probe && point<=count);++point) {
            runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;
            XrSourceCompiler *compiler=NULL;XrSourceProgram *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
            XrSourceCall *call=NULL;XrSourceResult *result=NULL;
            XrSourceStatus status=drive(mode,&compiler,&program,&diagnostic,&call,&result);
            if(!point){CHECK(status==XR_SOURCE_OK && result);count=runtime_attempts;CHECK(count);}
            else CHECK(runtime_attempts>runtime_fail_at);
            if(result) {
                XrSourceEvent event;CHECK(status==XR_SOURCE_OK && xray_source_result_event(result,&event)==XR_SOURCE_OK);
                CHECK(event.state==(point?XR_SOURCE_RUN_OUT_OF_MEMORY:mode==1?XR_SOURCE_RUN_PANICKED:XR_SOURCE_RUN_RETURNED));
                if(mode==5 && !point) {
                    const char *bytes=NULL;size_t length=0;static const char expected[]="owned\0中value";
                    CHECK(xray_source_result_bytes(result,&bytes,&length)==XR_SOURCE_OK &&
                        length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
                }
            } else {CHECK(point && status==XR_SOURCE_OUT_OF_MEMORY);}
            runtime_fail_at=SIZE_MAX;
            if(call && !result) {
                /* A failed facade allocation or result copy cannot consume the
                 * real terminal owner. Retry without injecting a second fault. */
                XrSourceEvent event;CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);
                if(event.state!=XR_SOURCE_RUN_READY && event.state!=XR_SOURCE_RUN_SUSPENDED)
                    CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
            }
            xray_source_compiler_free(compiler);xray_source_program_free(program);
            XrSourceEvent cleanup;CHECK(xray_source_call_drop(&call,&cleanup)==XR_SOURCE_OK && !call);
            xray_source_result_free(result);xray_source_diagnostic_free(diagnostic);
            CHECK(!runtime_live && !runtime_bytes);
        }
        if(mode>=5)printf("public API mode=%u observed=%zu swept=%zu physical=0/0\n",mode,count,probe?0:count);
        else printf("public API mode=%u all compiler/runtime allocations=%zu physical=0/0\n",mode,count);
    }
}
static void named_copy_guards(void) {
    runtime_fail_at=SIZE_MAX;
    XrSourceCompiler *compiler=NULL;XrSourceProgram *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
    XrSourceCall *call=NULL;XrSourceResult *result=NULL,*copy=NULL;
    CHECK(drive(5,&compiler,&program,&diagnostic,&call,&result)==XR_SOURCE_OK && result);
    CHECK(!compiler && !program && !diagnostic && !call);
    const char *bytes=NULL;size_t length=0;static const char expected[]="owned\0中value";
    CHECK(xray_source_result_bytes(result,&bytes,&length)==XR_SOURCE_OK &&
        length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    size_t live=runtime_live,physical=runtime_bytes;
    runtime_attempts=0;runtime_fail_at=0;
    CHECK(xray_source_result_copy(result,&copy)==XR_SOURCE_OUT_OF_MEMORY && !copy && runtime_attempts==1);
    CHECK(runtime_live==live && runtime_bytes==physical);
    CHECK(xray_source_result_bytes(result,&bytes,&length)==XR_SOURCE_OK &&
        length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    runtime_fail_at=SIZE_MAX;
    CHECK(xray_source_result_copy(result,&copy)==XR_SOURCE_OK && copy);
    xray_source_result_free(result);result=NULL;
    CHECK(xray_source_result_bytes(copy,&bytes,&length)==XR_SOURCE_OK &&
        length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    runtime_attempts=0;XrSourceResult *occupied=copy;
    CHECK(xray_source_result_copy(copy,&occupied)==XR_SOURCE_BAD_ARGUMENT && occupied==copy && !runtime_attempts);
    CHECK(xray_source_result_copy(NULL,&result)==XR_SOURCE_BAD_ARGUMENT && !result && !runtime_attempts);
    xray_source_result_free(copy);CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && (!strcmp(argv[1],"--scalar-task") || !strcmp(argv[1],"--constructed-task") ||
        !strcmp(argv[1],"--named") || !strcmp(argv[1],"--named-probe") || !strcmp(argv[1],"--named-copy-guards") ||
        !strcmp(argv[1],"--array-format") || !strcmp(argv[1],"--array-probe"))));
    if(argc==1)allocation_faults(0,3,false);
    else if(!strcmp(argv[1],"--scalar-task"))allocation_faults(3,4,false);
    else if(!strcmp(argv[1],"--constructed-task"))allocation_faults(4,5,false);
    else if(!strcmp(argv[1],"--named-copy-guards"))named_copy_guards();
    else if(!strcmp(argv[1],"--array-format") || !strcmp(argv[1],"--array-probe"))allocation_faults(6,7,!strcmp(argv[1],"--array-probe"));
    else allocation_faults(5,6,!strcmp(argv[1],"--named-probe"));
    return 0;
}
