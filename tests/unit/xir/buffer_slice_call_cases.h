/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * buffer_slice_call_cases.h - Actual Program leases across borrowed returns
 *
 * KEY CONCEPT:
 *   Incremental dispatch observes metadata leases while their real frames are
 *   live. Completion, cancellation and refusal must release each lease once.
 */
#ifndef BUFFER_SLICE_CALL_CASES_H
#define BUFFER_SLICE_CALL_CASES_H
#include "xir/xxir_program_internal.h"
#include "xir/xxir_instance_debug_internal.h"
static struct {uint32_t entry,instruction,callee;} buffer_call_trace;
static XrXirCallStatus buffer_trace_before(void *owner,const XirCallNativeSourcePoint *point,bool *stopped) {
    (void)owner;buffer_call_trace.entry=point->entry;buffer_call_trace.instruction=point->instruction;
    buffer_call_trace.callee=UINT32_MAX;*stopped=false;return XR_XIR_CALL_READY;
}
static XrXirCallStatus buffer_trace_call(void *owner,const XirCallNativeSourcePending *point) {
    (void)owner;buffer_call_trace.entry=point->entry;buffer_call_trace.instruction=point->instruction;
    buffer_call_trace.callee=point->callee;return XR_XIR_CALL_READY;
}
static const XirCallNativeSourceObserver buffer_call_observer={
    &buffer_call_trace,buffer_trace_before,buffer_trace_call};
static uint32_t buffer_program_refs(const XrXirProgram *program) {
    return atomic_load_explicit(&program->references,memory_order_acquire);
}
static XrXirCallStatus buffer_poll(XrXirInstance *instance) {
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for (uint32_t step=0;status==XR_XIR_CALL_READY;++step) {
        CHECK(step<100000);status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
    }
    return status;
}
static void buffer_initialize(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(buffer_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED &&
        value.type==XR_XIR_I64 && value.payload==0);xr_xir_value_drop(&value);
}
static void buffer_invoke(XrXirProgram *program,XrXirInstance *instance,uint32_t target) {
    uint32_t baseline=buffer_program_refs(program),peak=baseline;
    CHECK(xr_xir_instance_start(instance,target,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=XR_XIR_CALL_READY;XrXirInstanceResult observed={0};
    for (uint32_t step=0;status==XR_XIR_CALL_READY;++step) {
        CHECK(step<100000);observed=xr_xir_instance_poll_bounded(instance,1);status=observed.outcome.status;
        uint32_t refs=buffer_program_refs(program);if (refs>peak) peak=refs;
    }
    if (status!=XR_XIR_CALL_RETURNED || peak<baseline+2u || buffer_program_refs(program)!=baseline)
        fprintf(stderr,"Buffer invoke target%u status%u refs%u peak%u baseline%u last%u/%u callee%u value%u/%lld prior%u\n",
            target,status,buffer_program_refs(program),peak,baseline,
            buffer_call_trace.entry,buffer_call_trace.instruction,buffer_call_trace.callee,
            observed.outcome.value.type,(long long)observed.outcome.value.payload,observed.outcome.prior_status);
    CHECK(status==XR_XIR_CALL_RETURNED && peak>=baseline+2u && buffer_program_refs(program)==baseline);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED &&
        value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);
}
static void buffer_call_cases(XrXirProgram *program,const uint32_t ids[7]) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    uint32_t original=buffer_program_refs(program);XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xir_instance_native_source_bind(instance,program,&buffer_call_observer)==XR_XIR_CALL_READY);
    buffer_initialize(instance,ids[0]);buffer_invoke(program,instance,ids[1]);buffer_invoke(program,instance,ids[2]);
    buffer_invoke(program,instance,ids[3]);buffer_invoke(program,instance,ids[4]);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && buffer_program_refs(program)==original);
    /* Cancel only after a real borrowed-return frame owns its metadata lease. */
    instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    buffer_initialize(instance,ids[0]);uint32_t baseline=buffer_program_refs(program);
    CHECK(xr_xir_instance_start(instance,ids[1],NULL,0)==XR_XIR_CALL_READY);
    bool captured=false;
    for (uint32_t step=0;!captured;++step) {
        CHECK(step<100000 && xr_xir_instance_poll_bounded(instance,1).outcome.status==XR_XIR_CALL_READY);
        captured=buffer_program_refs(program)>baseline;
    }
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(buffer_poll(instance)==XR_XIR_CALL_CANCELLED && buffer_program_refs(program)==baseline);
    XrXirCallStatus closed=xr_xir_instance_free(instance);
    CHECK((closed==XR_XIR_CALL_READY || closed==XR_XIR_CALL_CANCELLED) && buffer_program_refs(program)==original);
    /* The real Program retain refuses before publishing the receiver proof. */
    instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    buffer_initialize(instance,ids[0]);baseline=buffer_program_refs(program);
    CHECK(xr_xir_instance_start(instance,ids[1],NULL,0)==XR_XIR_CALL_READY);
    atomic_store_explicit(&program->references,UINT32_MAX,memory_order_release);
    CHECK(buffer_poll(instance)==XR_XIR_CALL_LIMIT && buffer_program_refs(program)==UINT32_MAX);
    atomic_store_explicit(&program->references,baseline,memory_order_release);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_LIMIT && buffer_program_refs(program)==original);
    /* A callback panic unwinds both its reborrow and the enclosing Buffer loan. */
    instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    buffer_initialize(instance,ids[0]);baseline=buffer_program_refs(program);
    CHECK(xr_xir_instance_start(instance,ids[5],NULL,0)==XR_XIR_CALL_READY);
    CHECK(buffer_poll(instance)==XR_XIR_CALL_ASSERTION && buffer_program_refs(program)==baseline);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_ASSERTION && buffer_program_refs(program)==original);
    /* This cancellation occurs after the real callback frame has begun, rather
     * than merely after capture of a future native borrowed-return frame. */
    instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xir_instance_native_source_bind(instance,program,&buffer_call_observer)==XR_XIR_CALL_READY);
    buffer_initialize(instance,ids[0]);baseline=buffer_program_refs(program);
    buffer_call_trace.entry=UINT32_MAX;
    CHECK(xr_xir_instance_start(instance,ids[3],NULL,0)==XR_XIR_CALL_READY);
    for (uint32_t step=0;buffer_call_trace.entry!=ids[6];++step) {
        CHECK(step<100000 && xr_xir_instance_poll_bounded(instance,1).outcome.status==XR_XIR_CALL_READY);
    }
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(buffer_poll(instance)==XR_XIR_CALL_CANCELLED && buffer_program_refs(program)==baseline);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_CANCELLED && buffer_program_refs(program)==original);
}
#endif
