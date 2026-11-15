/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_array_execute.inc.c - Fixed Array owners and actual binding transactions
 */
static XrtManagedStatus rbc_value_binding_place(XrRbcStack *stack,RbcValueOrigin origin,
    XrtExternalOwner *external,XrtPlace *place) {
    if (!origin.activation) return XRT_MANAGED_ARGUMENT;
    const XrLirHomeBinding *binding=rbc_value_binding(origin.activation->code,origin.value);
    if (!binding) return XRT_MANAGED_ARGUMENT;
    if (binding->kind==XR_LIR_HOME_BINDING_LOCAL) {
        *place=(XrtPlace){.frame=origin.activation->frame,.slot=binding->first_owner,.nulls=XRT_PLACE_NONNULL};
        return XRT_MANAGED_OK;
    }
    if (binding->kind!=XR_LIR_HOME_BINDING_STATE_BORROW) return XRT_MANAGED_ARGUMENT;
    XrRbcRegister *reference=rbc_value_register(stack,origin.activation,binding->source);
    const XrtValueImage *image=origin.activation->image;
    if (!reference || !reference->state_ref || !image || !binding->type || binding->type>image->type_count ||
        reference->state_ref->kind!=image->types[binding->type-1].kind) return XRT_MANAGED_ARGUMENT;
    XrtManagedStatus status=xrt_module_external_owner(reference->state_ref->reference,0,external);
    if (status==XRT_MANAGED_OK) *place=(XrtPlace){.external=external,.nulls=XRT_PLACE_NONNULL};
    return status;
}
static XrtManagedStatus rbc_value_array_publish(RbcValueActivation *activation,uint32_t result) {
    XrtPlace destination,temporary;
    XrtManagedStatus status=rbc_value_place((RbcValueOrigin){activation,result},false,0,&destination);
    if (status==XRT_MANAGED_OK) status=rbc_value_place((RbcValueOrigin){activation,result},true,0,&temporary);
    return status==XRT_MANAGED_OK ? xrt_place_move_replace(destination,temporary,NULL) : status;
}
static XrtManagedStatus rbc_value_array_item(XrRbcStack *stack,RbcValueActivation *activation,
    uint32_t value,XrtBackingArrayKind kind,XrtArrayItem *item) {
    *item=(XrtArrayItem){.kind=kind};
    if (kind==XRT_BACKING_ARRAY_STRING) {
        const XrtStringBacking *string=NULL;
        XrtManagedStatus status=rbc_value_string_borrow(activation,value,&string);
        if (status==XRT_MANAGED_OK) item->value.string=(XrtStringBacking *)string;
        return status;
    }
    XrRbcRegister *scalar=rbc_value_register(stack,activation,value);
    if (!scalar) return XRT_MANAGED_ARGUMENT;
    if (kind==XRT_BACKING_ARRAY_I64) memcpy(&item->value.i64,&scalar->bits,8);
    else if (kind==XRT_BACKING_ARRAY_U8) item->value.u8=(uint8_t)scalar->bits;
    else item->value.u64=scalar->bits;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus rbc_value_array_new(XrRbcStack *stack,RbcValueActivation *activation,
    XrRbcValueAccess access,const XrtBackingTypes *types,XrtBackingArrayKind kind) {
    size_t stride=xrt_backing_array_stride(kind);
    if (access.count>activation->snapshot_capacity/stride) return XRT_MANAGED_ARGUMENT;
    for (uint32_t p=0;p<access.count;++p) {
        XrtArrayItem item;
        XrtManagedStatus status=rbc_value_array_item(stack,activation,activation->code->arguments[access.first+p],kind,&item);
        if (status!=XRT_MANAGED_OK) return status;
        const void *value=kind==XRT_BACKING_ARRAY_STRING ? (const void *)&item.value.string :
            kind==XRT_BACKING_ARRAY_U8 ? (const void *)&item.value.u8 :
            kind==XRT_BACKING_ARRAY_I64 ? (const void *)&item.value.i64 : (const void *)&item.value.u64;
        memcpy(activation->snapshots+(size_t)p*stride,value,stride);
    }
    size_t slot=rbc_value_home(activation->code,access.result)->temporary_first_owner;
    if (kind==XRT_BACKING_ARRAY_I64) return xrt_array_backing_i64_create(activation->frame,slot,types,
        (const int64_t *)activation->snapshots,access.count,access.count);
    if (kind==XRT_BACKING_ARRAY_U8) return xrt_array_backing_u8_create(activation->frame,slot,types,
        activation->snapshots,access.count,access.count);
    if (kind==XRT_BACKING_ARRAY_U64) return xrt_array_backing_u64_create(activation->frame,slot,types,
        (const uint64_t *)activation->snapshots,access.count,access.count);
    return xrt_array_backing_string_create(activation->frame,slot,types,
        (XrtStringBacking *const *)activation->snapshots,access.count,access.count);
}
static XrtManagedStatus rbc_value_binding_step(XrRbcStack *stack,RbcValueActivation *activation,XrRbcValueAccess access) {
    uint32_t binding=access.operation==XR_RBC_VALUE_BINDING_NEW ? access.result : access.source;
    RbcValueOrigin origin=rbc_value_origin(activation,binding);
    XrtExternalOwner external; XrtPlace place,other;
    XrtManagedStatus status=rbc_value_binding_place(stack,origin,&external,&place);
    if (status!=XRT_MANAGED_OK) return status;
    if (access.operation==XR_RBC_VALUE_BINDING_DROP) return xrt_managed_slot_drop(place.frame,place.slot);
    bool read=access.operation==XR_RBC_VALUE_BINDING_READ;
    uint32_t value=read ? access.result : access.operation==XR_RBC_VALUE_BINDING_NEW ? access.source : access.other;
    RbcValueOrigin from=read ? (RbcValueOrigin){activation,value} : rbc_value_origin(activation,value);
    status=rbc_value_place(from,false,0,&other);
    return status==XRT_MANAGED_OK ? xrt_place_copy_replace(read ? other : place,read ? place : other,NULL) : status;
}
static XrtManagedStatus rbc_value_array_step(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,uint32_t source) {
    if (access.operation!=XR_RBC_VALUE_ARRAY) return rbc_value_binding_step(stack,activation,access);
    const uint32_t *args=access.count ? activation->code->arguments+access.first : NULL;
    bool create=access.index==XR_LIR_ARRAY_OP_NEW || access.index==XR_LIR_ARRAY_OP_REPEAT;
    XrtValueKind value_kind=rbc_value_kind(module,activation->code,create ? access.result : args[0]);
    XrtBackingArrayKind kind=(XrtBackingArrayKind)(value_kind-XRT_VALUE_ARRAY_I64);
    if ((unsigned)kind>=XRT_BACKING_ARRAY_COUNT) return XRT_MANAGED_ARGUMENT;
    const XrtBackingTypes *types=NULL;
    if (create || access.index==XR_LIR_ARRAY_OP_SET || access.index==XR_LIR_ARRAY_OP_PUSH || access.index==XR_LIR_ARRAY_OP_DISPLAY) {
        XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source),&types);
        if (status!=XRT_MANAGED_OK) return status;
    }
    if (access.index==XR_LIR_ARRAY_OP_SET || access.index==XR_LIR_ARRAY_OP_PUSH) {
        XrtExternalOwner external; XrtPlace binding;
        XrtManagedStatus status=rbc_value_binding_place(stack,rbc_value_origin(activation,args[0]),&external,&binding);
        XrtArrayUpdate update={.operation=access.index==XR_LIR_ARRAY_OP_SET ? XRT_ARRAY_UPDATE_SET : XRT_ARRAY_UPDATE_PUSH};
        if (status==XRT_MANAGED_OK) status=rbc_value_array_item(stack,activation,args[access.count-1],kind,&update.item);
        if (access.index==XR_LIR_ARRAY_OP_SET) {
            XrRbcRegister *index=rbc_value_register(stack,activation,args[1]);
            if (!index) return XRT_MANAGED_ARGUMENT;
            memcpy(&update.index,&index->bits,8);
        }
        const XrLirHomeWorkspace *workspace=&activation->code->homes->workspaces[access.window];
        return status==XRT_MANAGED_OK ? xrt_array_update_place(binding,kind,activation->frame,workspace->first_owner,types,&update) : status;
    }
    const XrLirValueHome *result=rbc_value_home(activation->code,access.result);
    if (create) {
        XrtManagedStatus status;
        if (access.index==XR_LIR_ARRAY_OP_NEW) status=rbc_value_array_new(stack,activation,access,types,kind);
        else {
            XrRbcRegister *length=rbc_value_register(stack,activation,args[0]); XrtArrayItem item;
            if (!length) return XRT_MANAGED_ARGUMENT;
            int64_t count; memcpy(&count,&length->bits,8);
            status=rbc_value_array_item(stack,activation,args[1],kind,&item);
            if (status==XRT_MANAGED_OK) status=xrt_array_backing_repeat_create(activation->frame,result->temporary_first_owner,types,&item,count);
        }
        return status==XRT_MANAGED_OK ? rbc_value_array_publish(activation,access.result) : status;
    }
    XrtPlace place; const XrtManagedHeader *header=NULL;
    XrtManagedStatus status=rbc_value_place(rbc_value_origin(activation,args[0]),false,0,&place);
    if (status==XRT_MANAGED_OK) status=xrt_place_borrow(place,&header);
    if (status!=XRT_MANAGED_OK) return status;
    const XrtArrayBacking *array=(const XrtArrayBacking *)header;
    if (access.index==XR_LIR_ARRAY_OP_DISPLAY) {
        status=xrt_array_backing_display_create(activation->frame,result->temporary_first_owner,types,array,kind);
        return status==XRT_MANAGED_OK ? rbc_value_array_publish(activation,access.result) : status;
    }
    XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
    if (access.index==XR_LIR_ARRAY_OP_LEN) {
        if (!output) return XRT_MANAGED_ARGUMENT;
        if (array->length>INT64_MAX) return XRT_MANAGED_OVERFLOW;
        output->bits=(uint64_t)array->length; return XRT_MANAGED_OK;
    }
    XrRbcRegister *index=rbc_value_register(stack,activation,args[1]); XrtArrayItem item;
    if (!index) return XRT_MANAGED_ARGUMENT;
    int64_t at; memcpy(&at,&index->bits,8); status=xrt_array_backing_get(array,kind,at,&item);
    if (status!=XRT_MANAGED_OK) return status;
    if (kind==XRT_BACKING_ARRAY_STRING) {
        status=xrt_backing_slot_copy(activation->frame,result->temporary_first_owner,&item.value.string->managed);
        return status==XRT_MANAGED_OK ? rbc_value_array_publish(activation,access.result) : status;
    }
    if (!output) return XRT_MANAGED_ARGUMENT;
    output->bits=kind==XRT_BACKING_ARRAY_U8 ? item.value.u8 : kind==XRT_BACKING_ARRAY_U64 ? item.value.u64 : (uint64_t)item.value.i64;
    return XRT_MANAGED_OK;
}
