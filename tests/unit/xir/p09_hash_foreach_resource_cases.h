/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_foreach_resource_cases.h - Shared physical allocation and cancellation proof
 */
#ifndef P09_HASH_FOREACH_RESOURCE_CASES_H
#define P09_HASH_FOREACH_RESOURCE_CASES_H
#include "xir/xxir_array.h"
#include "xir/xxir_map.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
static uint32_t foreach_resource_count=5,foreach_resource_expected[5]={5,1,1,1,1};
static uint32_t foreach_resource_entry=0;
typedef struct ForeachResourceWait { uint64_t epoch,wake;uint32_t resumed;bool pending; } ForeachResourceWait;
static XrXirCallStatus foreach_tick(XrXirInstance *instance,ForeachResourceWait *wait) {
    if(wait->pending) {
        CHECK(xr_xir_instance_resume(instance,wait->epoch,wait->wake)==XR_XIR_CALL_READY);
        wait->pending=false;++wait->resumed;
    }
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1);
    if(result.outcome.status==XR_XIR_CALL_SUSPENDED) {
        CHECK(result.epoch && result.outcome.wake && result.outcome.wake!=wait->wake);
        CHECK(!wait->epoch || wait->epoch==result.epoch);
        wait->epoch=result.epoch;wait->wake=result.outcome.wake;wait->pending=true;
        XrXirWaitRequest request={0};
        CHECK(xr_xir_instance_wait_request(instance,wait->epoch,wait->wake,&request)==XR_XIR_CALL_READY);
        CHECK(request.kind==XR_XIR_WAIT_YIELD && !request.reserved && !request.after_ms &&
            !request.subject && !request.generation && !request.ticket);
    }
    return result.outcome.status;
}
static XrXirCallStatus foreach_poll(XrXirInstance *instance) {
    ForeachResourceWait wait={0};
    for(uint32_t tick=0;tick<10000;++tick) {
        XrXirCallStatus status=foreach_tick(instance,&wait);
        if(status==XR_XIR_CALL_READY || status==XR_XIR_CALL_SUSPENDED)continue;
        if(status==XR_XIR_CALL_RETURNED)CHECK(wait.resumed==foreach_resource_expected[foreach_resource_entry]);
        return status;
    }
    CHECK(false);return XR_XIR_CALL_BAD_STATE;
}
static XrXirCallStatus foreach_run(XrXirInstance *instance, uint32_t entry, XrXirValue *output, bool *driven_oom) {
    if (driven_oom) *driven_oom = false;
    XrXirCallStatus status = xr_xir_instance_start(instance,entry,NULL,0);
    if (status == XR_XIR_CALL_READY) {
        status = foreach_poll(instance);
        if (driven_oom) *driven_oom = status == XR_XIR_CALL_OOM;
    }
    if (status == XR_XIR_CALL_RETURNED)
        CHECK(xr_xir_instance_take_result(instance,output) == status);
    else CHECK(!output->type && !output->reserved && !output->payload);
    return status;
}
static void foreach_result(const XrXirValue *value,uint32_t entry) {
    if(foreach_resource_count==1 || !entry){CHECK(value->type==XR_XIR_I64 && value->payload==123);return;}
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(value),domain,NULL,NULL,100000,65536};
    XrXirValue key={XR_XIR_I64,0,entry},array={0};bool found=false,equal=false;
    CHECK(xr_xir_map_get(value,&key,&admission,&array,&found)==XR_XIR_VALUE_OK && found);
    CHECK(xr_xir_map_contains_value(value,&array,&admission,&equal)==XR_XIR_VALUE_OK && equal==(entry!=2));
    int64_t length=0;CHECK(xr_xir_array_len(&array,&admission,&length)==XR_XIR_VALUE_OK && length==(entry==1?2:1));
    xr_xir_value_drop(&array);xr_xir_domain_drop(domain);
}
static void foreach_allocations(XrXirProgram *program, const uint32_t *entries, bool full) {
    const size_t live = runtime_live, bytes = runtime_bytes;
    for (uint32_t entry = 0; entry < foreach_resource_count; ++entry) {
        foreach_resource_entry=entry;size_t points = 0;
        for (size_t pass = 0; pass <= (full ? points : 0); ++pass) {
            XrXirInstanceConfig config = {0}; XrXirInstance *instance = NULL; XrXirValue result = {0};
            bool driven_oom = false;
            CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
            runtime_attempts = 0; runtime_fail_at = pass ? pass - 1 : SIZE_MAX;
            XrXirCallStatus status = xr_xir_instance_new(program,&config,&instance);
            if (status == XR_XIR_CALL_READY) status = foreach_run(instance,entries[entry],&result, &driven_oom);
            size_t attempts = runtime_attempts; runtime_fail_at = SIZE_MAX;
            if (!pass) {
                CHECK(status == XR_XIR_CALL_RETURNED); foreach_result(&result,entry);
                points = attempts; CHECK(points && points < 20000);
            } else CHECK(attempts > pass - 1 && status == XR_XIR_CALL_OOM &&
                !result.type && !result.reserved && !result.payload);
            xr_xir_value_drop(&result);
            XrXirCallStatus expected_free = driven_oom ? XR_XIR_CALL_OOM : XR_XIR_CALL_READY;
            if (instance) {
                CHECK(xr_xir_task_executor_root_idle(instance->executor));
                CHECK(xr_xir_task_executor_completion_status(instance->executor) == expected_free);
                if (driven_oom) {
                    CHECK(instance->call);
                    const XrXirExecutorBinding binding = {instance->executor, instance->call->executor_activation,
                        instance->call->executor_generation, instance->call->executor_ticket};
                    XrXirCallStatus failure = XR_XIR_CALL_BAD_STATE;
                    CHECK(xr_xir_call_driver_failure(instance->call, &binding, &failure) && failure == XR_XIR_CALL_OOM);
                }
            }
            if (pass && instance && xr_xir_instance_state(instance) != XR_XIR_INSTANCE_FAILED) {
                CHECK(foreach_run(instance, entries[entry], &result, &driven_oom) == XR_XIR_CALL_RETURNED && !driven_oom);
                foreach_result(&result, entry); xr_xir_value_drop(&result);
                CHECK(xr_xir_task_executor_root_idle(instance->executor));
                expected_free = XR_XIR_CALL_READY;
                CHECK(xr_xir_task_executor_completion_status(instance->executor) == expected_free);
            }
            CHECK(xr_xir_instance_free(instance) == expected_free);
            CHECK(runtime_live == live && runtime_bytes == bytes);
        }
        printf("Map/Set forEach entry=%u actual runtime allocation sites=%zu mode=%s physical refund PASS\n",
            entry,points,full ? "full" : "baseline");
    }
}
static void foreach_cancellation(XrXirProgram *program, const uint32_t *entries) {
    const size_t live = runtime_live, bytes = runtime_bytes;
    for (uint32_t entry = 0; entry < foreach_resource_count; ++entry) {
        foreach_resource_entry=entry;
        uint32_t quanta=0,initialization_prefixes=0,retry_prefixes=0,suspension_prefixes=0;
        for (uint32_t prefix = 0; prefix < 10000; ++prefix) {
            XrXirInstanceConfig config = {0}; XrXirInstance *instance = NULL; XrXirValue result = {0};
            CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_start(instance,entries[entry],NULL,0) == XR_XIR_CALL_READY);
            XrXirCallStatus status=XR_XIR_CALL_READY;ForeachResourceWait wait={0};
            for(uint32_t tick=0;tick<prefix && (status==XR_XIR_CALL_READY || status==XR_XIR_CALL_SUSPENDED);++tick)
                status=foreach_tick(instance,&wait);
            if (status == XR_XIR_CALL_RETURNED) {
                CHECK(xr_xir_instance_take_result(instance,&result) == status);
                foreach_result(&result,entry); quanta = prefix; xr_xir_value_drop(&result);
            } else {
                CHECK(status==XR_XIR_CALL_READY || status==XR_XIR_CALL_SUSPENDED);
                suspension_prefixes+=status==XR_XIR_CALL_SUSPENDED;
                CHECK(xr_xir_instance_take_result(instance,&result) == XR_XIR_CALL_BAD_STATE);
                CHECK(!result.type && !result.reserved && !result.payload);
                CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
                CHECK(foreach_poll(instance) == XR_XIR_CALL_CANCELLED);
                CHECK(xr_xir_instance_take_result(instance,&result) == XR_XIR_CALL_BAD_STATE);
                /* Cancelling initialization permanently fails the Instance.
                 * After initialization, an ordinary cancelled call can retry. */
                bool initialization = xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED;
                XrXirCallStatus retry = foreach_run(instance,entries[entry],&result, NULL);
                if (initialization) {
                    CHECK(retry == XR_XIR_CALL_CANCELLED && !result.type && !result.reserved && !result.payload);
                    ++initialization_prefixes;
                } else {
                    CHECK(retry == XR_XIR_CALL_RETURNED); foreach_result(&result,entry);
                    xr_xir_value_drop(&result); ++retry_prefixes;
                }
            }
            CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
            CHECK(runtime_live == live && runtime_bytes == bytes);
            if (quanta) break;
        }
        CHECK(quanta && initialization_prefixes && retry_prefixes && suspension_prefixes==foreach_resource_expected[entry]);
        printf("Map/Set forEach entry=%u cancellation prefixes=%u initializing=%u retry=%u suspended=%u physical refund PASS\n",
            entry,quanta,initialization_prefixes,retry_prefixes,suspension_prefixes);
    }
}
#endif // P09_HASH_FOREACH_RESOURCE_CASES_H
