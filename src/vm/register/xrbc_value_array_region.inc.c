/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_array_region.inc.c - Borrowed windows over actual activation owners
 */
static void rbc_value_array_region_forget(RbcValueActivation *activation,uint32_t region,RbcArrayRegionPhase phase) {
    RbcArrayRegionState *state=&activation->array_regions[region-1];
    *state=(RbcArrayRegionState){.phase=phase};
    if (activation->active_array_region==region) activation->active_array_region=0;
}
static void rbc_value_array_region_move(RbcValueActivation *activation,const XrLirArrayRegionMove *move,
    const XrtArrayI64RegionCache *cache) {
    const XrtArrayI64RegionSlot *slots=activation->code->array_regions->slots;
    XrtManagedHeader **from=(XrtManagedHeader **)(activation->payload+slots[move->source_slot].offset);
    XrtManagedHeader **to=(XrtManagedHeader **)(activation->payload+slots[move->target_slot].offset);
    *to=cache->owner; *from=NULL;
}
static XrtManagedStatus rbc_value_array_region_edge(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,const XrLirArrayRegionMove *move,
    const XrtArrayI64RegionCache *cache) {
    const XrRbcValueCode *code=activation->code;
    const XrLirHomeEdge *edge=&code->homes->edges[access.index];
    RbcValueEdgeBuilder builder={.module=module,.stack=stack,.activation=activation};
    activation->publication_count=0;
    XrtManagedStatus status=XRT_MANAGED_OK;
    for (size_t n=0;status==XRT_MANAGED_OK && n<edge->count;++n) {
        const XrLirHomeTransfer *transfer=&code->homes->transfers[edge->first+n];
        if (transfer->source==move->step.source && transfer->target==move->step.target) {
            size_t bytes=code->homes->values[transfer->target].size;
            if (builder.rows>=activation->publication_capacity || builder.bytes>activation->snapshot_capacity ||
                bytes>activation->snapshot_capacity-builder.bytes) return XRT_MANAGED_ARGUMENT;
            ++builder.rows; builder.bytes+=bytes;
        } else status=rbc_value_edge_pair(&builder,transfer->source,transfer->target,transfer->transfer_mode,transfer);
    }
    for (uint32_t n=0;status==XRT_MANAGED_OK && n<access.count;++n) {
        uint32_t source=code->arguments[access.first+(size_t)n*2],target=code->arguments[access.first+(size_t)n*2+1];
        const XrLirValueHome *home=rbc_value_home(code,target);
        if (home && home->type) continue;
        status=rbc_value_edge_pair(&builder,source,target,XR_LIR_OWNERSHIP_TRANSFER_BORROW,NULL);
    }
    if (status!=XRT_MANAGED_OK) return status;
    /* The common edge has one Array and only scalar parallel arguments.
     * Snapshot all scalars before either the real owner or scalar publication. */
    if (builder.owners) return XRT_MANAGED_ARGUMENT;
    rbc_value_array_region_move(activation,move,cache);
    rbc_value_publication(activation);
    return XRT_MANAGED_OK;
}
static bool rbc_value_array_region_access(XrRbcStack *stack,RbcValueActivation *activation,
    const XrLirArrayRegionAccess *access,RbcArrayRegionState *state,XrtManagedStatus *status) {
    XrRbcRegister *output=access->result ? rbc_value_register(stack,activation,access->result) : NULL;
    if (access->operation==XR_LIR_ARRAY_OP_LEN) {
        if (!output) *status=XRT_MANAGED_ARGUMENT;
        else output->bits=(uint64_t)state->cache.length;
        return true;
    }
    XrRbcRegister *index=rbc_value_register(stack,activation,access->index);
    XrRbcRegister *item=access->item ? rbc_value_register(stack,activation,access->item) : NULL;
    if (!index || (access->operation==XR_LIR_ARRAY_OP_GET ? !output : !item)) {
        *status=XRT_MANAGED_ARGUMENT; return true;
    }
    int64_t at; memcpy(&at,&index->bits,sizeof(at));
    if (at<0 || (uint64_t)at>=(uint64_t)state->cache.length) return false;
    unsigned char *element=state->cache.elements+(size_t)at*sizeof(int64_t);
    if (access->operation==XR_LIR_ARRAY_OP_GET) memcpy(&output->bits,element,sizeof(output->bits));
    else memcpy(element,&item->bits,sizeof(item->bits));
    return true;
}

