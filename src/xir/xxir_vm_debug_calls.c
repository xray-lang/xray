/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_calls.c - Infallible Call notification capture and owned identities
 */
#include "xxir_vm_debug_calls_internal.h"
#include "xxir_value_internal.h"
#include <string.h>
static bool calls_binding(const XrXirExecutorBinding *a,const XrXirExecutorBinding *b) {
    return a->owner==b->owner && a->activation==b->activation && a->generation==b->generation && a->ticket==b->ticket;
}
static bool calls_charge(XirDebugCalls *calls,uint64_t work) {
    if (work>UINT64_MAX-calls->debt) { calls->failure=XR_XIR_VALUE_LIMIT;return false; }
    calls->debt+=work;return true;
}
static XirDebugCallFrame *calls_find(XirDebugCalls *calls,const XrXirCallEvent *event) {
    for (uint32_t i=0;i<calls->capacity;++i) {
        if (!calls_charge(calls,1)) return NULL;
        XirDebugCallFrame *frame=&calls->frames[i];
        if (frame->active && frame->event.frame==event->frame && calls_binding(&frame->event.binding,&event->binding)) return frame;
    }
    return NULL;
}
/* The producer authenticates notifications. This callback never allocates,
 * borrows state, invokes user code, or drives the Instance. A sticky receiver
 * failure is handled by the control owner after the current driver quantum. */
