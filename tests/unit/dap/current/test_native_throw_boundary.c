/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_throw_boundary.c - Real emitted throws held before native release
 */
#define main native_reference_main
#include "../../xir/test_xir_call_native.c"
#undef main
#include "xir/xxir_task.c"
#include "xir/xxir_call_native_observer_internal.h"

typedef struct NativeThrowWitness {
    XirCallNativeObserver observer;
    XrXirInstance *instance;
    const XrXirCallView *view;
    const XrXirProgramSpec *original;
    XirCallNativeFrame held;
    unsigned bodies[5],releases[5],events,code_releases;
    XrXirCallStatus rejection;
    bool pause,malformed;
} NativeThrowWitness;
static NativeThrowWitness *native_witness;

static XrXirCallStatus native_observe(void *owner,const XirCallNativeThrowEvent *event,bool *pause) {
    NativeThrowWitness *w=owner;CHECK(w==native_witness && event && pause && event->value);
    CHECK(event->frame.instance==w->instance && event->frame.phase==XR_XIR_CALL_NORMAL && event->frame.frame);
    if (event->frame.entry==5) {
        CHECK(w->events==3 && w->releases[2]==1 && event->frame.depth==1 && !event->frame.parent);
        CHECK(error_fixture_is_code(event->value,w->instance->domain,91));
        *pause=false;return XR_XIR_CALL_READY;
    }
    CHECK(event->frame.entry==2-w->events && w->events<3);
    CHECK(error_fixture_is_code(event->value,w->instance->domain,91));
    XrXirCall *call=((XirTaskActivation *)event->frame.binding.activation)->call;
    CHECK(call && xr_xir_call_executor_member(call,&event->frame.binding));
    size_t allocations=runtime_attempts;
    CHECK(w->view && !xr_xir_call_admission(w->view));
    CHECK(xir_call_native_observer_bind(w->view,&w->observer)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_poll_bounded(call,1).status==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_request_cancel(call)==XR_XIR_CALL_BUSY && xr_xir_call_free(call)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_poll_bounded(w->instance,1).outcome.status==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_instance_stop(w->instance)==XR_XIR_CALL_BUSY && xr_xir_instance_free(w->instance)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_task_executor_stop(w->instance->executor)==XR_XIR_CALL_BUSY);
    XirCallNativeThrow copy={0};
    CHECK(xir_call_native_throw_copy(call,&event->frame.binding,event->frame.frame,&copy)==XR_XIR_CALL_BUSY);
    CHECK(xir_call_native_throw_continue(call,&event->frame.binding,event->frame.frame)==XR_XIR_CALL_BUSY);
    CHECK(!copy.value.type && runtime_attempts==allocations && (event->frame.entry!=2 || !w->releases[2]));
    w->held=event->frame;++w->events;
    w->view=NULL;*pause=w->pause && w->events==1;return w->rejection;
}
static XrXirAction native_observed_resume(XrXirCallView *view) {
    NativeThrowWitness *w=native_witness;CHECK(w && view->instance==w->instance && !w->view);
    uint32_t function=xr_xir_call_current_entry(view->activation);CHECK(function<5);
    CHECK(view->environment==w->original->entries[function].environment);
    XrXirCallView copy=*view;
    CHECK(xir_call_native_observer_bind(&copy,&w->observer)==XR_XIR_CALL_BAD_STATE);
    XrXirCallStatus bound=xir_call_native_observer_bind(view,&w->observer);
    if (bound!=XR_XIR_CALL_READY) { CHECK(bound==XR_XIR_CALL_LIMIT);return xr_xir_call_fault(XR_XIR_RUN_FRAME_LIMIT); }
    size_t allocations=runtime_attempts;
    CHECK(xir_call_native_observer_bind(view,&w->observer)==XR_XIR_CALL_READY);
    XirCallNativeObserver foreign=w->observer;
    CHECK(xir_call_native_observer_bind(view,&foreign)==XR_XIR_CALL_BAD_STATE && runtime_attempts==allocations);
    ++w->bodies[function];w->view=view;
    XrXirAction action=w->original->entries[function].resume(view);
    if (w->malformed && function==2 && action.kind==XR_XIR_ACTION_THROW) action.value.reserved=1;
    if (action.kind!=XR_XIR_ACTION_THROW) w->view=NULL;
    /* The driver invalidates this exact stack view before notification. Its
     * address is used only by the synchronous reentry assertions above. */
    return action;
}
static void native_observed_release(XrXirCallView *view,XrXirCallStatus reason) {
    NativeThrowWitness *w=native_witness;
    uint32_t function=(uint32_t)(view->activation->top->entry-view->activation->config.entries);
    CHECK(function<5 && !xr_xir_call_admission(view));++w->releases[function];
    w->original->entries[function].release(view,reason);
}
static void native_code_release(void *owner) { ++((NativeThrowWitness *)owner)->code_releases; }

