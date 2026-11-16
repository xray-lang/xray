/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_slice_admission.inc.c - Natural borrowed windows and exact keeper homes
 */
static const XrLirHomeWorkspace *rbc_value_slice_workspace(const XrRbcValueCode *code,
    uint32_t ordinal) {
    if (!code || !code->homes || !ordinal || ordinal>code->homes->workspace_count || !code->homes->workspaces)
        return NULL;
    const XrLirValueHomes *homes=code->homes;
    const XrLirHomeWorkspace *workspace=&homes->workspaces[ordinal-1];
    if (workspace->kind!=XR_LIR_HOME_WORKSPACE_VIEW_HOLD || !homes->owned_frame_needed ||
        !homes->frame.owned_offsets || homes->frame.payload_alignment<_Alignof(XrtArraySliceBundle) ||
        workspace->first_owner>=homes->frame.slot_count || workspace->temporary_first_owner>=homes->frame.slot_count ||
        workspace->first_owner==workspace->temporary_first_owner) return NULL;
    size_t offsets[]={workspace->offset,workspace->temporary_offset};
    size_t slots[]={workspace->first_owner,workspace->temporary_first_owner};
    for (size_t n=0;n<2;++n)
        if (offsets[n]%_Alignof(XrtArraySliceBundle) || offsets[n]>homes->frame.payload_size ||
            sizeof(XrtArraySliceBundle)>homes->frame.payload_size-offsets[n] ||
            homes->frame.owned_offsets[slots[n]]!=offsets[n]+offsetof(XrtArraySliceBundle,hold)) return NULL;
    size_t distance=offsets[0]>offsets[1] ? offsets[0]-offsets[1] : offsets[1]-offsets[0];
    return distance<sizeof(XrtArraySliceBundle) ? NULL : workspace;
}
static bool rbc_value_slice(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value) {
    /* A foreign raw description cannot acquire a loan by reporting the same
     * plan, offsets, owner slot or optional lease as a real compiled program. */
    if (!xr_rbc_module_code_owner(module)) return false;
    const XrLirValueHome *home=rbc_value_home(code,value);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (!home || !image || !image->types || !home->type || home->type>image->type_count ||
        home->parameter || home->owner_count || home->first_owner!=SIZE_MAX || home->temporary_first_owner!=SIZE_MAX ||
        home->size!=sizeof(XrtReadonlyArraySlice) || code->locations[value].kind!=XR_RBC_VALUE_NOWHERE) return false;
    const XrtValueImageType *type=&image->types[home->type-1];
    if ((type->kind!=XRT_VALUE_SLICE_I64 && type->kind!=XRT_VALUE_SLICE_U8) || type->owned_count ||
        type->size!=sizeof(XrtReadonlyArraySlice) || type->alignment!=_Alignof(XrtReadonlyArraySlice) ||
        home->byte_count!=1 || !code->homes->bytes || home->first_bytes>=code->homes->byte_count ||
        code->homes->bytes[home->first_bytes].offset || code->homes->bytes[home->first_bytes].size!=sizeof(XrtReadonlyArraySlice)) return false;
    const XrLirHomeWorkspace *workspace=rbc_value_slice_workspace(code,home->view_workspace);
    return workspace && workspace->value==value && workspace->type==home->type &&
        workspace->offset==home->offset && workspace->temporary_offset==home->temporary_offset;
}
static bool rbc_value_slice_access_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess access) {
    if (!xr_rbc_module_code_owner(module) || access.source || access.other ||
        access.index<=XR_LIR_SLICE_OP_NONE || access.index>=XR_LIR_SLICE_OP_COUNT ||
        access.first>code->argument_count || access.count>code->argument_count-access.first || !code->arguments) return false;
    for (uint32_t p=0;p<access.count;++p) if (!rbc_value_id(code,code->arguments[access.first+p])) return false;
    bool construct=access.index==XR_LIR_SLICE_OP_NEW || access.index==XR_LIR_SLICE_OP_REBORROW || access.index==XR_LIR_SLICE_OP_BIND;
    uint32_t count=access.index==XR_LIR_SLICE_OP_NEW ? 3u : access.index==XR_LIR_SLICE_OP_REBORROW ? 4u :
        access.index==XR_LIR_SLICE_OP_GET ? 2u : 1u;
    if (access.count!=count) return false;
    const uint32_t *args=code->arguments+access.first;
    uint32_t value=construct ? access.result : args[0];
    if (!rbc_value_slice(module,code,value)) return false;
    const XrLirValueHome *home=rbc_value_home(code,value);
    XrtValueKind kind=rbc_value_kind(module,code,value);
    if (access.index==XR_LIR_SLICE_OP_NEW) {
        if (!rbc_value_binding_valid(module,code,function,args[0]) || rbc_value_kind(module,code,args[0])!=
            (kind==XRT_VALUE_SLICE_I64 ? XRT_VALUE_ARRAY_I64 : XRT_VALUE_ARRAY_U8)) return false;
    } else if (construct && (!rbc_value_slice(module,code,args[0]) || rbc_value_home(code,args[0])->type!=home->type)) return false;
    if (construct || access.index==XR_LIR_SLICE_OP_END) {
        if (!code->homes->events || !access.window || access.window>code->homes->event_count) return false;
        const XrLirHomeEvent *event=&code->homes->events[access.window-1];
        if (event->kind!=(construct ? XR_LIR_OWNERSHIP_VIEW_VALUE : XR_LIR_OWNERSHIP_VIEW_END) ||
            event->source!=value || event->target!=(construct ? args[0] : 0) || event->view_operation!=access.index ||
            event->view_workspace!=home->view_workspace ||
            event->transfer_mode!=(construct ? XR_LIR_OWNERSHIP_TRANSFER_BORROW : XR_LIR_OWNERSHIP_TRANSFER_NONE) ||
            event->hold_transfer_mode!=(construct ? XR_LIR_OWNERSHIP_TRANSFER_COPY : XR_LIR_OWNERSHIP_TRANSFER_NONE)) return false;
    } else if (access.window) return false;
    if (access.index==XR_LIR_SLICE_OP_NEW || access.index==XR_LIR_SLICE_OP_REBORROW)
        return rbc_value_scalar(code,function,args[1],XR_RBC_BITS) && rbc_value_scalar(code,function,args[2],XR_RBC_BITS) &&
            (access.index!=XR_LIR_SLICE_OP_REBORROW || rbc_value_scalar(code,function,args[3],XR_RBC_BITS));
    if (access.index==XR_LIR_SLICE_OP_LEN || access.index==XR_LIR_SLICE_OP_GET)
        return rbc_value_scalar(code,function,access.result,XR_RBC_BITS) &&
            (access.index!=XR_LIR_SLICE_OP_GET || rbc_value_scalar(code,function,args[1],XR_RBC_BITS));
    if (access.index==XR_LIR_SLICE_OP_COPY)
        return rbc_value_array(module,code,access.result,true) && rbc_value_kind(module,code,access.result)==
            (kind==XRT_VALUE_SLICE_I64 ? XRT_VALUE_ARRAY_I64 : XRT_VALUE_ARRAY_U8);
    return access.index!=XR_LIR_SLICE_OP_END || !access.result;
}
