/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_callable_argument_admission.h - Real active-view conversion and exact host boundaries
 */
#ifndef XIR_CALLABLE_ARGUMENT_ADMISSION_H
#define XIR_CALLABLE_ARGUMENT_ADMISSION_H
#include "xir/xxir_types.h"
static XrXirCallEntry callable_admission_selected[128];
static const XrXirTypes *callable_admission_types;
static XrXirValue callable_admission_captured;
static bool callable_admission_record;
static XrXirAction callable_admission_delegate(XrXirCallView *view) {
    uint32_t entry=xr_xir_call_current_entry(view->activation);CHECK(entry<128);
    XrXirAction action=callable_admission_selected[entry].resume(view);
    if(callable_admission_record && action.kind==XR_XIR_ACTION_CALL) for(uint32_t p=0;p<action.argument_count;++p) {
        const XrXirValue *value=&action.arguments[p];
        if(!xr_xir_callable_signature(callable_admission_types,(XrXirType)value->type)) continue;
        CHECK(!callable_admission_captured.type && xr_xir_value_copy(value,&callable_admission_captured)==XR_XIR_VALUE_OK);
    }
    return action;
}
static void callable_admission_entries(XrXirCallEntry *entries,uint32_t count,const XrXirTypes *types) {
    CHECK(count<=128);callable_admission_types=types;
    for(uint32_t f=0;f<count;++f) {callable_admission_selected[f]=entries[f];entries[f].resume=callable_admission_delegate;}
}
static XrXirValue callable_admission_argument;
static uint32_t callable_admission_target;
static unsigned callable_admission_actions;
static XrXirAction callable_admission_resume(XrXirCallView *view) {
    if(view->phase==XR_XIR_CALL_EXIT) return (XrXirAction){.kind=XR_XIR_ACTION_RETURN};
    if(view->inbox.status==XR_XIR_CALL_RETURNED)
        return (XrXirAction){.kind=XR_XIR_ACTION_RETURN,.value=view->inbox.value};
    CHECK(view->inbox.status==XR_XIR_CALL_READY);++callable_admission_actions;
    return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=callable_admission_target,
        .arguments=&callable_admission_argument,.argument_count=1};
}
static XrXirInstance *callable_admission_instance(XrXirProgram *program) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);return instance;
}
static XrXirValue callable_admission_value(XrXirInstance *instance,uint32_t entry) {
    CHECK(!callable_admission_captured.type);callable_admission_record=true;
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirValue result=callable_argument_result(instance);CHECK(result.type==XR_XIR_I64 && !result.payload);
    xr_xir_value_drop(&result);callable_admission_record=false;
    CHECK(callable_admission_captured.type);result=callable_admission_captured;callable_admission_captured=(XrXirValue){0};return result;
}
static void callable_admission_reject(XrXirProgram *program,const uint32_t ids[10],unsigned mode,const XrXirValue *external) {
    XrXirInstance *instance=callable_admission_instance(program);XrXirValue owned={0};
    if(mode<3) owned=callable_admission_value(instance,ids[mode==0 ? 6 : mode==1 ? 7 : 5]);
    callable_admission_argument=external ? *external : owned;
    if(mode==2) callable_admission_argument.reserved=1;
    CHECK(xr_xir_instance_start(instance,ids[9],NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    CHECK(result.outcome.status==XR_XIR_CALL_BAD_ARGUMENT && !result.outcome.wake && !result.outcome.value.type);
    CHECK(xr_xir_call_result_valid(&result.outcome));
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_BAD_ARGUMENT);xr_xir_value_drop(&owned);
}
static XrXirAction callable_opaque_resume(XrXirCallView *view) {
    CHECK(xr_xir_call_current_entry(view->activation)==0);
    static const XrXirValue argument={XR_XIR_I64,0,7};
    return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=1,.arguments=&argument,.argument_count=1};
}
static void callable_opaque_cases(void) {
    const XrXirType parameter=XR_XIR_BOOL;uint64_t marker=UINT64_C(0x123456789abcdef0);
    XrXirCallEntry entries[]={
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,0,callable_opaque_resume,NULL,NULL,0,0},
        {XR_XIR_CALL_ABI_VERSION,&parameter,1,XR_XIR_I64,0,callable_opaque_resume,NULL,NULL,0,0}};
    for(unsigned opaque=0;opaque<2;++opaque) {
        XrXirCallAccounting accounting={0};XrXirCallConfig config;
        CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.entries=entries;config.entry_count=2;config.accounting=&accounting;config.instance=opaque ? &marker : NULL;
        XrXirCall *call=NULL;CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY);
        XrXirCallResult result=xr_xir_call_poll_bounded(call,100);
        CHECK(result.status==XR_XIR_CALL_BAD_ARGUMENT && xr_xir_call_result_valid(&result));
        CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && !accounting.live_bytes && accounting.allocations==accounting.frees);
    }
    CHECK(marker==UINT64_C(0x123456789abcdef0));
}
static void callable_admission_cases(XrXirProgram *program,XrXirProgram *foreign,const uint32_t ids[10],const XrXirTypes *types) {
    callable_opaque_cases();
    XrXirInstance *owner=callable_admission_instance(program);
    XrXirValue good=callable_admission_value(owner,ids[5]);XrXirValue before=good;
    const XrXirTypeNode *shape=xr_xir_callable_signature(types,(XrXirType)good.type);
    CHECK(shape && !shape->parameter_count && shape->result==XR_XIR_I64 &&
        shape->flags==(XR_XIR_CALLABLE_ROOT_NONE|XR_XIR_CALLABLE_NO_SUSPEND));
    CHECK(xr_xir_instance_start(owner,ids[8],&good,1)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_value_valid(&good) && !memcmp(&good,&before,sizeof(good)));
    callable_admission_argument=good;callable_admission_target=ids[8];callable_admission_actions=0;
    CHECK(xr_xir_instance_start(owner,ids[9],NULL,0)==XR_XIR_CALL_READY);
    XrXirValue result=callable_argument_result(owner);CHECK(result.type==XR_XIR_I64 && result.payload==7);
    xr_xir_value_drop(&result);CHECK(xr_xir_value_valid(&good) && !memcmp(&good,&before,sizeof(good)));
    CHECK(xr_xir_instance_start_function(owner,&good,NULL,0)==XR_XIR_CALL_READY);
    result=callable_argument_result(owner);CHECK(result.type==XR_XIR_I64 && result.payload==7);xr_xir_value_drop(&result);
    XrXirValue plain=callable_admission_value(owner,ids[6]),wrong=callable_admission_value(owner,ids[7]);
    shape=xr_xir_callable_signature(types,(XrXirType)plain.type);
    CHECK(shape && shape->flags==XR_XIR_CALLABLE_ROOT_NONE && !shape->parameter_count && shape->result==XR_XIR_I64);
    shape=xr_xir_callable_signature(types,(XrXirType)wrong.type);
    CHECK(shape && shape->flags==(XR_XIR_CALLABLE_ROOT_NONE|XR_XIR_CALLABLE_NO_SUSPEND) &&
        shape->parameter_count==1 && shape->parameters[0].type==XR_XIR_I64 && shape->result==XR_XIR_I64);
    for(unsigned mode=0;mode<3;++mode) callable_admission_reject(program,ids,mode,NULL);
    callable_admission_reject(program,ids,3,&good);
    XrXirInstance *other=callable_admission_instance(foreign);
    XrXirValue outside=callable_admission_value(other,ids[5]);callable_admission_reject(program,ids,4,&outside);
    CHECK(callable_admission_actions==6);
    xr_xir_value_drop(&outside);CHECK(xr_xir_instance_free(other)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&good);xr_xir_value_drop(&plain);xr_xir_value_drop(&wrong);
    CHECK(xr_xir_instance_free(owner)==XR_XIR_CALL_READY);callable_admission_argument=(XrXirValue){0};
    callable_admission_types=NULL;memset(callable_admission_selected,0,sizeof(callable_admission_selected));
    puts("Callable arguments:active conversion, unchanged caller, exact host start +5 precise refusals +2 opaque Call cases PASS");
}
#endif
