/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * program_cases.c - Actual generated program ownership and host failures
 */
#include "execution/xr_xir_host_cli.h"
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(90); } } while (0)
#include "../xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_format.c"
#include "execution/xr_xir_host_execution.c"
static unsigned io_failure, mode_calls;
static bool policy_budget, config_failure;
static unsigned resource_calls;
static XrCompileResourceStatus observed_resources_new(const XrCompileResourceLimits *limits,
    XrCompileResources **output) {
    ++resource_calls;
    CHECK(limits->allocated_bytes==(UINT64_C(64)<<20) && limits->live_bytes==(UINT64_C(16)<<20) &&
        limits->work==(UINT64_C(1)<<30));
    XrCompileResourceLimits reduced=*limits;
    if(policy_budget)reduced.allocated_bytes=0;
    return xr_compile_resources_new(&reduced,output);
}
static int observed_setmode(int descriptor,int mode) {
    ++mode_calls;
    return (io_failure==1 && mode_calls==1) || (io_failure==2 && mode_calls==2) ?
        -1 : _setmode(descriptor,mode);
}
static size_t observed_write(const void *bytes,size_t size,size_t count,FILE *file) {
    return io_failure==3 ? 0 : fwrite(bytes,size,count,file);
}
static int observed_flush(FILE *file) {return io_failure==4 ? EOF : fflush(file);}
static XrXirCallStatus observed_config(XrXirInstanceConfig *config,size_t size) {
    return config_failure ? XR_XIR_CALL_BAD_ABI : xr_xir_instance_config_init(config,size);
}
#define xr_compile_resources_new observed_resources_new
#define xr_xir_instance_config_init observed_config
#define _setmode observed_setmode
#define fwrite observed_write
#define fflush observed_flush
#include "execution/xr_xir_host_cli.c"
#undef _setmode
#undef fwrite
#undef fflush
#undef xr_compile_resources_new
#undef xr_xir_instance_config_init
XR_DATA const XrXirProgramSpec host_fixture_program;
static XrXirProgram *owned_program(void) {
    const XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&host_fixture_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    return program;
}
static void program_owner(void) {
    uint32_t entry=host_fixture_program.declarations->entry_function;
    size_t attempts=runtime_attempts;mode_calls=0;resource_calls=0;
    CHECK(xr_xir_host_program_main(NULL,entry)==4 && runtime_attempts==attempts && !mode_calls);
    for(io_failure=1;io_failure<=4;++io_failure) {
        XrXirProgram *program=owned_program();attempts=runtime_attempts;mode_calls=0;
        CHECK(xr_xir_host_program_main(program,entry)==(io_failure<=2 ? 4 : 1));
        CHECK(!runtime_live && !runtime_bytes && !resource_calls);
        if(io_failure<=2)CHECK(runtime_attempts==attempts);
    }
    io_failure=0;config_failure=true;
    CHECK(xr_xir_host_program_main(owned_program(),entry)==4 && !runtime_live && !runtime_bytes);
    config_failure=false;
    CHECK(xr_xir_host_program_main(owned_program(),UINT32_MAX)==4 && !runtime_live && !runtime_bytes);
    XrXirProgram *program=owned_program();runtime_attempts=0;
    CHECK(xr_xir_host_program_main(program,entry)==0 && !runtime_live && !runtime_bytes);
    size_t sites=runtime_attempts;
    for(size_t i=0;i<sites;++i) {
        program=owned_program();runtime_attempts=0;runtime_fail_at=i;
        CHECK(xr_xir_host_program_main(program,entry)==4 && runtime_attempts>i);
        runtime_fail_at=SIZE_MAX;
        CHECK(!runtime_live && !runtime_bytes && !resource_calls);
    }
    printf("Owned Program: null/streams/config/entry and %zu OOM points consume with no new ledger, physical zero PASS\n",sites);
}
typedef struct Trace {uint32_t begins,ready,published,released,stack[32],outputs;} Trace;
static XrXirOutputStatus trace_output(void *context,const XrXirOutputGroup *group) {
    Trace *trace=context;
    CHECK(group && group->stream==XR_XIR_STDOUT);++trace->outputs;return XR_XIR_OUTPUT_OK;
}
static void trace_event(void *context,XrXirLifecycleEvent event,uint32_t index) {
    Trace *trace=context;
    if(event==XR_XIR_MODULE_BEGIN)++trace->begins;
    else if(event==XR_XIR_MODULE_READY)++trace->ready;
    else if(event==XR_XIR_SLOT_PUBLISHED){CHECK(trace->published<32);trace->stack[trace->published++]=index;}
    else {CHECK(event==XR_XIR_SLOT_RELEASED && trace->released<trace->published);
        CHECK(index==trace->stack[trace->published-1-trace->released++]);}
}
static void initialization(void) {
    const XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&host_fixture_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    XrXirInstance *instances[2]={0};XrXirCallResult owned[2]={0};Trace traces[2]={0};
    uint32_t entry=host_fixture_program.declarations->entry_function;
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,trace_output,&traces[i]};
        config.trace=trace_event;config.trace_context=&traces[i];
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for(unsigned i=0;i<2;++i) {
        XrXirInstanceResult result=xr_xir_instance_poll(instances[i]);
        CHECK(result.outcome.status==XR_XIR_CALL_SUSPENDED);
        CHECK(traces[i].begins==2 && traces[i].ready==1 && traces[i].outputs==2);
        CHECK(xr_xir_instance_resume(instances[i],result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);
        result=xr_xir_instance_poll(instances[i]);CHECK(result.outcome.status==XR_XIR_CALL_THROWN);
        CHECK(traces[i].published==3 && traces[i].released==3);
        for(unsigned attempt=0;attempt<3;++attempt)
            CHECK(xr_xir_instance_start(instances[i],entry,NULL,0)==XR_XIR_CALL_THROWN && traces[i].outputs==2);
        CHECK(xr_xir_instance_copy_failure(instances[i],&owned[i])==XR_XIR_CALL_THROWN);
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    }
    for(unsigned i=0;i<2;++i) {
        CHECK(xr_xir_host_report_result(&owned[i],(XrValueFormatSink){stderr,xr_value_format_file_write},false)==XR_XIR_HOST_REPORT_OK);
        xr_xir_call_result_drop(&owned[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
    puts("Two sticky failed instances, reverse cleanup, dead Program and first rendering PASS");
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    if(!strcmp(argv[1],"init")){initialization();return 0;}
    if(!strcmp(argv[1],"owner")){program_owner();return 0;}
    if(!strcmp(argv[1],"oom")) {
        runtime_attempts=0;CHECK(xr_xir_host_main(&host_fixture_program)==0);
        size_t sites=runtime_attempts;CHECK(sites && !runtime_live && !runtime_bytes);
        for(size_t i=0;i<sites;++i) {
            runtime_attempts=0;runtime_fail_at=i;
            int result=xr_xir_host_main(&host_fixture_program);
            CHECK(result==4 && runtime_attempts>i && !runtime_live && !runtime_bytes);
        }
        runtime_fail_at=SIZE_MAX;
        printf("Actual main: %zu OOM points, physical zero PASS\n",sites);return 0;
    }
    if(!strcmp(argv[1],"io")) {
        for(io_failure=1;io_failure<=4;++io_failure) {
            mode_calls=0;
            CHECK(xr_xir_host_main(&host_fixture_program)==(io_failure<=2 ? 4 : 1));
            CHECK(!runtime_live && !runtime_bytes);
        }
        io_failure=0;policy_budget=true;
        CHECK(xr_xir_host_main(&host_fixture_program)==1 && !runtime_live && !runtime_bytes);
        policy_budget=false;
        puts("Actual main: both setmode/write/flush and admission budget failures, physical zero PASS");return 0;
    }
    CHECK(!strcmp(argv[1],"main"));
    int result=xr_xir_host_main(&host_fixture_program);
    CHECK(!runtime_live && !runtime_bytes);return result;
}
