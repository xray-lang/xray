/* Real rest values use the same VM and generated-C Program/Instance receiver. */
#ifndef XR_REST_RUNTIME_H
#define XR_REST_RUNTIME_H
#include "xir/xxir_program_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_output.h"
static XrXirCallEntry rest_entries[256];
static XrXirType rest_type,plain_type;
static uint32_t rest_entry,plain_entry,answer_entry;
static bool rest_probe;
static XrXirTypeArena *rest_arena;
static unsigned rest_runtime_refusals;
static XrXirAction rest_delegate(XrXirCallView *view) {
    uint32_t entry=xr_xir_call_current_entry(view->activation);CHECK(entry<256);
    if(rest_probe && entry==answer_entry && view->phase==XR_XIR_CALL_NORMAL && view->inbox.status==XR_XIR_CALL_READY) {
        rest_probe=false;
        XrXirValue value={0};
        CHECK(xr_xir_instance_function(view,rest_type,plain_entry,NULL,0,&value)==XR_XIR_CALL_BAD_ARGUMENT && !value.type);
        CHECK(xr_xir_instance_function(view,plain_type,rest_entry,NULL,0,&value)==XR_XIR_CALL_BAD_ARGUMENT && !value.type);
        rest_runtime_refusals+=2;
        const XrXirType types[]={rest_type,plain_type};const uint32_t entries[]={rest_entry,plain_entry};
        for(unsigned i=0;i<2;++i) {
            CHECK(xr_xir_instance_function(view,types[i],entries[i],NULL,0,&value)==XR_XIR_CALL_READY);
            const XrXirFunctionBinding *actual=xr_xir_function_binding(&value);CHECK(actual);
            XrXirFunctionBinding invalid=*actual;invalid.entry=entries[1-i];XrXirValue refused={0};
            XrXirValueAdmission *admission=xr_xir_call_admission(view);CHECK(admission&&admission->arena==rest_arena);
            CHECK(xr_xir_function_new(admission->domain,rest_arena,types[i],&invalid,admission,&refused)==XR_XIR_VALUE_BAD_ARGUMENT && !refused.type);
            CHECK(xr_xir_value_valid(&value));xr_xir_value_drop(&value);++rest_runtime_refusals;
        }
    }
    return rest_entries[entry].resume(view);
}
static XrXirValue rest_run(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult status=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    if(status.outcome.status!=XR_XIR_CALL_RETURNED) {
        const char *message=NULL;size_t length=0;
        xr_xir_string_view(&status.outcome.panic.message,&message,&length);
        fprintf(stderr,"rest runtime entry%u status%u %.*s\n",entry,status.outcome.status,(int)length,message?message:"");
    }
    CHECK(status.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void rest_execute(XrXirProgram *program,uint32_t entry,uint32_t answer) {
    rest_arena=program->arena;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    for(unsigned generation=0;generation<2;++generation) {
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        XrXirValue value=rest_run(instance,entry);xr_xir_value_drop(&value);
        rest_probe=answer_entry!=UINT32_MAX;value=rest_run(instance,answer);
        CHECK(value.type==XR_XIR_I64 && value.payload==42);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&value);
    }
    xr_xir_compile_program_drop(program);
}
#endif
