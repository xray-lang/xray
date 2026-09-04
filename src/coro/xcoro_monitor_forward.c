/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_monitor_forward.c - Scheduler projection for monitor forwarding
 */

#include "xcoro_monitor_forward.h"

#include "xblock.h"
#include "xchannel.h"
#include "xcoro_registry.h"
#include "xcoroutine.h"
#include "xyieldable.h"
#include "../base/xmalloc.h"
#include "../runtime/xisolate_internal.h"

typedef struct XrCoroMonitorForwardContext {
    XrChannel *channel;
    XrCoroMonitorForwardNotify notify;
    XrCoroMonitorForwardDestroy destroy;
    void *user_context;
    XrValue reason;
} XrCoroMonitorForwardContext;

static void coro_monitor_forward_destroy(void *context) {
    XrCoroMonitorForwardContext *forward = (XrCoroMonitorForwardContext *) context;
    if (!forward)
        return;
    if (forward->channel)
        xr_channel_close(forward->channel);
    if (forward->destroy)
        forward->destroy(forward->user_context);
    xr_free(forward);
}

static XrCFuncResult coro_monitor_forward_complete(XrCoroMonitorForwardContext *forward) {
    forward->notify(forward->user_context, forward->reason);
    return XR_CFUNC_DONE;
}

static XrCFuncResult coro_monitor_forward_continue(XrVMRuntime *isolate, int status,
                                                   XrValue resume_value, void *context,
                                                   XrValue *result) {
    (void) resume_value;
    (void) result;
    XrCoroMonitorForwardContext *forward = (XrCoroMonitorForwardContext *) context;
    if (!forward || status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR)
        return XR_CFUNC_DONE;
    XrCoroutine *coroutine = xr_current_coro(isolate);
    XrCoroBlockResult resumed =
        xr_coro_chan_recv_resume(coroutine, xr_slot_xvalue_ptr(&forward->reason), xr_slot_none());
    if (resumed.kind == XR_CORO_BLOCK_READY)
        return coro_monitor_forward_complete(forward);
    if (resumed.kind == XR_CORO_BLOCK_CLOSED)
        return XR_CFUNC_DONE;
    return XR_CFUNC_ERROR;
}

static XrCFuncResult coro_monitor_forward_entry(XrVMRuntime *isolate, void *context,
                                                XrValue *result) {
    (void) result;
    XrCoroMonitorForwardContext *forward = (XrCoroMonitorForwardContext *) context;
    if (!forward)
        return XR_CFUNC_DONE;
    bool ready = false;
    forward->reason = xr_channel_try_recv(forward->channel, &ready);
    if (ready)
        return coro_monitor_forward_complete(forward);
    if (xr_channel_is_closed(forward->channel))
        return XR_CFUNC_DONE;
    if (!xr_yield_set_continuation(isolate, coro_monitor_forward_continue, forward))
        return XR_CFUNC_ERROR;
    XrCoroutine *coroutine = xr_current_coro(isolate);
    XrChanResult received =
        xr_channel_recv_slot(forward->channel, &forward->reason, coroutine, -1,
                             xr_slot_xvalue_ptr(&forward->reason), xr_slot_none(), false);
    if (received == XR_CHAN_BLOCK)
        return XR_CFUNC_BLOCKED;
    if (received == XR_CHAN_OK)
        return coro_monitor_forward_complete(forward);
    return received == XR_CHAN_CLOSED ? XR_CFUNC_DONE : XR_CFUNC_ERROR;
}

bool xr_coro_monitor_forward(XrVMRuntime *isolate, const char *coroutine_name,
                             XrCoroMonitorForwardNotify notify, void *context,
                             XrCoroMonitorForwardDestroy destroy) {
    if (!isolate || !coroutine_name || !notify) {
        if (destroy)
            destroy(context);
        return false;
    }
    XrCoroState *scheduler = (XrCoroState *) isolate->vm.coro_state;
    if (!scheduler || !scheduler->coro_registry) {
        if (destroy)
            destroy(context);
        return false;
    }
    XrChannel *channel = xr_coro_monitor(isolate, scheduler->coro_registry, coroutine_name);
    if (!channel) {
        if (destroy)
            destroy(context);
        return false;
    }
    XrCoroMonitorForwardContext *forward =
        (XrCoroMonitorForwardContext *) xr_calloc(1, sizeof(*forward));
    if (!forward) {
        xr_channel_close(channel);
        if (destroy)
            destroy(context);
        return false;
    }
    forward->channel = channel;
    forward->notify = notify;
    forward->destroy = destroy;
    forward->user_context = context;
    forward->reason = xr_null();
    XrCoroutine *coroutine =
        xr_coro_create_native_yieldable(isolate, coro_monitor_forward_entry, forward,
                                        coro_monitor_forward_destroy, "coro_monitor_forward");
    if (!coroutine) {
        coro_monitor_forward_destroy(forward);
        return false;
    }
    xr_coro_spawn(isolate, coroutine);
    return true;
}
