/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * array_materialization_runtime.h - Actual VM and generated C share typed output and retirement checks
 */
#ifndef ARRAY_MATERIALIZATION_RUNTIME_H
#define ARRAY_MATERIALIZATION_RUNTIME_H
#include "xir/xxir_output.h"
#include "xir_source_compile_owner.h"
#define xr_xir_call_view_operation array_real_view_operation
#include "xir_runtime_allocations.h"
#undef xr_xir_call_view_operation
static unsigned array_raw_view_checks;
/* The original operation always runs unchanged. These bounded invalid requests
 * use the same genuine active root and actual empty frame output, never forged
 * authority. The low-level range checker remains strict beneath Source clamp. */
XR_FUNC XrXirValueStatus xr_xir_call_view_operation(XrXirCallView *view,
    const XrXirCallViewOperation *request,XrXirValue *output,XrXirFaultDetail *fault) {
    if (!array_raw_view_checks && request && request->operation==XR_XIR_VIEW_NEW && runtime_fail_at==SIZE_MAX) {
        XrXirFaultDetail detail={0};XrXirCallViewOperation bad=*request;
        bad.start=-1;
        CHECK(array_real_view_operation(view,&bad,output,&detail)==XR_XIR_VALUE_BOUNDS && !output->type && !output->reserved && !output->payload && detail.code==XR_XIR_PANIC_BOUNDS && detail.index==-1);
        int64_t length=detail.length;bad.start=0;bad.end=INT64_MAX;bad.to_end=false;
        CHECK(array_real_view_operation(view,&bad,output,&detail)==XR_XIR_VALUE_BOUNDS && !output->type && !output->reserved && !output->payload && detail.length==length);
        bad.start=length+1;bad.end=0;
        CHECK(array_real_view_operation(view,&bad,output,&detail)==XR_XIR_VALUE_BOUNDS && !output->type && !output->reserved && !output->payload);
        XrXirValue absent={0};bad=*request;bad.root=&absent;
        CHECK(array_real_view_operation(view,&bad,output,&detail)==XR_XIR_VALUE_BAD_ARGUMENT && !output->type && !output->reserved && !output->payload);
        bad=*request;bad.type=XR_XIR_BOOL;
        CHECK(array_real_view_operation(view,&bad,output,&detail)==XR_XIR_VALUE_BAD_ARGUMENT && !output->type && !output->reserved && !output->payload);
        array_raw_view_checks=5;
    }
    return array_real_view_operation(view,request,output,fault);
}
typedef struct ArrayOutput {char bytes[512];size_t count;} ArrayOutput;
static XrXirOutputStatus array_write(void *data,XrXirOutputStream stream,const char *bytes,size_t count) {
    ArrayOutput *out=data;CHECK(stream==XR_XIR_STDOUT && count<=sizeof(out->bytes)-out->count);
    memcpy(out->bytes+out->count,bytes,count);out->count+=count;return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceResult array_drive(XrXirInstance *instance) {
    XrXirInstanceResult result;unsigned ticks=0;
    do {CHECK(++ticks<100000);result=xr_xir_instance_poll_bounded(instance,1024);}while(result.outcome.status==XR_XIR_CALL_READY);
    return result;
}
/* Three bounded physical failures exercise start, body and later payload work.
 * The caller keeps the genuine Program; every failed instance retires to it. */
static void array_memory_faults(XrXirProgram *program,const uint32_t *ids,const XrXirCompileContext *context) {
    const size_t points[]={0,8,16};
    for(unsigned i=0;i<sizeof(points)/sizeof(*points);++i) {
        size_t live=runtime_live,bytes=runtime_bytes;XrXirInstanceConfig config;
        XrCompileResourceStats before,after;
        CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,ids[0],NULL,0)==XR_XIR_CALL_READY);
        CHECK(array_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);xr_xir_value_drop(&value);
        runtime_fail_at=runtime_attempts+points[i];
        XrXirCallStatus status=xr_xir_instance_start(instance,ids[0],NULL,0);
        if(status==XR_XIR_CALL_READY)status=array_drive(instance).outcome.status;
        CHECK(status==XR_XIR_CALL_OOM && runtime_attempts>runtime_fail_at);runtime_fail_at=SIZE_MAX;
        XrXirValue rejected={0};XrXirCallStatus taken=xr_xir_instance_take_result(instance,&rejected);
        CHECK((taken==XR_XIR_CALL_OOM || taken==XR_XIR_CALL_BAD_STATE) && !rejected.type && !rejected.reserved && !rejected.payload);
        /* The injection is disabled before retirement. Executor shutdown may
         * report this same sticky OOM; no other cleanup failure is accepted. */
        XrXirCallStatus closed=xr_xir_instance_free(instance);
        fprintf(stderr,"Array OOM point%zu body%u take%u close%u\n",points[i],status,taken,closed);
        CHECK(closed==XR_XIR_CALL_READY || closed==XR_XIR_CALL_OOM);
        CHECK(runtime_live==live&&runtime_bytes==bytes);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    }
}
static void array_bounds_and_cancel(XrXirProgram *program,const uint32_t *ids,const XrXirCompileContext *context) {
    for(unsigned i=2;i<=ARRAY_MATERIALIZATION_ENTRY_COUNT;++i) {
        size_t live=runtime_live,bytes=runtime_bytes;XrCompileResourceStats before,after;
        CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,ids[i<ARRAY_MATERIALIZATION_ENTRY_COUNT?i:0],NULL,0)==XR_XIR_CALL_READY);
        if(i==ARRAY_MATERIALIZATION_ENTRY_COUNT) {
            CHECK(xr_xir_instance_poll_bounded(instance,1).outcome.status==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
        }
        XrXirCallResult result=array_drive(instance).outcome;
        if(i==5) CHECK(result.status==XR_XIR_CALL_BOUNDS && result.panic.detail.code==XR_XIR_PANIC_BOUNDS &&
            result.panic.detail.index==-1 && result.panic.detail.length==1);
        else if(i<ARRAY_MATERIALIZATION_ENTRY_COUNT) {
            CHECK(result.status==XR_XIR_CALL_RETURNED);
            XrXirValue length={0};CHECK(xr_xir_instance_take_result(instance,&length)==XR_XIR_CALL_RETURNED);
            CHECK(length.type==XR_XIR_I64 && length.payload==(i==3?0:1));xr_xir_value_drop(&length);
        } else CHECK(result.status==XR_XIR_CALL_CANCELLED);
        XrXirCallStatus closed=xr_xir_instance_free(instance);
        CHECK(closed==XR_XIR_CALL_READY || (i==5 && closed==XR_XIR_CALL_BOUNDS));
        CHECK(runtime_live==live && runtime_bytes==bytes);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    }
}
static void array_run(XrXirProgram *program,const uint32_t ids[ARRAY_MATERIALIZATION_ENTRY_COUNT],bool original,const XrXirCompileContext *context) {
    if(!original){array_memory_faults(program,ids,context);array_bounds_and_cancel(program,ids,context);}
    ArrayOutput output={0};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,array_write,&output,sizeof(output.bytes)};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue value={0};
    for(unsigned repeat=0;repeat<(original?1u:2u);++repeat) {
        CHECK(xr_xir_instance_start(instance,ids[0],NULL,0)==XR_XIR_CALL_READY);
        CHECK(array_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && value.payload==(original?0u:42u));xr_xir_value_drop(&value);
    }
    if(original)CHECK(output.count==sizeof(array_demo_expected)-1 && !memcmp(output.bytes,array_demo_expected,output.count));
    else {
        CHECK(output.count==0);
        CHECK(xr_xir_instance_start(instance,ids[1],NULL,0)==XR_XIR_CALL_READY);
        CHECK(array_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    if(!original) {
        const char *bytes=NULL;size_t count=0;CHECK(xr_xir_string_view(&value,&bytes,&count) && count==3 && !memcmp(bytes,"A\0B",3));
        xr_xir_value_drop(&value);
    }
}
#endif
