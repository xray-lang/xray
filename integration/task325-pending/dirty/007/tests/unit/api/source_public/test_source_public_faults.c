/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_public_faults.c - Complete public compiler and runtime allocation cuts
 */
#include "xray_source.h"
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
    const char *source=mode==0?"print(42)\n":mode==1?"assert(false, \"a\\0中z\")\n":
        "fn worker() -> i64 { return 42 }\nfn main() -> i64 { return await go worker() }\nprint(main())\n";
    XrSourceText text={NULL,"public-faults",source,strlen(source)};
    status=xray_source_compile_text(*compiler,&text,program,diagnostic);if(status!=XR_SOURCE_OK)return status;
    xray_source_compiler_free(*compiler);*compiler=NULL;
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);runtime.write=discard;
    if(mode!=1)return xray_source_execute(*program,&runtime,result,NULL);
    status=xray_source_call_begin(*program,&runtime,call);if(status!=XR_SOURCE_OK)return status;
    xray_source_program_free(*program);*program=NULL;
    XrSourceEvent event;size_t steps=0;
    do {CHECK(++steps<4096);status=xray_source_call_poll(*call,8,&event);if(status!=XR_SOURCE_OK)return status;}
    while(event.state==XR_SOURCE_RUN_READY);
    return xray_source_call_take(*call,result);
}
static void allocation_faults(void) {
    for(unsigned mode=0;mode<3;++mode) {
        size_t count=0;
        for(size_t point=0;!point || point<=count;++point) {
            runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;
            XrSourceCompiler *compiler=NULL;XrSourceProgram *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
            XrSourceCall *call=NULL;XrSourceResult *result=NULL;
            XrSourceStatus status=drive(mode,&compiler,&program,&diagnostic,&call,&result);
            if(!point){CHECK(status==XR_SOURCE_OK && result);count=runtime_attempts;CHECK(count);}
            else CHECK(runtime_attempts>runtime_fail_at);
            if(result) {
                XrSourceEvent event;CHECK(status==XR_SOURCE_OK && xray_source_result_event(result,&event)==XR_SOURCE_OK);
                CHECK(event.state==(point?XR_SOURCE_RUN_OUT_OF_MEMORY:mode==1?XR_SOURCE_RUN_PANICKED:XR_SOURCE_RUN_RETURNED));
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
        printf("public API mode=%u all compiler/runtime allocations=%zu physical=0/0\n",mode,count);
    }
}
int main(void){allocation_faults();return 0;}
