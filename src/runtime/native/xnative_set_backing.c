/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_set_backing.c - Ordered scalar Set updates in real Root transactions
 *
 * KEY CONCEPT:
 *   Dense scalar entries, probe buckets and Fenwick insertion ranks share one
 *   allocation. The common Place transaction protects every physical edit.
 */
#include "xnative_set_backing.h"
#include "xnative_string_backing.h"
#include "base/xchecks.h"
#include "base/xhash.h"
#include <string.h>
typedef struct SetProbe { uint64_t hash; size_t bucket, entry; bool found; } SetProbe;
typedef struct SetEdit { SetProbe probe; size_t length, capacity; bool noop; } SetEdit;
typedef struct SetDelete { size_t end, last_bucket; } SetDelete;
typedef struct SetDestination {
    XrtManagedFrame *frame; size_t slot; const XrtBackingTypes *types;
    const XrtManagedSlotReservation *reservation;
} SetDestination;
static bool set_shape(const XrtSetBacking *set) {
    return set && ((!set->capacity && !set->length && !set->order_count) ||
        (set->capacity >= 8 && !(set->capacity & (set->capacity-1)) &&
         set->length <= set->capacity/2 && set->length <= set->order_count && set->order_count <= set->capacity));
}
static XrtManagedStatus set_capacity(size_t current, size_t required, size_t *output) {
    size_t maximum = (SIZE_MAX-sizeof(XrtSetBacking))/sizeof(XrtSetBackingRow), capacity=current;
    if (required > INT64_MAX || required > maximum/2) return XRT_MANAGED_OVERFLOW;
    if (required > capacity/2) {
        capacity=8;
        while (required > capacity/2) {
            if (capacity > maximum/2) return XRT_MANAGED_OVERFLOW;
            capacity*=2;
        }
    }
    if (capacity > maximum) return XRT_MANAGED_OVERFLOW;
    *output=capacity; return XRT_MANAGED_OK;
}
static XrtManagedStatus set_probe(const XrtSetBacking *set, int64_t key, SetProbe *output) {
    SetProbe probe={.hash=xr_hash_int(key)};
    if (!set->capacity) { *output=probe; return XRT_MANAGED_OK; }
    size_t mask=set->capacity-1; probe.bucket=(size_t)probe.hash&mask;
    for (size_t n=0;n<set->capacity;++n,probe.bucket=(probe.bucket+1)&mask) {
        size_t index=set->rows[probe.bucket].bucket;
        if (!index) { *output=probe; return XRT_MANAGED_OK; }
        if (index>set->length) return XRT_MANAGED_ARGUMENT;
        probe.entry=index-1;
        if (set->rows[probe.entry].hash==probe.hash && set->rows[probe.entry].key==key) {
            probe.found=true; *output=probe; return XRT_MANAGED_OK;
        }
    }
    return XRT_MANAGED_ARGUMENT;
}
static void set_order_rebuild_ranks(XrtSetBacking *set) {
    for (size_t p=0;p<set->capacity;++p) set->rows[p].rank=p<set->length;
    for (size_t p=0;p<set->capacity;++p) {
        size_t parent=p|(p+1);
        if (parent<set->capacity) set->rows[parent].rank+=set->rows[p].rank;
    }
}
static void set_order_compact(XrtSetBacking *set) {
    size_t count=0;
    for (size_t p=0;p<set->order_count;++p) {
        size_t entry=set->rows[p].order;
        if (!entry) continue;
        set->rows[count].order=entry; set->rows[entry-1].entry_order=count++;
    }
    XR_CHECK(count==set->length,"compaction preserves every live Set insertion");
    for (size_t p=count;p<set->capacity;++p) set->rows[p].order=0;
    set_order_rebuild_ranks(set);
    set->order_count=count;
}
static void set_publish(XrtSetBacking *set, int64_t key, uint64_t hash, size_t bucket) {
    size_t entry=set->length;
    XR_CHECK(entry<set->capacity/2,"half load reserves a distinct empty bucket");
    if (set->order_count==set->capacity) set_order_compact(set);
    size_t position=set->order_count++;
    set->rows[entry].key=key; set->rows[entry].hash=hash; set->rows[entry].entry_order=position;
    set->rows[bucket].bucket=entry+1; set->rows[position].order=entry+1;
    for (size_t p=position;p<set->capacity;p|=p+1) ++set->rows[p].rank;
    ++set->length;
}
static XrtManagedStatus set_append(XrtSetBacking *set, int64_t key, uint64_t hash) {
    size_t mask=set->capacity-1,bucket=(size_t)hash&mask;
    for (size_t n=0;n<set->capacity;++n,bucket=(bucket+1)&mask) {
        if (set->rows[bucket].bucket) continue;
        /* Private construction is compact. Rebuild all ranks once before
         * commit instead of paying a Fenwick update for every copied key. */
        size_t entry=set->length;
        XR_CHECK(entry<set->capacity/2 && set->order_count==entry,"private Set copies have dense insertion order");
        set->rows[entry].key=key; set->rows[entry].hash=hash; set->rows[entry].entry_order=entry;
        set->rows[bucket].bucket=entry+1; set->rows[entry].order=entry+1;
        ++set->length; ++set->order_count;
        return XRT_MANAGED_OK;
    }
    return XRT_MANAGED_ARGUMENT;
}
static XrtManagedStatus set_prepare(const SetDestination *to, size_t capacity, XrtSetBacking **output) {
    if (!to->types || !to->types->set) return XRT_MANAGED_ARGUMENT;
    XrtManagedHeader *payload=NULL;
    size_t bytes=sizeof(XrtSetBacking)+capacity*sizeof(XrtSetBackingRow);
    XrtManagedStatus status=to->reservation ?
        xrt_managed_slot_reserved_prepare(to->reservation,to->types->set,bytes,&payload) :
        xrt_managed_slot_prepare(to->frame,to->slot,to->types->set,bytes,&payload);
    if (status==XRT_MANAGED_OK) *output=(XrtSetBacking *)payload;
    return status;
}
static XrtManagedStatus set_finish(const SetDestination *to, XrtManagedStatus status) {
    if (status==XRT_MANAGED_OK) status=to->reservation ? xrt_managed_slot_reserved_commit(to->reservation) :
        xrt_managed_slot_commit(to->frame,to->slot);
    if (status!=XRT_MANAGED_OK) {
        XrtManagedStatus cleanup=to->reservation ? xrt_managed_slot_reserved_abort(to->reservation) :
            xrt_managed_slot_abort(to->frame,to->slot);
        XR_CHECK(cleanup==XRT_MANAGED_OK,"only this Set candidate is aborted");
    }
    return status;
}
static XrtManagedStatus set_edit(const XrtSetBacking *set, const XrtSetUpdate *update, SetEdit *output) {
    if (!set_shape(set) || !update) return XRT_MANAGED_ARGUMENT;
    SetEdit edit={.length=set->length,.capacity=set->capacity};
    if (update->operation==XRT_SET_UPDATE_CLEAR) {
        edit.noop=!set->length; edit.length=edit.capacity=0;
    } else if (update->operation==XRT_SET_UPDATE_ADD || update->operation==XRT_SET_UPDATE_REMOVE) {
        XrtManagedStatus status=set_probe(set,update->key,&edit.probe);
        if (status!=XRT_MANAGED_OK) return status;
        if (update->operation==XRT_SET_UPDATE_ADD) {
            edit.noop=edit.probe.found;
            if (!edit.probe.found) ++edit.length;
            status=set_capacity(set->capacity,edit.length,&edit.capacity);
            if (status!=XRT_MANAGED_OK) return status;
        } else {
            edit.noop=!edit.probe.found;
            if (edit.probe.found) --edit.length;
        }
    } else return XRT_MANAGED_ARGUMENT;
    *output=edit; return XRT_MANAGED_OK;
}
static XrtManagedStatus set_delete_preflight(const XrtSetBacking *set, SetProbe probe, SetDelete *output) {
    SetDelete plan={0}; size_t last=set->length-1,mask=set->capacity-1;
    size_t position=set->rows[probe.entry].entry_order;
    if (position>=set->order_count || set->rows[position].order!=probe.entry+1) return XRT_MANAGED_ARGUMENT;
    for (size_t p=position;p<set->capacity;p|=p+1)
        if (!set->rows[p].rank || set->rows[p].rank>set->length) return XRT_MANAGED_ARGUMENT;
    if (probe.entry!=last) {
        size_t bucket=(size_t)set->rows[last].hash&mask; bool found=false;
        for (size_t n=0;n<set->capacity;++n,bucket=(bucket+1)&mask) {
            size_t entry=set->rows[bucket].bucket;
            if (!entry || entry>set->length) return XRT_MANAGED_ARGUMENT;
            if (entry==last+1) { plan.last_bucket=bucket; found=true; break; }
        }
        if (!found) return XRT_MANAGED_ARGUMENT;
    }
    size_t bucket=(probe.bucket+1)&mask;
    for (size_t n=0;n<set->capacity;++n,bucket=(bucket+1)&mask) {
        if (!set->rows[bucket].bucket) { plan.end=bucket; *output=plan; return XRT_MANAGED_OK; }
        if (set->rows[bucket].bucket>set->length) return XRT_MANAGED_ARGUMENT;
    }
    return XRT_MANAGED_ARGUMENT;
}
static void set_delete_publish(XrtSetBacking *set, SetProbe probe, SetDelete plan) {
    size_t last=set->length-1,mask=set->capacity-1,hole=probe.bucket;
    size_t position=set->rows[probe.entry].entry_order;
    set->rows[position].order=0;
    for (size_t p=position;p<set->capacity;p|=p+1) --set->rows[p].rank;
    for (size_t bucket=(hole+1)&mask;bucket!=plan.end;bucket=(bucket+1)&mask) {
        size_t entry=set->rows[bucket].bucket-1,home=(size_t)set->rows[entry].hash&mask;
        if (((bucket-home)&mask)>=((bucket-hole)&mask)) {
            set->rows[hole].bucket=set->rows[bucket].bucket;
            if (entry==last) plan.last_bucket=hole;
            hole=bucket;
        }
    }
    set->rows[hole].bucket=0;
    if (probe.entry!=last) {
        set->rows[probe.entry].key=set->rows[last].key; set->rows[probe.entry].hash=set->rows[last].hash;
        set->rows[probe.entry].entry_order=set->rows[last].entry_order;
        set->rows[plan.last_bucket].bucket=probe.entry+1;
        set->rows[set->rows[probe.entry].entry_order].order=probe.entry+1;
    }
    set->rows[last].key=0; set->rows[last].hash=0; set->rows[last].entry_order=0;
    if (!--set->length) set->order_count=0;
}
XR_FUNCDEF XrtManagedStatus xrt_set_backing_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const int64_t *values, size_t count) {
    if (count && !values) return XRT_MANAGED_ARGUMENT;
    size_t capacity=0; XrtManagedStatus status=set_capacity(0,count,&capacity);
    if (status!=XRT_MANAGED_OK) return status;
    XrtSetBacking *set=NULL; SetDestination to={frame,slot,types,NULL};
    status=set_prepare(&to,capacity,&set); if (status!=XRT_MANAGED_OK) return status;
    for (size_t n=0;n<count;++n) {
        SetProbe probe={0}; status=set_probe(set,values[n],&probe);
        if (status==XRT_MANAGED_OK && !probe.found) status=set_append(set,values[n],probe.hash);
        if (status!=XRT_MANAGED_OK) break;
    }
    if (status==XRT_MANAGED_OK) set_order_rebuild_ranks(set);
    return set_finish(&to,status);
}
XR_FUNCDEF XrtManagedStatus xrt_set_backing_contains(const XrtSetBacking *set, int64_t key, bool *found) {
    if (!set_shape(set) || !found) return XRT_MANAGED_ARGUMENT;
    SetProbe probe={0}; XrtManagedStatus status=set_probe(set,key,&probe);
    if (status==XRT_MANAGED_OK) *found=probe.found;
    return status;
}
static const XrtSetBackingRow *set_order_row(const XrtSetBacking *set, size_t ordinal) {
    if (ordinal>=set->length) return NULL;
    size_t position=0;
    for (size_t step=set->capacity;step;step>>=1) {
        size_t next=position+step;
        if (next<=set->capacity && set->rows[next-1].rank<=ordinal) {
            ordinal-=set->rows[next-1].rank; position=next;
        }
    }
    if (position>=set->order_count) return NULL;
    size_t entry=set->rows[position].order;
    if (!entry || entry>set->length || set->rows[entry-1].entry_order!=position) return NULL;
    return &set->rows[entry-1];
}
XR_FUNCDEF XrtManagedStatus xrt_set_backing_at(const XrtSetBacking *set, size_t ordinal, int64_t *value) {
    if (!set_shape(set) || !value) return XRT_MANAGED_ARGUMENT;
    const XrtSetBackingRow *row=set_order_row(set,ordinal);
    if (!row) return XRT_MANAGED_ARGUMENT;
    *value=row->key;
    return XRT_MANAGED_OK;
}

