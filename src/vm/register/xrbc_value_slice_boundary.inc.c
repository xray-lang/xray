/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_slice_boundary.inc.c - Borrowed calls and caller-owned loan landings
 */
static XrtManagedStatus rbc_value_slice_storage(RbcValueActivation *activation,
    uint32_t ordinal,XrtInlineValueHome *output) {
    const XrLirHomeWorkspace *workspace=rbc_value_slice_workspace(activation->code,ordinal);
    if (!workspace || !activation->frame || !activation->payload) return XRT_MANAGED_ARGUMENT;
    *output=(XrtInlineValueHome){.frame=activation->frame,.value=activation->payload+workspace->offset,
        .temporary=activation->payload+workspace->temporary_offset,.first_owner=workspace->first_owner,
        .temporary_first_owner=workspace->temporary_first_owner};
    return XRT_MANAGED_OK;
}

static const XrLirHomeWorkspace *rbc_value_borrow_stage(const XrRbcValueCode *code,XrLirHomeTransfer transfer) {
    const XrLirValueHomes *homes=code ? code->homes : NULL;
    if (homes && homes->workspaces && transfer.borrowed_owner_workspace<homes->workspace_count &&
        homes->workspaces[transfer.borrowed_owner_workspace].kind==XR_LIR_HOME_WORKSPACE_HEADER_BORROW_STAGE)
        return rbc_value_header_stage(code,transfer);
    return transfer.source_storage==XR_LIR_SLICE_STORAGE_BORROWED_FORMAL ?
        rbc_value_slice_stage(code,transfer) : rbc_value_cell_stage(code,transfer);
}
static XrtManagedStatus rbc_value_borrow_stage_owner(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrLirHomeTransfer transfer,XrtManagedHeader **output) {
    if (rbc_value_header_stage(activation->code,transfer)) {
        const XrtSetBacking *set=NULL;
        XrtManagedStatus status=rbc_value_set_borrow(module,activation,transfer.source,&set);
        if (status==XRT_MANAGED_OK) *output=(XrtManagedHeader *)set;
        return status;
    }
    if (transfer.source_storage!=XR_LIR_SLICE_STORAGE_BORROWED_FORMAL)
        return rbc_value_cell_borrow(module,stack,activation,transfer.source,output);
    RbcValuePayload payload;
    XrtManagedStatus status=rbc_value_payload(module,stack,rbc_value_origin(activation,transfer.source),&payload);
    if (status!=XRT_MANAGED_OK || payload.size!=sizeof(XrtReadonlyWindow) || payload.lanes)
        return status!=XRT_MANAGED_OK ? status : XRT_MANAGED_ARGUMENT;
    XrtReadonlyWindow slice; memcpy(&slice,payload.address,sizeof(slice));
    if (!slice.owner) return XRT_MANAGED_ARGUMENT;
    *output=(XrtManagedHeader *)slice.owner;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus rbc_value_slice_return(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access) {
    RbcValueActivation *caller=activation->previous;
    const XrLirHomeCall *call=activation->receiving_slice_call;
    if (!activation->receiving_call || !caller || caller->depth+1!=activation->depth ||
        !call || call->view_result_parameter!=activation->code->homes->view_result_parameter ||
        !rbc_value_slice_return_valid(module,activation->code,access)) return XRT_MANAGED_ARGUMENT;
    uint32_t result=stack->frames[activation->depth-1].destination;
    const XrLirValueHome *home=rbc_value_home(caller->code,result);
    if (!home || call->result!=result || call->view_result_workspace!=home->view_workspace ||
        !rbc_value_slice(module,caller->code,call->view_result_source_argument)) return XRT_MANAGED_ARGUMENT;
    RbcValuePayload payload;
    XrtManagedStatus status=rbc_value_payload(module,stack,rbc_value_origin(activation,access.source),&payload);
    if (status!=XRT_MANAGED_OK || payload.size!=sizeof(XrtReadonlyWindow) || payload.lanes)
        return status!=XRT_MANAGED_OK ? status : XRT_MANAGED_ARGUMENT;
    XrtReadonlyWindowBundle borrowed={0}; memcpy(&borrowed.value,payload.address,sizeof(borrowed.value));
    if (!borrowed.value.owner) return XRT_MANAGED_ARGUMENT;
    /* The common return event proves the unique formal origin. Its live
     * backing is the physical hold source, never an address-indexed directory.
     * COPY publishes the caller's complete bundle before this callee retires. */
    borrowed.hold=(XrtManagedHeader *)borrowed.value.owner;
    XrtInlineValueHome landing={0};
    status=rbc_value_slice_storage(caller,call->view_result_workspace,&landing);
    XrtInlineValueSpec spec=xrt_array_slice_bundle_spec();
    return status==XRT_MANAGED_OK ? xrt_inline_value_replace(&landing,&spec,&borrowed) : status;
}
