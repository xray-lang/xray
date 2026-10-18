/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_value.inc.c - Bounded owned queue preparation and publication
 */
static bool channel_work(XrXirValueAdmission *admission, uint64_t units) {
    if (!admission || !admission->domain || admission->work < units ||
        !xr_xir_domain_work(admission->domain, units)) return false;
    admission->work -= units;
    return true;
}
static bool channel_lock(XirChannel *channel) {
    bool unlocked = false;
    return atomic_compare_exchange_strong_explicit(&channel->locked, &unlocked, true,
        memory_order_acquire, memory_order_relaxed);
}
static void channel_unlock(XirChannel *channel) {
    atomic_store_explicit(&channel->locked, false, memory_order_release);
}
#include "xxir_channel_watch.inc.c"
static void channel_changed(XirChannel *channel) {
    XR_CHECK(channel->sequence != UINT64_MAX, "channel publication excludes exhausted sequence");
    ++channel->sequence;
    channel_watch_notify(channel);
    graph_lock(); graph_dirty(channel->object.domain); graph_unlock();
}
static XrXirValueStatus channel_receiver(const XrXirValue *value,
    XrXirValueAdmission *admission, XirChannel **output) {
    if (!admission || !admission->domain || !admission->arena || !value ||
        !xr_xir_type_is_channel(xr_xir_compile_type_arena_types(admission->arena), (XrXirType)value->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_admit(value, (XrXirType)value->type, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    if (!channel_work(admission, 1)) return XR_XIR_VALUE_LIMIT;
    *output = (XirChannel *)object_pointer(value);
    return XR_XIR_VALUE_OK;
}
static XrXirValueStatus channel_element_admit(XirChannel *channel, const XrXirValue *value,
    XrXirValueAdmission *admission) {
    if (channel->element == XR_XIR_UNIT) {
        if (!unit_value(value)) return XR_XIR_VALUE_BAD_ARGUMENT;
        return channel_work(admission, 1) ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT;
    }
    return xr_xir_value_admit(value, channel->element, admission);
}
static bool channel_recv_wait_available(XirChannel *channel,XrXirValueAdmission *admission,bool *available);
#include "xxir_channel_rendezvous.inc.c"
#include "xxir_channel_recv_wait.inc.c"
static XrXirValueStatus channel_new_operation(XrXirType type, int64_t capacity,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !admission->domain || !admission->arena || !unit_value(output))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena), type);
    if (!node || node->kind != XR_XIR_TYPE_CHANNEL || node->parameter_span)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (capacity < 0 || (uint64_t)capacity > UINT32_MAX) return XR_XIR_VALUE_BOUNDS;
    uint64_t bytes = sizeof(XirChannel) + (uint64_t)capacity * sizeof(XrXirValue);
    if (bytes > SIZE_MAX || !channel_work(admission, (uint64_t)capacity + 1)) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XirChannel *channel = (XirChannel *)constructed_allocate(admission->domain,
        (XrXirTypeArena *)admission->arena, type, (size_t)bytes, &status);
    if (!channel) return status;
    atomic_init(&channel->locked, false); atomic_init(&channel->closed, false);
    channel->buffer = (XrChannelBufferState){.capacity = (uint32_t)capacity};
    channel->element = node->element; channel->allocation_bytes = (size_t)bytes; channel->sequence = 0;
    channel->watch_head=channel->watch_tail=NULL;channel->watch_count=0;
    channel->send_head=channel->send_tail=NULL;channel->send_count=0;
    channel->recv_head=channel->recv_tail=NULL;channel->recv_count=0;
    if (!atomic_is_lock_free(&channel->locked)) {
        constructed_discard(&channel->object, (size_t)bytes); return XR_XIR_VALUE_UNSUPPORTED;
    }
    if (capacity) memset(channel_slots(channel), 0, (size_t)capacity * sizeof(XrXirValue));
    XrXirValue value = {(uint32_t)type,0,0};
    memcpy(&value.payload, &channel, sizeof(channel));
    xr_xir_value_object_publish(&channel->object); *output = value;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_channel_new(XrXirType type, int64_t capacity,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = channel_new_operation(type, capacity, admission, output);
    xr_xir_value_graph_end(); return status;
}
static XrXirValueStatus channel_send_operation(const XrXirValue *value, XrXirValue *owned,
    XrXirValueAdmission *admission, XrXirChannelState *state) {
    if (!state || !owned || value == owned) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirChannel *channel = NULL;
    XrXirValueStatus status = channel_receiver(value, admission, &channel);
    if (status != XR_XIR_VALUE_OK) return status;
    status = channel_element_admit(channel, owned, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirClassPublication publication = {0};
    status = xr_xir_class_publication_prepare(owned, 1, admission, &publication);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirChannelState outcome = XR_XIR_CHANNEL_RETRY;XrXirChannelRecvWait *released=NULL;
    if (channel_lock(channel)) {
        if (atomic_load_explicit(&channel->closed, memory_order_relaxed)) outcome = XR_XIR_CHANNEL_CLOSED;
        else if (!channel->buffer.capacity) status=channel_send_to_waiter(channel,owned,admission,&publication,&outcome,&released);
        else if (channel->buffer.count == channel->buffer.capacity) outcome = XR_XIR_CHANNEL_FULL;
        else if (channel->sequence == UINT64_MAX || !channel_watch_work(channel,admission)) status = XR_XIR_VALUE_LIMIT;
        else {
            uint32_t slot = UINT32_MAX;
            xr_xir_class_publication_commit(&publication);
            XR_CHECK(xr_channel_buffer_push(&channel->buffer, &slot), "validated channel enqueue cannot fail");
            XR_CHECK(unit_value(&channel_slots(channel)[slot]), "empty channel slot owns no value");
            channel_slots(channel)[slot] = *owned; *owned = (XrXirValue){0};
            channel_changed(channel); outcome = XR_XIR_CHANNEL_SENT;
        }
        channel_unlock(channel);
    }
    channel_recv_wait_release_list(released);
    xr_xir_class_publication_drop(&publication);
    if (status == XR_XIR_VALUE_OK) *state = outcome;
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_channel_send_owned(const XrXirValue *channel,
    XrXirValue *owned, XrXirValueAdmission *admission, XrXirChannelState *state) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = channel_send_operation(channel, owned, admission, state);
    xr_xir_value_graph_end(); return status;
}
static XrXirValueStatus channel_send_result_operation(const XrXirValue *value,
    XrXirValue *owned, XrXirType result_type, XrXirValueAdmission *admission,
    XrXirValue *output, XrXirChannelState *state) {
    if (!state || !owned || !unit_value(output) || value==output || value==owned || owned==output ||
        !admission || !admission->arena) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!channel_work(admission,40)) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_nominal_native_send_result(xr_xir_compile_type_arena_types(admission->arena),result_type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue prepared[3]={{0}};
    const uint32_t variants[3]={0,1,3};
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    for (unsigned i=0;i<3 && status==XR_XIR_VALUE_OK;++i)
        status=xr_xir_enum_new(result_type,variants[i],NULL,0,admission,&prepared[i]);
    XrXirChannelState outcome=XR_XIR_CHANNEL_RETRY;
    if (status==XR_XIR_VALUE_OK) status=channel_send_operation(value,owned,admission,&outcome);
    if (status==XR_XIR_VALUE_OK) {
        if (outcome!=XR_XIR_CHANNEL_RETRY) {
            unsigned selected=outcome==XR_XIR_CHANNEL_SENT ? 0u : outcome==XR_XIR_CHANNEL_FULL ? 1u : 2u;
            XR_CHECK(outcome==XR_XIR_CHANNEL_SENT || outcome==XR_XIR_CHANNEL_FULL || outcome==XR_XIR_CHANNEL_CLOSED,
                "send publication has a prepared result for every terminal queue outcome");
            *output=prepared[selected]; prepared[selected]=(XrXirValue){0};
        }
        *state=outcome;
    }
    for (unsigned i=0;i<3;++i) xr_xir_value_drop(&prepared[i]);
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_channel_send_result(const XrXirValue *channel,
    XrXirValue *owned, XrXirType result_type, XrXirValueAdmission *admission,
    XrXirValue *output, XrXirChannelState *state) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status=channel_send_result_operation(channel,owned,result_type,admission,output,state);
    xr_xir_value_graph_end(); return status;
}
XR_FUNC XrXirValueStatus xr_xir_channel_try_send(const XrXirValue *channel,
    const XrXirValue *message, XrXirType result_type, XrXirValueAdmission *admission,
    XrXirValue *output, XrXirChannelState *state) {
    if (!admission_domain_current(admission) || !channel || !message || !state || !unit_value(output) ||
        message==output || channel==output)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XrXirValue owned={0};
    XrXirValueStatus status=channel_work(admission,1) ? xr_xir_value_copy(message,&owned) : XR_XIR_VALUE_LIMIT;
    if (status==XR_XIR_VALUE_OK) status=channel_send_result_operation(channel,&owned,result_type,admission,output,state);
    xr_xir_value_drop(&owned);xr_xir_value_graph_end();return status;
}
typedef struct ChannelReceive {
    XirChannel *channel;
    XrXirValue prepared;
    uint64_t sequence;
    uint32_t slot;
    XrXirChannelState state;
    XrXirChannelSendWait *sender;
} ChannelReceive;
static void channel_receive_clear(ChannelReceive *prepared) {
    xr_xir_value_drop(&prepared->prepared);
    channel_send_wait_release(prepared->sender);prepared->sender=NULL;
}
static XrXirValueStatus channel_receive_prepare(XirChannel *channel,
    XrXirValueAdmission *admission, ChannelReceive *prepared) {
    prepared->channel=channel; prepared->state=XR_XIR_CHANNEL_RETRY;
    if (!channel_lock(channel)) return XR_XIR_VALUE_OK;
    if (!channel->buffer.count) {
        XrXirValueStatus status=channel_receive_pending_prepare(channel,admission,&prepared->sender,&prepared->prepared);
        prepared->sequence=channel->sequence;
        prepared->state=prepared->sender ? XR_XIR_CHANNEL_VALUE :
            atomic_load_explicit(&channel->closed,memory_order_relaxed) ? XR_XIR_CHANNEL_CLOSED : XR_XIR_CHANNEL_EMPTY;
        channel_unlock(channel);
        if (status==XR_XIR_VALUE_OK && prepared->sender) status=channel_element_admit(channel,&prepared->prepared,admission);
        return status;
    }
    if (channel->sequence==UINT64_MAX) { channel_unlock(channel); return XR_XIR_VALUE_LIMIT; }
    prepared->slot=xr_channel_buffer_slot(&channel->buffer,0); prepared->sequence=channel->sequence;
    XrXirValueStatus status=xr_xir_value_copy(&channel_slots(channel)[prepared->slot],&prepared->prepared);
    channel_unlock(channel);
    if (status!=XR_XIR_VALUE_OK) return status;
    /* Admission can call an execution gate. It never runs under the queue lock. */
    status=channel_element_admit(channel,&prepared->prepared,admission);
    if (status==XR_XIR_VALUE_OK) prepared->state=XR_XIR_CHANNEL_VALUE;
    return status;
}
static XrXirValueStatus channel_receive_commit(ChannelReceive *prepared,XrXirValueAdmission *admission,
    XrXirValue *output,XrXirChannelState *state) {
    XirChannel *channel=prepared->channel;
    XR_CHECK(prepared->state==XR_XIR_CHANNEL_VALUE && unit_value(output),"receive commit owns a prepared frontier");
    if (!channel_lock(channel)) { *state=XR_XIR_CHANNEL_RETRY;return XR_XIR_VALUE_OK; }
    if (prepared->sender) {
        XrXirChannelSendWait *released=NULL;
        XrXirValueStatus status=channel_receive_pending_commit(channel,prepared->sender,prepared->sequence,
            admission,output,state,&released);
        channel_unlock(channel);channel_send_wait_release_list(released);return status;
    }
    XrXirChannelState outcome=XR_XIR_CHANNEL_RETRY;XrXirValueStatus status=XR_XIR_VALUE_OK;
    if (channel->sequence==prepared->sequence && !channel_watch_work(channel,admission)) status=XR_XIR_VALUE_LIMIT;
    else if (channel->sequence==prepared->sequence) {
        uint32_t removed=UINT32_MAX;
        XR_CHECK(xr_channel_buffer_pop(&channel->buffer,&removed) && removed==prepared->slot,
            "unchanged channel frontier transfers its exact prepared value");
        *output=channel_slots(channel)[removed]; channel_slots(channel)[removed]=(XrXirValue){0};
        channel_changed(channel); outcome=XR_XIR_CHANNEL_VALUE;
    }
    channel_unlock(channel);if (status==XR_XIR_VALUE_OK) *state=outcome;return status;
}
static XrXirValueStatus channel_receive_operation(const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirChannelState *state) {
    if (!state || !unit_value(output) || value==output) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirChannel *channel=NULL;
    XrXirValueStatus status=channel_receiver(value,admission,&channel);
    if (status!=XR_XIR_VALUE_OK) return status;
    ChannelReceive prepared={0}; status=channel_receive_prepare(channel,admission,&prepared);
    if (status==XR_XIR_VALUE_OK) {
        if (prepared.state==XR_XIR_CHANNEL_VALUE) status=channel_receive_commit(&prepared,admission,output,state);
        else *state=prepared.state;
    }
    channel_receive_clear(&prepared); return status;
}
XR_FUNC XrXirValueStatus xr_xir_channel_receive(const XrXirValue *channel,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirChannelState *state) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = channel_receive_operation(channel, admission, output, state);
    xr_xir_value_graph_end(); return status;
}
static XrXirValueStatus channel_receive_result_operation(const XrXirValue *value,
    XrXirType result_type, XrXirValueAdmission *admission, XrXirValue *output, XrXirChannelState *state) {
    if (!state || !unit_value(output) || value==output) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirChannel *channel=NULL; XrXirValueStatus status=channel_receiver(value,admission,&channel);
    if (status!=XR_XIR_VALUE_OK) return status;
    if (!channel_work(admission,40)) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_nominal_native_recv(xr_xir_compile_type_arena_types(admission->arena),result_type,channel->element))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    ChannelReceive prepared={0}; status=channel_receive_prepare(channel,admission,&prepared);
    XrXirValue result={0},removed={0};
    if (status==XR_XIR_VALUE_OK && prepared.state!=XR_XIR_CHANNEL_RETRY) {
        uint32_t variant=prepared.state==XR_XIR_CHANNEL_VALUE ? 0u : prepared.state==XR_XIR_CHANNEL_EMPTY ? 1u : 3u;
        status=xr_xir_enum_new(result_type,variant,variant ? NULL : &prepared.prepared,variant ? 0u : 1u,admission,&result);
        if (status==XR_XIR_VALUE_OK) {
            XrXirChannelState outcome=prepared.state;
            if (prepared.state==XR_XIR_CHANNEL_VALUE) status=channel_receive_commit(&prepared,admission,&removed,&outcome);
            if (status==XR_XIR_VALUE_OK) {
                if (outcome!=XR_XIR_CHANNEL_RETRY) { *output=result; result=(XrXirValue){0}; }
                *state=outcome;
            }
        }
    } else if (status==XR_XIR_VALUE_OK) *state=XR_XIR_CHANNEL_RETRY;
    xr_xir_value_drop(&removed); xr_xir_value_drop(&result); channel_receive_clear(&prepared);
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_channel_receive_result(const XrXirValue *channel,
    XrXirType result_type, XrXirValueAdmission *admission, XrXirValue *output, XrXirChannelState *state) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status=channel_receive_result_operation(channel,result_type,admission,output,state);
    xr_xir_value_graph_end(); return status;
}
static XrXirValueStatus channel_close_operation(const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirChannelState *state) {
    if (!state) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirChannel *channel = NULL;
    XrXirValueStatus status = channel_receiver(value, admission, &channel);
    if (status != XR_XIR_VALUE_OK) return status;
    if (!channel_lock(channel)) { *state = XR_XIR_CHANNEL_RETRY; return XR_XIR_VALUE_OK; }
    XrXirChannelSendWait *released=NULL;XrXirChannelRecvWait *receivers=NULL;
    if (!atomic_load_explicit(&channel->closed, memory_order_relaxed)) {
        if (channel->sequence == UINT64_MAX || !channel_watch_work(channel,admission) ||
            !channel_send_wait_work(channel,admission) || !channel_recv_wait_work(channel,admission)) status = XR_XIR_VALUE_LIMIT;
        else {
            atomic_store_explicit(&channel->closed, true, memory_order_release);
            channel_send_wait_close(channel,&released);channel_recv_wait_close(channel,&receivers);channel_changed(channel);
        }
    }
    channel_unlock(channel);
    channel_send_wait_release_list(released);
    channel_recv_wait_release_list(receivers);
    if (status == XR_XIR_VALUE_OK) *state = XR_XIR_CHANNEL_CLOSED;
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_channel_close(const XrXirValue *channel,
    XrXirValueAdmission *admission, XrXirChannelState *state) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = channel_close_operation(channel, admission, state);
    xr_xir_value_graph_end(); return status;
}
XR_FUNC XrXirValueStatus xr_xir_channel_info(const XrXirValue *value,
    XrXirValueAdmission *admission, int64_t *capacity, bool *closed) {
    if (!capacity || !closed || !admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XirChannel *channel = NULL;
    XrXirValueStatus status = channel_receiver(value, admission, &channel);
    if (status == XR_XIR_VALUE_OK) {
        *capacity = channel->buffer.capacity;
        *closed = atomic_load_explicit(&channel->closed, memory_order_acquire);
    }
    xr_xir_value_graph_end(); return status;
}
