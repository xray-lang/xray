/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_inline_execute.inc.c - Natural values over common stable owner leaves
 */
static XrtManagedStatus rbc_value_inline_pack(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,uint32_t value,uint32_t type,uint8_t *bytes) {
    const XrtValueImageType *row=&activation->image->types[type-1];
    const XrLirValueHome *home=rbc_value_home(activation->code,value);
    if (home && home->type) {
        RbcValuePayload input;
        XrtManagedStatus status=rbc_value_payload(module,stack,rbc_value_origin(activation,value),&input);
        if (status!=XRT_MANAGED_OK) return status;
        if (input.size!=row->size) return XRT_MANAGED_ARGUMENT;
        if (input.lanes) xr_rbc_aggregate_pack(input.packed,input.address,bytes);
        else if (input.size) memcpy(bytes,input.address,input.size);
        return XRT_MANAGED_OK;
    }
    XrRbcRegister *input=rbc_value_register(stack,activation,value);
    return input && rbc_state_scalar_store(row->kind,input->bits,bytes) ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
}
static XrtManagedStatus rbc_value_inline_extract(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,uint32_t value,uint32_t type,const uint8_t *bytes) {
    const XrLirValueHome *home=rbc_value_home(activation->code,value);
    if (home && home->type) return rbc_value_inline_replace(module,stack,(RbcValueOrigin){activation,value},bytes);
    XrRbcRegister *output=rbc_value_register(stack,activation,value);
    return output && rbc_state_scalar_load(activation->image->types[type-1].kind,bytes,&output->bits) ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
}
static XrtManagedStatus rbc_class_string_replace(const XrRbcModule *module, XrRbcStack *stack,
    RbcValueActivation *activation, uint32_t value, const XrtValueImageType *type,
    const XrtClassFieldAccess *field) {
    if (type->kind==XRT_VALUE_STRING) {
        const XrtStringBacking *incoming=NULL;
        XrtManagedStatus status=rbc_value_string_borrow(activation,value,&incoming);
        return status==XRT_MANAGED_OK ?
            xrt_class_string_field_replace(field,(const XrtManagedHeader *)incoming,1,false) : status;
    }
    RbcValuePayload input;
    XrtManagedStatus status=rbc_value_payload(module,stack,rbc_value_origin(activation,value),&input);
    if (status!=XRT_MANAGED_OK) return status;
    XrtHostNullableOwner incoming;
    if (input.size!=sizeof(incoming) || input.lanes || !input.address) return XRT_MANAGED_ARGUMENT;
    memcpy(&incoming,input.address,sizeof(incoming));
    return xrt_class_string_field_replace(field,incoming.owner,incoming.present,true);
}
static XrtManagedStatus rbc_value_inline_step(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,uint32_t source) {
    bool class_new=access.operation==XR_RBC_VALUE_CLASS_NEW, class_field=access.operation==XR_RBC_VALUE_CLASS_FIELD;
    bool class_push=access.operation==XR_RBC_VALUE_CLASS_ARRAY_PUSH, class_write=access.operation==XR_RBC_VALUE_CLASS_ARRAY_WRITE;
    bool scalar_write=access.operation==XR_RBC_VALUE_CLASS_SCALAR_WRITE;
    bool string_write=access.operation==XR_RBC_VALUE_CLASS_STRING_WRITE;
    bool create=class_new || access.operation==XR_RBC_VALUE_RECORD_NEW ||
        access.operation==XR_RBC_VALUE_SUM_NEW ||
        access.operation==XR_RBC_VALUE_NULLABLE_NONE || access.operation==XR_RBC_VALUE_NULLABLE_SOME;
    const XrLirValueHome *home=rbc_value_home(activation->code,create ? access.result : access.source);
    const XrtValueImageType *type=&activation->image->types[home->type-1];
    if (access.operation>=XR_RBC_VALUE_SUM_NEW && access.operation<=XR_RBC_VALUE_SUM_GET) {
        if (type->kind!=XRT_VALUE_SUM || !activation->image->variants ||
            type->first_variant>activation->image->variant_count ||
            type->variant_count>activation->image->variant_count-type->first_variant)
            return XRT_MANAGED_ARGUMENT;
        if (access.operation==XR_RBC_VALUE_SUM_TAG) {
            RbcValuePayload input;
            XrtManagedStatus status=rbc_value_payload(module,stack,
                rbc_value_origin(activation,access.source),&input);
            if (status!=XRT_MANAGED_OK) return status;
            uint32_t tag=0;
            if (type->sum_tag_size) {
                if (type->sum_tag>type->size || type->sum_tag_size>type->size-type->sum_tag)
                    return XRT_MANAGED_ARGUMENT;
                memcpy(&tag,input.address+type->sum_tag,type->sum_tag_size);
            }
            if (tag>=type->variant_count) return XRT_MANAGED_ARGUMENT;
            XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
            if (!output) return XRT_MANAGED_ARGUMENT;
            output->bits=tag;
            return XRT_MANAGED_OK;
        }
        if (access.operation==XR_RBC_VALUE_SUM_GET) {
            RbcValuePayload input;
            XrtManagedStatus status=rbc_value_payload(module,stack,
                rbc_value_origin(activation,access.source),&input);
            if (status!=XRT_MANAGED_OK) return status;
            if (access.index>=type->field_count ||
                type->first_field>activation->image->slot_count ||
                type->field_count>activation->image->slot_count-type->first_field)
                return XRT_MANAGED_ARGUMENT;
            XrtValueImageSlot slot=activation->image->slots[type->first_field+access.index];
            if (slot.type>activation->image->type_count) return XRT_MANAGED_ARGUMENT;
            size_t field_size=activation->image->types[slot.type-1].size;
            if (slot.offset>type->size || field_size>type->size-slot.offset)
                return XRT_MANAGED_ARGUMENT;
            return rbc_value_inline_extract(module,stack,activation,access.result,slot.type,
                input.address+slot.offset);
        }
        if (access.index>=type->variant_count) return XRT_MANAGED_ARGUMENT;
        XrtValueImageVariant variant=activation->image->variants[type->first_variant+access.index];
        if (variant.field_count!=access.count || type->size>activation->snapshot_capacity)
            return XRT_MANAGED_ARGUMENT;
        uint8_t *snapshot=activation->snapshots;
        memset(snapshot,0,type->size);
        if (type->sum_tag_size) {
            uint32_t tag=access.index;
            if (type->sum_tag>type->size || type->sum_tag_size>type->size-type->sum_tag)
                return XRT_MANAGED_ARGUMENT;
            memcpy(snapshot+type->sum_tag,&tag,type->sum_tag_size);
        }
        for (uint32_t p=0;p<access.count;++p) {
            uint32_t at=type->first_field+variant.first_field+p;
            if (at>=activation->image->slot_count) return XRT_MANAGED_ARGUMENT;
            XrtValueImageSlot slot=activation->image->slots[at];
            if (slot.offset>type->size || type->size-slot.offset<activation->image->types[slot.type-1].size)
                return XRT_MANAGED_ARGUMENT;
            XrtManagedStatus status=rbc_value_inline_pack(module,stack,activation,
                activation->code->arguments[access.first+p],slot.type,snapshot+slot.offset);
            if (status!=XRT_MANAGED_OK) return status;
        }
        return rbc_value_inline_replace(module,stack,
            (RbcValueOrigin){activation,access.result},snapshot);
    }
    XrtBoundClass binding={0};
    if (class_new || class_field || class_push || class_write || scalar_write || string_write) {
        XrtManagedStatus status=rbc_value_plan_set(module,stack);
        if (status==XRT_MANAGED_OK) status=stack->layout ? xrt_module_layout_class(stack->layout,home->type,&binding) :
            xrt_value_image_class(stack->values->plans,home->type,&binding);
        if (status!=XRT_MANAGED_OK) return status;
        type=&activation->image->types[type->element-1];
        if (!class_new) {
            const XrtManagedHeader *instance=NULL;
            status=rbc_value_borrow_header(rbc_value_origin(activation,access.source),home,&instance);
            if (status!=XRT_MANAGED_OK) return status;
            const XrtClassFieldAccess field_access={(XrtManagedHeader *)instance,binding.plan,access.index};
            if (class_push) {
                const XrtBackingTypes *types=NULL;
                status=xrt_native_values_backings(xrt_value_root_current(source),&types);
                if (status!=XRT_MANAGED_OK) return status;
                const XrLirHomeWorkspace *workspace=&activation->code->homes->workspaces[access.window-1];
                XrtValueImageSlot field=activation->image->slots[type->first_field+access.index];
                if (activation->image->types[field.type-1].kind==XRT_VALUE_ARRAY_STRING) {
                    const XrtStringBacking *item=NULL;
                    status=rbc_value_string_borrow(activation,access.other,&item);
                    return status==XRT_MANAGED_OK ? xrt_class_array_field_push_string(&field_access,
                        activation->frame,workspace->first_owner,types,item) : status;
                }
                XrRbcRegister *item=rbc_value_register(stack,activation,access.other);
                return item ? xrt_class_array_field_push(&field_access,
                    activation->frame,workspace->first_owner,types,(int64_t)item->bits) : XRT_MANAGED_ARGUMENT;
            }
            if (class_write) {
                const XrLirValueHome *incoming_home=rbc_value_home(activation->code,access.other);
                const XrtManagedHeader *incoming=NULL;
                status=rbc_value_borrow_header(rbc_value_origin(activation,access.other),incoming_home,&incoming);
                return status==XRT_MANAGED_OK ? xrt_class_array_field_replace(&field_access,incoming) : status;
            }
            XrtValueImageSlot slot=activation->image->slots[type->first_field+access.index];
            const XrtValueImageType *field_type=&activation->image->types[slot.type-1];
            if (string_write)
                return rbc_class_string_replace(module,stack,activation,access.other,field_type,&field_access);
            if (scalar_write) {
                uint8_t bytes[2 * sizeof(uint64_t)];
                if (field_type->size>sizeof(bytes)) return XRT_MANAGED_ARGUMENT;
                status=rbc_value_inline_pack(module,stack,activation,access.other,slot.type,bytes);
                return status==XRT_MANAGED_OK ? xrt_class_scalar_field_write(field_access.instance,
                    binding.plan,access.index,bytes,field_type->size) : status;
            }
            if ((slot.flags & XRT_VALUE_FIELD_MUTABLE) && !rbc_writable_class_scalar(activation->image,field_type)) {
                const XrLirValueHome *destination=rbc_value_home(activation->code,access.result);
                if (rbc_writable_class_string(activation->image,field_type)) {
                    size_t nullable_offset=field_type->kind==XRT_VALUE_NULLABLE ? destination->offset : SIZE_MAX;
                    return xrt_class_string_field_copy(&field_access,activation->frame,
                        destination->first_owner,nullable_offset);
                }
                if (!rbc_writable_class_array(field_type->kind)) return XRT_MANAGED_ARGUMENT;
                status=xrt_class_array_field_copy(&field_access,activation->frame,destination->temporary_first_owner);
                return status==XRT_MANAGED_OK ? rbc_value_publish_leaf(activation,access.result) : status;
            }
            const XrtValuePlan *field_plan=NULL; const void *field=NULL;
            status=xrt_class_field_borrow(instance,binding.plan,access.index,&field_plan,&field);
            if (status!=XRT_MANAGED_OK) return status;
            return rbc_value_inline_extract(module,stack,activation,access.result,slot.type,field);
        }
    }
    if (access.operation==XR_RBC_VALUE_NULLABLE_NONE)
        return rbc_value_inline_replace(module,stack,(RbcValueOrigin){activation,access.result},NULL);
    if (create) {
        if (type->size>activation->snapshot_capacity) return XRT_MANAGED_ARGUMENT;
        uint8_t *snapshot=activation->snapshots; memset(snapshot,0,type->size);
        if (class_new || access.operation==XR_RBC_VALUE_RECORD_NEW) {
            for (uint32_t p=0;p<access.count;++p) {
                XrtValueImageSlot field=activation->image->slots[type->first_field+p];
                XrtManagedStatus status=rbc_value_inline_pack(module,stack,activation,
                    activation->code->arguments[access.first+p],field.type,snapshot+field.offset);
                if (status!=XRT_MANAGED_OK) return status;
            }
        } else {
            XrtManagedStatus status=rbc_value_inline_pack(module,stack,activation,access.source,
                type->nullable_element,snapshot+type->nullable_payload);
            if (status!=XRT_MANAGED_OK) return status;
            snapshot[type->nullable_tag]=1;
        }
        if (class_new) {
            XrtManagedStatus status=xrt_class_create(activation->frame,home->temporary_first_owner,binding.ops,snapshot);
            return status==XRT_MANAGED_OK ? rbc_value_publish_leaf(activation,access.result) : status;
        }
        /* The snapshot only borrows bytes. The shared transaction acquires its
         * owners in the actual Root staging slots before publishing anything. */
        return rbc_value_inline_replace(module,stack,(RbcValueOrigin){activation,access.result},snapshot);
    }
    RbcValuePayload input;
    XrtManagedStatus status=rbc_value_payload(module,stack,rbc_value_origin(activation,access.source),&input);
    if (status!=XRT_MANAGED_OK) return status;
    if (access.operation==XR_RBC_VALUE_RECORD_FIELD) {
        XrtValueImageSlot field=activation->image->slots[type->first_field+access.index];
        return rbc_value_inline_extract(module,stack,activation,access.result,field.type,input.address+field.offset);
    }
    uint8_t tag=input.address[type->nullable_tag];
    if (tag>1) return XRT_MANAGED_ARGUMENT;
    if (access.operation==XR_RBC_VALUE_NULLABLE_IS_SOME) {
        XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
        if (!output) return XRT_MANAGED_ARGUMENT;
        output->bits=tag; return XRT_MANAGED_OK;
    }
    if (!tag) xrt_null_unwrap(source);
    return rbc_value_inline_extract(module,stack,activation,access.result,type->nullable_element,input.address+type->nullable_payload);
}