enum SetFormatNode { SET_FORMAT_ROOT, SET_FORMAT_KEY };
static int set_format_read(const void *context, XrValueFormatNode node, XrValueFormatView *view) {
    if (node.type==SET_FORMAT_ROOT) {
        view->kind=XR_VALUE_FORMAT_SET;
        view->unsigned_value=((const XrtSetBacking *)context)->length;
    } else if (node.type==SET_FORMAT_KEY) {
        view->kind=XR_VALUE_FORMAT_SIGNED;
        view->signed_value=*(const int64_t *)node.value;
    } else return 0;
    return 1;
}
static int set_format_child(const void *context, XrValueFormatNode node, uint32_t index, XrValueFormatNode *child) {
    if (node.type!=SET_FORMAT_ROOT) return 0;
    const XrtSetBackingRow *row=set_order_row(context,index);
    if (!row) return 0;
    *child=(XrValueFormatNode){&row->key,SET_FORMAT_KEY};
    return 1;
}
XR_FUNCDEF XrtManagedStatus xrt_set_backing_display_held(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtSetBacking *set) {
    if (!set_shape(set)) return XRT_MANAGED_ARGUMENT;
    if (set->length>INT64_MAX) return XRT_MANAGED_OVERFLOW;
    XrValueFormatReader reader={set,set_format_read,set_format_child};
    return xrt_string_backing_format_create(frame,slot,types,reader,(XrValueFormatNode){set,SET_FORMAT_ROOT});
}
XR_FUNCDEF XrtManagedStatus xrt_set_backing_try_update(XrtSetBacking *set,
    const XrtSetUpdate *update, bool *applied, bool *removed) {
    if (!applied || !removed) return XRT_MANAGED_ARGUMENT;
    *applied=false; SetEdit edit={0}; XrtManagedStatus status=set_edit(set,update,&edit);
    if (status!=XRT_MANAGED_OK) return status;
    if (edit.noop) { *applied=true; *removed=false; return XRT_MANAGED_OK; }
    int32_t references=xr_signed_rc_read(&set->managed.references);
    if (edit.capacity>set->capacity || (references!=1 && references!=-1)) return XRT_MANAGED_OK;
    SetDelete deletion={0};
    if (update->operation==XRT_SET_UPDATE_REMOVE) {
        status=set_delete_preflight(set,edit.probe,&deletion);
        if (status!=XRT_MANAGED_OK) return status;
    }
    if (!xr_signed_rc_unique(&set->managed.references)) return XRT_MANAGED_OK;
    if (update->operation==XRT_SET_UPDATE_ADD) set_publish(set,update->key,edit.probe.hash,edit.probe.bucket);
    else if (update->operation==XRT_SET_UPDATE_REMOVE) set_delete_publish(set,edit.probe,deletion);
    else { memset(set->rows,0,set->capacity*sizeof(*set->rows)); set->length=set->order_count=0; }
    *applied=true; *removed=update->operation==XRT_SET_UPDATE_REMOVE; return XRT_MANAGED_OK;
}
static XrtManagedStatus set_update_create(const SetDestination *to, const XrtSetBacking *source,
    const XrtSetUpdate *update, bool *removed) {
    if (!removed) return XRT_MANAGED_ARGUMENT;
    SetEdit edit={0}; XrtManagedStatus status=set_edit(source,update,&edit);
    if (status!=XRT_MANAGED_OK) return status;
    XrtSetBacking *set=NULL; status=set_prepare(to,edit.capacity,&set);
    if (status!=XRT_MANAGED_OK) return status;
    if (update->operation!=XRT_SET_UPDATE_CLEAR) {
        size_t copied=0;
        for (size_t p=0;p<source->order_count;++p) {
            size_t entry=source->rows[p].order;
            if (!entry) continue;
            if (entry>source->length || source->rows[entry-1].entry_order!=p) { status=XRT_MANAGED_ARGUMENT; break; }
            ++copied;
            if (update->operation==XRT_SET_UPDATE_REMOVE && edit.probe.found && entry-1==edit.probe.entry) continue;
            status=set_append(set,source->rows[entry-1].key,source->rows[entry-1].hash);
            if (status!=XRT_MANAGED_OK) break;
        }
        if (status==XRT_MANAGED_OK && copied!=source->length) status=XRT_MANAGED_ARGUMENT;
        if (status==XRT_MANAGED_OK && update->operation==XRT_SET_UPDATE_ADD && !edit.probe.found)
            status=set_append(set,update->key,edit.probe.hash);
    }
    bool did_remove=update->operation==XRT_SET_UPDATE_REMOVE && edit.probe.found;
    if (status==XRT_MANAGED_OK) set_order_rebuild_ranks(set);
    status=set_finish(to,status);
    if (status==XRT_MANAGED_OK) *removed=did_remove;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_set_backing_update_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtSetBacking *source, const XrtSetUpdate *update, bool *removed) {
    SetDestination to={frame,slot,types,NULL}; return set_update_create(&to,source,update,removed);
}
typedef struct SetBindingUpdate { const XrtSetUpdate *update; XrtSetBacking *source; bool applied, removed; } SetBindingUpdate;
static XrtManagedStatus set_binding_try(XrtManagedHeader *value, void *context) {
    SetBindingUpdate *binding=context; binding->source=(XrtSetBacking *)value;
    return xrt_set_backing_try_update(binding->source,binding->update,&binding->applied,&binding->removed);
}
XR_FUNCDEF XrtManagedStatus xrt_set_update_place(XrtPlace binding, XrtManagedFrame *frame, size_t workspace,
    const XrtBackingTypes *types, const XrtSetUpdate *update, bool *removed) {
    if (!types || !types->set || !update || !removed) return XRT_MANAGED_ARGUMENT;
    XrtPlaceReplacement replacement={0}; XrtManagedStatus status=xrt_place_replacement_begin(binding,frame,workspace,&replacement);
    if (status!=XRT_MANAGED_OK) return status;
    SetBindingUpdate edit={.update=update};
    status=xrt_managed_slot_reserved_apply(&replacement,set_binding_try,&edit);
    if (status==XRT_MANAGED_OK && !edit.applied) {
        SetDestination to={frame,workspace,types,&replacement};
        status=set_update_create(&to,edit.source,update,&edit.removed);
        if (status==XRT_MANAGED_OK) status=xrt_place_replacement_publish(&replacement);
    }
    XR_CHECK(xrt_place_replacement_end(&replacement)==XRT_MANAGED_OK,"every Set edit releases its real domains after cleanup");
    if (status==XRT_MANAGED_OK) *removed=edit.removed;
    return status;
}
