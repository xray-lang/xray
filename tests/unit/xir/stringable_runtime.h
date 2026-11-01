/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * stringable_runtime.h - Owned scalar witness execution
 *
 * KEY CONCEPT:
 *   A program and its output sink remain live through actual guest execution.
 */
#ifndef STRINGABLE_RUNTIME_H
#define STRINGABLE_RUNTIME_H
#include "xir/xxir_output.h"
typedef struct StringableOutput {char bytes[512];size_t count;} StringableOutput;
static XrXirOutputStatus stringable_write(void *data,XrXirOutputStream stream,const char *bytes,size_t count) {
    StringableOutput *out=data;CHECK(stream==XR_XIR_STDOUT && count<=sizeof(out->bytes)-out->count);
    memcpy(out->bytes+out->count,bytes,count);out->count+=count;return XR_XIR_OUTPUT_OK;
}
static inline void stringable_run(XrXirProgram *program,uint32_t entry,const char *expected) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    StringableOutput output={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,stringable_write,&output,sizeof(output.bytes)};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result;unsigned ticks=0;
    do {CHECK(++ticks<100000);result=xr_xir_instance_poll_bounded(instance,4096);}while(result.outcome.status==XR_XIR_CALL_READY);
    fprintf(stderr,"STRINGABLE_RUN entry%u status%u ticks%u\n",entry,result.outcome.status,ticks);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && !value.payload);xr_xir_value_drop(&value);
    CHECK(output.count==strlen(expected) && !memcmp(output.bytes,expected,output.count));
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
}
#endif // STRINGABLE_RUNTIME_H
