/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_map_execute.inc.c - Fixed Map owners and common binding updates
 */
static XrtManagedStatus rbc_value_map_literal(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *activation, XrRbcValueAccess access, uint32_t source) {
    if (!access.index || access.index > module->map_literal_count || !module->map_literals)
        return XRT_MANAGED_ARGUMENT;
    const XrtMapLiteral *literal = &module->map_literals[access.index - 1];
    if (literal->count > activation->snapshot_capacity / sizeof(XrtMapLiteralInput)) return XRT_MANAGED_ARGUMENT;
    XrtMapLiteralInput *inputs = (XrtMapLiteralInput *)activation->snapshots;
    uint32_t operand = 0;
    for (size_t n = 0; n < literal->count; ++n) {
        if (operand >= access.count) return XRT_MANAGED_ARGUMENT;
        uint32_t value = activation->code->arguments[access.first + operand++];
        if (literal->items[n].kind) {
            const XrtMapBacking *map = NULL;
            XrtManagedStatus status = rbc_value_map_borrow(module,activation,value,&map);
            if (status != XRT_MANAGED_OK) return status;
            inputs[n] = (XrtMapLiteralInput){.source.spread=map};
        } else {
            const XrtStringBacking *key = NULL;
            XrtManagedStatus status = rbc_value_string_borrow(activation,value,&key);
            if (status != XRT_MANAGED_OK) return status;
            if (operand >= access.count) return XRT_MANAGED_ARGUMENT;
            value = activation->code->arguments[access.first + operand++];
            XrRbcRegister *scalar = rbc_value_register(stack,activation,value);
            if (!scalar) return XRT_MANAGED_ARGUMENT;
            inputs[n] = (XrtMapLiteralInput){.source.key=key};
            memcpy(&inputs[n].value,&scalar->bits,sizeof(inputs[n].value));
        }
    }
    if (operand != access.count) return XRT_MANAGED_ARGUMENT;
    const XrLirValueHome *home = rbc_value_home(activation->code,access.result);
    if (!home) return XRT_MANAGED_ARGUMENT;
    xrt_native_map_literal_create(activation->frame,home->temporary_first_owner,literal,inputs,source);
    return rbc_value_array_publish(activation,access.result);
}
static XrtManagedStatus rbc_value_map_new(XrRbcStack *stack,RbcValueActivation *activation,
    XrRbcValueAccess access,const XrtBackingTypes *types) {
    size_t count=access.count/2;
    if (count>activation->snapshot_capacity/(sizeof(XrtStringBacking *)+sizeof(int64_t))) return XRT_MANAGED_ARGUMENT;
    XrtStringBacking **keys=(XrtStringBacking **)activation->snapshots;
    int64_t *values=(int64_t *)(activation->snapshots+count*sizeof(*keys));
    for (uint32_t p=0;p<access.count;p+=2) {
        const XrtStringBacking *key=NULL;
        XrtManagedStatus status=rbc_value_string_borrow(activation,activation->code->arguments[access.first+p],&key);
        if (status!=XRT_MANAGED_OK) return status;
        XrRbcRegister *value=rbc_value_register(stack,activation,activation->code->arguments[access.first+p+1]);
        if (!value) return XRT_MANAGED_ARGUMENT;
        keys[p/2]=(XrtStringBacking *)key;
        memcpy(&values[p/2],&value->bits,8);
    }
    const XrLirValueHome *home=rbc_value_home(activation->code,access.result);
    XrtManagedStatus status=xrt_map_backing_create(activation->frame,home->temporary_first_owner,types,keys,values,count);
    return status==XRT_MANAGED_OK ? rbc_value_array_publish(activation,access.result) : status;
}
static inline const XrtManagedHeader *rbc_map_borrow_local_slot(
    const XrtManagedFrameLocations *locations, uint32_t index) {
    if (index >= locations->slot_count) return NULL;
    const XrtManagedSlot *slot = &locations->slots[index];
    if (slot->owner_offset == SIZE_MAX || slot->pending.value || slot->pending.allocation_bytes) return NULL;
    const unsigned char *payload = locations->payload;
    return *(XrtManagedHeader *const *)(payload + slot->owner_offset);
}

