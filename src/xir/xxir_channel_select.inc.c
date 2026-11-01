/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_select.inc.c - Joint buffered frontiers and one receive commit
 *
 * KEY CONCEPT:
 *   Admission runs outside queue locks. Commit rechecks the complete observed
 *   set before transferring one selected value; notifications own no frame.
 */
typedef struct ChannelSelectArm {
    XrXirValue channel;
    XirChannel *storage;
    XrXirChannelWatch *watch;
    uint64_t sequence;
} ChannelSelectArm;
enum { CHANNEL_SELECT_OPEN,CHANNEL_SELECT_DONE,CHANNEL_SELECT_CANCELLED };
struct XrXirChannelSelect {
    XrXirDomain *domain;
    XrXirTypeArena *arena;
    size_t bytes;
    uint64_t start_ns,last_ns,duration_ns;
    uint32_t count,unique_count,arm;
    _Atomic(uint32_t) state;
    XrXirChannelSelectKind kind;
    XrXirValue candidate,value;
    bool has_default,has_timeout,prepared,taken;
    ChannelSelectArm arms[];
};
static uint32_t *channel_select_order(XrXirChannelSelect *select) {
    return (uint32_t *)(select->arms+select->count);
}
static bool channel_select_owner(const XrXirChannelSelect *select,XrXirValueAdmission *admission) {
    return select && admission_domain_current(admission) && admission->domain==select->domain && admission->arena==select->arena;
}
static void channel_select_notifications_clear(XrXirChannelSelect *select) {
    for(uint32_t i=0;i<select->count;++i) {
        xr_xir_channel_watch_drop(select->arms[i].watch);select->arms[i].watch=NULL;
    }
}
static void channel_select_candidate_clear(XrXirChannelSelect *select) {
    xr_xir_value_drop(&select->candidate);select->prepared=false;
}
static void channel_select_unlock(XrXirChannelSelect *select,uint32_t count) {
    uint32_t *order=channel_select_order(select);
    while(count)channel_unlock(select->arms[order[--count]].storage);
}
static XrXirValueStatus channel_select_lock(XrXirChannelSelect *select,XrXirValueAdmission *admission,bool *locked) {
    if(!channel_work(admission,(uint64_t)select->count+select->unique_count))return XR_XIR_VALUE_LIMIT;
    uint32_t *order=channel_select_order(select),count=0;
    for(;count<select->unique_count;++count)if(!channel_lock(select->arms[order[count]].storage))break;
    *locked=count==select->unique_count;
    if(!*locked)channel_select_unlock(select,count);
    return XR_XIR_VALUE_OK;
}
static bool channel_select_same(const XrXirChannelSelect *select) {
    for(uint32_t i=0;i<select->count;++i)if(select->arms[i].sequence!=select->arms[i].storage->sequence)return false;
    return true;
}
static XrXirValueStatus channel_select_arm_prepare(XrXirChannelSelect *select,
    const XrXirValue *value,uint32_t index,XrXirValueAdmission *admission) {
    ChannelSelectArm *arm=&select->arms[index];
    XrXirValueStatus status=channel_receiver(value,admission,&arm->storage);
    if(status!=XR_XIR_VALUE_OK)return status;
    if(!arm->storage->buffer.capacity)return XR_XIR_VALUE_UNSUPPORTED;
    status=xr_xir_value_copy(value,&arm->channel);
    if(status!=XR_XIR_VALUE_OK)return status;
    uint32_t *order=channel_select_order(select),at=0;
    for(;at<select->unique_count;++at) {
        if(!channel_work(admission,1))return XR_XIR_VALUE_LIMIT;
        uintptr_t current=(uintptr_t)select->arms[order[at]].storage;
        if(current==(uintptr_t)arm->storage)return XR_XIR_VALUE_OK;
        if(current>(uintptr_t)arm->storage)break;
    }
    if(!channel_work(admission,select->unique_count-at))return XR_XIR_VALUE_LIMIT;
    memmove(order+at+1,order+at,(size_t)(select->unique_count-at)*sizeof(*order));
    order[at]=index;++select->unique_count;return XR_XIR_VALUE_OK;
}
XR_FUNC void xr_xir_channel_select_drop(XrXirChannelSelect *select) {
    if(!select)return;
    xr_xir_value_graph_begin();
    channel_select_notifications_clear(select);channel_select_candidate_clear(select);xr_xir_value_drop(&select->value);
    for(uint32_t i=0;i<select->count;++i)xr_xir_value_drop(&select->arms[i].channel);
    XrXirDomain *domain=select->domain;XrXirTypeArena *arena=select->arena;
    xr_xir_domain_deallocate(domain,select,select->bytes);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);xr_xir_value_graph_end();
}
XR_FUNC XrXirValueStatus xr_xir_channel_select_begin(const XrXirChannelSelectRequest *request,
    XrXirValueAdmission *admission,XrXirChannelSelect **output) {
    if(!request||!output||*output||!admission_domain_current(admission)||!admission->arena||
        (request->count&&!request->channels))return XR_XIR_VALUE_BAD_ARGUMENT;
    if(!request->count||(request->has_default&&request->has_timeout))return XR_XIR_VALUE_UNSUPPORTED;
    if((!request->has_timeout&&(request->timeout_ms||request->now_ns))||
        (request->has_timeout&&request->timeout_ms>0&&!request->now_ns))return XR_XIR_VALUE_BAD_ARGUMENT;
    if(request->has_timeout&&(request->timeout_ms<0||(uint64_t)request->timeout_ms>UINT64_C(86400000)))return XR_XIR_VALUE_BOUNDS;
    uint64_t bytes=sizeof(XrXirChannelSelect)+(uint64_t)request->count*(sizeof(ChannelSelectArm)+sizeof(uint32_t));
    if(bytes>SIZE_MAX||!channel_work(admission,request->count))return XR_XIR_VALUE_LIMIT;
    if(!xr_xir_domain_retain(admission->domain))return XR_XIR_VALUE_REFCOUNT_LIMIT;
    if(!xr_xir_compile_type_arena_retain((XrXirTypeArena *)admission->arena)) {
        xr_xir_domain_drop(admission->domain);return XR_XIR_VALUE_REFCOUNT_LIMIT;
    }
    xr_xir_value_graph_begin();XrXirValueStatus status=XR_XIR_VALUE_OK;
    XrXirChannelSelect *select=xr_xir_domain_allocate(admission->domain,(size_t)bytes,&status);
    if(!select) {
        xr_xir_compile_type_arena_drop((XrXirTypeArena *)admission->arena);xr_xir_domain_drop(admission->domain);
        xr_xir_value_graph_end();return status;
    }
    memset(select,0,(size_t)bytes);atomic_init(&select->state,CHANNEL_SELECT_OPEN);
    select->domain=admission->domain;select->arena=(XrXirTypeArena *)admission->arena;
    select->bytes=(size_t)bytes;select->count=request->count;select->has_default=request->has_default;
    select->has_timeout=request->has_timeout;select->start_ns=select->last_ns=request->now_ns;
    select->duration_ns=request->has_timeout?(uint64_t)request->timeout_ms*UINT64_C(1000000):0;
    for(uint32_t i=0;i<select->count&&status==XR_XIR_VALUE_OK;++i)
        status=channel_select_arm_prepare(select,&request->channels[i],i,admission);
    if(status==XR_XIR_VALUE_OK)*output=select;else xr_xir_channel_select_drop(select);
    xr_xir_value_graph_end();return status;
}
static XrXirValueStatus channel_select_clock(XrXirChannelSelect *select,uint64_t now,bool *due) {
    *due=false;
    if(!select->has_timeout)return now?XR_XIR_VALUE_BAD_ARGUMENT:XR_XIR_VALUE_OK;
    if(select->duration_ns&&(!now||now<select->last_ns||now<select->start_ns))return XR_XIR_VALUE_BAD_ARGUMENT;
    select->last_ns=now;*due=now-select->start_ns>=select->duration_ns;return XR_XIR_VALUE_OK;
}
/* Locks are deduplicated; the random candidate set retains every source arm. */
static XrXirValueStatus channel_select_snapshot(XrXirChannelSelect *select,uint64_t entropy,
    bool due,XrXirValueAdmission *admission,XrXirChannelSelectState *state,bool *empty) {
    bool locked=false;XrXirValueStatus status=channel_select_lock(select,admission,&locked);
    if(status!=XR_XIR_VALUE_OK||!locked)return status;
    uint64_t ready=0;
    for(uint32_t i=0;i<select->count;++i) {
        ChannelSelectArm *arm=&select->arms[i];arm->sequence=arm->storage->sequence;
        if(arm->storage->buffer.count)++ready;
    }
    if(ready) {
        uint64_t threshold=(UINT64_C(0)-ready)%ready;
        if(entropy<threshold) {channel_select_unlock(select,select->unique_count);return XR_XIR_VALUE_OK;}
        uint64_t choice=entropy%ready;
        for(uint32_t i=0;i<select->count;++i)if(select->arms[i].storage->buffer.count) {
            if(choice--)continue;
            XirChannel *channel=select->arms[i].storage;
            if(channel->sequence==UINT64_MAX)status=XR_XIR_VALUE_LIMIT;
            else status=xr_xir_value_copy(&channel_slots(channel)[xr_channel_buffer_slot(&channel->buffer,0)],&select->candidate);
            select->arm=i;select->kind=XR_XIR_CHANNEL_SELECT_RECEIVE;break;
        }
    } else if(select->has_default||due) {
        select->kind=select->has_default?XR_XIR_CHANNEL_SELECT_DEFAULT:XR_XIR_CHANNEL_SELECT_TIMEOUT;select->arm=UINT32_MAX;
    } else *empty=true;
    channel_select_unlock(select,select->unique_count);
    if(status!=XR_XIR_VALUE_OK)return status;
    if(ready)status=channel_element_admit(select->arms[select->arm].storage,&select->candidate,admission);
    if(status==XR_XIR_VALUE_OK&&(ready||select->has_default||due)) {
        select->prepared=true;*state=XR_XIR_CHANNEL_SELECT_CANDIDATE;
    }
    return status;
}
static XrXirValueStatus channel_select_register(XrXirChannelSelect *select,
    XrXirValueAdmission *admission,XrXirChannelSelectState *state) {
    uint32_t *order=channel_select_order(select);
    for(uint32_t n=0;n<select->unique_count;++n) {
        ChannelSelectArm *arm=&select->arms[order[n]];
        if(atomic_load_explicit(&arm->storage->closed,memory_order_acquire))continue;
        XrXirChannelWatchState watched=XR_XIR_CHANNEL_WATCH_RETRY;
        XrXirValueStatus status=xr_xir_channel_watch_begin(&arm->channel,arm->sequence,admission,&arm->watch,&watched);
        if(status!=XR_XIR_VALUE_OK)return status;
        if(watched!=XR_XIR_CHANNEL_WATCH_PENDING)return XR_XIR_VALUE_OK;
    }
    bool locked=false;XrXirValueStatus status=channel_select_lock(select,admission,&locked);
    if(status!=XR_XIR_VALUE_OK||!locked)return status;
    bool same=channel_select_same(select);channel_select_unlock(select,select->unique_count);
    if(same)*state=XR_XIR_CHANNEL_SELECT_PENDING;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_channel_select_probe(XrXirChannelSelect *select,
    uint64_t now_ns,uint64_t entropy,XrXirValueAdmission *admission,XrXirChannelSelectState *state) {
    if(!state||!channel_select_owner(select,admission))return XR_XIR_VALUE_BAD_ARGUMENT;
    uint32_t current=atomic_load_explicit(&select->state,memory_order_acquire);
    if(current==CHANNEL_SELECT_DONE)return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();channel_select_notifications_clear(select);channel_select_candidate_clear(select);
    XrXirChannelSelectState outcome=XR_XIR_CHANNEL_SELECT_RETRY;XrXirValueStatus status=XR_XIR_VALUE_OK;
    bool due=false,empty=false;
    if(current==CHANNEL_SELECT_CANCELLED)outcome=XR_XIR_CHANNEL_SELECT_CANCELLED;
    else status=channel_select_clock(select,now_ns,&due);
    if(current==CHANNEL_SELECT_OPEN&&status==XR_XIR_VALUE_OK)
        status=channel_select_snapshot(select,entropy,due,admission,&outcome,&empty);
    if(current==CHANNEL_SELECT_OPEN&&status==XR_XIR_VALUE_OK&&empty)
        status=channel_select_register(select,admission,&outcome);
    if(atomic_load_explicit(&select->state,memory_order_acquire)==CHANNEL_SELECT_CANCELLED)outcome=XR_XIR_CHANNEL_SELECT_CANCELLED;
    if(status!=XR_XIR_VALUE_OK||outcome!=XR_XIR_CHANNEL_SELECT_PENDING)channel_select_notifications_clear(select);
    if(status!=XR_XIR_VALUE_OK||outcome==XR_XIR_CHANNEL_SELECT_CANCELLED)channel_select_candidate_clear(select);
    if(status==XR_XIR_VALUE_OK)*state=outcome;
    xr_xir_value_graph_end();return status;
}
XR_FUNC bool xr_xir_channel_select_candidate(const XrXirChannelSelect *select,
    XrXirChannelSelectKind *kind,uint32_t *arm,const XrXirValue **value) {
    if(!select||!kind||!arm||!value||!select->prepared||
        atomic_load_explicit(&select->state,memory_order_acquire)!=CHANNEL_SELECT_OPEN)return false;
    *kind=select->kind;*arm=select->arm;*value=&select->candidate;return true;
}
XR_FUNC XrXirValueStatus xr_xir_channel_select_commit(XrXirChannelSelect *select,
    XrXirValueAdmission *admission,XrXirChannelSelectState *state) {
    if(!state||!channel_select_owner(select,admission)||!select->prepared)return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();bool locked=false;XrXirValueStatus status=channel_select_lock(select,admission,&locked);
    XrXirChannelSelectState outcome=XR_XIR_CHANNEL_SELECT_RETRY;
    if(status==XR_XIR_VALUE_OK&&locked) {
        bool same=channel_select_same(select);XirChannel *channel=NULL;
        if(same&&select->kind==XR_XIR_CHANNEL_SELECT_RECEIVE) {
            channel=select->arms[select->arm].storage;
            if(!channel_watch_work(channel,admission))status=XR_XIR_VALUE_LIMIT;
        }
        if(same&&status==XR_XIR_VALUE_OK) {
            uint32_t expected=CHANNEL_SELECT_OPEN;
            if(atomic_compare_exchange_strong_explicit(&select->state,&expected,CHANNEL_SELECT_DONE,memory_order_acq_rel,memory_order_acquire)) {
                if(channel) {
                    uint32_t slot=UINT32_MAX;
                    XR_CHECK(xr_channel_buffer_pop(&channel->buffer,&slot),"one selected frontier still owns its prepared message");
                    select->value=channel_slots(channel)[slot];channel_slots(channel)[slot]=(XrXirValue){0};channel_changed(channel);
                }
                outcome=XR_XIR_CHANNEL_SELECT_COMMITTED;
            } else if(expected==CHANNEL_SELECT_CANCELLED)outcome=XR_XIR_CHANNEL_SELECT_CANCELLED;
        }
        channel_select_unlock(select,select->unique_count);
    }
    if(outcome!=XR_XIR_CHANNEL_SELECT_COMMITTED&&atomic_load_explicit(&select->state,memory_order_acquire)==CHANNEL_SELECT_CANCELLED)
        outcome=XR_XIR_CHANNEL_SELECT_CANCELLED;
    channel_select_candidate_clear(select);if(status==XR_XIR_VALUE_OK)*state=outcome;
    xr_xir_value_graph_end();return status;
}
XR_FUNC bool xr_xir_channel_select_notified(const XrXirChannelSelect *select) {
    if(!select)return false;
    if(atomic_load_explicit(&select->state,memory_order_acquire)!=CHANNEL_SELECT_OPEN)return true;
    for(uint32_t i=0;i<select->count;++i)if(xr_xir_channel_watch_ready(select->arms[i].watch))return true;
    return false;
}
XR_FUNC bool xr_xir_channel_select_take(XrXirChannelSelect *select,
    XrXirChannelSelectKind *kind,uint32_t *arm,XrXirValue *output) {
    if(!select||!kind||!arm||!unit_value(output)||select->taken||
        atomic_load_explicit(&select->state,memory_order_acquire)!=CHANNEL_SELECT_DONE)return false;
    *kind=select->kind;*arm=select->arm;*output=select->value;select->value=(XrXirValue){0};select->taken=true;return true;
}
XR_FUNC bool xr_xir_channel_select_cancel(XrXirChannelSelect *select) {
    if(!select)return false;
    uint32_t expected=CHANNEL_SELECT_OPEN;
    return atomic_compare_exchange_strong_explicit(&select->state,&expected,CHANNEL_SELECT_CANCELLED,memory_order_acq_rel,memory_order_acquire);
}
