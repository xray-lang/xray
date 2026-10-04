/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nested_nullable_cases.h - Independent VM and native three-state expectations
 *
 * KEY CONCEPT:
 *   Results retain exact type metadata and only one unwrap removes one layer.
 */
#ifndef XIR_NESTED_NULLABLE_CASES_H
#define XIR_NESTED_NULLABLE_CASES_H
#include "xir/xxir_nullable.h"
static XrXirCallResult nested_poll(XrXirInstance *instance) {
    XrXirCallResult result={0};
    for (uint32_t tick=0;tick<1024;++tick) {
        result=xr_xir_instance_poll_bounded(instance,1).outcome;
        if (result.status!=XR_XIR_CALL_READY) return result;
    }
    CHECK(false);return result;
}
static XrXirCallStatus nested_run(XrXirInstance *instance,uint32_t entry,
    const XrXirValue *arguments,XrXirValue *output) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,arguments,arguments?1:0);
    if (status!=XR_XIR_CALL_READY) return status;
    XrXirCallResult result=nested_poll(instance);
    if (result.status==XR_XIR_CALL_RETURNED)
        CHECK(xr_xir_instance_take_result(instance,output)==XR_XIR_CALL_RETURNED);
    else if (result.status==XR_XIR_CALL_RUNTIME_PANIC) {
        CHECK(result.panic.detail.code==XR_XIR_PANIC_NULL_UNWRAP &&
            !result.panic.detail.reserved && !result.panic.detail.index && !result.panic.detail.length);
        CHECK(xr_xir_instance_take_result(instance,output)==XR_XIR_CALL_BAD_STATE);
        CHECK(!output->type && !output->reserved && !output->payload);
    }
    return result.status;
}
static void nested_shape(const XrXirValue *value,uint32_t state) {
    CHECK(value->type==(uint32_t)NESTED_OUTER && xr_xir_value_valid(value));
    bool some=false;const XrXirValue *inner=NULL,*number=NULL;
    CHECK(xr_xir_nullable_view(value,&some,&inner) && some==(state!=0));
    if (!state) { CHECK(!inner);return; }
    CHECK(inner && inner->type==(uint32_t)NESTED_INNER);
    CHECK(xr_xir_nullable_view(inner,&some,&number) && some==(state==2));
    if (state==1) CHECK(!number);
    else CHECK(number && number->type==XR_XIR_I64 && number->payload==7);
}
static void nested_vector(XrXirInstance *instance,XrXirValue held[3]) {
    XrXirValue number={0};CHECK(nested_run(instance,1,NULL,&number)==XR_XIR_CALL_RETURNED);
    CHECK(number.type==XR_XIR_I64 && number.payload==41);xr_xir_value_drop(&number);
    for (uint32_t state=0;state<3;++state) {
        XrXirValue value={0},presence={0},inner={0};
        CHECK(nested_run(instance,2+state,NULL,&value)==XR_XIR_CALL_RETURNED);nested_shape(&value,state);
        CHECK(nested_run(instance,7,&value,&presence)==XR_XIR_CALL_RETURNED);
        CHECK(presence.type==XR_XIR_BOOL && presence.payload==(state!=0));xr_xir_value_drop(&presence);
        XrXirCallStatus status=nested_run(instance,5,&value,&inner);
        if (!state) CHECK(status==XR_XIR_CALL_RUNTIME_PANIC);
        else {
            CHECK(status==XR_XIR_CALL_RETURNED && inner.type==(uint32_t)NESTED_INNER);
            CHECK(nested_run(instance,6,&inner,&number)==
                (state==1?XR_XIR_CALL_RUNTIME_PANIC:XR_XIR_CALL_RETURNED));
            if (state==2) {CHECK(number.type==XR_XIR_I64 && number.payload==7);xr_xir_value_drop(&number);}
            if (state==1) {CHECK(xr_xir_value_copy(&inner,&held[2])==XR_XIR_VALUE_OK);}
            xr_xir_value_drop(&inner);
        }
        if (state) CHECK(xr_xir_value_copy(&value,&held[state-1])==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&value);
    }
}
static void nested_instance_config(XrXirInstanceConfig *config) {
    CHECK(xr_xir_instance_config_init(config,sizeof(*config))==XR_XIR_CALL_READY);
    config->value_limit=UINT64_C(1048576);
}
static void nested_pair(XrXirProgram *program,XrXirValue held[6]) {
    XrXirInstance *instances[2]={NULL,NULL};XrXirInstanceConfig config;
    nested_instance_config(&config);
    for (uint32_t i=0;i<2;++i) CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    for (uint32_t i=0;i<2;++i) nested_vector(instances[i],held+3*i);
    for (uint32_t i=0;i<2;++i) CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
}
static void nested_retained(XrXirValue held[6]) {
    for (uint32_t i=0;i<2;++i) {
        XrXirValue *group=held+3*i;bool some=true;const XrXirValue *payload=NULL;
        nested_shape(group,1);nested_shape(group+1,2);
        xr_xir_value_drop(group);xr_xir_value_drop(group+1);
        CHECK(group[2].type==(uint32_t)NESTED_INNER &&
            xr_xir_nullable_view(group+2,&some,&payload) && !some && !payload);
        xr_xir_value_drop(group+2);
    }
}
static void nested_runtime_faults(XrXirProgram *program) {
    const size_t live=runtime_live,bytes=runtime_bytes;
    for (uint32_t entry=1;entry<=4;++entry) {
        size_t points=0;
        for (size_t pass=0;pass<=points;++pass) {
            XrXirInstance *instance=NULL;XrXirInstanceConfig config;XrXirValue result={0};
            nested_instance_config(&config);
            runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
            XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
            if (status==XR_XIR_CALL_READY) status=nested_run(instance,entry,NULL,&result);
            size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
            if (!pass) {CHECK(status==XR_XIR_CALL_RETURNED);points=attempts;CHECK(points && points<20000);}
            else CHECK(attempts>pass-1 && status==XR_XIR_CALL_OOM && !result.type && !result.payload);
            xr_xir_value_drop(&result);if(instance) CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
            CHECK(runtime_live==live && runtime_bytes==bytes);
        }
        printf("nested entry %u runtime OOM sites=%zu physical baseline restored\n",entry,points);
    }
}
static void nested_cancel_prefixes(XrXirProgram *program) {
    const size_t live=runtime_live,bytes=runtime_bytes;
    for (uint32_t entry=1;entry<=4;++entry) {
        uint32_t quanta=0;
        for (uint32_t prefix=0;prefix<1024;++prefix) {
            XrXirInstance *instance=NULL;XrXirInstanceConfig config;XrXirValue result={0};
            nested_instance_config(&config);
            CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
            CHECK(nested_run(instance,1,NULL,&result)==XR_XIR_CALL_RETURNED);xr_xir_value_drop(&result);
            CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
            XrXirCallStatus status=XR_XIR_CALL_READY;
            for (uint32_t tick=0;tick<prefix;++tick) {
                status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
                if (status!=XR_XIR_CALL_READY) break;
            }
            if (status==XR_XIR_CALL_RETURNED) {
                CHECK(xr_xir_instance_take_result(instance,&result)==status);
                xr_xir_value_drop(&result);quanta=prefix;
            } else {
                CHECK(status==XR_XIR_CALL_READY && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_BAD_STATE);
                CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
                CHECK(nested_poll(instance).status==XR_XIR_CALL_CANCELLED);
                CHECK(!result.type && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_BAD_STATE);
                CHECK(nested_run(instance,1,NULL,&result)==XR_XIR_CALL_RETURNED);
                CHECK(result.type==XR_XIR_I64 && result.payload==41);xr_xir_value_drop(&result);
            }
            CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
            CHECK(runtime_live==live && runtime_bytes==bytes);
            if(quanta) break;
        }
        CHECK(quanta);printf("nested entry %u cancellation prefixes=%u physical baseline restored\n",entry,quanta);
    }
}
#endif // XIR_NESTED_NULLABLE_CASES_H