static XrtManagedStatus rbc_value_map_edit_local(XrRbcStack *stack, RbcValueActivation *activation,
    XrRbcValueAccess access, const uint32_t *args, bool *applied) {
    const XrtManagedFrameLocations *locations = &activation->frame_locations;
    if (!locations->frame || *locations->busy) return XRT_MANAGED_OK;
    bool remove = access.index == XR_LIR_MAP_OP_REMOVE;
    XrRbcRegister *item = rbc_value_register(stack, activation, remove ? access.result : args[2]);
    if (!item) return XRT_MANAGED_ARGUMENT;
    XrtMapUpdate update = {.operation=remove ? XRT_MAP_UPDATE_REMOVE : XRT_MAP_UPDATE_SET};
    if (access.other) update.key = (const XrtStringBacking *)rbc_map_borrow_local_slot(locations, access.other - 1);
    else {
        XrtManagedStatus status = rbc_value_string_borrow(activation, args[1], &update.key);
        if (status != XRT_MANAGED_OK) return status;
    }
    if (!update.key) return XRT_MANAGED_ARGUMENT;
    if (!remove) memcpy(&update.value, &item->bits, sizeof(update.value));
    const XrLirHomeWorkspace *stage = &activation->code->homes->workspaces[access.window];
    bool removed = false;
    XrtManagedStatus status = xrt_map_try_update_local(locations, access.source - 1,
        stage->first_owner, &update, applied, &removed);
    if (*applied && remove) item->bits = removed;
    return status;
}

static XrtManagedStatus rbc_value_map_step(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,uint32_t source) {
    const uint32_t *args=access.count ? activation->code->arguments+access.first : NULL;
    bool mutate=access.index==XR_LIR_MAP_OP_SET || access.index==XR_LIR_MAP_OP_REMOVE || access.index==XR_LIR_MAP_OP_CLEAR;
    const XrtBackingTypes *types=NULL;
    if (access.index==XR_LIR_MAP_OP_NEW || mutate) {
        XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source),&types);
        if (status!=XRT_MANAGED_OK) return status;
        activation->backings_ready=true;
    }
    if (access.index==XR_LIR_MAP_OP_NEW) return rbc_value_map_new(stack,activation,access,types);
    if (mutate) {
        XrtExternalOwner external; XrtPlace binding;
        XrtManagedStatus status=rbc_value_binding_place(module,stack,rbc_value_origin(activation,args[0]),&external,&binding);
        XrtMapUpdate update={.operation=access.index==XR_LIR_MAP_OP_SET ? XRT_MAP_UPDATE_SET :
            access.index==XR_LIR_MAP_OP_REMOVE ? XRT_MAP_UPDATE_REMOVE : XRT_MAP_UPDATE_CLEAR};
        if (status==XRT_MANAGED_OK && access.count>1) status=rbc_value_string_borrow(activation,args[1],&update.key);
        if (status!=XRT_MANAGED_OK) return status;
        if (access.index==XR_LIR_MAP_OP_SET) {
            XrRbcRegister *value=rbc_value_register(stack,activation,args[2]);
            if (!value) return XRT_MANAGED_ARGUMENT;
            memcpy(&update.value,&value->bits,8);
        }
        const XrLirHomeWorkspace *workspace=&activation->code->homes->workspaces[access.window];
        bool removed=false;
        status=xrt_map_update_place(binding,activation->frame,workspace->first_owner,types,&update,&removed);
        if (status==XRT_MANAGED_OK && access.result) {
            XrRbcRegister *result=rbc_value_register(stack,activation,access.result);
            if (!result) return XRT_MANAGED_ARGUMENT;
            result->bits=removed;
        }
        return status;
    }
    const XrtMapBacking *map = NULL;
    XrtManagedStatus status = rbc_value_map_borrow(module, activation, args[0], &map);
    if (status != XRT_MANAGED_OK) return status;
    if (!map) return XRT_MANAGED_ARGUMENT;
    if (access.index==XR_LIR_MAP_OP_LEN) {
        XrRbcRegister *result=rbc_value_register(stack,activation,access.result);
        if (!result) return XRT_MANAGED_ARGUMENT;
        if (map->length>INT64_MAX) return XRT_MANAGED_OVERFLOW;
        result->bits=map->length; return XRT_MANAGED_OK;
    }
    if (access.index==XR_LIR_MAP_OP_DISPLAY || xr_lir_map_snapshot_element(access.index)) {
        const XrLirValueHome *home=rbc_value_home(activation->code,access.result);
        XrtMapProjection projection=access.index==XR_LIR_MAP_OP_DISPLAY ? XRT_MAP_DISPLAY :
            access.index==XR_LIR_MAP_OP_KEYS ? XRT_MAP_KEYS : XRT_MAP_VALUES;
        xrt_native_map_project(activation->frame,home->temporary_first_owner,&map->managed,projection,source);
        return rbc_value_array_publish(activation,access.result);
    }
    return XRT_MANAGED_ARGUMENT;
}