static void native_boundary_case(bool pause,unsigned finish,XrXirCallStatus rejection,bool malformed) {
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    NativeThrowWitness witness={0};NativeThrowWitness *w=&witness;native_witness=w;
    w->observer=(XirCallNativeObserver){w,native_observe};w->original=&fixture_calls1_program;
    w->pause=pause;w->rejection=rejection;w->malformed=malformed;
    CHECK(w->original->entry_count==5);
    XrXirCallEntry entries[5];memcpy(entries,w->original->entries,sizeof(entries));
    for (unsigned i=0;i<5;++i) { entries[i].resume=native_observed_resume;entries[i].release=native_observed_release; }
    XrXirProgramSpec spec=*w->original;spec.entries=entries;spec.code=(XrXirCodeLease){w,native_code_release};
    XrXirProgram *program=NULL;CHECK(native_program_new(&compiler.context,&spec,&program));
    w->instance=call_instance_new(program);xr_xir_compile_program_drop(program);
    XrXirValue args[]={{XR_XIR_I64,0,9},{XR_XIR_I64,0,4}};
    CHECK(xr_xir_instance_start(w->instance,0,args,2)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(w->instance,UINT64_MAX);
    CHECK(result.outcome.status==XR_XIR_CALL_SUSPENDED && !w->events);
    CHECK(xr_xir_instance_resume(w->instance,result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);
    w->view=NULL;
    result=xr_xir_instance_poll_bounded(w->instance,1);
    for (unsigned i=0;i<20 && !w->instance->budget.native_hold && result.outcome.status==XR_XIR_CALL_READY;++i) {
        w->view=NULL;result=xr_xir_instance_poll_bounded(w->instance,1);
    }
    XirCallNativeThrow escaped={0};
    if (pause && rejection==XR_XIR_CALL_READY && !malformed) {
        fprintf(stderr,"native held: events=%u status=%u compareSteps=%u releases=%u held=%p\n",
            w->events,result.outcome.status,w->bodies[2],w->releases[2],(void *)w->instance->budget.native_hold);
        /* The emitted body returns once for each of SUSPEND, CONST, ENUM_NEW
         * and THROW; the independent source fixture contains those four ops. */
        CHECK(w->events==1 && result.outcome.status==XR_XIR_CALL_READY && w->bodies[2]==4 && !w->releases[2]);
        XrXirCall *call=w->instance->budget.native_hold;CHECK(call && !call->top->exiting);
        unsigned bodies[5],releases[5];memcpy(bodies,w->bodies,sizeof(bodies));memcpy(releases,w->releases,sizeof(releases));
        XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(w->instance->domain);
        const size_t allocations=runtime_attempts;
        for (unsigned i=0;i<16;++i) CHECK(xr_xir_instance_poll_bounded(w->instance,UINT64_MAX).outcome.status==XR_XIR_CALL_READY);
        CHECK(!memcmp(bodies,w->bodies,sizeof(bodies)) && !memcmp(releases,w->releases,sizeof(releases)));
        CHECK(xr_xir_domain_budget_stats(w->instance->domain).work==before.work && runtime_attempts==allocations);
        XrXirExecutorBinding forged=w->held.binding;++forged.ticket;
        CHECK(xir_call_native_throw_copy(call,&forged,w->held.frame,&escaped)==XR_XIR_CALL_BAD_STATE && !escaped.value.type);
        CHECK(xir_call_native_throw_continue(call,&forged,w->held.frame)==XR_XIR_CALL_BAD_STATE);
        CHECK(xir_call_native_throw_copy(call,&w->held.binding,w->held.frame+1,&escaped)==XR_XIR_CALL_BAD_STATE);
        XirObject *carrier=object_pointer(&call->top->pending.value);CHECK(carrier);
        uint32_t references=atomic_load_explicit(&carrier->references,memory_order_relaxed);
        atomic_store_explicit(&carrier->references,UINT32_MAX,memory_order_relaxed);
        CHECK(xir_call_native_throw_copy(call,&w->held.binding,w->held.frame,&escaped)==XR_XIR_CALL_LIMIT && !escaped.value.type);
        CHECK(w->instance->budget.native_hold==call && !call->top->exiting);
        atomic_store_explicit(&carrier->references,references,memory_order_relaxed);
        CHECK(xir_call_native_throw_copy(call,&w->held.binding,w->held.frame,&escaped)==XR_XIR_CALL_READY);
        CHECK(escaped.frame.frame==w->held.frame && escaped.frame.entry==2 && error_fixture_is_code(&escaped.value,w->instance->domain,91));
        XirCallNativeThrow occupied=escaped;
        CHECK(xir_call_native_throw_copy(call,&w->held.binding,w->held.frame,&occupied)==XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!memcmp(&occupied,&escaped,sizeof(occupied)));
        if (finish==3) {
            XrXirDomainBudgetStats remaining=xr_xir_domain_budget_stats(w->instance->domain);
            CHECK(xr_xir_domain_work(w->instance->domain,remaining.work_limit-remaining.work));
            XirCallNativeThrow refused={0};
            CHECK(xir_call_native_throw_copy(call,&w->held.binding,w->held.frame,&refused)==XR_XIR_CALL_LIMIT && !refused.value.type);
            CHECK(w->instance->budget.native_hold==call && !call->top->exiting);
        }
        if (finish==1) CHECK(xr_xir_instance_cancel_current(w->instance)==XR_XIR_CALL_CANCEL_REQUESTED);
        else if (finish==2) CHECK(xr_xir_instance_stop(w->instance)==XR_XIR_CALL_READY);
        else {
            CHECK(xir_call_native_throw_continue(call,&w->held.binding,w->held.frame)==XR_XIR_CALL_READY);
            CHECK(xir_call_native_throw_continue(call,&w->held.binding,w->held.frame)==XR_XIR_CALL_BAD_STATE);
        }
    }
    for (unsigned i=0;i<30 && result.outcome.status==XR_XIR_CALL_READY;++i) {
        w->view=NULL;result=xr_xir_instance_poll_bounded(w->instance,1);
    }
    XrXirCallStatus expected=malformed ? XR_XIR_CALL_BAD_STATE : rejection!=XR_XIR_CALL_READY ?
        (rejection==XR_XIR_CALL_OOM || rejection==XR_XIR_CALL_LIMIT ? rejection : XR_XIR_CALL_BAD_STATE) : finish==3 ? XR_XIR_CALL_LIMIT :
        finish ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_THROWN;
    CHECK(result.outcome.status==expected);
    if (expected==XR_XIR_CALL_THROWN) CHECK(w->events==3 && error_fixture_is_code(&result.outcome.value,w->instance->domain,91));
    if (malformed) CHECK(!w->events);
    CHECK(w->releases[2]==1 && !w->instance->budget.native_hold);
    CHECK(call_instance_retire(w->instance)==(expected==XR_XIR_CALL_OOM || expected==XR_XIR_CALL_LIMIT ||
        expected==XR_XIR_CALL_BAD_STATE ? expected : XR_XIR_CALL_READY));w->instance=NULL;
    CHECK(w->code_releases==1);
    if (escaped.value.type) {
        XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
        CHECK(error_fixture_is_code(&escaped.value,reader,91));xr_xir_value_drop(&escaped.value);xr_xir_domain_drop(reader);
    }
    CHECK(!runtime_live && !runtime_bytes);source_fixture_owner_free(&compiler);native_witness=NULL;
}
int main(void) {
    native_boundary_case(false,0,XR_XIR_CALL_READY,false);
    native_boundary_case(true,0,XR_XIR_CALL_READY,false);
    native_boundary_case(true,1,XR_XIR_CALL_READY,false);
    native_boundary_case(true,2,XR_XIR_CALL_READY,false);
    native_boundary_case(true,3,XR_XIR_CALL_READY,false);
    native_boundary_case(true,0,XR_XIR_CALL_OOM,false);
    native_boundary_case(true,0,XR_XIR_CALL_LIMIT,false);
    native_boundary_case(true,0,XR_XIR_CALL_BAD_ARGUMENT,false);
    native_boundary_case(true,0,XR_XIR_CALL_READY,true);
    puts("Admitted emitted-native boundary: hold before unwind, no replay, exact binding, cancellation, owned escape and physical zero: PASS");
    return 0;
}
