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
static bool rbc_value_array_region_access(XrRbcStack *stack, RbcValueActivation *activation,
    XrRbcValueAccess access, RbcArrayRegionState *state, XrtManagedStatus *status) {
    XrRbcRegister *output = access.result ? rbc_value_register(stack, activation, access.result) : NULL;
    if (access.index == XR_LIR_ARRAY_OP_LEN) {
        if (!output) *status = XRT_MANAGED_ARGUMENT;
        else output->bits = (uint64_t)state->cache.length;
        return true;
    }
    const uint32_t *arguments = activation->code->arguments + access.first;
    bool read = access.index == XR_LIR_ARRAY_OP_GET;
    const XrRbcRegister *index = rbc_value_register(stack, activation, arguments[1]);
    const XrRbcRegister *item = read ? NULL : rbc_value_register(stack, activation, arguments[2]);
    if (!index || (read ? !output : !item)) {
        *status = XRT_MANAGED_ARGUMENT;
        return true;
    }
    int64_t at;
    memcpy(&at, &index->bits, sizeof(at));
    if (at < 0 || (uint64_t)at >= state->cache.length) return false;
    unsigned char *element = state->cache.elements + (size_t)at * sizeof(int64_t);
    if (read) memcpy(&output->bits, element, sizeof(output->bits));
    else memcpy(element, &item->bits, sizeof(item->bits));
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

static bool rbc_value_fixed_array_push(XrRbcStack *stack, RbcValueActivation *activation,
    uint32_t region, XrRbcValueAccess access, XrtManagedStatus *status) {
    RbcArrayRegionState *state = &activation->array_regions[region - 1];
    XrtArrayBacking *array = (XrtArrayBacking *)state->cache.owner;
    size_t length = state->cache.length;
    if (length == array->capacity || (uint64_t)length == (uint64_t)INT64_MAX) {
        /* Resume this PUSH with its evaluated RHS and real Cell. A successful
         * ordinary growth can install a different backing for the next latch. */
        rbc_value_array_region_forget(activation, region, RBC_ARRAY_REGION_COLD);
        return false;
    }
    const uint32_t *arguments = activation->code->arguments + access.first;
    const XrRbcRegister *item = rbc_value_register(stack, activation, arguments[1]);
    if (!item) { *status = XRT_MANAGED_ARGUMENT; return true; }
    memcpy(state->cache.elements + length * sizeof(item->bits), &item->bits, sizeof(item->bits));
    array->length = state->cache.length = length + 1;
    return true;
}

/* At the real latch, ordinary owner and scalar copies have already
 * published the header values. The remaining recurrence is wrapping i64. */
static XR_NOINLINE void rbc_value_fixed_array_counted_push(XrRbcStack *stack, RbcValueActivation *activation,
    const XrLirFixedArrayUpdateHome *row, const XrRbcArrayCountedPush *counted, uint32_t region) {
    if (!activation->backings_ready) return;
    XrRbcRegister *iv = rbc_value_register(stack, activation, counted->iv);
    if (!iv) return;
    uint64_t start = iv->bits, end = counted->bits;
    if (counted->limit) {
        const XrRbcRegister *limit = rbc_value_register(stack, activation, counted->limit);
        if (!limit || limit == iv) return;
        end = limit->bits;
    }
    if (start > INT64_MAX || end > INT64_MAX || start >= end) return;

    XrtArrayI64RegionSlot slots[XR_LIR_FIXED_ARRAY_GUARD_SLOT_COUNT];
    const XrtArrayI64RegionSpec spec = xr_lir_fixed_array_guard_spec(activation->code->homes, row, slots);
    const XrtArrayI64RegionContext context = {activation->frame, activation->payload,
        &activation->frame_locations, &activation->code->homes->frame};
    XrtArrayI64RegionCache cache = {0};
    if (!xrt_array_i64_region_try_begin(&context, &spec, true, &cache)) {
        rbc_value_array_region_forget(activation, region, RBC_ARRAY_REGION_BYPASS);
        return;
    }
    XrtArrayBacking *array = (XrtArrayBacking *)cache.owner;
    size_t room = array->capacity - cache.length;
    uint64_t length_room = (uint64_t)INT64_MAX - cache.length, remaining = end - start;
    if (length_room < room) room = (size_t)length_room;
    size_t count = remaining < room ? (size_t)remaining : room;
    const XrLirArrayPushSpan *span = &row->region.push_span;
    const XrLirArrayPushUpdate *updates = NULL;
    if (span->update_count) {
        updates = activation->code->homes->array_push_updates + span->first_update;
        for (uint32_t n = 0; n < span->update_count; ++n)
            if (!rbc_value_register(stack, activation, updates[n].value)) return;
    }
    uint64_t item = span->factor * start + span->bias;
    for (size_t n = 0; n < count; ++n) {
        memcpy(cache.elements + (cache.length + n) * sizeof(item), &item, sizeof(item));
        item += span->factor;
    }
    array->length = cache.length + count;
    iv->bits = start + count;
    for (uint32_t n = 0; n < span->update_count; ++n) {
        XrRbcRegister *value = rbc_value_register(stack, activation, updates[n].value);
        value->bits += (uint64_t)count * updates[n].delta;
    }
    rbc_value_array_region_forget(activation, region, RBC_ARRAY_REGION_COLD);
}

static bool rbc_value_fixed_array_before(XrRbcStack *stack, RbcValueActivation *activation,
    const XrRbcArrayRegionAccess *role, XrRbcValueAccess access, XrtManagedStatus *status) {
    RbcArrayRegionState *state = &activation->array_regions[role->region - 1];
    if (role->fixed_edge) {
        const XrRbcValueCode *code = activation->code;
        const XrLirValueHomes *homes = code->homes;
        size_t ordinal = role->region - 1;
        const XrLirFixedArrayUpdateHome *row = &homes->fixed_array_updates[ordinal];
        if (role->fixed_edge == XR_LIR_ARRAY_REGION_EDGE_ENTRY) {
            /* The selected BODY follows publication of the initial owner.
             * Reads can borrow it immediately; writes keep the first transaction. */
            RbcArrayRegionPhase phase = row->region.primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_GET ?
                RBC_ARRAY_REGION_ELIGIBLE : RBC_ARRAY_REGION_COLD;
            rbc_value_array_region_forget(activation, role->region, phase);
        } else if (role->fixed_edge == XR_LIR_ARRAY_REGION_EDGE_EXIT)
            rbc_value_array_region_forget(activation, role->region, RBC_ARRAY_REGION_BYPASS);
        else if (role->fixed_edge == XR_LIR_ARRAY_REGION_EDGE_LATCH &&
                state->phase == RBC_ARRAY_REGION_COLD && state->completed_write) {
            state->phase = RBC_ARRAY_REGION_ELIGIBLE;
            const XrRbcArrayCountedPush *counted = code->array_counted_push ? &code->array_counted_push[ordinal] : NULL;
            if (counted && counted->iv)
                rbc_value_fixed_array_counted_push(stack, activation, row, counted, role->region);
        } else if (role->sum_checkpoint && state->phase == RBC_ARRAY_REGION_ACTIVE && !state->sum_attempted) {
            state->sum_attempted = true;
            rbc_value_array_read_fold(stack, activation, &row->region, &state->cache);
        } else if (role->fixed_edge == XR_LIR_ARRAY_REGION_EDGE_BODY && state->phase == RBC_ARRAY_REGION_ELIGIBLE) {
            XrtArrayI64RegionSlot slots[XR_LIR_FIXED_ARRAY_GUARD_SLOT_COUNT];
            const XrtArrayI64RegionSpec spec = xr_lir_fixed_array_guard_spec(homes, row, slots);
            rbc_value_array_region_begin(activation, role->region, state, &spec);
        }
        return true; /* The actual scalar copies and ownership publication are separate words. */
    }
    if (state->phase != RBC_ARRAY_REGION_ACTIVE || access.operation != XR_RBC_VALUE_ARRAY) return false;
    if (access.index == XR_LIR_ARRAY_OP_PUSH)
        return rbc_value_fixed_array_push(stack, activation, role->region, access, status);
    if (access.index == XR_LIR_ARRAY_OP_LEN || access.index == XR_LIR_ARRAY_OP_GET || access.index == XR_LIR_ARRAY_OP_SET) {
        if (rbc_value_array_region_access(stack, activation, access, state, status) && *status == XRT_MANAGED_OK)
            return true;
    }
    rbc_value_array_region_forget(activation, role->region, RBC_ARRAY_REGION_BYPASS);
    return *status != XRT_MANAGED_OK;
}

static bool rbc_value_array_region_before(XrRbcStack *stack, RbcValueActivation *activation,
    uint32_t index, XrtManagedStatus *status) {
    /* Only sealed bytecode owns the registered routes and their state. */
    if (!activation->array_regions) return false;
    const XrRbcArrayRegionAccess *role = &activation->code->array_region_accesses[index];
    if (activation->active_array_region && activation->active_array_region != role->region)
        rbc_value_array_region_forget(activation, activation->active_array_region, RBC_ARRAY_REGION_BYPASS);
    return role->region && rbc_value_fixed_array_before(stack, activation, role, activation->code->accesses[index], status);
}

static void rbc_value_array_region_after(RbcValueActivation *activation, uint32_t index) {
    const XrRbcArrayRegionAccess *role = &activation->code->array_region_accesses[index];
    if (!role->region) return;
    XrRbcValueAccess access = activation->code->accesses[index];
    RbcArrayRegionState *state = &activation->array_regions[role->region - 1];
    if (access.operation == XR_RBC_VALUE_ARRAY && state->phase == RBC_ARRAY_REGION_COLD &&
        (access.index == XR_LIR_ARRAY_OP_SET || access.index == XR_LIR_ARRAY_OP_PUSH))
        state->completed_write = true;
}
