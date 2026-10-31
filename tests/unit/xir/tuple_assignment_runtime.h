/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * tuple_assignment_runtime.h - Actual VM and generated C share typed output and retirement checks
 */
#ifndef TUPLE_ASSIGNMENT_RUNTIME_H
#define TUPLE_ASSIGNMENT_RUNTIME_H
#include "xir/xxir_output.h"
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
typedef struct TupleOutput {char bytes[512];size_t count;} TupleOutput;
static XrXirOutputStatus tuple_write(void *data,XrXirOutputStream stream,const char *bytes,size_t count) {
    TupleOutput *out=data;CHECK(stream==XR_XIR_STDOUT && count<=sizeof(out->bytes)-out->count);
    memcpy(out->bytes+out->count,bytes,count);out->count+=count;return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceResult tuple_drive(XrXirInstance *instance) {
    XrXirInstanceResult result;unsigned ticks=0;
    do {CHECK(++ticks<100000);result=xr_xir_instance_poll_bounded(instance,1024);}while(result.outcome.status==XR_XIR_CALL_READY);
    return result;
}
static void tuple_run(XrXirProgram *program,const uint32_t ids[2],bool original) {
    TupleOutput output={0};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,tuple_write,&output,sizeof(output.bytes)};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue value={0};
    for(unsigned repeat=0;repeat<(original?1u:2u);++repeat) {
        CHECK(xr_xir_instance_start(instance,ids[0],NULL,0)==XR_XIR_CALL_READY);
        CHECK(tuple_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && value.payload==(original?0u:42u));xr_xir_value_drop(&value);
    }
    if(original)CHECK(output.count==sizeof(tuple_demo_expected)-1 && !memcmp(output.bytes,tuple_demo_expected,output.count));
    else {
        CHECK(output.count==0 && xr_xir_instance_start(instance,ids[1],NULL,0)==XR_XIR_CALL_READY);
        CHECK(tuple_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    if(!original) {
        const char *bytes=NULL;size_t count=0;CHECK(xr_xir_string_view(&value,&bytes,&count) && count==3 && !memcmp(bytes,"A\0B",3));
        xr_xir_value_drop(&value);
    }
}
#endif
