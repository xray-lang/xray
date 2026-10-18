/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_recv_wait.inc.c - Owned native receive reservation and handoff
 *
 * KEY CONCEPT:
 *   Unpublished storage owns no payload. A sender admits its complete Sendable
 *   value before transferring it into the exact same immutable type arena.
 */
enum { CHANNEL_RECV_WAITING,CHANNEL_RECV_VALUE,CHANNEL_RECV_CLOSED,CHANNEL_RECV_CANCELLED,CHANNEL_RECV_COMMITTING };
static bool channel_recv_wait_available(XirChannel *channel,XrXirValueAdmission *admission,bool *available) {
    *available=false;
    for (XrXirChannelRecvWait *recv=channel->recv_head;recv;recv=recv->next) {
        if (!channel_work(admission,1)) return false;
        if (atomic_load_explicit(&recv->state,memory_order_acquire)==CHANNEL_RECV_WAITING) { *available=true;break; }
    }
    return true;
}
static void channel_recv_wait_release(XrXirChannelRecvWait *recv) {
    if (!recv || !xr_xir_reference_release(&recv->references)) return;
    XR_CHECK(!recv->linked && unit_value(&recv->owner_channel),"final receive wait has no queue or external Channel root");
    XrXirValue closed=recv->closed,result=recv->result;XrXirDomain *domain=recv->domain;
    if (recv->reservation) constructed_discard(&recv->reservation->object,sizeof(XirNominalValue)+sizeof(XrXirValue));
    xr_xir_domain_deallocate(domain,recv,sizeof(*recv));
    xr_xir_value_drop(&closed);xr_xir_value_drop(&result);xr_xir_domain_drop(domain);
}
static void channel_recv_wait_release_list(XrXirChannelRecvWait *list) {
    while (list) {
        XrXirChannelRecvWait *recv=list;list=recv->release_next;recv->release_next=NULL;
        channel_recv_wait_release(recv);
    }
}
static void channel_recv_wait_unlink(XirChannel *channel,XrXirChannelRecvWait *recv) {
    XR_CHECK(recv->linked && channel->recv_count,"receive unlink retains its exact queue reference");
    if (recv->previous) recv->previous->next=recv->next;else channel->recv_head=recv->next;
    if (recv->next) recv->next->previous=recv->previous;else channel->recv_tail=recv->previous;
    recv->previous=recv->next=NULL;recv->linked=false;--channel->recv_count;
    graph_lock();graph_dirty(channel->object.domain);graph_unlock();
}
static void channel_recv_wait_defer(XrXirChannelRecvWait *recv,XrXirChannelRecvWait **list) {
    recv->release_next=*list;*list=recv;
}
static bool channel_recv_wait_prepare(XirChannel *channel,XrXirValueAdmission *admission,
    XrXirChannelRecvWait **receiver,uint32_t *prune) {
    *receiver=NULL;*prune=0;
    for (XrXirChannelRecvWait *recv=channel->recv_head;recv;recv=recv->next) {
        if (!channel_work(admission,1)) return false;
        if (atomic_load_explicit(&recv->state,memory_order_acquire)!=CHANNEL_RECV_CANCELLED) { *receiver=recv;break; }
        if (!channel_work(admission,1)) return false;
        ++*prune;
    }
    return true;
}
static void channel_recv_wait_prune(XirChannel *channel,uint32_t count,XrXirChannelRecvWait **released) {
    while (count--) {
        XrXirChannelRecvWait *recv=channel->recv_head;channel_recv_wait_unlink(channel,recv);
        XR_CHECK(atomic_load_explicit(&recv->state,memory_order_acquire)==CHANNEL_RECV_CANCELLED,"prune consumes its prepaid cancelled receive prefix");
        channel_recv_wait_defer(recv,released);
    }
}
XR_FUNC XrXirChannelRecvWaitState xr_xir_channel_recv_wait_state(const XrXirChannelRecvWait *recv) {
    if (!recv) return XR_XIR_CHANNEL_RECV_WAIT_RETRY;
    uint32_t state=atomic_load_explicit(&recv->state,memory_order_acquire);
    return state==CHANNEL_RECV_VALUE ? XR_XIR_CHANNEL_RECV_WAIT_VALUE : state==CHANNEL_RECV_CLOSED ? XR_XIR_CHANNEL_RECV_WAIT_CLOSED :
        state==CHANNEL_RECV_CANCELLED ? XR_XIR_CHANNEL_RECV_WAIT_CANCELLED : XR_XIR_CHANNEL_RECV_WAIT_PENDING;
}
XR_FUNC bool xr_xir_channel_recv_wait_owner(const XrXirChannelRecvWait *recv,const XrXirValueAdmission *admission) {
    return recv && admission_domain_current(admission) && recv->domain==admission->domain &&
        xr_xir_value_argument(&recv->owner_channel,admission->arena,(XrXirType)recv->owner_channel.type);
}
XR_FUNC bool xr_xir_channel_recv_wait_cancel_pending(XrXirChannelRecvWait *recv) {
    if (!recv || unit_value(&recv->owner_channel)) return false;
    xr_xir_value_graph_begin();uint32_t expected=CHANNEL_RECV_WAITING;
    bool won=atomic_compare_exchange_strong_explicit(&recv->state,&expected,CHANNEL_RECV_CANCELLED,
        memory_order_release,memory_order_relaxed);
    xr_xir_value_graph_end();return won;
}
XR_FUNC bool xr_xir_channel_recv_wait_take(XrXirChannelRecvWait *recv,XrXirValue *output) {
    if (!recv || !unit_value(output)) return false;
    XrXirChannelRecvWaitState state=xr_xir_channel_recv_wait_state(recv);
    if ((state!=XR_XIR_CHANNEL_RECV_WAIT_VALUE && state!=XR_XIR_CHANNEL_RECV_WAIT_CLOSED) || recv->taken) return false;
    *output=recv->result;recv->result=(XrXirValue){0};recv->taken=true;return true;
}
XR_FUNC void xr_xir_channel_recv_wait_drop(XrXirChannelRecvWait *recv) {
    if (!recv) return;
    xr_xir_value_graph_begin();XrXirValue owner=recv->owner_channel;recv->owner_channel=(XrXirValue){0};
    uint32_t expected=CHANNEL_RECV_WAITING;
    (void)atomic_compare_exchange_strong_explicit(&recv->state,&expected,CHANNEL_RECV_CANCELLED,
        memory_order_release,memory_order_relaxed);
    XirChannel *channel=(XirChannel *)object_pointer(&owner);bool unlinked=false;
    if (channel_lock(channel)) {
        if (recv->linked) { channel_recv_wait_unlink(channel,recv);unlinked=true; }
        channel_unlock(channel);
    }
    if (unlinked) channel_recv_wait_release(recv);
    xr_xir_value_drop(&owner);channel_recv_wait_release(recv);xr_xir_value_graph_end();
}
static XrXirValueStatus channel_recv_wait_new(const XrXirValue *channel,XrXirType type,bool raw,
    XrXirValueAdmission *admission,XrXirChannelRecvWait **output) {
    XrXirValue owner={0};XrXirValueStatus status=xr_xir_value_copy(channel,&owner);
    bool retained=status==XR_XIR_VALUE_OK && xr_xir_domain_retain(admission->domain);
    if (status==XR_XIR_VALUE_OK && !retained) status=XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirChannelRecvWait *recv=retained ? xr_xir_domain_allocate(admission->domain,sizeof(*recv),&status) : NULL;
    if (!recv) {
        if (retained) xr_xir_domain_drop(admission->domain);
        xr_xir_value_drop(&owner);return status;
    }
    *recv=(XrXirChannelRecvWait){0};atomic_init(&recv->references,1);atomic_init(&recv->state,CHANNEL_RECV_WAITING);
    recv->domain=admission->domain;recv->owner_channel=owner;recv->raw=raw;
    if (!raw) status=xr_xir_enum_new(type,3,NULL,0,admission,&recv->closed);
    if (status==XR_XIR_VALUE_OK && !raw) {
        recv->reservation=(XirNominalValue *)constructed_allocate(admission->domain,(XrXirTypeArena *)admission->arena,
            type,sizeof(XirNominalValue)+sizeof(XrXirValue),&status);
        if (recv->reservation) {
            recv->reservation->variant=0;recv->reservation->count=1;recv->reservation->fields=(XrXirValue *)(recv->reservation+1);
            recv->reservation->fields[0]=(XrXirValue){0};
        }
    }
    if (status==XR_XIR_VALUE_OK) *output=recv;else xr_xir_channel_recv_wait_drop(recv);
    return status;
}
static XrXirValueStatus channel_rendezvous_recv_begin(const XrXirValue *value,XrXirType type,bool raw,
    XrXirValueAdmission *admission,XrXirChannelRecvWait **output,XrXirChannelRecvWaitState *state) {
    XirChannel *channel=NULL;XrXirValueStatus status=channel_receiver(value,admission,&channel);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (channel->buffer.capacity || !channel_work(admission,raw ? 4u : 40u)) return channel->buffer.capacity ? XR_XIR_VALUE_BAD_ARGUMENT : XR_XIR_VALUE_LIMIT;
    if (!raw && !xr_xir_nominal_native_recv(xr_xir_compile_type_arena_types(admission->arena),type,channel->element)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirChannelRecvWait *recv=NULL;status=channel_recv_wait_new(value,type,raw,admission,&recv);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirChannelRecvWaitState outcome=XR_XIR_CHANNEL_RECV_WAIT_RETRY;
    XrXirChannelRecvWait *released=NULL,*frontier=NULL;uint32_t prune=0;
    if (channel_lock(channel)) {
        if (atomic_load_explicit(&channel->closed,memory_order_relaxed)) outcome=XR_XIR_CHANNEL_RECV_WAIT_CLOSED;
        else {
            /* A sender published before this lock must be received normally.
             * Registering both sides would strand two independently owned waits. */
            bool pending=false;
            for (XrXirChannelSendWait *send=channel->send_head;send;send=send->next) {
                if (!channel_work(admission,1)) { status=XR_XIR_VALUE_LIMIT;break; }
                if (atomic_load_explicit(&send->state,memory_order_acquire)==CHANNEL_SEND_WAITING) { pending=true;break; }
            }
            if (status==XR_XIR_VALUE_OK && !pending) {
                if (channel->sequence==UINT64_MAX || channel->recv_count==UINT32_MAX ||
                    !channel_recv_wait_prepare(channel,admission,&frontier,&prune) || !channel_watch_work(channel,admission)) status=XR_XIR_VALUE_LIMIT;
                else {
                    channel_recv_wait_prune(channel,prune,&released);recv->linked=true;
                    atomic_store_explicit(&recv->references,2,memory_order_relaxed);recv->previous=channel->recv_tail;
                    if (channel->recv_tail) channel->recv_tail->next=recv;else channel->recv_head=recv;
                    channel->recv_tail=recv;++channel->recv_count;channel_changed(channel);outcome=XR_XIR_CHANNEL_RECV_WAIT_PENDING;
                }
            }
        }
        channel_unlock(channel);
    }
    channel_recv_wait_release_list(released);
    if (status==XR_XIR_VALUE_OK && outcome==XR_XIR_CHANNEL_RECV_WAIT_PENDING) *output=recv;else xr_xir_channel_recv_wait_drop(recv);
    if (status==XR_XIR_VALUE_OK) *state=outcome;return status;
}
XR_FUNC XrXirValueStatus xr_xir_channel_rendezvous_recv_begin(const XrXirValue *channel,XrXirType type,
    XrXirValueAdmission *admission,XrXirChannelRecvWait **output,XrXirChannelRecvWaitState *state) {
    if (!admission_domain_current(admission) || !output || *output || !state) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XrXirValueStatus status=channel_rendezvous_recv_begin(channel,type,false,admission,output,state);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_channel_rendezvous_recv_raw_begin(const XrXirValue *channel,
    XrXirValueAdmission *admission,XrXirChannelRecvWait **output,XrXirChannelRecvWaitState *state) {
    if (!admission_domain_current(admission) || !output || *output || !state) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XrXirValueStatus status=channel_rendezvous_recv_begin(channel,XR_XIR_UNIT,true,admission,output,state);
    xr_xir_value_graph_end();return status;
}
static XrXirValueStatus channel_send_to_waiter(XirChannel *channel,XrXirValue *owned,
    XrXirValueAdmission *admission,XrXirClassPublication *publication,XrXirChannelState *state,XrXirChannelRecvWait **released) {
    XrXirChannelRecvWait *recv=NULL;uint32_t prune=0;
    if (!channel_recv_wait_prepare(channel,admission,&recv,&prune)) return XR_XIR_VALUE_LIMIT;
    if (!recv) { channel_recv_wait_prune(channel,prune,released);*state=XR_XIR_CHANNEL_FULL;return XR_XIR_VALUE_OK; }
    if (channel->sequence==UINT64_MAX || !channel_work(admission,2) || !channel_watch_work(channel,admission)) return XR_XIR_VALUE_LIMIT;
    uint32_t expected=CHANNEL_RECV_WAITING;
    if (!atomic_compare_exchange_strong_explicit(&recv->state,&expected,CHANNEL_RECV_COMMITTING,
        memory_order_acquire,memory_order_relaxed)) { *state=XR_XIR_CHANNEL_RETRY;return XR_XIR_VALUE_OK; }
    XirNominalValue *record=recv->reservation;
    xr_xir_class_publication_commit(publication);
    if (recv->raw) {
        XR_CHECK(!record && unit_value(&recv->closed) && !recv->taken,"bare receive owns no wrapper or consumed result");
        recv->result=*owned;*owned=(XrXirValue){0};
    } else {
        XR_CHECK(record && !record->object.graph_registered && unit_value(&record->fields[0]) &&
            record->object.arena==admission->arena && record->count==1 && record->variant==0,
            "receive publication owns exact unpublished native storage in the admitted Channel arena");
        record->fields[0]=*owned;*owned=(XrXirValue){0};
        xr_xir_value_object_publish(&record->object);recv->result=(XrXirValue){(uint32_t)record->object.type,0,0};
        memcpy(&recv->result.payload,&record,sizeof(record));recv->reservation=NULL;
    }
    channel_recv_wait_unlink(channel,recv);channel_recv_wait_defer(recv,released);channel_recv_wait_prune(channel,prune,released);
    atomic_store_explicit(&recv->state,CHANNEL_RECV_VALUE,memory_order_release);channel_changed(channel);*state=XR_XIR_CHANNEL_SENT;
    return XR_XIR_VALUE_OK;
}
static bool channel_recv_wait_work(XirChannel *channel,XrXirValueAdmission *admission) {
    return !channel->recv_count || channel_work(admission,channel->recv_count);
}
static void channel_recv_wait_close(XirChannel *channel,XrXirChannelRecvWait **released) {
    while (channel->recv_head) {
        XrXirChannelRecvWait *recv=channel->recv_head;uint32_t expected=CHANNEL_RECV_WAITING;
        if (atomic_compare_exchange_strong_explicit(&recv->state,&expected,CHANNEL_RECV_COMMITTING,
            memory_order_acquire,memory_order_relaxed)) {
            recv->result=recv->closed;recv->closed=(XrXirValue){0};
            atomic_store_explicit(&recv->state,CHANNEL_RECV_CLOSED,memory_order_release);
        } else XR_CHECK(expected==CHANNEL_RECV_CANCELLED,"only waiting or cancelled receivers remain queued");
        channel_recv_wait_unlink(channel,recv);channel_recv_wait_defer(recv,released);
    }
}
static void channel_recv_wait_dispose_queue(XirChannel *channel) {
    while (channel->recv_head) {
        XrXirChannelRecvWait *recv=channel->recv_head;
        XR_CHECK(atomic_load_explicit(&recv->state,memory_order_acquire)==CHANNEL_RECV_CANCELLED &&
            unit_value(&recv->owner_channel),"last Channel root retains only cancelled receive storage");
        channel_recv_wait_unlink(channel,recv);channel_recv_wait_release(recv);
    }
    XR_CHECK(!channel->recv_count && !channel->recv_tail,"Channel destruction consumes every receive reservation");
}
