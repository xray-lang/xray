/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * The exact same checked range program executes in VM and generated C11. */
#ifndef MUTSLICE_INDEX_RUNTIME_H
#define MUTSLICE_INDEX_RUNTIME_H
#include "xir/xxir_output.h"
#include "xir_source_compile_owner.h"
#define xr_xir_call_view_operation mutslice_real_view_operation
#include "xir_runtime_allocations.h"
#undef xr_xir_call_view_operation
static unsigned mutslice_creations;
static bool mutslice_fail_creation;
XR_FUNC XrXirValueStatus xr_xir_call_view_operation(XrXirCallView *view,
    const XrXirCallViewOperation *request,XrXirValue *output,XrXirFaultDetail *fault) {
    bool injected=mutslice_fail_creation&&request->operation==XR_XIR_VIEW_NEW;
    if(injected){mutslice_fail_creation=false;runtime_fail_at=runtime_attempts;}
    XrXirValueStatus status=mutslice_real_view_operation(view,request,output,fault);
    if(injected){runtime_fail_at=SIZE_MAX;CHECK(status==XR_XIR_VALUE_OOM&&!output->type&&!output->reserved&&!output->payload);}
    if(status==XR_XIR_VALUE_OK&&request->operation==XR_XIR_VIEW_NEW)++mutslice_creations;
    return status;
}
typedef struct MutsliceOutput{char bytes[256];size_t count;}MutsliceOutput;
static XrXirOutputStatus mutslice_write(void *data,XrXirOutputStream stream,const char *bytes,size_t count) {
    MutsliceOutput *out=data;CHECK(stream==XR_XIR_STDOUT&&count<=sizeof(out->bytes)-out->count);
    memcpy(out->bytes+out->count,bytes,count);out->count+=count;return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceResult mutslice_drive(XrXirInstance *instance) {
    XrXirInstanceResult result;unsigned ticks=0;
    do{CHECK(++ticks<100000);result=xr_xir_instance_poll_bounded(instance,1024);}
    while(result.outcome.status==XR_XIR_CALL_READY);return result;
}
static void mutslice_faults(XrXirProgram *program,const uint32_t *ids) {
    for(unsigned which=0;which<3;++which) {
        size_t live=runtime_live,bytes=runtime_bytes;XrXirInstanceConfig config;XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        if(which==2)mutslice_fail_creation=true;
        CHECK(xr_xir_instance_start(instance,ids[which==1?1:2],NULL,0)==XR_XIR_CALL_READY);
        unsigned before=mutslice_creations,steps=0;XrXirCallStatus status=XR_XIR_CALL_READY;
        if(which==0) {
            do{CHECK(++steps<100000);status=xr_xir_instance_poll_bounded(instance,1).outcome.status;}
            while(status==XR_XIR_CALL_READY&&mutslice_creations==before);
            CHECK(status==XR_XIR_CALL_READY&&mutslice_creations>before);
            CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
        }
        status=mutslice_drive(instance).outcome.status;runtime_fail_at=SIZE_MAX;
        CHECK(status==(which==0?XR_XIR_CALL_CANCELLED:which==1?XR_XIR_CALL_BOUNDS:XR_XIR_CALL_OOM));
        XrXirCallStatus closed=xr_xir_instance_free(instance);
        CHECK(closed==XR_XIR_CALL_READY||closed==status);
        CHECK(runtime_live==live&&runtime_bytes==bytes);
    }
}
static void mutslice_run(XrXirProgram *program,const uint32_t *ids) {
    mutslice_faults(program,ids);MutsliceOutput output={0};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,mutslice_write,&output,sizeof(output.bytes)};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue value={0};
    CHECK(xr_xir_instance_start(instance,ids[0],NULL,0)==XR_XIR_CALL_READY);
    CHECK(mutslice_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64&&value.payload==42);xr_xir_value_drop(&value);
    CHECK(output.count==sizeof(mutslice_index_expected)-1&&!memcmp(output.bytes,mutslice_index_expected,output.count));
    CHECK(xr_xir_instance_start(instance,ids[3],NULL,0)==XR_XIR_CALL_READY);
    CHECK(mutslice_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    XrXirValueAdmission admission={.arena=xr_xir_value_arena(&value),.work=10000,.scratch_bytes=65536};
    XrXirValue first={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(&value,0,&admission,&first,&fault)==XR_XIR_VALUE_OK&&first.type==XR_XIR_I64&&first.payload==42);
    xr_xir_value_drop(&first);xr_xir_value_drop(&value);CHECK(!runtime_live&&!runtime_bytes);
}
#endif
