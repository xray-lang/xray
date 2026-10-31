/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef ARRAY_BINDING_RUNTIME_H
#define ARRAY_BINDING_RUNTIME_H
#include "xir/xxir_output.h"
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
typedef struct ArrayBindingOutput {char bytes[512];size_t count;} ArrayBindingOutput;
static XrXirOutputStatus array_binding_write(void *opaque,XrXirOutputStream stream,const char *bytes,size_t count) {
    ArrayBindingOutput *output=opaque;CHECK(stream==XR_XIR_STDOUT&&count<=sizeof(output->bytes)-output->count);
    memcpy(output->bytes+output->count,bytes,count);output->count+=count;return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceResult array_binding_drive(XrXirInstance *instance) {
    XrXirInstanceResult outcome;unsigned slices=0;
    do {CHECK(++slices<100000);outcome=xr_xir_instance_poll_bounded(instance,1024);}while(outcome.outcome.status==XR_XIR_CALL_READY);
    return outcome;
}
static void array_binding_run(XrXirProgram *program,const uint32_t ids[6],bool original) {
    ArrayBindingOutput output={0};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,array_binding_write,&output,sizeof(output.bytes)};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    if(original) {
        CHECK(xr_xir_instance_start(instance,ids[0],NULL,0)==XR_XIR_CALL_READY);
        CHECK(array_binding_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);xr_xir_value_drop(&value);
        CHECK(output.count==sizeof(array_variables_expected)-1&&!memcmp(output.bytes,array_variables_expected,output.count));
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);return;
    }
    for(unsigned repeat=0;repeat<2;++repeat) {
        CHECK(xr_xir_instance_start(instance,ids[0],NULL,0)==XR_XIR_CALL_READY);
        CHECK(array_binding_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64&&value.payload==42);xr_xir_value_drop(&value);
    }
    XrXirValue panic_message={0};
    for(unsigned i=1;i<=4;++i) {
        CHECK(xr_xir_instance_start(instance,ids[i],NULL,0)==XR_XIR_CALL_READY);
        XrXirCallResult outcome=array_binding_drive(instance).outcome;
        CHECK(outcome.status==XR_XIR_CALL_ASSERTION&&outcome.panic.detail.code==445);
        CHECK(output.count==0&&xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);
        if(i==1)CHECK(xr_xir_value_copy(&outcome.panic.message,&panic_message)==XR_XIR_VALUE_OK);
    }
    CHECK(xr_xir_instance_start(instance,ids[5],NULL,0)==XR_XIR_CALL_READY);
    CHECK(array_binding_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&value,&bytes,&length)&&length==3&&!memcmp(bytes,"A\0B",3));
    xr_xir_value_drop(&value);
    static const char expected[]="Array binding length must match its pattern";
    CHECK(xr_xir_string_view(&panic_message,&bytes,&length)&&length==sizeof(expected)-1&&!memcmp(bytes,expected,length));
    xr_xir_value_drop(&panic_message);
}
#endif