static void rbc_value_array_region_begin(RbcValueActivation *activation, uint32_t region,
    RbcArrayRegionState *state, const XrtArrayI64RegionSpec *spec) {
    const XrtArrayI64RegionContext context={activation->frame,activation->payload,
        &activation->frame_locations,&activation->code->homes->frame};
    if (xrt_array_i64_region_try_begin(&context,spec,state->phase==RBC_ARRAY_REGION_ELIGIBLE,&state->cache)) {
        state->phase=RBC_ARRAY_REGION_ACTIVE;
        activation->active_array_region=region;
    } else rbc_value_array_region_forget(activation,region,RBC_ARRAY_REGION_BYPASS);
}

static bool rbc_value_fixed_array_before(XrRbcStack *stack, RbcValueActivation *activation,
    const XrRbcArrayRegionAccess *role, XrRbcValueAccess access, XrtManagedStatus *status) {
    RbcArrayRegionState *state = &activation->array_regions[role->region - 1];
    if (role->fixed_edge) {
        const XrRbcValueCode *code = activation->code;
        const XrLirValueHomes *homes = code->homes;
        const XrLirFixedArrayUpdateHome *row =
            &homes->fixed_array_updates[role->region - 1 - rbc_value_legacy_array_region_count(code)];
        if (role->fixed_edge == XR_LIR_ARRAY_REGION_EDGE_ENTRY) {
            /* The selected BODY follows publication of the initial owner.
             * Reads can borrow it immediately; writes keep the first transaction. */
            RbcArrayRegionPhase phase = row->region.primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_GET ?
                RBC_ARRAY_REGION_ELIGIBLE : RBC_ARRAY_REGION_COLD;
            rbc_value_array_region_forget(activation, role->region, phase);
        } else if (role->fixed_edge == XR_LIR_ARRAY_REGION_EDGE_EXIT)
            rbc_value_array_region_forget(activation, role->region, RBC_ARRAY_REGION_BYPASS);
        else if (role->fixed_edge == XR_LIR_ARRAY_REGION_EDGE_LATCH &&
                state->phase == RBC_ARRAY_REGION_COLD && state->completed_set)
            state->phase = RBC_ARRAY_REGION_ELIGIBLE;
        else if (role->fixed_edge == XR_LIR_ARRAY_REGION_EDGE_BODY && state->phase == RBC_ARRAY_REGION_ELIGIBLE) {
            XrtArrayI64RegionSlot slots[XR_LIR_FIXED_ARRAY_GUARD_SLOT_COUNT];
            const XrtArrayI64RegionSpec spec = xr_lir_fixed_array_guard_spec(homes, row, slots);
            rbc_value_array_region_begin(activation, role->region, state, &spec);
        }
        return true; /* The actual scalar copies and ownership publication are separate words. */
    }
    if (state->phase != RBC_ARRAY_REGION_ACTIVE || access.operation != XR_RBC_VALUE_ARRAY) return false;
    if (access.index == XR_LIR_ARRAY_OP_LEN || access.index == XR_LIR_ARRAY_OP_GET || access.index == XR_LIR_ARRAY_OP_SET) {
        const uint32_t *arguments = activation->code->arguments + access.first;
        XrLirArrayRegionAccess operation = {.operation=(XrLirArrayOperation)access.index, .result=access.result,
            .index=access.count > 1 ? arguments[1] : 0, .item=access.count > 2 ? arguments[2] : 0};
        if (rbc_value_array_region_access(stack, activation, &operation, state, status) && *status == XRT_MANAGED_OK)
            return true;
    }
    rbc_value_array_region_forget(activation, role->region, RBC_ARRAY_REGION_BYPASS);
    return *status != XRT_MANAGED_OK;
}

