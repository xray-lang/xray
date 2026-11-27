/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_array_execute.inc.c - Fixed Array owners and actual binding transactions
 */
static inline bool rbc_value_binding_forward(RbcValueActivation *activation, XrRbcValueAccess access) {
    bool create = access.operation == XR_RBC_VALUE_BINDING_NEW;
    if ((!create && access.operation != XR_RBC_VALUE_BINDING_READ) ||
        !activation->owned_code || !access.index) return false;
    const XrLirValueHomes *homes = activation->code->homes;
    XR_DCHECK(homes && access.index <= homes->event_count, "owned binding events were admitted");
    /* Admission paired this immutable instruction with its release word.
     * Only the current Frame's eligibility to move can vary at execution. */
    if (homes->events[access.index - 1].forwarding_mode != XR_LIR_OWNERSHIP_TRANSFER_MOVE) return false;
    const XrLirHomeBinding *binding = &homes->bindings[create ? access.result : access.source];
    const XrLirValueHome *home = &homes->values[create ? access.source : access.result];
    return xrt_managed_frame_try_forward(activation->frame,activation->payload,&activation->frame_locations,
        (XrtManagedFrameForward){create ? home->first_owner : binding->first_owner,
            create ? home->offset : binding->offset,create ? binding->first_owner : home->first_owner,
            create ? binding->offset : home->offset});
}
static XrtManagedStatus rbc_value_binding_place(const XrRbcModule *module,XrRbcStack *stack,RbcValueOrigin origin,
    XrtExternalOwner *external,XrtPlace *place) {
    if (!origin.activation) return XRT_MANAGED_ARGUMENT;
    const XrLirHomeBinding *binding=rbc_value_binding(origin.activation->code,origin.value);
    if (!binding) return XRT_MANAGED_ARGUMENT;
    if (binding->kind==XR_LIR_HOME_BINDING_LOCAL) {
        *place=(XrtPlace){.frame=origin.activation->frame,.slot=binding->first_owner,.nulls=XRT_PLACE_NONNULL};
        return XRT_MANAGED_OK;
    }
    if (binding->kind==XR_LIR_HOME_BINDING_CELL_PAYLOAD_BORROW) {
        XrtManagedHeader *cell=NULL;
        XrtManagedStatus status=rbc_value_cell_borrow(module,stack,origin.activation,binding->source,&cell);
        *external=(XrtExternalOwner){0};
        if (status==XRT_MANAGED_OK) status=xrt_map_cell_external((XrtMapCell *)cell,external);
        if (status==XRT_MANAGED_OK) *place=(XrtPlace){.external=external,.nulls=XRT_PLACE_NONNULL};
        return status;
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
static XrtManagedStatus rbc_value_i64_ref_step(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,RbcValueOrigin origin,XrRbcValueAccess access,uint32_t source) {
    bool read=access.operation==XR_RBC_VALUE_BINDING_READ;
    if (!origin.activation || (!read && access.operation!=XR_RBC_VALUE_BINDING_WRITE)) return XRT_MANAGED_ARGUMENT;
    XrRbcRegister *reference=rbc_value_register(stack,origin.activation,origin.value);
    XrRbcRegister *value=rbc_value_register(stack,activation,read ? access.result : access.other);
    if (!reference || !value) return XRT_MANAGED_ARGUMENT;
    const XrRbcFunction *function=&module->functions[origin.activation->function];
    if (rbc_value_scalar(origin.activation->code,function,origin.value,XR_RBC_I64_BINDING)) {
        XrRbcRegister *cell=xr_rbc_cell(stack,module,reference->bits,XR_RBC_BITS);
        if (!cell) return XRT_MANAGED_ARGUMENT;
        if (read) value->bits=cell->bits;
        else cell->bits=value->bits;
    } else {
        if (!rbc_value_scalar(origin.activation->code,function,origin.value,XR_RBC_STATE_REF)) return XRT_MANAGED_ARGUMENT;
        const XrtModuleAccess *state=reference->state_ref;
        if (!state || state->kind!=XRT_VALUE_I64 || !state->is_mutable) return XRT_MANAGED_ARGUMENT;
        if ((*state->flags & state->required_flags)!=state->required_flags) xrt_module_uninitialized(source);
        if (read) memcpy(&value->bits,state->payload,sizeof(value->bits));
        else memcpy(state->payload,&value->bits,sizeof(value->bits));
    }
    return XRT_MANAGED_OK;
}
static XrtManagedStatus rbc_value_binding_step(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,uint32_t source) {
    uint32_t binding=access.operation==XR_RBC_VALUE_BINDING_NEW ? access.result : access.source;
    RbcValueOrigin origin=rbc_value_origin(activation,binding);
    if (rbc_value_i64_ref(module,activation->code,binding))
        return rbc_value_i64_ref_step(module,stack,activation,origin,access,source);
    XrtExternalOwner external; XrtPlace place,other;
    XrtManagedStatus status=rbc_value_binding_place(module,stack,origin,&external,&place);
    if (status!=XRT_MANAGED_OK) return status;
    if (access.operation==XR_RBC_VALUE_BINDING_DROP) return xrt_managed_slot_drop(place.frame,place.slot);
    bool read=access.operation==XR_RBC_VALUE_BINDING_READ;
    uint32_t value=read ? access.result : access.operation==XR_RBC_VALUE_BINDING_NEW ? access.source : access.other;
    XrtValueKind kind=rbc_value_kind(module,activation->code,binding);
    if (!read && (kind==XRT_VALUE_STRING || kind==XRT_VALUE_MAP_STRING_I64 || kind==XRT_VALUE_SET_I64) && access.window!=UINT32_MAX) {
        const XrLirHomeWorkspace *workspace=&activation->code->homes->workspaces[access.window];
        const XrtManagedHeader *borrowed=NULL;
        if (kind==XRT_VALUE_STRING) {
            const XrtStringBacking *string=NULL;
            status=rbc_value_string_borrow(activation,value,&string); borrowed=(const XrtManagedHeader *)string;
        } else if (kind==XRT_VALUE_SET_I64) {
            const XrtSetBacking *set=NULL;
            status=rbc_value_set_borrow(module,activation,value,&set); borrowed=(const XrtManagedHeader *)set;
        } else {
            const XrtMapBacking *map=NULL;
            status=rbc_value_map_borrow(module,activation,value,&map); borrowed=(const XrtManagedHeader *)map;
        }
        if (status==XRT_MANAGED_OK) status=xrt_backing_slot_copy(activation->frame,workspace->first_owner,borrowed);
        if (status!=XRT_MANAGED_OK) return status;
        XrtPlace stage={.frame=activation->frame,.slot=workspace->first_owner,.nulls=XRT_PLACE_NONNULL};
        status=xrt_place_move_replace(place,stage,NULL);
        XrtManagedStatus cleanup=xrt_managed_slot_drop(activation->frame,workspace->first_owner);
        return status==XRT_MANAGED_OK ? cleanup : status;
    }
    RbcValueOrigin from=read ? (RbcValueOrigin){activation,value} : rbc_value_origin(activation,value);
    status=rbc_value_place(from,false,0,&other);
    return status==XRT_MANAGED_OK ? xrt_place_copy_replace(read ? other : place,read ? place : other,NULL) : status;
}
static XrtManagedStatus rbc_value_string_binding_append(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,uint32_t source) {
    XrtExternalOwner external; XrtPlace place;
    XrtManagedStatus status=rbc_value_binding_place(module,stack,rbc_value_origin(activation,access.source),&external,&place);
    const XrtStringBacking *suffix=NULL;
    if (status==XRT_MANAGED_OK) status=rbc_value_string_borrow(activation,access.other,&suffix);
    if (status!=XRT_MANAGED_OK) return status;
    const XrLirHomeWorkspace *workspace=&activation->code->homes->workspaces[access.window];
    xrt_native_string_append_place(place,activation->frame,workspace->first_owner,suffix,source);
    return XRT_MANAGED_OK;
}
static bool rbc_value_array_local_update(XrRbcStack *stack,RbcValueActivation *activation,
    XrRbcValueAccess access) {
    if (!activation->owned_code || !activation->backings_ready || !access.other) return false;
    const uint32_t *args=activation->code->arguments+access.first;
    const XrLirHomeBinding *binding=rbc_value_binding(activation->code,args[0]);
    const XrLirHomeWorkspace *workspace=&activation->code->homes->workspaces[access.window];
    XrRbcRegister *item=rbc_value_register(stack,activation,args[access.count-1]);
    if (!item) return false;
    XrtArrayI64LocalUpdate update={
        .operation=access.index==XR_LIR_ARRAY_OP_SET ? XRT_ARRAY_UPDATE_SET : XRT_ARRAY_UPDATE_PUSH};
    memcpy(&update.value,&item->bits,sizeof(update.value));
    if (access.index==XR_LIR_ARRAY_OP_SET) {
        XrRbcRegister *index=rbc_value_register(stack,activation,args[1]);
        if (!index) return false;
        memcpy(&update.index,&index->bits,sizeof(update.index));
    }
    XrtArrayI64LocalView view={0};
    return xrt_array_i64_local_prepare(activation->frame,activation->payload,&activation->frame_locations,
        &activation->code->homes->frame,(XrtArrayI64LocalGeometry){binding->first_owner,binding->offset,
            workspace->first_owner,workspace->offset},&view) && xrt_array_i64_try_update_local(&view,update);
}
static XrtManagedStatus rbc_value_inline_class_push(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *activation, XrRbcValueAccess access, const uint32_t *args) {
    const XrLirHomeBinding *receiver = rbc_value_binding(activation->code, args[0]);
    const XrLirHomeWorkspace *workspace = &activation->code->homes->workspaces[access.window];
    XrtBoundInlineArray array = {0};
    XrtManagedStatus status = rbc_value_plan_set(module, stack);
    if (status == XRT_MANAGED_OK)
        status = stack->layout ? xrt_module_layout_inline_array(stack->layout, receiver->type, &array) :
            xrt_value_image_inline_array(stack->values->plans, receiver->type, &array);
    if (status != XRT_MANAGED_OK) return status;

    RbcValueOrigin origin = rbc_value_origin(activation, args[1]);
    const XrLirValueHome *home = origin.activation ? rbc_value_home(origin.activation->code, origin.value) : NULL;
    const XrtManagedHeader *item = NULL;
    status = rbc_value_borrow_header(origin, home, &item);
    if (status != XRT_MANAGED_OK) return status;

    XrtPlace binding = {.frame = activation->frame, .slot = receiver->first_owner, .nulls = XRT_PLACE_NONNULL};
    return xrt_array_backing_inline_class_append(binding, workspace->first_owner, array.ops,
        array.element, (XrtManagedHeader *)item);
}

static XrtManagedStatus rbc_value_array_inline(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,const uint32_t *args) {
    bool create=access.index==XR_LIR_ARRAY_OP_NEW;
    const XrLirValueHome *home=rbc_value_home(activation->code,create ? access.result : args[0]);
    uint32_t element=activation->image->types[home->type-1].element;
    const XrtManagedHeader *owner = NULL;
    if (!create) {
        RbcValueOrigin origin = rbc_value_origin(activation, args[0]);
        const XrLirValueHome *borrow = origin.activation ? rbc_value_home(origin.activation->code, origin.value) : NULL;
        XrtManagedStatus status = rbc_value_borrow_header(origin, borrow, &owner);
        if (status != XRT_MANAGED_OK) return status;
    }
    if (!create && access.index==XR_LIR_ARRAY_OP_LEN) {
        XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
        const XrtArrayBacking *array=(const XrtArrayBacking *)owner;
        if (!output) return XRT_MANAGED_ARGUMENT;
        if (array->length>INT64_MAX) return XRT_MANAGED_OVERFLOW;
        output->bits=array->length;
        return XRT_MANAGED_OK;
    }
    XrtBoundInlineArray binding={0};
    XrtManagedStatus status=rbc_value_plan_set(module,stack);
    if (status==XRT_MANAGED_OK)
        status=stack->layout ? xrt_module_layout_inline_array(stack->layout,home->type,&binding) :
            xrt_value_image_inline_array(stack->values->plans,home->type,&binding);
    if (status!=XRT_MANAGED_OK) return status;
    if (create) {
        size_t stride=activation->image->types[element-1].size;
        if (access.count>activation->snapshot_capacity/stride) return XRT_MANAGED_ARGUMENT;
        for (uint32_t p=0;p<access.count && status==XRT_MANAGED_OK;++p)
            status=rbc_value_inline_pack(module,stack,activation,args[p],element,activation->snapshots+(size_t)p*stride);
        if (status==XRT_MANAGED_OK)
            status=xrt_array_backing_inline_create(activation->frame,home->temporary_first_owner,binding.ops,
                activation->snapshots,access.count,access.count);
        return status==XRT_MANAGED_OK ? rbc_value_publish_leaf(activation,access.result) : status;
    }
    if (access.index!=XR_LIR_ARRAY_OP_GET) return XRT_MANAGED_ARGUMENT;
    XrRbcRegister *index=rbc_value_register(stack,activation,args[1]);
    if (!index) return XRT_MANAGED_ARGUMENT;
    int64_t at; memcpy(&at,&index->bits,sizeof(at));
    const void *row=NULL;
    status=xrt_array_backing_inline_at((const XrtArrayBacking *)owner,binding.element,at,&row);
    return status==XRT_MANAGED_OK ? rbc_value_inline_extract(module,stack,activation,access.result,element,row) : status;
}
static XrtManagedStatus rbc_value_array_step(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,uint32_t source) {
    if (access.operation!=XR_RBC_VALUE_ARRAY) return rbc_value_binding_step(module,stack,activation,access,source);
    const uint32_t *args=access.count ? activation->code->arguments+access.first : NULL;
    bool create=access.index==XR_LIR_ARRAY_OP_NEW || access.index==XR_LIR_ARRAY_OP_REPEAT ||
        access.index==XR_LIR_ARRAY_OP_COPY_STRING_BYTES || access.index==XR_LIR_ARRAY_OP_WITH_CAPACITY;
    XrtValueKind value_kind=rbc_value_kind(module,activation->code,create ? access.result : args[0]);
    if (value_kind == XRT_VALUE_ARRAY_INLINE) {
        if (access.index == XR_LIR_ARRAY_OP_PUSH)
            return rbc_value_inline_class_push(module, stack, activation, access, args);
        return rbc_value_array_inline(module, stack, activation, access, args);
    }
    XrtBackingArrayKind kind=(XrtBackingArrayKind)(value_kind-XRT_VALUE_ARRAY_I64);
    if ((unsigned)kind>=XRT_BACKING_ARRAY_COUNT) return XRT_MANAGED_ARGUMENT;
    bool random_bytes=access.index==XR_LIR_ARRAY_OP_RANDOM_BYTES;
    bool update_binding=xr_lir_array_binding_update((XrLirArrayOperation)access.index);
    if (!random_bytes && access.other && rbc_value_array_local_update(stack,activation,access)) return XRT_MANAGED_OK;
    const XrtBackingTypes *types=NULL;
    if (create || update_binding || access.index==XR_LIR_ARRAY_OP_DISPLAY) {
        XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source),&types);
        if (status!=XRT_MANAGED_OK) return status;
        activation->backings_ready=true;
    }
    if (update_binding) {
        XrtExternalOwner external; XrtPlace binding;
        XrtManagedStatus status=rbc_value_binding_place(module,stack,rbc_value_origin(activation,args[0]),&external,&binding);
        XrtArrayUpdate update={.operation=random_bytes ? XRT_ARRAY_UPDATE_RANDOM_BYTES :
            access.index==XR_LIR_ARRAY_OP_SET ? XRT_ARRAY_UPDATE_SET : XRT_ARRAY_UPDATE_PUSH,
            .item={.kind=kind}};
        if (status==XRT_MANAGED_OK && !random_bytes)
            status=rbc_value_array_item(stack,activation,args[access.count-1],kind,&update.item);
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
        else if (access.index==XR_LIR_ARRAY_OP_COPY_STRING_BYTES) {
            const XrtStringBacking *string = NULL;
            status = rbc_value_string_borrow(activation,args[0],&string);
            if (status==XRT_MANAGED_OK)
                status=xrt_string_backing_copy_bytes_create(activation->frame,result->temporary_first_owner,types,string);
        } else if (access.index==XR_LIR_ARRAY_OP_WITH_CAPACITY) {
            XrRbcRegister *capacity=rbc_value_register(stack,activation,args[0]);
            if (!capacity) return XRT_MANAGED_ARGUMENT;
            int64_t rows; memcpy(&rows,&capacity->bits,sizeof(rows));
            xrt_native_array_with_capacity(activation->frame,result->temporary_first_owner,kind,rows,source);
            activation->backings_ready=true;
            return rbc_value_publish_leaf(activation,access.result);
        } else {
            XrRbcRegister *length=rbc_value_register(stack,activation,args[0]); XrtArrayItem item;
            if (!length) return XRT_MANAGED_ARGUMENT;
            int64_t count; memcpy(&count,&length->bits,8);
            status=rbc_value_array_item(stack,activation,args[1],kind,&item);
            if (status==XRT_MANAGED_OK) status=xrt_array_backing_repeat_create(activation->frame,result->temporary_first_owner,types,&item,count);
        }
        return status==XRT_MANAGED_OK ? rbc_value_publish_leaf(activation,access.result) : status;
    }
    XrtPlace place; const XrtManagedHeader *header=NULL;
    XrtManagedStatus status=rbc_value_place(rbc_value_origin(activation,args[0]),false,0,&place);
    if (status==XRT_MANAGED_OK) status=xrt_place_borrow(place,&header);
    if (status!=XRT_MANAGED_OK) return status;
    const XrtArrayBacking *array=(const XrtArrayBacking *)header;
    if (access.index==XR_LIR_ARRAY_OP_DISPLAY) {
        status=xrt_array_backing_display_create(activation->frame,result->temporary_first_owner,types,array,kind);
        return status==XRT_MANAGED_OK ? rbc_value_publish_leaf(activation,access.result) : status;
    }
    XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
    if (access.index==XR_LIR_ARRAY_OP_LEN) {
        if (!output) return XRT_MANAGED_ARGUMENT;
        if (array->length>INT64_MAX) return XRT_MANAGED_OVERFLOW;
        output->bits=(uint64_t)array->length; return XRT_MANAGED_OK;
    }
    if (access.index==XR_LIR_ARRAY_OP_CAPACITY) {
        if (!output) return XRT_MANAGED_ARGUMENT;
        output->bits=(uint64_t)xrt_native_array_capacity(header,kind,source);
        return XRT_MANAGED_OK;
    }
    XrRbcRegister *index=rbc_value_register(stack,activation,args[1]); XrtArrayItem item;
    if (!index) return XRT_MANAGED_ARGUMENT;
    int64_t at; memcpy(&at,&index->bits,sizeof(at));
    if (access.index==XR_LIR_ARRAY_OP_RESERVE) {
        xrt_native_array_reserve(activation->frame,result->temporary_first_owner,header,kind,at,source);
        activation->backings_ready=true;
        return rbc_value_publish_leaf(activation,access.result);
    }
    status=xrt_array_backing_get(array,kind,at,&item);
    if (status!=XRT_MANAGED_OK) return status;
    if (kind==XRT_BACKING_ARRAY_STRING) {
        status=xrt_backing_slot_copy(activation->frame,result->temporary_first_owner,&item.value.string->managed);
        return status==XRT_MANAGED_OK ? rbc_value_publish_leaf(activation,access.result) : status;
    }
    if (!output) return XRT_MANAGED_ARGUMENT;
    output->bits=kind==XRT_BACKING_ARRAY_U8 ? item.value.u8 : kind==XRT_BACKING_ARRAY_U64 ? item.value.u64 : (uint64_t)item.value.i64;
    return XRT_MANAGED_OK;
}
