/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_host_execution.c - Physical ownership across host poll and destruction
 */
#include "execution/xr_xir_host_execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "execution/xr_xir_host_execution.c"
XR_DATA const XrXirProgramSpec panics_checked_program,panics_matrix_program;
XR_DATA const uint32_t panics_checked_functions[4],panics_matrix_functions[14];
static const XrCompileResourceLimits host_compile_limits={16777216,8388608,64000000};

static XrXirStatus host_seal(const XrXirProgramSpec *spec,const XrCompileResourceLimits *limits,
    XrCompileResourceStats *stats) {
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};XrXirProgram *program=NULL;
    XrCompileResourceStatus resource=xr_compile_resources_new(limits,&context.resources);
    XrXirStatus status=resource==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        resource==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    if(status==XR_XIR_OK)status=xr_xir_compile_program_seal(&context,spec,&program);
    if(context.resources)CHECK(xr_compile_resources_stats(context.resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context.resources);xr_xir_compile_program_drop(program);return status;
}
static void host_seal_faults(const XrXirProgramSpec *spec) {
    size_t live=runtime_live,bytes=runtime_bytes;runtime_attempts=0;
    XrCompileResourceStats exact={0};CHECK(host_seal(spec,&host_compile_limits,&exact)==XR_XIR_OK);
    size_t sites=runtime_attempts;CHECK(sites && runtime_live==live && runtime_bytes==bytes);
    for(size_t point=0;point<sites;++point) {
        runtime_attempts=0;runtime_fail_at=point;XrCompileResourceStats failed={0};
        CHECK(host_seal(spec,&host_compile_limits,&failed)==XR_XIR_OUT_OF_MEMORY && runtime_attempts>point);
        runtime_fail_at=SIZE_MAX;CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits={exact.allocated_bytes,exact.peak_bytes,exact.work};
        if(axis==0)limits.allocated_bytes-=minus;else if(axis==1)limits.live_bytes-=minus;else limits.work-=minus;
        XrCompileResourceStats actual={0};
        CHECK(host_seal(spec,&limits,&actual)==(minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    printf("Host compiler+Program seal %zu actual malloc OOM; finite16MiB/8MiB/64M; allocated=%llu peak=%llu work=%llu three axes exact/minus1 physical zero PASS\n",
        sites,(unsigned long long)exact.allocated_bytes,(unsigned long long)exact.peak_bytes,(unsigned long long)exact.work);
}

static void host_faults(XrXirProgram *program,uint32_t entry) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirHostExecutionRequest request={program,&config,entry,NULL,0};
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t point=0;point<=sites;++point) {
        XrXirCallResult result={0};runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirCallStatus status=xr_xir_host_execute(&request,&result);
        if (!point) {CHECK(status==XR_XIR_CALL_RETURNED && result.status==status);sites=runtime_attempts;}
        else CHECK(status==XR_XIR_CALL_OOM && runtime_attempts>runtime_fail_at);
        runtime_fail_at=SIZE_MAX;xr_xir_call_result_drop(&result);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    CHECK(sites);
    XrXirCallResult held={XR_XIR_CALL_RETURNED,{XR_XIR_I64,0,7},0,{0}},before=held;
    runtime_attempts=0;
    CHECK(xr_xir_host_execute(&request,&held)==XR_XIR_CALL_BAD_ARGUMENT && !runtime_attempts &&
        !memcmp(&held,&before,sizeof(held)));xr_xir_call_result_drop(&held);
    printf("Host entry=%u actual allocation sites=%zu; outcome and physical refunds PASS\n",entry,sites);
}
static XrXirCallResult host_outcome(XrXirProgram *program,uint32_t entry) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirHostExecutionRequest request={program,&config,entry,NULL,0};XrXirCallResult result={0};
    CHECK(xr_xir_host_execute(&request,&result)==XR_XIR_CALL_RETURNED && result.status==XR_XIR_CALL_RETURNED);
    return result;
}
int main(void) {
    const int64_t expected[]={445,445,445,445,445,0,11,11,420,420,123,3,19,445};
    for (uint32_t matrix=0;matrix<2;++matrix) {
        const XrXirProgramSpec *spec=matrix ? &panics_matrix_program : &panics_checked_program;
        const uint32_t *functions=matrix ? panics_matrix_functions : panics_checked_functions;
        host_seal_faults(spec);XrXirProgram *program=NULL;
        XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
        CHECK(xr_compile_resources_new(&host_compile_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_xir_compile_program_seal(&context,spec,&program)==XR_XIR_OK);
        xr_compile_resources_release(context.resources);
        for (uint32_t f=0;f<(matrix ? 14u : 4u);++f) {
            host_faults(program,functions[f]);XrXirCallResult result=host_outcome(program,functions[f]);
            if (matrix || f==0 || f==2) CHECK(result.value.type==XR_XIR_I64 &&
                result.value.payload==(matrix ? expected[f] : f==0 ? 7 : 445));
            else {
                const char *bytes=NULL;size_t length=0;const char *text=f==1 ? "unit" : "string";
                CHECK(xr_xir_string_view(&result.value,&bytes,&length) &&
                    length==strlen(text) && !memcmp(bytes,text,length));
            }
            xr_xir_call_result_drop(&result);
        }
        XrXirCallResult held=host_outcome(program,functions[matrix ? 12 : 1]);
        xr_xir_compile_program_drop(program);
        CHECK(xr_xir_call_result_valid(&held));
        if (!matrix) {
            const char *bytes=NULL;size_t length=0;
            CHECK(xr_xir_string_view(&held.value,&bytes,&length) && length==4 && !memcmp(bytes,"unit",4));
        } else CHECK(held.value.type==XR_XIR_I64 && held.value.payload==19);
        xr_xir_call_result_drop(&held);CHECK(!runtime_live && !runtime_bytes);
    }
    puts("Host owned outcomes after instance/program destruction; two suspension entries and physical baseline PASS");
    return 0;
}