static bool rbc_value_array_region_before(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,uint32_t index,XrtManagedStatus *status) {
    /* Raw modules may borrow the same numeric descriptions, but never receive
     * these states: their mutable CFG cannot borrow the producer's window. */
    if (!activation->array_regions) return false;
    const XrRbcArrayRegionAccess *role=&activation->code->array_region_accesses[index];
    if (activation->active_array_region && activation->active_array_region!=role->region)
        rbc_value_array_region_forget(activation,activation->active_array_region,RBC_ARRAY_REGION_BYPASS);
    if (!role->region) return false;
    XrRbcValueAccess access=activation->code->accesses[index];
    if (role->fixed) return rbc_value_fixed_array_before(stack,activation,role,access,status);
    const XrLirArrayRegionBindings *bindings=activation->code->array_regions;
    const XrLirArrayRegionBinding *region=&bindings->regions[role->region-1];
    RbcArrayRegionState *state=&activation->array_regions[role->region-1];
    if (access.operation==XR_RBC_VALUE_EDGE) {
        const XrLirArrayRegionEdge *edge=&bindings->edges[access.index];
        const XrLirArrayRegionMove *move=&bindings->moves[edge->move-1];
        if (edge->kind==XR_LIR_ARRAY_REGION_EDGE_ENTRY) {
            rbc_value_array_region_forget(activation,role->region,RBC_ARRAY_REGION_COLD); return false;
        }
        if (state->phase==RBC_ARRAY_REGION_ELIGIBLE && move->step.block==region->region.header &&
            edge->kind==XR_LIR_ARRAY_REGION_EDGE_BODY) {
            const XrtArrayI64RegionSpec spec=xr_lir_array_region_guard_spec(bindings,region);
            rbc_value_array_region_begin(activation,role->region,state,&spec);
        }
        if (state->phase!=RBC_ARRAY_REGION_ACTIVE) return false;
        *status=rbc_value_array_region_edge(module,stack,activation,access,move,&state->cache);
        if (*status!=XRT_MANAGED_OK || edge->kind==XR_LIR_ARRAY_REGION_EDGE_EXIT)
            rbc_value_array_region_forget(activation,role->region,RBC_ARRAY_REGION_BYPASS);
        return true;
    }
    if (state->phase!=RBC_ARRAY_REGION_ACTIVE) return false;
    XrLirArrayRegionOperation operation=role->operation;
    if (operation.move) rbc_value_array_region_move(activation,&bindings->moves[operation.move-1],&state->cache);
    else if (operation.access) {
        if (!rbc_value_array_region_access(stack,activation,&bindings->accesses[operation.access-1],state,status)) {
            /* Resume this exact ordinary operation with its RHS and real
             * current owner intact. No prefix or consumed pair is replayed. */
            rbc_value_array_region_forget(activation,role->region,RBC_ARRAY_REGION_BYPASS); return false;
        }
    }
    if (*status!=XRT_MANAGED_OK) rbc_value_array_region_forget(activation,role->region,RBC_ARRAY_REGION_BYPASS);
    return true;
}
static void rbc_value_array_region_after(RbcValueActivation *activation,uint32_t index) {
    if (!activation->array_regions) return;
    const XrRbcArrayRegionAccess *role=&activation->code->array_region_accesses[index];
    if (!role->region) return;
    XrRbcValueAccess access=activation->code->accesses[index];
    RbcArrayRegionState *state=&activation->array_regions[role->region-1];
    if (role->fixed) {
        if (access.operation==XR_RBC_VALUE_ARRAY && access.index==XR_LIR_ARRAY_OP_SET && state->phase==RBC_ARRAY_REGION_COLD)
            state->completed_set=true;
        return;
    }
    const XrLirArrayRegionBindings *bindings=activation->code->array_regions;
    if (access.operation==XR_RBC_VALUE_EDGE) {
        XrLirArrayRegionEdgeKind kind=bindings->edges[access.index].kind;
        if (kind==XR_LIR_ARRAY_REGION_EDGE_LATCH && state->phase==RBC_ARRAY_REGION_COLD && state->completed_set)
            state->phase=RBC_ARRAY_REGION_ELIGIBLE;
        if (kind==XR_LIR_ARRAY_REGION_EDGE_EXIT)
            rbc_value_array_region_forget(activation,role->region,RBC_ARRAY_REGION_BYPASS);
    } else if (role->operation.access && state->phase==RBC_ARRAY_REGION_COLD &&
        bindings->accesses[role->operation.access-1].operation==XR_LIR_ARRAY_OP_SET) state->completed_set=true;
}
