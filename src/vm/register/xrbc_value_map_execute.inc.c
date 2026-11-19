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
static bool rbc_value_map_overwrite(XrRbcStack *stack, RbcValueActivation *activation,
    const uint32_t *args, uint32_t workspace) {
    if (!activation->owned_code || !activation->backings_ready) return false;
    const XrLirHomeBinding *binding = rbc_value_binding(activation->code, args[0]);
    if (!binding || binding->kind != XR_LIR_HOME_BINDING_LOCAL) return false;
    const XrtStringBacking *key = NULL;
    XrRbcRegister *item = rbc_value_register(stack, activation, args[2]);
    if (!item || rbc_value_string_borrow(activation, args[1], &key) != XRT_MANAGED_OK) return false;
    int64_t value;
    memcpy(&value, &item->bits, sizeof(value));
    const XrLirHomeWorkspace *stage = &activation->code->homes->workspaces[workspace];
    return xrt_map_try_overwrite_local(&activation->frame_locations, binding->first_owner,
        stage->first_owner, key, value);
}
static XrtManagedStatus rbc_value_map_step(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,uint32_t source) {
    const uint32_t *args=access.count ? activation->code->arguments+access.first : NULL;
    if (access.index==XR_LIR_MAP_OP_SET && rbc_value_map_overwrite(stack, activation, args, access.window))
        return XRT_MANAGED_OK;
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
    const XrtMapBacking *map=NULL;
    XrtManagedStatus status=rbc_value_map_borrow(module,activation,args[0],&map);
    if (status!=XRT_MANAGED_OK) return status;
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
    const XrtStringBacking *key=NULL;
    status=rbc_value_string_borrow(activation,args[1],&key);
    int64_t value=0; bool found=false;
    if (status==XRT_MANAGED_OK) status=xrt_map_backing_get(map,key,&value,&found);
    if (status!=XRT_MANAGED_OK) return status;
    if (access.index==XR_LIR_MAP_OP_GET) {
        RbcValuePayload payload;
        status=rbc_value_payload(module,stack,(RbcValueOrigin){activation,access.result},&payload);
        if (status!=XRT_MANAGED_OK) return status;
        const XrLirValueHome *home=rbc_value_home(activation->code,access.result);
        const XrtValueImageType *type=&activation->image->types[home->type-1];
        memset(payload.address,0,payload.size);
        payload.address[type->nullable_tag]=(uint8_t)found;
        memcpy(payload.address+type->nullable_payload,&value,8);
        return XRT_MANAGED_OK;
    }
    if (access.index==XR_LIR_MAP_OP_INDEX && !found) xrt_typed_panic(431,source);
    XrRbcRegister *result=rbc_value_register(stack,activation,access.result);
    if (!result) return XRT_MANAGED_ARGUMENT;
    if (access.index==XR_LIR_MAP_OP_CONTAINS) result->bits=found;
    else memcpy(&result->bits,&value,8);
    return XRT_MANAGED_OK;
}
