/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array_region_binding.c - Owned numeric coordinates from sealed homes
 *
 * KEY CONCEPT:
 *   Existing steps describe the closed window. This projection binds their
 *   real fields and operation positions without revisiting the source body.
 */
#include "xlir_array_region_binding.h"
#include "base/xmalloc.h"
#include <string.h>

typedef struct RegionBindingVector { void *data; size_t count, capacity; } RegionBindingVector;
typedef struct RegionBindingBuilder {
    const XrLirValueHomes *homes;
    const XrLirValueLayouts *layouts;
    XrLirArrayRegionBindings *output;
    RegionBindingVector slots, moves, accesses, blocks, operations;
    uint32_t *slot_stamps, *block_next, *block_heads, *block_tails;
    size_t *slot_indices, *event_heads, *event_tails, *event_next;
    size_t *edge_index, *workspace_first, *workspace_index, *binding_index;
    uint32_t stamp;
} RegionBindingBuilder;

static XrLirValueLayoutStatus region_binding_storage(void **output, size_t count, size_t stride) {
    if (count && stride>SIZE_MAX/count) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    if (!count) return XR_LIR_VALUE_LAYOUT_OK;
    *output=xr_calloc(count,stride);
    return *output ? XR_LIR_VALUE_LAYOUT_OK : XR_LIR_VALUE_LAYOUT_OOM;
}
static XrLirValueLayoutStatus region_binding_append(RegionBindingVector *v, const void *row,
    size_t count, size_t stride) {
    if (count>SIZE_MAX-v->count || (v->count+count && stride>SIZE_MAX/(v->count+count)))
        return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    size_t needed=v->count+count;
    if (needed>v->capacity) {
        size_t capacity=v->capacity ? v->capacity : 8;
        while (capacity<needed) {
            if (capacity>SIZE_MAX/2) { capacity=needed; break; }
            capacity*=2;
        }
        if (stride>SIZE_MAX/capacity) capacity=needed;
        void *grown=xr_realloc(v->data,capacity*stride);
        if (!grown) return XR_LIR_VALUE_LAYOUT_OOM;
        v->data=grown; v->capacity=capacity;
    }
    if (count) {
        void *destination=(unsigned char *)v->data+v->count*stride;
        if (row) memcpy(destination,row,count*stride); else memset(destination,0,count*stride);
    }
    v->count=needed; return XR_LIR_VALUE_LAYOUT_OK;
}
XR_FUNCDEF void xr_lir_array_region_bindings_destroy(XrLirArrayRegionBindings *bindings) {
    if (!bindings) return;
    xr_free((void *)bindings->regions); xr_free((void *)bindings->slots);
    xr_free((void *)bindings->moves); xr_free((void *)bindings->accesses);
    xr_free((void *)bindings->blocks); xr_free((void *)bindings->block_index);
    xr_free((void *)bindings->operations); xr_free((void *)bindings->edges); xr_free(bindings);
}
static void region_binding_scratch_free(RegionBindingBuilder *b) {
    xr_free(b->slot_stamps); xr_free(b->slot_indices); xr_free(b->block_next);
    xr_free(b->block_heads); xr_free(b->block_tails); xr_free(b->event_heads);
    xr_free(b->event_tails); xr_free(b->event_next); xr_free(b->edge_index);
    xr_free(b->workspace_first); xr_free(b->workspace_index); xr_free(b->binding_index);
}
static bool region_binding_frame(const XrtManagedFrameSpec *frame) {
    if (!frame->slot_count || !frame->owned_offsets || frame->payload_alignment<_Alignof(XrtManagedHeader *) ||
        (frame->payload_alignment&(frame->payload_alignment-1))) return false;
    for (size_t n=0;n<frame->slot_count;++n) {
        size_t offset=frame->owned_offsets[n];
        if (offset%_Alignof(XrtManagedHeader *) || offset>frame->payload_size ||
            sizeof(XrtManagedHeader *)>frame->payload_size-offset ||
            (n && (offset<frame->owned_offsets[n-1] ||
                offset-frame->owned_offsets[n-1]<sizeof(XrtManagedHeader *)))) return false;
    }
    return true;
}
static XrLirValueLayoutStatus region_binding_type(const RegionBindingBuilder *b, uint32_t type) {
    if (!type || type>xr_lir_value_layouts_count(b->layouts)) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    const XrtValuePlan *plan=NULL; XrLirValueLayoutKey key; XrtValuePlanSpec spec;
    XrLirValueLayoutStatus status=xr_lir_value_layouts_at(b->layouts,type-1,&key,&plan);
    if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
    if (xrt_value_plan_view(plan,&spec)!=XRT_MANAGED_OK) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    return spec.kind==XRT_VALUE_ARRAY_I64 && spec.size==sizeof(XrtManagedHeader *) &&
        spec.alignment==_Alignof(XrtManagedHeader *) && spec.owned_count==1 &&
        spec.owned_offsets && !spec.owned_offsets[0] ? XR_LIR_VALUE_LAYOUT_OK : XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
}
static XrLirValueLayoutStatus region_binding_slot(RegionBindingBuilder *b, size_t slot,
    size_t offset, size_t *index) {
    const XrtManagedFrameSpec *frame=&b->homes->frame;
    if (slot>=frame->slot_count || offset==SIZE_MAX || frame->owned_offsets[slot]!=offset)
        return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
    if (b->slot_stamps[slot]==b->stamp) { *index=b->slot_indices[slot]; return XR_LIR_VALUE_LAYOUT_OK; }
    XrtArrayI64RegionSlot row={slot,offset}; *index=b->slots.count;
    XrLirValueLayoutStatus status=region_binding_append(&b->slots,&row,1,sizeof(row));
    if (status==XR_LIR_VALUE_LAYOUT_OK) { b->slot_stamps[slot]=b->stamp; b->slot_indices[slot]=*index; }
    return status;
}
static XrLirValueLayoutStatus region_binding_value(RegionBindingBuilder *b, uint32_t value, size_t *index) {
    if (!value || value>b->homes->value_count) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    const XrLirValueHome *home=&b->homes->values[value];
    const XrLirHomeBinding *binding=b->homes->bindings ? &b->homes->bindings[value] : NULL;
    if (binding && binding->type) {
        if (binding->kind!=XR_LIR_HOME_BINDING_LOCAL || binding->parameter || binding->source)
            return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
        XrLirValueLayoutStatus status=region_binding_type(b,binding->type);
        return status==XR_LIR_VALUE_LAYOUT_OK ? region_binding_slot(b,binding->first_owner,binding->offset,index) : status;
    }
    XrLirValueLayoutStatus status=region_binding_type(b,home->type);
    if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
    if (home->parameter || home->owner_count!=1 || home->size!=sizeof(XrtManagedHeader *) || home->byte_count ||
        home->transport!=XR_LIR_CALLABLE_TRANSPORT_NONE || home->borrow_source || home->view_workspace)
        return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
    status=region_binding_slot(b,home->first_owner,home->offset,index);
    if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
    if (home->temporary_first_owner==SIZE_MAX && home->temporary_offset==SIZE_MAX) return XR_LIR_VALUE_LAYOUT_OK;
    size_t ignored=0;
    return region_binding_slot(b,home->temporary_first_owner,home->temporary_offset,&ignored);
}
static XrLirValueLayoutStatus region_binding_block(RegionBindingBuilder *b, uint32_t region,
    uint32_t block, uint32_t instruction) {
    if (!block || block>=b->output->block_index_count || !instruction) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    XrLirArrayRegionBlock *index=(XrLirArrayRegionBlock *)b->output->block_index;
    if (index[block].region && index[block].region!=region+1) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    if (!index[block].region) {
        index[block].region=region+1;
        if (b->block_tails[region]) b->block_next[b->block_tails[region]]=block;
        else b->block_heads[region]=block;
        b->block_tails[region]=block;
    }
    if (instruction>index[block].instruction_count) index[block].instruction_count=instruction;
    return XR_LIR_VALUE_LAYOUT_OK;
}
static XrLirValueLayoutStatus region_binding_prepare(RegionBindingBuilder *b) {
    const XrLirValueHomes *h=b->homes; size_t maximum=0;
    for (uint32_t n=0;n<h->array_region_count;++n) {
        XrLirArraySetRegion r=h->array_regions[n];
        if (r.first_step>h->array_region_step_count || r.step_count>h->array_region_step_count-r.first_step ||
            r.function!=h->source_function || !r.primary_block || !r.primary_instruction ||
            r.push_counted ||
            (r.primary_kind!=XR_LIR_ARRAY_REGION_PRIMARY_SET && r.primary_kind!=XR_LIR_ARRAY_REGION_PRIMARY_GET))
            return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        uint32_t blocks[]={r.header,r.entry_block,r.latch,r.exit_block};
        for (size_t p=0;p<4;++p) if (blocks[p]>maximum) maximum=blocks[p];
    }
    for (uint32_t n=0;n<h->array_region_step_count;++n)
        if (h->array_region_steps[n].block>maximum) maximum=h->array_region_steps[n].block;
    for (size_t n=0;n<h->edge_count;++n) {
        if (h->edges[n].block>maximum) maximum=h->edges[n].block;
        if (h->edges[n].target_block>maximum) maximum=h->edges[n].target_block;
    }
    if (maximum==SIZE_MAX) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    b->output->block_index_count=maximum+1; b->output->edge_count=h->edge_count;
    XrLirValueLayoutStatus status=XR_LIR_VALUE_LAYOUT_OK;
#define REGION_STORAGE(field,count,type) do { \
    void *memory=NULL; status=region_binding_storage(&memory,(count),sizeof(type)); \
    (field)=memory; \
    if (status!=XR_LIR_VALUE_LAYOUT_OK) return status; \
} while (0)
    REGION_STORAGE(b->output->block_index,maximum+1,XrLirArrayRegionBlock);
    REGION_STORAGE(b->output->edges,h->edge_count,XrLirArrayRegionEdge);
    if (maximum+1>SIZE_MAX/2) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
    REGION_STORAGE(b->edge_index,(maximum+1)*2,size_t);
    REGION_STORAGE(b->block_next,maximum+1,uint32_t);
    REGION_STORAGE(b->workspace_first,maximum+1,size_t);
    REGION_STORAGE(b->block_heads,h->array_region_count,uint32_t);
    REGION_STORAGE(b->block_tails,h->array_region_count,uint32_t);
    REGION_STORAGE(b->event_heads,h->array_region_count,size_t);
    REGION_STORAGE(b->event_tails,h->array_region_count,size_t);
    REGION_STORAGE(b->event_next,h->event_count,size_t);
    REGION_STORAGE(b->slot_stamps,h->frame.slot_count,uint32_t);
    REGION_STORAGE(b->slot_indices,h->frame.slot_count,size_t);
