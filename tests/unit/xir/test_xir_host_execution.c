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
#include "execution/xr_xir_host_execution.c"
XR_DATA const XrXirProgramSpec panics_checked_program,panics_matrix_program;
XR_DATA const uint32_t panics_checked_functions[4],panics_matrix_functions[14];

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
        XrXirProgram *program=NULL;
        CHECK(xr_xir_program_seal(spec,(XrXirProgramBudget){16777216,64000000},&program)==XR_XIR_OK);
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
        xr_xir_program_drop(program);
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
