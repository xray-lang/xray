/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_rendezvous.inc.c - Owned pending sends and receive-before-ack commit
 *
 * KEY CONCEPT:
 *   Queue references own payloads; only the unique external owner roots Channel.
 *   A receive prepares its actual result before acknowledging a pending sender.
 */
enum { CHANNEL_SEND_WAITING,CHANNEL_SEND_SENT,CHANNEL_SEND_CLOSED,CHANNEL_SEND_CANCELLED,CHANNEL_SEND_COMMITTING };
static void channel_send_wait_release(XrXirChannelSendWait *send) {
    if (!send || !xr_xir_reference_release(&send->references)) return;
    XR_CHECK(!send->linked && unit_value(&send->owner_channel),"final pending send has no queue or external Channel root");
    XrXirValue message=send->message;XrXirDomain *domain=send->domain;
    xr_xir_domain_deallocate(domain,send,sizeof(*send));
    xr_xir_value_drop(&message);xr_xir_domain_drop(domain);
}
static void channel_send_wait_release_list(XrXirChannelSendWait *list) {
    while (list) {
        XrXirChannelSendWait *send=list;list=send->release_next;send->release_next=NULL;
        channel_send_wait_release(send);
    }
}
static void channel_send_wait_unlink(XirChannel *channel,XrXirChannelSendWait *send) {
    XR_CHECK(send->linked && channel->send_count,"pending send unlink retains one exact queue reference");
    if (send->previous) send->previous->next=send->next;else channel->send_head=send->next;
    if (send->next) send->next->previous=send->previous;else channel->send_tail=send->previous;
    send->previous=send->next=NULL;send->linked=false;--channel->send_count;
    graph_lock();graph_dirty(channel->object.domain);graph_unlock();
}
static bool channel_send_wait_work(XirChannel *channel,XrXirValueAdmission *admission) {
    return !channel->send_count || channel_work(admission,channel->send_count);
}
static void channel_send_wait_defer(XrXirChannelSendWait *send,XrXirChannelSendWait **list) {
    send->release_next=*list;*list=send;
}
static bool channel_send_wait_prepare_prune(XirChannel *channel,XrXirChannelSendWait *skip,
    XrXirValueAdmission *admission,uint32_t *count) {
    *count=0;
    for (XrXirChannelSendWait *send=channel->send_head;send;send=send->next) {
        if (send==skip) continue;
        if (!channel_work(admission,1)) return false;
        if (atomic_load_explicit(&send->state,memory_order_acquire)!=CHANNEL_SEND_CANCELLED) break;
        if (!channel_work(admission,1)) return false;
        ++*count;
    }
    return true;
}
static void channel_send_wait_prune(XirChannel *channel,uint32_t count,XrXirChannelSendWait **released) {
    while (count--) {
        XrXirChannelSendWait *send=channel->send_head;channel_send_wait_unlink(channel,send);
        XR_CHECK(atomic_load_explicit(&send->state,memory_order_acquire)==CHANNEL_SEND_CANCELLED,"prune consumes only its prepaid cancelled prefix");
        channel_send_wait_defer(send,released);
    }
}
static void channel_send_wait_close(XirChannel *channel,XrXirChannelSendWait **released) {
    while (channel->send_head) {
        XrXirChannelSendWait *send=channel->send_head;
        uint32_t expected=CHANNEL_SEND_WAITING;
        (void)atomic_compare_exchange_strong_explicit(&send->state,&expected,CHANNEL_SEND_CLOSED,
            memory_order_release,memory_order_relaxed);
        XR_CHECK(expected==CHANNEL_SEND_WAITING || expected==CHANNEL_SEND_CANCELLED,"only waiting or cancelled sends remain queued");
        channel_send_wait_unlink(channel,send);channel_send_wait_defer(send,released);
    }
}
static void channel_send_wait_dispose_queue(XirChannel *channel) {
    while (channel->send_head) {
        XrXirChannelSendWait *send=channel->send_head;
        XR_CHECK(atomic_load_explicit(&send->state,memory_order_acquire)==CHANNEL_SEND_CANCELLED &&
            unit_value(&send->owner_channel),"last Channel root retains only cancelled queue owners");
        channel_send_wait_unlink(channel,send);channel_send_wait_release(send);
    }
    XR_CHECK(!channel->send_count && !channel->send_tail,"Channel destruction consumes every pending send owner");
}
XR_FUNC XrXirChannelSendWaitState xr_xir_channel_send_wait_state(const XrXirChannelSendWait *send) {
    if (!send) return XR_XIR_CHANNEL_SEND_WAIT_RETRY;
    uint32_t state=atomic_load_explicit(&send->state,memory_order_acquire);
    /* The receiver owns its complete result before claiming COMMITTING.
     * No fallible preparation remains after that linearization point. */
    return state==CHANNEL_SEND_SENT || state==CHANNEL_SEND_COMMITTING ? XR_XIR_CHANNEL_SEND_WAIT_SENT : state==CHANNEL_SEND_CLOSED ? XR_XIR_CHANNEL_SEND_WAIT_CLOSED :
        state==CHANNEL_SEND_CANCELLED ? XR_XIR_CHANNEL_SEND_WAIT_CANCELLED : XR_XIR_CHANNEL_SEND_WAIT_PENDING;
}
XR_FUNC bool xr_xir_channel_send_wait_owner(const XrXirChannelSendWait *send,const XrXirValueAdmission *admission) {
    return send && admission_domain_current(admission) && send->domain==admission->domain &&
        xr_xir_value_argument(&send->owner_channel,admission->arena,(XrXirType)send->owner_channel.type);
}
XR_FUNC bool xr_xir_channel_send_wait_cancel_pending(XrXirChannelSendWait *send) {
    if (!send || unit_value(&send->owner_channel)) return false;
    xr_xir_value_graph_begin();uint32_t expected=CHANNEL_SEND_WAITING;
    bool won=atomic_compare_exchange_strong_explicit(&send->state,&expected,CHANNEL_SEND_CANCELLED,
        memory_order_release,memory_order_relaxed);
    xr_xir_value_graph_end();return won;
}
XR_FUNC void xr_xir_channel_send_wait_drop(XrXirChannelSendWait *send) {
    if (!send) return;
    xr_xir_value_graph_begin();XrXirValue owner=send->owner_channel;send->owner_channel=(XrXirValue){0};
    uint32_t expected=CHANNEL_SEND_WAITING;
    (void)atomic_compare_exchange_strong_explicit(&send->state,&expected,CHANNEL_SEND_CANCELLED,
        memory_order_release,memory_order_relaxed);
    XirChannel *channel=(XirChannel *)object_pointer(&owner);bool unlinked=false;
    if (channel_lock(channel)) {
        if (send->linked) { channel_send_wait_unlink(channel,send);unlinked=true; }
        channel_unlock(channel);
    }
    if (unlinked) channel_send_wait_release(send);
    xr_xir_value_drop(&owner);channel_send_wait_release(send);xr_xir_value_graph_end();
}
static XrXirValueStatus channel_send_wait_new(const XrXirValue *channel,const XrXirValue *message,
    XrXirValueAdmission *admission,XrXirChannelSendWait **output) {
    XrXirValue payload={0},owner={0};XrXirValueStatus status=xr_xir_value_copy(message,&payload);
    if (status==XR_XIR_VALUE_OK) status=xr_xir_value_copy(channel,&owner);
    bool retained=status==XR_XIR_VALUE_OK && xr_xir_domain_retain(admission->domain);
    if (status==XR_XIR_VALUE_OK && !retained) status=XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirChannelSendWait *send=retained ? xr_xir_domain_allocate(admission->domain,sizeof(*send),&status) : NULL;
    if (!send) {
        if (retained) xr_xir_domain_drop(admission->domain);
        xr_xir_value_drop(&owner);xr_xir_value_drop(&payload);return status;
    }
    *send=(XrXirChannelSendWait){0};atomic_init(&send->references,1);atomic_init(&send->state,CHANNEL_SEND_WAITING);
    send->domain=admission->domain;send->owner_channel=owner;send->message=payload;*output=send;
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus channel_rendezvous_send_begin(const XrXirValue *value,const XrXirValue *message,
    XrXirValueAdmission *admission,XrXirChannelSendWait **output,XrXirChannelSendWaitState *state) {
    XirChannel *channel=NULL;XrXirValueStatus status=channel_receiver(value,admission,&channel);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (channel->buffer.capacity) return XR_XIR_VALUE_BAD_ARGUMENT;
    status=channel_element_admit(channel,message,admission);if (status!=XR_XIR_VALUE_OK) return status;
    if (!channel_lock(channel)) { *state=XR_XIR_CHANNEL_SEND_WAIT_RETRY;return XR_XIR_VALUE_OK; }
    bool closed=atomic_load_explicit(&channel->closed,memory_order_relaxed);channel_unlock(channel);
    if (closed) { *state=XR_XIR_CHANNEL_SEND_WAIT_CLOSED;return XR_XIR_VALUE_OK; }
    if (!channel_work(admission,4)) return XR_XIR_VALUE_LIMIT;
    XrXirChannelSendWait *send=NULL;status=channel_send_wait_new(value,message,admission,&send);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirClassPublication publication={0};status=xr_xir_class_publication_prepare(&send->message,1,admission,&publication);
    XrXirChannelSendWaitState outcome=XR_XIR_CHANNEL_SEND_WAIT_RETRY;XrXirChannelSendWait *released=NULL;
    if (status==XR_XIR_VALUE_OK && channel_lock(channel)) {
        uint32_t prune=0;bool receiver=false;
        if (atomic_load_explicit(&channel->closed,memory_order_relaxed)) outcome=XR_XIR_CHANNEL_SEND_WAIT_CLOSED;
        else if (!channel_recv_wait_available(channel,admission,&receiver)) status=XR_XIR_VALUE_LIMIT;
        else if (receiver) outcome=XR_XIR_CHANNEL_SEND_WAIT_RETRY;
        else if (channel->sequence==UINT64_MAX || channel->send_count==UINT32_MAX ||
            !channel_work(admission,1) || !channel_send_wait_prepare_prune(channel,NULL,admission,&prune) || !channel_watch_work(channel,admission))
            status=XR_XIR_VALUE_LIMIT;
        else {
            channel_send_wait_prune(channel,prune,&released);xr_xir_class_publication_commit(&publication);
            atomic_store_explicit(&send->references,2,memory_order_relaxed);send->linked=true;
            send->previous=channel->send_tail;
            if (channel->send_tail) channel->send_tail->next=send;else channel->send_head=send;
            channel->send_tail=send;++channel->send_count;channel_changed(channel);outcome=XR_XIR_CHANNEL_SEND_WAIT_PENDING;
        }
        channel_unlock(channel);
    }
    xr_xir_class_publication_drop(&publication);channel_send_wait_release_list(released);
    if (status==XR_XIR_VALUE_OK && outcome==XR_XIR_CHANNEL_SEND_WAIT_PENDING) *output=send;
    else xr_xir_channel_send_wait_drop(send);
    if (status==XR_XIR_VALUE_OK) *state=outcome;return status;
}
XR_FUNC XrXirValueStatus xr_xir_channel_rendezvous_send_begin(const XrXirValue *value,const XrXirValue *message,
    XrXirValueAdmission *admission,XrXirChannelSendWait **output,XrXirChannelSendWaitState *state) {
    if (!admission_domain_current(admission) || !output || *output || !message || !state) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XrXirValueStatus status=channel_rendezvous_send_begin(value,message,admission,output,state);
    xr_xir_value_graph_end();return status;
}
/* The caller holds the queue lock only while retaining an immutable snapshot.
 * Cancellation never mutates message storage before every such reference dies. */
static XrXirValueStatus channel_receive_pending_prepare(XirChannel *channel,XrXirValueAdmission *admission,
    XrXirChannelSendWait **sender,XrXirValue *message) {
    for (XrXirChannelSendWait *send=channel->send_head;send;send=send->next) {
        if (!channel_work(admission,1)) return XR_XIR_VALUE_LIMIT;
        if (atomic_load_explicit(&send->state,memory_order_acquire)!=CHANNEL_SEND_WAITING) continue;
        if (channel->sequence==UINT64_MAX) return XR_XIR_VALUE_LIMIT;
        if (!xr_xir_reference_retain(&send->references)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
        *sender=send;return xr_xir_value_copy(&send->message,message);
    }
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus channel_receive_pending_commit(XirChannel *channel,XrXirChannelSendWait *send,uint64_t sequence,
    XrXirValueAdmission *admission,XrXirValue *output,XrXirChannelState *state,XrXirChannelSendWait **released) {
    if (channel->sequence!=sequence || !send->linked ||
        atomic_load_explicit(&send->state,memory_order_acquire)!=CHANNEL_SEND_WAITING) {
        *state=XR_XIR_CHANNEL_RETRY;return XR_XIR_VALUE_OK;
    }
    uint32_t prune=0;
    if (!channel_send_wait_prepare_prune(channel,send,admission,&prune) || !channel_watch_work(channel,admission)) return XR_XIR_VALUE_LIMIT;
    uint32_t expected=CHANNEL_SEND_WAITING;
    if (!atomic_compare_exchange_strong_explicit(&send->state,&expected,CHANNEL_SEND_COMMITTING,
        memory_order_acquire,memory_order_relaxed)) { *state=XR_XIR_CHANNEL_RETRY;return XR_XIR_VALUE_OK; }
    *output=send->message;send->message=(XrXirValue){0};channel_send_wait_unlink(channel,send);
    channel_send_wait_defer(send,released);channel_send_wait_prune(channel,prune,released);
    atomic_store_explicit(&send->state,CHANNEL_SEND_SENT,memory_order_release);channel_changed(channel);*state=XR_XIR_CHANNEL_VALUE;
    return XR_XIR_VALUE_OK;
}
