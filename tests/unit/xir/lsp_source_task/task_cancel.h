/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_SOURCE_TASK_CANCEL_H
#define LSP_SOURCE_TASK_CANCEL_H
#include <inttypes.h>
static _Atomic bool task_poll_armed,task_poll_entered,task_poll_released;
static size_t task_poll_calls;
static const size_t task_poll_trigger=2;
static XrCompileResourceStatus task_cut_status;
static const char *task_cut_kind;
static const XrCompileResourceLimits *task_axis_limits;
static XrCompileResourceStats task_last_stats;
static XrCompileResources *task_last_created_resources;
static size_t task_resource_created_count;
XR_FUNC void xr_compile_resources_release(XrCompileResources *resources) {
    if(resources)task_last_stats=sdk_stats(resources);
    task_resource_release_impl(resources);
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(const XrCompileResourceLimits *limits,XrCompileResources **out) {
    if(limits)fprintf(stderr,"Task capture limits mode=%u memory=%" PRIu64 "\n",(unsigned)limits->memory_mode,limits->memory_limit);
    CHECK(limits&&limits->memory_mode==XR_COMPILE_MEMORY_FINITE&&limits->memory_limit==UINT64_C(8)*1024*1024);
    XrCompileResourceStatus status=task_resource_new_impl(task_axis_limits?task_axis_limits:limits,out);
    if(status==XR_COMPILE_RESOURCE_OK){task_last_created_resources=*out;++task_resource_created_count;}
    return status;
}
static bool task_cancel_checkpoint(bool selected,const char *kind) {
    if(!selected||!atomic_load_explicit(&task_poll_armed,memory_order_acquire)||
        atomic_load_explicit(&task_poll_entered,memory_order_acquire)||++task_poll_calls!=task_poll_trigger)return false;
    task_cut_kind=kind;atomic_store_explicit(&task_poll_entered,true,memory_order_release);
    uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load_explicit(&task_poll_released,memory_order_acquire)){
        CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);
    }
    return true;
}
XR_FUNC XrCompileResourceStatus xr_compile_request_control_poll(XrCompileRequestControl *control) {
    bool cutting=task_cancel_checkpoint(control&&control->initialized&&control->cancel_poll,"request");
    XrCompileResourceStatus status=task_request_poll_impl(control);
    if(cutting)task_cut_status=status;
    return status;
}
XR_FUNC XrCompileResourceStatus xr_compile_session_poll(XrCompilerSession *session) {
    bool cutting=task_cancel_checkpoint(session&&xr_compile_session_current_arena(session)!=NULL,"session");
    XrCompileResourceStatus status=task_session_poll_impl(session);
    if(cutting)task_cut_status=status;
    return status;
}
static void task_cancel_arm(void) {
    task_poll_calls=0;task_cut_status=XR_COMPILE_RESOURCE_OK;task_cut_kind=NULL;
    atomic_store_explicit(&task_poll_entered,false,memory_order_release);
    atomic_store_explicit(&task_poll_released,false,memory_order_release);
    atomic_store_explicit(&task_poll_armed,true,memory_order_release);
}
static void task_cancel_wait(void) {
    uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load_explicit(&task_poll_entered,memory_order_acquire)){
        CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);
    }
}
static void task_cancel_release(void) {
    atomic_store_explicit(&task_poll_released,true,memory_order_release);
}
/* Inspect the actual safe-point status only after worker join and owner cleanup. */
static int task_cancel_check(unsigned mode) {
    CHECK(atomic_load_explicit(&task_poll_entered,memory_order_acquire)&&task_poll_calls>=task_poll_trigger&&task_cut_kind);
    bool rejected=task_cut_status==XR_COMPILE_RESOURCE_CANCELLED;
    fprintf(stderr,"INTERIOR mode=%u trigger=%zu calls=%zu rejected=%u point=%s held8Mi physical0/0 JSON0/0 app0/0\n",
        mode,task_poll_trigger,task_poll_calls,rejected?1u:0u,task_cut_kind);
    if(!rejected){fprintf(stderr,"Actual Source task cancellation did not interrupt mode=%u\n",mode);return 1;}
    return 0;
}
#endif // LSP_SOURCE_TASK_CANCEL_H