static XrtManagedStatus rbc_map_read(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *activation, const XrRbcValueAccess *access, const XrtStringBacking *key, bool *missing) {
    const XrtManagedFrameLocations *locations = &activation->frame_locations;
    if ((access->source || access->other) && (!locations->frame || *locations->busy))
        return XRT_MANAGED_ARGUMENT;

    const XrtMapBacking *map = NULL;
    const uint32_t *args = activation->code->arguments + access->first;
    if (access->source) {
        map = (const XrtMapBacking *)rbc_map_borrow_local_slot(locations, access->source - 1);
    } else {
        const RbcMapParameter *parameter = access->window != UINT32_MAX ?
            &activation->parameter_storage[access->window].map : NULL;
        if (parameter && parameter->address) {
            if (parameter->slot && (*parameter->busy || parameter->slot->pending.value ||
                parameter->slot->pending.allocation_bytes)) return XRT_MANAGED_ARGUMENT;
            memcpy(&map, parameter->address, sizeof(map));
        } else {
            XrtManagedStatus status = rbc_value_map_borrow(module, activation, args[0], &map);
            if (status != XRT_MANAGED_OK) return status;
        }
    }
    if (!map) return XRT_MANAGED_ARGUMENT;
    if (!key) {
        if (access->other) {
            key = (const XrtStringBacking *)rbc_map_borrow_local_slot(locations, access->other - 1);
        } else {
            XrtManagedStatus status = rbc_value_string_borrow(activation, args[1], &key);
            if (status != XRT_MANAGED_OK) return status;
        }
    }
    if (!key) return XRT_MANAGED_ARGUMENT;

    int64_t value = 0;
    bool found = false;
    XrtManagedStatus status = xrt_map_backing_get(map, key, &value, &found);
    if (status != XRT_MANAGED_OK) return status;
    *missing = access->index == XR_LIR_MAP_OP_INDEX && !found;
    if (*missing) return XRT_MANAGED_OK;
    if (access->index == XR_LIR_MAP_OP_GET) {
        RbcValuePayload payload;
        status = rbc_value_payload(module, stack, (RbcValueOrigin){activation, access->result}, &payload);
        if (status != XRT_MANAGED_OK) return status;
        const XrLirValueHome *home = rbc_value_home(activation->code, access->result);
        const XrtValueImageType *type = &activation->image->types[home->type - 1];
        memset(payload.address, 0, payload.size);
        payload.address[type->nullable_tag] = (uint8_t)found;
        memcpy(payload.address + type->nullable_payload, &value, sizeof(value));
    } else {
        XrRbcRegister *result = rbc_value_register(stack, activation, access->result);
        if (!result) return XRT_MANAGED_ARGUMENT;
        if (access->index == XR_LIR_MAP_OP_CONTAINS) result->bits = found;
        else memcpy(&result->bits, &value, sizeof(value));
    }
    return XRT_MANAGED_OK;
}

static XR_NOINLINE XrRbcStatus xr_rbc_map_read_step(const XrRbcModule *module, XrRbcStack *stack,
    uint32_t index, size_t pc, uint32_t literal_index) {
    RbcValueActivation *activation = stack->values ? stack->values->top : NULL;
    if ((!activation || activation->depth != stack->depth) && stack->depth) {
        size_t first_pc = literal_index == UINT32_MAX ? pc : pc - 1;
        XrRbcStatus status = xr_rbc_values_begin(module, stack,
            (uint32_t)stack->frames[stack->depth - 1].function, rbc_value_source(module, stack, first_pc));
        if (status != XR_RBC_OK) return status;
        activation = stack->values ? stack->values->top : NULL;
    }
    if (!activation || !activation->owned_code || activation->depth != stack->depth ||
        index >= activation->code->access_count) return XR_RBC_INVALID;
    const XrRbcValueAccess *access = &activation->code->accesses[index];
    /* This instruction has no Array region role and cannot enter another
     * activation. Preserve the same region exit as an ordinary access. */
    if (activation->active_array_region)
        rbc_value_array_region_forget(activation, activation->active_array_region, RBC_ARRAY_REGION_BYPASS);
    const XrtManagedHeader *keeper = NULL;
    if (literal_index != UINT32_MAX) {
        /* The code owner proved the preceding word creates this exact key.
         * Borrow it once. A cold creation fails at the literal, before any
         * Map lookup or result publication. */
        const XrRbcValueAccess *literal = &activation->code->accesses[literal_index];
        const XrLirValueHome *home = &activation->code->homes->values[literal->result];
        XrtManagedStatus status = rbc_value_read_literal_cache(activation, home, &keeper);
        if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, rbc_value_source(module, stack, pc - 1));
        if (!keeper) {
            uint32_t source = rbc_value_source(module, stack, pc - 1);
            status = rbc_value_literal_factory(module, activation, *literal, source, home);
            if (status == XRT_MANAGED_OK) status = rbc_value_read_literal_cache(activation, home, &keeper);
            if (status != XRT_MANAGED_OK || !keeper)
                xrt_native_managed_failure(status == XRT_MANAGED_OK ? XRT_MANAGED_ARGUMENT : status, source);
        }
    }
    bool missing = false;
    XrtManagedStatus status = rbc_map_read(module, stack, activation, access, (const XrtStringBacking *)keeper, &missing);
    if (status != XRT_MANAGED_OK) xrt_native_managed_failure(status, rbc_value_source(module, stack, pc));
    if (missing) xrt_typed_panic(431, rbc_value_source(module, stack, pc));
    return XR_RBC_OK;
}