#undef REGION_STORAGE
    for (size_t n=0;n<h->edge_count;++n) {
        const XrLirHomeEdge *edge=&h->edges[n];
        if (!edge->block || !edge->edge || edge->edge>2 || edge->first>h->transfer_count ||
            edge->count>h->transfer_count-edge->first) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        size_t key=(size_t)edge->block*2+edge->edge-1;
        if (b->edge_index[key]) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        b->edge_index[key]=n+1;
    }
    for (uint32_t n=0;n<h->array_region_count;++n) {
        const XrLirArraySetRegion *r=&h->array_regions[n]; size_t count=0;
        for (uint32_t p=0;p<r->step_count;++p) {
            XrLirArraySetRegionStep step=h->array_region_steps[r->first_step+p];
            if (step.kind==XR_LIR_ARRAY_SET_REGION_EDGE_MOVE || step.kind==XR_LIR_ARRAY_SET_REGION_EXIT_MOVE) {
                status=region_binding_block(b,n,step.block,step.instruction);
                if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
            }
        }
        for (uint32_t block=b->block_heads[n];block;block=b->block_next[block]) ++count;
        if (!r->header || r->header>=b->output->block_index_count ||
            ((const XrLirArrayRegionBlock *)b->output->block_index)[r->header].region!=n+1 ||
            count!=(size_t)r->body_block_count+1) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    }
    return XR_LIR_VALUE_LAYOUT_OK;
}
static XrLirValueLayoutStatus region_binding_positions(RegionBindingBuilder *b) {
    const XrLirValueHomes *h=b->homes; size_t extent=0;
    const XrLirArrayRegionBlock *blocks=b->output->block_index;
    for (size_t block=1;block<b->output->block_index_count;++block) {
        b->workspace_first[block]=extent;
        if (blocks[block].instruction_count>SIZE_MAX-extent) return XR_LIR_VALUE_LAYOUT_OVERFLOW;
        extent+=blocks[block].instruction_count;
    }
    void *storage=NULL;
    XrLirValueLayoutStatus status=region_binding_storage(&storage,extent,sizeof(size_t));
    b->workspace_index=storage;
    if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
    storage=NULL; status=region_binding_storage(&storage,extent,sizeof(size_t)); b->binding_index=storage;
    if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
    for (size_t n=0;n<h->workspace_count;++n) {
        XrLirHomeWorkspace row=h->workspaces[n];
        if (row.block>=b->output->block_index_count || !blocks[row.block].region ||
            row.kind!=XR_LIR_HOME_WORKSPACE_VALUE) continue;
        if (!row.instruction || row.instruction>blocks[row.block].instruction_count)
            return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        size_t at=b->workspace_first[row.block]+row.instruction-1;
        if (b->workspace_index[at]) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        b->workspace_index[at]=n+1;
    }
    for (size_t n=0;n<h->event_count;++n) {
        const XrLirHomeEvent *event=&h->events[n];
        if (event->block>=b->output->block_index_count || !blocks[event->block].region) continue;
        bool binding=event->kind==XR_LIR_OWNERSHIP_BINDING_NEW || event->kind==XR_LIR_OWNERSHIP_BINDING_READ;
        if (event->kind!=XR_LIR_OWNERSHIP_ARRAY_OPERATION && !binding) continue;
        if (!event->instruction || event->instruction>blocks[event->block].instruction_count)
            return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        if (binding) {
            size_t at=b->workspace_first[event->block]+event->instruction-1;
            if (b->binding_index[at]) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
            b->binding_index[at]=n+1; continue;
        }
        size_t region=blocks[event->block].region-1;
        if (b->event_tails[region]) b->event_next[b->event_tails[region]-1]=n+1;
        else b->event_heads[region]=n+1;
        b->event_tails[region]=n+1;
    }
    return XR_LIR_VALUE_LAYOUT_OK;
}
static XrLirValueLayoutStatus region_binding_access(RegionBindingBuilder *b, const XrLirHomeEvent *event) {
    if (event->operation!=XR_LIR_ARRAY_OP_LEN && event->operation!=XR_LIR_ARRAY_OP_GET &&
        event->operation!=XR_LIR_ARRAY_OP_SET) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
    XrLirArrayRegionAccess row={.block=event->block,.instruction=event->instruction,.receiver=event->source,
        .result=event->array_result,.index=event->array_index,.item=event->array_item,
        .operation=(XrLirArrayOperation)event->operation,.workspace_slot=SIZE_MAX};
    if (row.result>b->homes->value_count || row.index>b->homes->value_count || row.item>b->homes->value_count ||
        (row.operation==XR_LIR_ARRAY_OP_SET ? row.result || !row.index || !row.item :
            !row.result || row.item || (row.operation==XR_LIR_ARRAY_OP_GET ? !row.index : row.index!=0)))
        return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    XrLirValueLayoutStatus status=region_binding_value(b,row.receiver,&row.receiver_slot);
    if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
    if (row.operation==XR_LIR_ARRAY_OP_SET) {
        size_t workspace=b->workspace_index[b->workspace_first[row.block]+row.instruction-1];
        if (!workspace) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
        const XrLirHomeWorkspace *w=&b->homes->workspaces[workspace-1];
        status=region_binding_type(b,w->type);
        if (status==XR_LIR_VALUE_LAYOUT_OK) status=region_binding_slot(b,w->first_owner,w->offset,&row.workspace_slot);
        if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
    }
    return region_binding_append(&b->accesses,&row,1,sizeof(row));
}
static XrLirValueLayoutStatus region_binding_owner_home(const RegionBindingBuilder *b,
    XrLirArrayRegionBinding *row) {
    XrLirArraySetRegionOwnerHome home = row->region.owner_home;
    if (home.kind == XR_LIR_ARRAY_SET_OWNER_HOME_NONE)
        return home.first_active_step || home.exit_step ? XR_LIR_VALUE_LAYOUT_ARGUMENT : XR_LIR_VALUE_LAYOUT_OK;
    uint32_t expected = row->region.primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_GET ?
        XR_LIR_ARRAY_SET_OWNER_HOME_READ_ONLY_WINDOW : XR_LIR_ARRAY_SET_OWNER_HOME_CANONICAL_WINDOW;
    uint32_t range = row->region.range.kind;
    if (home.kind != expected ||
        (range != XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN &&
            (range != XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT ||
                expected != XR_LIR_ARRAY_SET_OWNER_HOME_READ_ONLY_WINDOW)) ||
        home.first_active_step < 2 || home.first_active_step >= home.exit_step || home.exit_step != row->move_count ||
        row->move_count != row->region.step_count) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    const XrLirArrayRegionMove *moves = b->moves.data;
    size_t first = home.first_active_step - 1, last = home.exit_step - 1;
    if (first != 1 || moves[row->first_move].step.kind != XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE)
        return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    for (size_t n = first; n < last; ++n) {
        XrLirArraySetRegionStep step = moves[row->first_move + n].step;
        if (step.kind != XR_LIR_ARRAY_SET_REGION_EDGE_MOVE && step.kind != XR_LIR_ARRAY_SET_REGION_NEW_CANDIDATE &&
            step.kind != XR_LIR_ARRAY_SET_REGION_READ_CANDIDATE) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    }
    XrLirArrayRegionMove begin = moves[row->first_move + first], exit = moves[row->first_move + last];
    if (begin.step.kind != XR_LIR_ARRAY_SET_REGION_EDGE_MOVE || begin.step.block != row->region.header || begin.step.edge != 1 ||
        begin.step.source != row->region.canonical_value || begin.source_slot != row->canonical_slot ||
        exit.step.kind != XR_LIR_ARRAY_SET_REGION_EXIT_MOVE || exit.step.source != row->region.canonical_value ||
        exit.step.target != row->region.exit_value || exit.step.block != row->region.header ||
        exit.step.edge != row->region.exit_edge || exit.source_slot != row->canonical_slot)
        return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    if (row->canonical_slot == exit.target_slot) return XR_LIR_VALUE_LAYOUT_OK;
    /* These are the original typed Frame fields. Only the optional window
     * projects a stable home; the real admission still probes every old slot. */
    row->owner_home = (XrLirArrayRegionOwnerHomeBinding){home.kind,row->canonical_slot,exit.target_slot};
    return XR_LIR_VALUE_LAYOUT_OK;
}
static XrLirValueLayoutStatus region_binding_primary(const RegionBindingBuilder *b,
    const XrLirArrayRegionBinding *row) {
    XrLirArraySetRegion region=row->region;
    size_t gets=0,sets=0,matches=0;
    const XrLirArrayRegionAccess *accesses=b->accesses.data;
    for (size_t n=0;n<row->access_count;++n) {
        XrLirArrayRegionAccess access=accesses[row->first_access+n];
        gets+=access.operation==XR_LIR_ARRAY_OP_GET; sets+=access.operation==XR_LIR_ARRAY_OP_SET;
        if (access.block==region.primary_block && access.instruction==region.primary_instruction) {
            if (access.operation!=(region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_SET ? XR_LIR_ARRAY_OP_SET : XR_LIR_ARRAY_OP_GET))
                return XR_LIR_VALUE_LAYOUT_ARGUMENT;
            ++matches;
        }
    }
    if (matches!=1) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    if (region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_SET) return sets==1 ? XR_LIR_VALUE_LAYOUT_OK : XR_LIR_VALUE_LAYOUT_ARGUMENT;
    bool guarded=region.range.kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT &&
        region.range.length && region.range.length<=b->homes->value_count;
    return gets==1 && !sets && (region.range.kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN || guarded) ?
        XR_LIR_VALUE_LAYOUT_OK : XR_LIR_VALUE_LAYOUT_ARGUMENT;
}
static XrLirValueLayoutStatus region_binding_project(RegionBindingBuilder *b, uint32_t ordinal,
    XrLirArrayRegionBinding *row) {
    XrLirArraySetRegion r=b->homes->array_regions[ordinal];
    *row=(XrLirArrayRegionBinding){.region=r,.first_slot=b->slots.count,.canonical_slot=SIZE_MAX,
        .first_move=b->moves.count,.first_access=b->accesses.count,.first_block=b->blocks.count};
    XrLirValueLayoutStatus status=region_binding_value(b,r.canonical_value,&row->canonical_slot);
    if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
    for (uint32_t n=0;n<r.step_count;++n) {
        XrLirArrayRegionMove move={.step=b->homes->array_region_steps[r.first_step+n]};
        if (move.step.kind==XR_LIR_ARRAY_SET_REGION_NEW_CANDIDATE || move.step.kind==XR_LIR_ARRAY_SET_REGION_READ_CANDIDATE) {
            if (!move.step.block || move.step.block>=b->output->block_index_count || !move.step.instruction ||
                move.step.instruction>=b->output->block_index[move.step.block].instruction_count ||
                move.step.drop!=move.step.instruction+1) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
            size_t event=b->binding_index[b->workspace_first[move.step.block]+move.step.instruction-1];
            bool create=move.step.kind==XR_LIR_ARRAY_SET_REGION_NEW_CANDIDATE;
            if (!event) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
            const XrLirHomeEvent *fact=&b->homes->events[event-1];
            if (fact->kind!=(uint32_t)(create ? XR_LIR_OWNERSHIP_BINDING_NEW : XR_LIR_OWNERSHIP_BINDING_READ) ||
                fact->source!=(create ? move.step.target : move.step.source) ||
                fact->target!=(create ? move.step.source : move.step.target) ||
                fact->transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE ||
                fact->forwarding_mode!=XR_LIR_OWNERSHIP_TRANSFER_MOVE ||
                fact->forwarding_drop!=move.step.drop) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        }
        status=region_binding_value(b,move.step.source,&move.source_slot);
        if (status==XR_LIR_VALUE_LAYOUT_OK) status=region_binding_value(b,move.step.target,&move.target_slot);
        if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
        if (move.source_slot==move.target_slot) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
        status=region_binding_append(&b->moves,&move,1,sizeof(move));
        if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
    }
    for (size_t event=b->event_heads[ordinal];event;event=b->event_next[event-1]) {
        status=region_binding_access(b,&b->homes->events[event-1]);
        if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
    }
    row->slot_count=b->slots.count-row->first_slot; row->move_count=b->moves.count-row->first_move;
    row->access_count=b->accesses.count-row->first_access;
    status=region_binding_primary(b,row);
    if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
    return region_binding_owner_home(b,row);
}
static XrLirArrayRegionOperation *region_binding_operation(RegionBindingBuilder *b, uint32_t block,
    uint32_t instruction) {
    if (!block || block>=b->output->block_index_count) return NULL;
    const XrLirArrayRegionBlock *row=&b->output->block_index[block];
    if (row->region!=b->stamp || !instruction || instruction>row->instruction_count) return NULL;
    return &((XrLirArrayRegionOperation *)b->operations.data)[row->first_operation+instruction-1];
}
static XrLirValueLayoutStatus region_binding_edges_available(const RegionBindingBuilder *b,
    const XrLirArrayRegionBinding *region) {
    const XrLirArrayRegionMove *moves=b->moves.data;
    for (size_t n=0;n<region->move_count;++n) {
        XrLirArraySetRegionStep step=moves[region->first_move+n].step;
        if (step.kind==XR_LIR_ARRAY_SET_REGION_NEW_CANDIDATE || step.kind==XR_LIR_ARRAY_SET_REGION_READ_CANDIDATE)
            continue;
        if (!step.block || step.block>=b->output->block_index_count || !step.edge || step.edge>2)
            return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        size_t edge=b->edge_index[(size_t)step.block*2+step.edge-1];
        if (!edge) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        /* One loop's exit can be another loop's entry. Stable SET-first
         * projection preserves existing SET coverage; a conflicting optional
         * read-only window keeps its original ordinary schedule. */
        if (b->output->edges[edge-1].region) return XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
    }
    return XR_LIR_VALUE_LAYOUT_OK;
}
static XrLirValueLayoutStatus region_binding_edge(RegionBindingBuilder *b, const XrLirArrayRegionMove *move,
    size_t index, const XrLirArraySetRegion *region) {
    XrLirArraySetRegionStep step=move->step;
    if (!step.block || step.block>=b->output->block_index_count || !step.edge || step.edge>2)
        return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    size_t edge=b->edge_index[(size_t)step.block*2+step.edge-1];
    if (!edge) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    const XrLirHomeEdge *home=&b->homes->edges[edge-1]; bool matched=false;
    for (size_t n=0;n<home->count;++n) {
        const XrLirHomeTransfer *transfer=&b->homes->transfers[home->first+n];
        if (transfer->source==step.source && transfer->target==step.target &&
            transfer->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE) matched=true;
    }
    if (!matched) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    XrLirArrayRegionEdge *rows=(XrLirArrayRegionEdge *)b->output->edges;
    if (rows[edge-1].region) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    XrLirArrayRegionEdgeKind kind=step.kind==XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE ? XR_LIR_ARRAY_REGION_EDGE_ENTRY :
        step.kind==XR_LIR_ARRAY_SET_REGION_EXIT_MOVE ? XR_LIR_ARRAY_REGION_EDGE_EXIT :
        step.block==region->latch ? XR_LIR_ARRAY_REGION_EDGE_LATCH : XR_LIR_ARRAY_REGION_EDGE_BODY;
    rows[edge-1]=(XrLirArrayRegionEdge){b->stamp,kind,index+1};
    ((XrLirArrayRegionBlock *)b->output->block_index)[step.block].edges[step.edge-1]=edge;
    return XR_LIR_VALUE_LAYOUT_OK;
}
static XrLirValueLayoutStatus region_binding_index(RegionBindingBuilder *b, uint32_t ordinal,
    XrLirArrayRegionBinding *row) {
    XrLirArrayRegionBlock *blocks=(XrLirArrayRegionBlock *)b->output->block_index;
    for (uint32_t block=b->block_heads[ordinal];block;block=b->block_next[block]) {
        blocks[block].first_operation=b->operations.count;
        XrLirValueLayoutStatus status=region_binding_append(&b->operations,NULL,blocks[block].instruction_count,
            sizeof(XrLirArrayRegionOperation));
        if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
        status=region_binding_append(&b->blocks,&block,1,sizeof(block));
        if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
        ++row->block_count;
    }
    for (size_t n=0;n<row->move_count;++n) {
        size_t index=row->first_move+n;
        XrLirArrayRegionMove move=((const XrLirArrayRegionMove *)b->moves.data)[index];
        if (move.step.kind==XR_LIR_ARRAY_SET_REGION_NEW_CANDIDATE || move.step.kind==XR_LIR_ARRAY_SET_REGION_READ_CANDIDATE) {
            XrLirArrayRegionOperation *op=region_binding_operation(b,move.step.block,move.step.instruction);
            XrLirArrayRegionOperation *drop=region_binding_operation(b,move.step.block,move.step.drop);
            if (!op || !drop || op->move || drop->drop) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
            op->move=index+1; drop->drop=index+1;
        } else {
            XrLirValueLayoutStatus status=region_binding_edge(b,&move,index,&row->region);
            if (status!=XR_LIR_VALUE_LAYOUT_OK) return status;
        }
    }
    for (size_t n=0;n<row->access_count;++n) {
        size_t index=row->first_access+n;
        XrLirArrayRegionAccess access=((const XrLirArrayRegionAccess *)b->accesses.data)[index];
        XrLirArrayRegionOperation *op=region_binding_operation(b,access.block,access.instruction);
        if (!op || op->access) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
        op->access=index+1;
    }
    return XR_LIR_VALUE_LAYOUT_OK;
}
static void region_binding_unsupported(RegionBindingBuilder *b, uint32_t ordinal,
    const XrLirArrayRegionBinding *pending) {
    b->slots.count=pending->first_slot; b->moves.count=pending->first_move;
    b->accesses.count=pending->first_access; b->blocks.count=pending->first_block;
    XrLirArrayRegionBlock *blocks=(XrLirArrayRegionBlock *)b->output->block_index;
    for (uint32_t block=b->block_heads[ordinal];block;block=b->block_next[block]) {
        /* A supported successor may already index its entry at this block.
         * This candidate has published no edges, so keep those earlier roles. */
        blocks[block].region=0; blocks[block].first_operation=0; blocks[block].instruction_count=0;
    }
}
XR_FUNCDEF XrLirValueLayoutStatus xr_lir_array_region_bindings_create(const XrLirValueHomes *homes,
    const XrLirValueLayouts *layouts, XrLirArrayRegionBindings **output) {
    if (!homes || !layouts || !output || *output || (homes->value_count && !homes->values) ||
        (homes->event_count && !homes->events) || (homes->edge_count && !homes->edges) ||
        (homes->transfer_count && !homes->transfers) || (homes->workspace_count && !homes->workspaces) ||
        (homes->array_region_count && !homes->array_regions) ||
        (homes->array_region_step_count && !homes->array_region_steps)) return XR_LIR_VALUE_LAYOUT_ARGUMENT;
    XrLirArrayRegionBindings *result=xr_calloc(1,sizeof(*result));
    if (!result) return XR_LIR_VALUE_LAYOUT_OOM;
    RegionBindingBuilder b={.homes=homes,.layouts=layouts,.output=result};
    result->region_count=homes->array_region_count;
    void *regions=NULL;
    XrLirValueLayoutStatus status=region_binding_storage(&regions,result->region_count,sizeof(XrLirArrayRegionBinding));
    result->regions=regions;
    if (status==XR_LIR_VALUE_LAYOUT_OK && homes->array_region_count) status=region_binding_prepare(&b);
    if (status==XR_LIR_VALUE_LAYOUT_OK && homes->array_region_count) status=region_binding_positions(&b);
    bool frame=status==XR_LIR_VALUE_LAYOUT_OK && homes->array_region_count && region_binding_frame(&homes->frame);
    /* Only physical publication order changes. Each class keeps original
     * order, and stamps/region rows retain their original numeric identity. */
    for (unsigned pass=0;status==XR_LIR_VALUE_LAYOUT_OK && pass<2;++pass)
    for (uint32_t n=0;status==XR_LIR_VALUE_LAYOUT_OK && n<homes->array_region_count;++n) {
        if ((pass==0)!=(homes->array_regions[n].primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_SET)) continue;
        b.stamp=n+1; XrLirArrayRegionBinding row={0};
        if (frame) status=region_binding_project(&b,n,&row);
        else {
            row=(XrLirArrayRegionBinding){.region=homes->array_regions[n],.first_slot=b.slots.count,
                .first_move=b.moves.count,.first_access=b.accesses.count,.first_block=b.blocks.count,.canonical_slot=SIZE_MAX};
            status=XR_LIR_VALUE_LAYOUT_UNSUPPORTED;
        }
        if (status==XR_LIR_VALUE_LAYOUT_OK) status=region_binding_edges_available(&b,&row);
        if (status==XR_LIR_VALUE_LAYOUT_UNSUPPORTED) {
            region_binding_unsupported(&b,n,&row);
            row=(XrLirArrayRegionBinding){.region=homes->array_regions[n],.canonical_slot=SIZE_MAX};
            status=XR_LIR_VALUE_LAYOUT_OK;
        } else if (status==XR_LIR_VALUE_LAYOUT_OK) status=region_binding_index(&b,n,&row);
        ((XrLirArrayRegionBinding *)result->regions)[n]=row;
    }
    region_binding_scratch_free(&b);
    if (status!=XR_LIR_VALUE_LAYOUT_OK) {
        xr_free(b.slots.data); xr_free(b.moves.data); xr_free(b.accesses.data); xr_free(b.blocks.data); xr_free(b.operations.data);
        xr_lir_array_region_bindings_destroy(result); return status;
    }
    result->slots=b.slots.data; result->slot_count=b.slots.count;
    result->moves=b.moves.data; result->move_count=b.moves.count;
    result->accesses=b.accesses.data; result->access_count=b.accesses.count;
    result->blocks=b.blocks.data; result->block_count=b.blocks.count;
    result->operations=b.operations.data; result->operation_count=b.operations.count;
    *output=result; return XR_LIR_VALUE_LAYOUT_OK;
}