static void calls_event(void *owner,const XrXirCallEvent *event) {
    XirDebugCalls *calls=owner;
    if (calls->disabled || calls->failure!=XR_XIR_VALUE_OK) return;
    if (!event || event->instance!=calls->target || !event->binding.owner || !event->binding.activation ||
        !event->binding.generation || !event->binding.ticket || !event->frame || !event->depth || event->entry>calls->functions) {
        calls->failure=XR_XIR_VALUE_BAD_ARGUMENT;return;
    }
    XirDebugCallFrame *frame=calls_find(calls,event);
    if (event->kind==XR_XIR_CALL_FRAME_ENTER || event->kind==XR_XIR_CALL_FRAME_PRESENT) {
        if (frame) { calls->failure=XR_XIR_VALUE_BAD_ARGUMENT;return; }
        if (calls->count==calls->capacity || calls->next_id==UINT64_MAX || calls->next_thread==UINT64_MAX) {
            calls->failure=XR_XIR_VALUE_LIMIT;return;
        }
        uint64_t thread=0;
        for (uint32_t i=0;i<calls->capacity;++i) {
            if (!calls_charge(calls,1)) return;
            XirDebugCallFrame *other=&calls->frames[i];
            if (other->active && calls_binding(&other->event.binding,&event->binding)) thread=other->thread;
            if (!other->active && !frame) frame=other;
        }
        if (!thread) thread=++calls->next_thread;
        *frame=(XirDebugCallFrame){.event=*event,.id=++calls->next_id,.thread=thread,.active=true};++calls->count;
        if (event->kind==XR_XIR_CALL_FRAME_PRESENT && calls->installing &&
            xr_xir_call_executor_member(calls->installing,&event->binding) &&
            (calls->current==UINT32_MAX || calls->frames[calls->current].event.depth<event->depth))
            calls->current=(uint32_t)(frame-calls->frames);
        return;
    }
    if (!frame || frame->event.parent!=event->parent || frame->event.depth!=event->depth || frame->event.entry!=event->entry) {
        calls->failure=XR_XIR_VALUE_BAD_ARGUMENT;return;
    }
    frame->event=*event;
    if (event->kind==XR_XIR_CALL_FRAME_LEAVE) {
        frame->active=false;--calls->count;
        if (calls->current==(uint32_t)(frame-calls->frames)) calls->current=UINT32_MAX;
    } else if (event->kind==XR_XIR_CALL_DISPATCH_BEGIN) {
        frame->native_waiting=false;calls->current=(uint32_t)(frame-calls->frames);
    }
    else if (event->kind==XR_XIR_CALL_DISPATCH_END) calls->current=UINT32_MAX;
}
XR_FUNC XrXirValueStatus xir_debug_calls_new(XirDebugCalls *calls,XrXirDomain *domain,uint32_t capacity) {
    if (!calls || calls->frames || !capacity || !xr_xir_domain_current(domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status;XirDebugCallFrame *frames=xr_xir_domain_metadata_allocate(domain,(uint64_t)capacity*sizeof(*frames),&status);
    if (!frames) return status;
    memset(frames,0,(size_t)capacity*sizeof(*frames));
    *calls=(XirDebugCalls){.observer={calls,calls_event},.domain=domain,.frames=frames,
        .capacity=capacity,.current=UINT32_MAX,.next_thread=1};return XR_XIR_VALUE_OK;
}
XR_FUNC void xir_debug_calls_free(XirDebugCalls *calls) {
    if (!calls || !calls->frames) return;
    xr_xir_domain_metadata_deallocate(calls->domain,calls->frames,(uint64_t)calls->capacity*sizeof(*calls->frames));
    *calls=(XirDebugCalls){0};
}
XR_FUNC XrXirValueStatus xir_debug_calls_pay(XirDebugCalls *calls) {
    if (!calls || !calls->frames) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t debt=calls->debt;calls->debt=0;
    if (debt && !xr_xir_domain_work(calls->domain,debt) && calls->failure==XR_XIR_VALUE_OK) calls->failure=XR_XIR_VALUE_LIMIT;
    if (calls->failure!=XR_XIR_VALUE_OK) return calls->failure;
    if (!xr_xir_domain_work(calls->domain,calls->capacity)) { calls->failure=XR_XIR_VALUE_LIMIT;return calls->failure; }
    /* The internal Instance driver identifies the stable logical root thread.
     * Frame permissions still use the entire binding, including its ticket. */
    for (uint32_t i=0;i<calls->capacity;++i) {
        XirDebugCallFrame *root=&calls->frames[i];
        if (!root->active || root->event.parent || root->event.entry!=calls->functions) continue;
        if (!xr_xir_domain_work(calls->domain,calls->capacity)) { calls->failure=XR_XIR_VALUE_LIMIT;return calls->failure; }
        for (uint32_t j=0;j<calls->capacity;++j)
            if (calls->frames[j].active && calls_binding(&root->event.binding,&calls->frames[j].event.binding)) calls->frames[j].thread=1;
    }
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xir_debug_calls_bind(XirDebugCalls *calls,const XrXirCallView *view) {
    if (!calls || !view || calls->target!=view->instance || calls->disabled) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!calls->installed) {
        calls->installing=view->activation;
        XrXirCallStatus status=xr_xir_call_observer_bind(view,&calls->observer);calls->installing=NULL;
        if (status!=XR_XIR_CALL_READY) return status==XR_XIR_CALL_LIMIT ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
        calls->installed=true;
    }
    return xir_debug_calls_pay(calls);
}
XR_FUNC const XirDebugCallFrame *xir_debug_calls_current(const XirDebugCalls *calls) {
    return calls && calls->current<calls->capacity && calls->frames[calls->current].active ? &calls->frames[calls->current] : NULL;
}
XR_FUNC const XirDebugCallFrame *xir_debug_calls_parent(const XirDebugCalls *calls,const XirDebugCallFrame *child) {
    if (!calls || !child || !child->active || !child->event.parent) return NULL;
    for (uint32_t i=0;i<calls->capacity;++i) {
        const XirDebugCallFrame *frame=&calls->frames[i];
        if (frame->active && frame->event.frame==child->event.parent && calls_binding(&frame->event.binding,&child->event.binding)) return frame;
    }
    return NULL;
}
XR_FUNC const XirDebugCallFrame *xir_debug_calls_id(const XirDebugCalls *calls,uint64_t id) {
    if (!calls || !id) return NULL;
    for (uint32_t i=0;i<calls->capacity;++i) if (calls->frames[i].active && calls->frames[i].id==id) return &calls->frames[i];
    return NULL;
}
XR_FUNC XrXirValueStatus xir_debug_calls_native_pending(XirDebugCalls *calls,const XrXirCallView *view,
    uint32_t instruction,uint32_t callee,bool invoked) {
    const XirDebugCallFrame *current=xir_debug_calls_current(calls);
    if (!current || !xr_xir_call_admission(view) || calls->target!=view->instance ||
        current->event.code!=XR_XIR_CALL_CODE_NATIVE || current->event.phase!=XR_XIR_CALL_NORMAL ||
        current->event.entry!=xr_xir_call_current_entry(view->activation) || callee>=calls->functions) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_work(calls->domain,sizeof(*current)+1)) return XR_XIR_VALUE_LIMIT;
    XirDebugCallFrame *owned=&calls->frames[calls->current];
    owned->native_instruction=instruction;owned->native_callee=callee;
    owned->native_invoked=invoked;owned->native_waiting=true;return XR_XIR_VALUE_OK;
}
