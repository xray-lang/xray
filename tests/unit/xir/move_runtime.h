/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * move_runtime.h - Actual consuming calls and physical owner retirement
 *
 * KEY CONCEPT:
 *   Both backends execute the same cold checked functions and restore the
 *   actual Program frame after each Instance, without a synthetic owner.
 */
#ifndef MOVE_RUNTIME_H
#define MOVE_RUNTIME_H
#include "xir/xxir_output.h"
#include "xir_runtime_allocations.h"
typedef struct MoveTestOutput {char bytes[256];size_t count;} MoveTestOutput;
static XrXirOutputStatus move_test_write(void *data,XrXirOutputStream stream,const char *bytes,size_t count) {
    MoveTestOutput *out=data;CHECK(stream==XR_XIR_STDOUT&&count<=sizeof(out->bytes)-out->count);
    memcpy(out->bytes+out->count,bytes,count);out->count+=count;return XR_XIR_OUTPUT_OK;
}
static void move_test_run(XrXirProgram *program,uint32_t entry,const XrXirCompileContext *context) {
    size_t blocks=runtime_live,bytes=runtime_bytes;XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    MoveTestOutput output={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,move_test_write,&output,sizeof(output.bytes)};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result;unsigned ticks=0;
    do {CHECK(++ticks<100000);result=xr_xir_instance_poll_bounded(instance,4096);}while(result.outcome.status==XR_XIR_CALL_READY);
    fprintf(stderr,"MOVE_RUN entry%u status%u ticks%u\n",entry,result.outcome.status,ticks);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64&&!value.payload);xr_xir_value_drop(&value);
    static const char expected[]="42 2 42 42 42 42 20\n";
    CHECK(output.count==sizeof(expected)-1&&!memcmp(output.bytes,expected,output.count));
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==blocks&&runtime_bytes==bytes);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    xr_xir_compile_program_drop(program);
}
#endif /* MOVE_RUNTIME_H */
