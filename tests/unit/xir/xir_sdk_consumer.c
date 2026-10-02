/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_consumer.c - Ordinary typed native entry with an owned host outcome
 */
#include "execution/xr_xir_host_execution.h"
#include "xir/xxir_output.h"
#include <stdio.h>
XR_DATA const XrXirProgramSpec original_source_program;
static XrXirOutputStatus sdk_consumer_bytes(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    (void)context;
    FILE *file=stream==XR_XIR_STDOUT ? stdout : stream==XR_XIR_STDERR ? stderr : NULL;
    if (!file) return XR_XIR_OUTPUT_BAD_ABI;
    return fwrite(bytes,1,length,file)==length && !fflush(file) ? XR_XIR_OUTPUT_OK : XR_XIR_OUTPUT_ERROR;
}
int main(void) {
    const XrXirProgramSpec *spec=&original_source_program;XrXirProgram *program=NULL;
    if (xr_xir_program_seal(spec,(XrXirProgramBudget){16777216,64000000},&program)!=XR_XIR_OK) return 1;
    XrXirInstanceConfig config;
    if (xr_xir_instance_config_init(&config,sizeof(config))!=XR_XIR_CALL_READY) {xr_xir_program_drop(program);return 2;}
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,sdk_consumer_bytes,NULL,16777216};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    config.poll_limit=64000000;
    XrXirCallResult result={0};
    XrXirHostExecutionRequest request={program,&config,spec->declarations->entry_function,NULL,0};
    XrXirCallStatus status=xr_xir_host_execute(&request,&result);
    xr_xir_program_drop(program);
    bool returned=status==XR_XIR_CALL_RETURNED && result.status==status &&
        result.value.type==XR_XIR_I64 && !result.value.reserved && !result.value.payload;
    xr_xir_call_result_drop(&result);return returned ? 0 : 3;
}
