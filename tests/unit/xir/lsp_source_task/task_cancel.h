/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_SOURCE_TASK_CANCEL_H
#define LSP_SOURCE_TASK_CANCEL_H
#include <inttypes.h>
static _Atomic bool task_work_armed,task_work_entered,task_work_released;
static size_t task_work_calls;
static const size_t task_work_trigger=4096;
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(const XrCompileResourceLimits *limits,XrCompileResources **out) {
    if(limits)fprintf(stderr,"Task capture limits allocated=%" PRIu64 " live=%" PRIu64 " work=%" PRIu64 "\n",
        limits->allocated_bytes,limits->live_bytes,limits->work);
    CHECK(limits&&limits->allocated_bytes==UINT64_C(64)*1024*1024&&
        limits->live_bytes==UINT64_C(8)*1024*1024&&limits->work==UINT64_C(128000000));
    return task_resource_new_impl(limits,out);
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources,uint64_t units) {
    if(atomic_load_explicit(&task_work_armed,memory_order_acquire)&&++task_work_calls==task_work_trigger){
        atomic_store_explicit(&task_work_entered,true,memory_order_release);
        uint64_t deadline=xr_time_monotonic_ms()+5000;
        while(!atomic_load_explicit(&task_work_released,memory_order_acquire)){
            CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);
        }
    }
    return task_work_impl(resources,units);
}
static void task_cancel_arm(void) {
    task_work_calls=0;
    atomic_store_explicit(&task_work_armed,true,memory_order_release);
}
static void task_cancel_wait(void) {
    uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load_explicit(&task_work_entered,memory_order_acquire)){
        CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);
    }
}
static void task_cancel_release(void) {
    atomic_store_explicit(&task_work_released,true,memory_order_release);
}
/* Called only after the real worker has joined and terminal owners are gone. */
static int task_cancel_check(unsigned mode) {
    CHECK(atomic_load_explicit(&task_work_entered,memory_order_acquire)&&task_work_calls>=task_work_trigger);
    fprintf(stderr,"INTERIOR mode=%u trigger=%zu calls=%zu strict64Mi8Mi128000000 physical0/0 JSON0/0 app0/0\n",mode,task_work_trigger,task_work_calls);
    if(task_work_calls>task_work_trigger+64){
        fprintf(stderr,"Actual Source task cancellation did not interrupt mode=%u\n",mode);return 1;
    }
    return 0;
}
#endif
