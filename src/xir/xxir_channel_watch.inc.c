/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_watch.inc.c - Frontier comparison and owned readiness notification
 */
enum { CHANNEL_WATCH_WAITING=0,CHANNEL_WATCH_NOTIFIED=1,CHANNEL_WATCH_CANCELLED=2 };
static XrXirValueStatus channel_receiver(const XrXirValue *,XrXirValueAdmission *,XirChannel **);
static void channel_watch_release(XrXirChannelWatch *watch) {
    if (!xr_xir_reference_release(&watch->references)) return;
    XR_CHECK(unit_value(&watch->owner_channel) && !watch->linked,"queue storage never releases an external watch root");
    XrXirDomain *domain=watch->domain;
    xr_xir_domain_deallocate(domain,watch,sizeof(*watch));xr_xir_domain_drop(domain);
}
static void channel_watch_unlink(XirChannel *channel,XrXirChannelWatch *watch) {
    XR_CHECK(watch->linked && channel->watch_count,"watch unlink retains its exact queue reference");
    if (watch->previous) watch->previous->next=watch->next;else channel->watch_head=watch->next;
    if (watch->next) watch->next->previous=watch->previous;else channel->watch_tail=watch->previous;
    watch->previous=watch->next=NULL;watch->linked=false;--channel->watch_count;
}
static bool channel_watch_work(XirChannel *channel,XrXirValueAdmission *admission) {
    return !channel->watch_count || channel_work(admission,channel->watch_count);
}
static void channel_watch_notify(XirChannel *channel) {
    while (channel->watch_head) {
        XrXirChannelWatch *watch=channel->watch_head;channel_watch_unlink(channel,watch);
        uint32_t expected=CHANNEL_WATCH_WAITING;
        (void)atomic_compare_exchange_strong_explicit(&watch->state,&expected,CHANNEL_WATCH_NOTIFIED,
            memory_order_release,memory_order_relaxed);
        channel_watch_release(watch);
    }
}
static void channel_watch_dispose_queue(XirChannel *channel) {
    while (channel->watch_head) {
        XrXirChannelWatch *watch=channel->watch_head;
        XR_CHECK(atomic_load_explicit(&watch->state,memory_order_acquire)==CHANNEL_WATCH_CANCELLED &&
            unit_value(&watch->owner_channel),"last Channel owner retains no active external watch");
        channel_watch_unlink(channel,watch);channel_watch_release(watch);
    }
    XR_CHECK(!channel->watch_count && !channel->watch_tail,"watch destruction consumes the complete queue");
}
XR_FUNC bool xr_xir_channel_watch_ready(const XrXirChannelWatch *watch) {
    return watch && atomic_load_explicit(&watch->state,memory_order_acquire)==CHANNEL_WATCH_NOTIFIED;
}
XR_FUNC bool xr_xir_channel_watch_cancel_pending(XrXirChannelWatch *watch) {
    if (!watch || unit_value(&watch->owner_channel)) return false;
    xr_xir_value_graph_begin();uint32_t expected=CHANNEL_WATCH_WAITING;
    bool won=atomic_compare_exchange_strong_explicit(&watch->state,&expected,CHANNEL_WATCH_CANCELLED,
        memory_order_release,memory_order_relaxed);
    xr_xir_value_graph_end();return won;
}
XR_FUNC void xr_xir_channel_watch_drop(XrXirChannelWatch *watch) {
    if (!watch) return;
    xr_xir_value_graph_begin();
    XrXirValue channel_value=watch->owner_channel;watch->owner_channel=(XrXirValue){0};
    atomic_store_explicit(&watch->state,CHANNEL_WATCH_CANCELLED,memory_order_release);
    XirChannel *channel=(XirChannel *)object_pointer(&channel_value);bool release=false;
    if (channel_lock(channel)) {
        if (watch->linked) { channel_watch_unlink(channel,watch);release=true; }
        channel_unlock(channel);
    }
    if (release) channel_watch_release(watch);
    xr_xir_value_drop(&channel_value);channel_watch_release(watch);xr_xir_value_graph_end();
}
XR_FUNC XrXirValueStatus xr_xir_channel_observe(const XrXirValue *value,
    XrXirValueAdmission *admission,XrXirChannelObservation *output,XrXirChannelState *state) {
    if (!output || !state || !admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XirChannel *channel=NULL;
    XrXirValueStatus status=channel_receiver(value,admission,&channel);
    if (status==XR_XIR_VALUE_OK) {
        *state=XR_XIR_CHANNEL_RETRY;
        if (channel_lock(channel)) {
            *output=(XrXirChannelObservation){channel->sequence,channel->buffer.capacity,channel->buffer.count,
                atomic_load_explicit(&channel->closed,memory_order_relaxed)};
            channel_unlock(channel);*state=XR_XIR_CHANNEL_VALUE;
        }
    }
    xr_xir_value_graph_end();return status;
}
static XrXirValueStatus channel_watch_register(XirChannel *channel,const XrXirValue *value,uint64_t sequence,
    XrXirValueAdmission *admission,XrXirChannelWatch **output,XrXirChannelWatchState *state) {
    if (!channel_lock(channel)) { *state=XR_XIR_CHANNEL_WATCH_RETRY;return XR_XIR_VALUE_OK; }
    bool changed=channel->sequence!=sequence || atomic_load_explicit(&channel->closed,memory_order_relaxed);
    bool full=channel->watch_count==UINT32_MAX;channel_unlock(channel);
    if (changed) { *state=XR_XIR_CHANNEL_WATCH_READY;return XR_XIR_VALUE_OK; }
    if (full || !channel_work(admission,1)) return XR_XIR_VALUE_LIMIT;
    XrXirValue owned={0};XrXirValueStatus status=xr_xir_value_copy(value,&owned);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (!xr_xir_domain_retain(admission->domain)) { xr_xir_value_drop(&owned);return XR_XIR_VALUE_REFCOUNT_LIMIT; }
    XrXirChannelWatch *watch=xr_xir_domain_allocate(admission->domain,sizeof(*watch),&status);
    if (!watch) { xr_xir_domain_drop(admission->domain);xr_xir_value_drop(&owned);return status; }
    *watch=(XrXirChannelWatch){0};atomic_init(&watch->references,1);atomic_init(&watch->state,CHANNEL_WATCH_WAITING);
    watch->domain=admission->domain;watch->owner_channel=owned;
    XrXirChannelWatchState outcome=XR_XIR_CHANNEL_WATCH_RETRY;
    if (channel_lock(channel)) {
        if (channel->sequence!=sequence || atomic_load_explicit(&channel->closed,memory_order_relaxed)) outcome=XR_XIR_CHANNEL_WATCH_READY;
        else if (channel->watch_count==UINT32_MAX) status=XR_XIR_VALUE_LIMIT;
        else {
            atomic_store_explicit(&watch->references,2,memory_order_relaxed);watch->linked=true;
            watch->previous=channel->watch_tail;
            if (channel->watch_tail) channel->watch_tail->next=watch;else channel->watch_head=watch;
            channel->watch_tail=watch;++channel->watch_count;outcome=XR_XIR_CHANNEL_WATCH_PENDING;
        }
        channel_unlock(channel);
    }
    if (status==XR_XIR_VALUE_OK && outcome==XR_XIR_CHANNEL_WATCH_PENDING) *output=watch;
    else xr_xir_channel_watch_drop(watch);
    if (status==XR_XIR_VALUE_OK) *state=outcome;
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_channel_watch_begin(const XrXirValue *value,uint64_t sequence,
    XrXirValueAdmission *admission,XrXirChannelWatch **output,XrXirChannelWatchState *state) {
    if (!output || *output || !state || !admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XirChannel *channel=NULL;
    XrXirValueStatus status=channel_receiver(value,admission,&channel);
    if (status==XR_XIR_VALUE_OK) status=channel_watch_register(channel,value,sequence,admission,output,state);
    xr_xir_value_graph_end();return status;
}
