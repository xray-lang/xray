/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_slice_execute.inc.c - Borrowed windows over common Frame keepers
 */
static XrtManagedStatus rbc_value_slice_step(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,uint32_t source) {
    if (access.index==XR_LIR_SLICE_OP_CLEANUP) {
        if (!activation->frame) return XRT_MANAGED_OK;
        const XrLirHomeEvent *event=&activation->code->homes->events[access.window-1];
        XrtInlineValueHome destination={0};
        XrtManagedStatus status=rbc_value_slice_storage(activation,event->view_workspace,&destination);
        return status==XRT_MANAGED_OK ? xrt_array_slice_clear(destination.frame,destination.value,
            destination.temporary,destination.first_owner,destination.temporary_first_owner) : status;
    }
    const uint32_t *args=activation->code->arguments+access.first;
    bool construct=access.index==XR_LIR_SLICE_OP_NEW || access.index==XR_LIR_SLICE_OP_REBORROW || access.index==XR_LIR_SLICE_OP_BIND;
    XrtValueKind value_kind=rbc_value_kind(module,activation->code,construct ? access.result : args[0]);
    XrtBackingArrayKind kind=value_kind==XRT_VALUE_SLICE_I64 ? XRT_BACKING_ARRAY_I64 : XRT_BACKING_ARRAY_U8;
    XrtInlineValueHome destination={0};
    XrtManagedStatus status=XRT_MANAGED_OK;
    if (construct || access.index==XR_LIR_SLICE_OP_END) {
        const XrLirHomeEvent *event=&activation->code->homes->events[access.window-1];
        if (event->view_storage==XR_LIR_SLICE_STORAGE_BORROWED_FORMAL)
            return access.index==XR_LIR_SLICE_OP_END ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
        status=rbc_value_slice_storage(activation,event->view_workspace,&destination);
        if (status!=XRT_MANAGED_OK) return status;
    }
    if (access.index==XR_LIR_SLICE_OP_END) return xrt_array_slice_clear(destination.frame,destination.value,
        destination.temporary,destination.first_owner,destination.temporary_first_owner);
    int64_t start=0,limit=0;
    bool string=access.source==XR_LIR_STRING_CELL;
    if ((access.index==XR_LIR_SLICE_OP_NEW && !string) || access.index==XR_LIR_SLICE_OP_REBORROW) {
        XrRbcRegister *first=rbc_value_register(stack,activation,args[1]),*last=rbc_value_register(stack,activation,args[2]);
        if (!first || !last) return XRT_MANAGED_ARGUMENT;
        memcpy(&start,&first->bits,8); memcpy(&limit,&last->bits,8);
    }
    if (access.index==XR_LIR_SLICE_OP_NEW) {
        /* Entry has already allocated every Frame and setup. The checked source
         * borrow now reaches allocation-free temporary retention without a gap. */
        const XrtManagedHeader *backing=NULL;
        RbcValueOrigin origin=rbc_value_origin(activation,args[0]);
        if (access.source==XR_LIR_STATE_REF) {
            XrRbcRegister *reference=rbc_value_register(stack,origin.activation,origin.value);
            XrtValueKind expected=kind==XRT_BACKING_ARRAY_I64 ? XRT_VALUE_ARRAY_I64 : XRT_VALUE_ARRAY_U8;
            if (!reference || !reference->state_ref || reference->state_ref->kind!=expected ||
                reference->state_ref->size!=sizeof(backing)) return XRT_MANAGED_ARGUMENT;
            const void *borrowed=NULL;
            status=xrt_module_borrow(reference->state_ref->reference,&borrowed);
            if (status==XRT_MANAGED_OK) memcpy(&backing,borrowed,sizeof(backing));
        } else {
            XrtExternalOwner external; XrtPlace binding;
            status=rbc_value_binding_place(module,stack,origin,&external,&binding);
            if (status==XRT_MANAGED_OK) status=xrt_place_borrow(binding,&backing);
        }
        if (status!=XRT_MANAGED_OK) return status;
        if (string) return xrt_string_bytes_bind(&destination,backing);
        return xrt_window_range_bind(&destination,kind,backing,start,limit);
    }
    RbcValuePayload payload;
    status=rbc_value_payload(module,stack,rbc_value_origin(activation,args[0]),&payload);
    if (status!=XRT_MANAGED_OK || payload.size!=sizeof(XrtReadonlyWindow) || payload.lanes)
        return status!=XRT_MANAGED_OK ? status : XRT_MANAGED_ARGUMENT;
    XrtReadonlyWindow slice; memcpy(&slice,payload.address,sizeof(slice));
    if (access.index==XR_LIR_SLICE_OP_REBORROW) {
        XrRbcRegister *open=rbc_value_register(stack,activation,args[3]);
        if (!open || open->bits>1) return XRT_MANAGED_ARGUMENT;
        return xrt_window_range_reborrow(&destination,kind,&slice,start,limit,open->bits!=0);
    }
    if (access.index==XR_LIR_SLICE_OP_BIND)
        return xrt_window_rebind(&destination,kind,&slice);
    if (access.index==XR_LIR_SLICE_OP_STRICT_UTF8) {
        const XrLirValueHome *home=rbc_value_home(activation->code,access.result);
        return xrt_window_strict_utf8(activation->frame,home->first_owner,home->temporary_first_owner,
            home->offset,home->temporary_offset + offsetof(XrtHostNullableOwner, owner),&slice,source);
    }
    if (access.index==XR_LIR_SLICE_OP_COPY) {
        const XrtBackingTypes *types=NULL;
        status=xrt_native_values_backings(xrt_value_root_current(source),&types);
        const XrLirValueHome *home=rbc_value_home(activation->code,access.result);
        if (status==XRT_MANAGED_OK) status=xrt_window_copy_create(activation->frame,home->temporary_first_owner,types,kind,&slice);
        return status==XRT_MANAGED_OK ? rbc_value_publish_leaf(activation,access.result) : status;
    }
    XrRbcRegister *output=rbc_value_register(stack,activation,access.result);
    if (!output) return XRT_MANAGED_ARGUMENT;
    if (access.index==XR_LIR_SLICE_OP_LEN) {
        int64_t length=0; status=xrt_window_length(&slice,&length);
        if (status==XRT_MANAGED_OK) memcpy(&output->bits,&length,8);
        return status;
    }
    XrRbcRegister *index=rbc_value_register(stack,activation,args[1]);
    if (access.index!=XR_LIR_SLICE_OP_GET || !index) return XRT_MANAGED_ARGUMENT;
    int64_t at; memcpy(&at,&index->bits,8);
    return xrt_window_read_bits(&slice,kind,at,&output->bits);
}
