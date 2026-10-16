/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_call_wait.c - Real Call ownership, queue events and rejection
 */
#pragma push_macro("main")
#undef main
#define main channel_results_reference_main
#include "test_xir_channel_results.c"
#pragma pop_macro("main")
#include "xir/xxir_channel_watch_internal.h"
#include "xir/xxir_call_internal.h"
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc channel_runtime_malloc
#define xr_calloc channel_runtime_calloc
#define xr_free channel_runtime_free
#include "xir/xxir_call.c"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")

enum { CALL_WATCH_NORMAL, CALL_WATCH_FORGED, CALL_WATCH_WRONG_ACTION, CALL_WATCH_CANCEL, CALL_WATCH_CHANGED,
    CALL_WATCH_BAD_VALUE,CALL_WATCH_BAD_FLAGS,CALL_WATCH_BAD_CALLEE,CALL_WATCH_BAD_PANIC,CALL_WATCH_BAD_RESERVED };
typedef struct ChannelCallWitness {
    XrXirValue *channel;
    uint64_t sequence;
    uint32_t mode, callbacks, releases, code_releases;
    XrXirCallStatus staged, released;
} ChannelCallWitness;
static XrXirAction channel_call_action(XrXirActionKind kind, XrXirValue value) {
    return (XrXirAction){.kind=kind,.value=value};
}
static XrXirAction channel_call_entry(XrXirCallView *view) {
    ChannelCallWitness *w=view->environment ? (ChannelCallWitness *)view->environment : NULL;
    CHECK(w); ++w->callbacks;
    const XrXirValue *channel=view->argument_count ? &view->arguments[0] : w->channel;
    CHECK(!view->argument_count || (view->argument_count==1 && channel->type==RESULT_CHANNEL_I64));
    uint32_t *phase=view->state;
    if (!*phase) {
        if (w->mode==CALL_WATCH_FORGED) return channel_call_action(XR_XIR_ACTION_AWAIT_CHANNEL,(XrXirValue){0});
        XrXirCallView copy=*view;
        CHECK(xr_xir_call_watch_channel(&copy,channel,w->sequence)==XR_XIR_CALL_BAD_STATE);
        w->staged=xr_xir_call_watch_channel(view,channel,w->sequence);
        if (w->staged==XR_XIR_CALL_BUSY) return channel_call_action(XR_XIR_ACTION_CONTINUE,(XrXirValue){0});
        if (w->staged==XR_XIR_CALL_OOM || w->staged==XR_XIR_CALL_LIMIT)
            return xr_xir_call_fault(w->staged==XR_XIR_CALL_OOM ? XR_XIR_RUN_OUT_OF_MEMORY : XR_XIR_RUN_FRAME_LIMIT);
        if (w->mode==CALL_WATCH_CHANGED) {
            CHECK(w->staged==XR_XIR_CALL_READY);
            return channel_call_action(XR_XIR_ACTION_RETURN,result_integer(17));
        }
        CHECK(w->staged==XR_XIR_CALL_SUSPENDED);
        CHECK(xr_xir_call_watch_channel(view,channel,w->sequence)==XR_XIR_CALL_BAD_STATE);
        ++*phase;
        if (w->mode==CALL_WATCH_CANCEL)
            CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_CANCEL_REQUESTED);
        XrXirAction action=channel_call_action(w->mode==CALL_WATCH_WRONG_ACTION ? XR_XIR_ACTION_CONTINUE : XR_XIR_ACTION_AWAIT_CHANNEL,
            (XrXirValue){0});
        if (w->mode==CALL_WATCH_BAD_VALUE) action.value=result_integer(1);
        if (w->mode==CALL_WATCH_BAD_FLAGS) action.flags=XR_XIR_ACTION_PROTECTED;
        if (w->mode==CALL_WATCH_BAD_CALLEE) action.callee=1;
        if (w->mode==CALL_WATCH_BAD_PANIC) action.panic.detail.code=XR_XIR_PANIC_RANGE;
        if (w->mode==CALL_WATCH_BAD_RESERVED) action.value.reserved=1;
        return action;
    }
    CHECK(view->inbox.status==XR_XIR_CALL_RETURNED && result_empty(&view->inbox.value));
    return channel_call_action(XR_XIR_ACTION_RETURN,result_integer(17));
}
static void channel_call_release(XrXirCallView *view,XrXirCallStatus reason) {
    ChannelCallWitness *w=(ChannelCallWitness *)view->environment;++w->releases;w->released=reason;
}
static uint64_t channel_call_sequence(ResultOwner *owner,const XrXirValue *channel) {
    XrXirChannelObservation observed={0};XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    CHECK(xr_xir_channel_observe(channel,&owner->admission,&observed,&state)==XR_XIR_VALUE_OK && state==XR_XIR_CHANNEL_VALUE);
    return observed.sequence;
}
static XrXirCall *channel_call_new(ResultOwner *owner,ChannelCallWitness *w,XrXirCallAccounting *accounting) {
    XrXirCallEntry entry={XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,sizeof(uint32_t),
        channel_call_entry,channel_call_release,w,0,0};
    XrXirCallConfig config={0};CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.accounting=accounting;config.admission=owner->admission;
    XrXirCall *call=NULL;CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY);return call;
}
static void channel_call_event(unsigned event) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0},output={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    if (event==1) { XrXirValue message=result_integer(93);result_send(&owner,&channel,&message); }
    ChannelCallWitness w={.channel=&channel,.sequence=channel_call_sequence(&owner,&channel)};
    XrXirCallAccounting accounting={0};XrXirCall *call=channel_call_new(&owner,&w,&accounting);
    XrXirCallResult pending=xr_xir_call_poll_bounded(call,1);CHECK(pending.status==XR_XIR_CALL_SUSPENDED && pending.wake);
    XrXirWaitRequest request={0};CHECK(xr_xir_call_wait_request(call,pending.wake,&request)==XR_XIR_CALL_READY &&
        request.kind==XR_XIR_WAIT_CHANNEL && !request.subject && !request.generation && !request.ticket && !request.after_ms);
    size_t attempts=runtime_memory.attempts;
    for (unsigned i=0;i<256;++i) CHECK(xr_xir_call_poll_bounded(call,1).status==XR_XIR_CALL_SUSPENDED);
    CHECK(w.callbacks==1 && accounting.polls==1 && runtime_memory.attempts==attempts);
    CHECK(xr_xir_call_resume(call,pending.wake)==XR_XIR_CALL_BAD_STATE);
    XrXirChannelState state=XR_XIR_CHANNEL_RETRY;
    if (!event) { XrXirValue message=result_integer(81);result_send(&owner,&channel,&message); }
    else if (event==1) CHECK(xr_xir_channel_receive(&channel,&owner.admission,&output,&state)==XR_XIR_VALUE_OK &&
        state==XR_XIR_CHANNEL_VALUE && output.payload==93);
    else CHECK(xr_xir_channel_close(&channel,&owner.admission,&state)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_call_channel_ready(call) && xr_xir_call_resume(call,pending.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_call_poll_bounded(call,1).status==XR_XIR_CALL_READY && w.callbacks==1);
    XrXirCallResult result=xr_xir_call_poll_bounded(call,4);
    CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.payload==17 && w.callbacks==2 && w.releases==1);
    CHECK(xr_xir_call_resume(call,pending.wake)==XR_XIR_CALL_BAD_STATE && !xr_xir_call_channel_waiting(call));
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && !accounting.live_bytes);
    xr_xir_value_drop(&output);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void channel_call_reject(unsigned mode) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    ChannelCallWitness w={.channel=&channel,.sequence=channel_call_sequence(&owner,&channel),.mode=mode};
    XrXirCallAccounting accounting={0};XrXirCall *call=channel_call_new(&owner,&w,&accounting);
    XrXirCallStatus expected=mode==CALL_WATCH_CANCEL ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_BAD_STATE;
    CHECK(xr_xir_call_poll_bounded(call,8).status==expected && w.releases==1 && w.released==expected);
    CHECK(!((XirChannel *)object_pointer(&channel))->watch_count && !xr_xir_call_channel_waiting(call));
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
static void channel_call_cancel(bool free_directly,bool last_owner) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    ChannelCallWitness w={.channel=&channel,.sequence=channel_call_sequence(&owner,&channel)};
    XrXirCallAccounting accounting={0};XrXirCall *call=channel_call_new(&owner,&w,&accounting);
    XrXirCallResult pending=xr_xir_call_poll_bounded(call,1);CHECK(pending.status==XR_XIR_CALL_SUSPENDED);
    XirChannel *storage=(XirChannel *)object_pointer(&channel);atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    if (last_owner) xr_xir_value_drop(&channel);
    if (!free_directly) {
        CHECK(xr_xir_call_request_cancel(call)==XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_call_resume(call,pending.wake)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_poll_bounded(call,8).status==XR_XIR_CALL_CANCELLED);
    }
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && w.releases==1 && w.released==XR_XIR_CALL_CANCELLED);
    if (!last_owner) {
        atomic_store_explicit(&storage->locked,false,memory_order_release);
        XrXirValue message=result_integer(57);result_send(&owner,&channel,&message);
        CHECK(!storage->watch_count);xr_xir_value_drop(&channel);
    }
    result_owner_free(&owner);
}
static void channel_call_registration(bool oom,bool changed) {
    ResultOwner owner={0};result_owner_ready(&owner);XrXirValue channel={0};
    CHECK(xr_xir_channel_new((XrXirType)RESULT_CHANNEL_I64,1,&owner.admission,&channel)==XR_XIR_VALUE_OK);
    ChannelCallWitness w={.channel=&channel,.sequence=channel_call_sequence(&owner,&channel),
        .mode=changed ? CALL_WATCH_CHANGED : CALL_WATCH_NORMAL};
    XrXirCallAccounting accounting={0};XrXirCall *call=channel_call_new(&owner,&w,&accounting);
    XirChannel *storage=(XirChannel *)object_pointer(&channel);
    if (changed) { XrXirValue message=result_integer(71);result_send(&owner,&channel,&message); }
    size_t attempts=runtime_memory.attempts;
    if (oom) runtime_memory.fail_at=attempts;
    else if (!changed) atomic_store_explicit(&storage->locked,true,memory_order_relaxed);
    XrXirCallResult result=xr_xir_call_poll_bounded(call,oom || changed ? 8 : 1);
    runtime_memory.fail_at=SIZE_MAX;
    if (oom) CHECK(result.status==XR_XIR_CALL_OOM && w.staged==XR_XIR_CALL_OOM && !storage->watch_count);
    else if (changed) CHECK(result.status==XR_XIR_CALL_RETURNED && runtime_memory.attempts==attempts && !storage->watch_count);
    else {
        CHECK(result.status==XR_XIR_CALL_READY && w.staged==XR_XIR_CALL_BUSY && !storage->watch_count && runtime_memory.attempts==attempts);
        atomic_store_explicit(&storage->locked,false,memory_order_release);
        CHECK(xr_xir_call_poll_bounded(call,1).status==XR_XIR_CALL_SUSPENDED && storage->watch_count==1);
    }
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);xr_xir_value_drop(&channel);result_owner_free(&owner);
}
#include "xir_channel_task_wait_cases.h"
#include "xir_channel_call_wait_controls.h"
int main(void) {
    _Static_assert(XR_XIR_VALUE_ABI_VERSION==29 && XR_XIR_CALL_ABI_VERSION==33 && XR_XIR_PROGRAM_ABI_VERSION==34,
        "Owned Channel waits require the exact driver and Program generation");
    for (unsigned event=0;event<3;++event) channel_call_event(event);
    channel_call_reject(CALL_WATCH_FORGED);channel_call_reject(CALL_WATCH_WRONG_ACTION);channel_call_reject(CALL_WATCH_CANCEL);
    for (unsigned mode=CALL_WATCH_BAD_VALUE;mode<=CALL_WATCH_BAD_RESERVED;++mode) channel_call_reject(mode);
    for (unsigned direct=0;direct<2;++direct) for (unsigned last=0;last<2;++last) channel_call_cancel(direct!=0,last!=0);
    channel_call_registration(true,false);channel_call_registration(false,false);channel_call_registration(false,true);
    channel_task_wait_cases();
    channel_call_wait_controls();
    puts("Channel Call/Task waits: real events, authenticated ownership, bounded wake and cancellation passed");return 0;
}
