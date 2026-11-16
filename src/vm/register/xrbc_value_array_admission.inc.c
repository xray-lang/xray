/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_array_admission.inc.c - Exact Array operands over authentic common homes
 */
static bool rbc_value_binding_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,uint32_t value) {
    const XrLirHomeBinding *binding=rbc_value_binding(code,value); XrtValueKind kind=rbc_value_kind(module,code,value);
    if (!binding || (kind!=XRT_VALUE_STRING && kind!=XRT_VALUE_MAP_STRING_I64 &&
            (kind<XRT_VALUE_ARRAY_I64 || kind>XRT_VALUE_ARRAY_U64))) return false;
    if (kind==XRT_VALUE_STRING && !xr_rbc_module_code_owner(module)) return false;
    if (kind==XRT_VALUE_STRING) {
        const XrtValueImage *image=xr_rbc_module_value_image(module);
        const XrtValueImageType *type=&image->types[binding->type-1];
        if (type->size!=sizeof(XrtManagedHeader *) || type->alignment!=_Alignof(XrtManagedHeader *) ||
            type->owned_count!=1 || type->first_owned>=image->owned_count || !image->owned_offsets ||
            image->owned_offsets[type->first_owned]) return false;
    }
    bool state_cap=binding->kind==XR_LIR_HOME_BINDING_STATE_BORROW && binding->source==value;
    if (state_cap) {
        if (kind!=XRT_VALUE_STRING || !rbc_value_scalar(code,function,value,XR_RBC_STATE_REF)) return false;
    } else if (code->locations[value].kind!=XR_RBC_VALUE_NOWHERE) return false;
    if (binding->kind==XR_LIR_HOME_BINDING_LOCAL)
        return (!binding->parameter && !binding->source) && binding->offset%_Alignof(XrtManagedHeader *)==0 &&
            binding->offset<=code->homes->frame.payload_size && sizeof(XrtManagedHeader *)<=code->homes->frame.payload_size-binding->offset &&
            binding->first_owner<code->homes->frame.slot_count && code->homes->frame.owned_offsets &&
            code->homes->frame.owned_offsets[binding->first_owner]==binding->offset;
    if (binding->offset!=SIZE_MAX || binding->first_owner!=SIZE_MAX) return false;
    if (binding->kind==XR_LIR_HOME_BINDING_PARAMETER)
        return binding->parameter && binding->parameter<=code->parameter_count &&
            code->parameters[binding->parameter-1]==value;
    if (binding->kind==XR_LIR_HOME_BINDING_STATE_BORROW)
        return rbc_value_scalar(code,function,binding->source,XR_RBC_STATE_REF);
    if (binding->kind==XR_LIR_HOME_BINDING_CELL_PAYLOAD_BORROW)
        return kind==XRT_VALUE_MAP_STRING_I64 && binding->offset==SIZE_MAX && binding->first_owner==SIZE_MAX &&
            rbc_value_cell(module,code,binding->source,false) &&
            rbc_value_home(code,binding->source)->cell_payload_type==binding->type;
    const XrLirHomeBinding *source=rbc_value_binding(code,binding->source);
    return binding->kind==XR_LIR_HOME_BINDING_CELL_BORROW && source && source->type==binding->type &&
        (source->kind==XR_LIR_HOME_BINDING_LOCAL || source->kind==XR_LIR_HOME_BINDING_PARAMETER);
}
static const XrLirHomeWorkspace *rbc_value_string_workspace(const XrRbcValueCode *code,
    XrRbcValueAccess access,const XrLirHomeEvent *event,uint32_t receiver,uint32_t input) {
    const XrLirValueHomes *homes=code->homes;
    if (!homes || !homes->workspaces || access.window>=homes->workspace_count || !homes->owned_frame_needed ||
        !homes->frame.owned_offsets || homes->frame.payload_alignment<_Alignof(XrtManagedHeader *)) return NULL;
    const XrLirHomeWorkspace *workspace=&homes->workspaces[access.window];
    const XrLirHomeBinding *binding=rbc_value_binding(code,receiver);
    if (!binding || workspace->kind!=XR_LIR_HOME_WORKSPACE_VALUE || workspace->value!=receiver ||
        workspace->type!=binding->type || workspace->block!=event->block || workspace->instruction!=event->instruction ||
        workspace->temporary_offset!=SIZE_MAX || workspace->temporary_first_owner!=SIZE_MAX ||
        workspace->first_owner>=homes->frame.slot_count || workspace->offset%_Alignof(XrtManagedHeader *) ||
        workspace->offset>homes->frame.payload_size || sizeof(XrtManagedHeader *)>homes->frame.payload_size-workspace->offset ||
        homes->frame.owned_offsets[workspace->first_owner]!=workspace->offset ||
        workspace->first_owner==binding->first_owner) return NULL;
    const XrLirValueHome *source=rbc_value_home(code,input);
    if (source && (workspace->first_owner==source->first_owner || workspace->first_owner==source->temporary_first_owner ||
        workspace->first_owner==source->literal_cache_owner)) return NULL;
    return workspace;
}
static bool rbc_value_string_binding_event(const XrRbcModule *module,const XrRbcValueCode *code,
    XrRbcValueAccess access,uint32_t receiver,uint32_t target,XrLirOwnershipFactKind kind) {
    if (!xr_rbc_module_code_owner(module) || !code->string_binding_setup || !code->homes->events ||
        !access.index || access.index>code->homes->event_count) return false;
    const XrLirHomeEvent *event=&code->homes->events[access.index-1];
    if (event->kind!=(uint32_t)kind || event->source!=receiver || event->target!=target || !event->block || !event->instruction ||
        event->argument || event->edge || event->target_block || event->callable_transport || event->cell_transport ||
        event->view_operation || event->hold_transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE) return false;
    bool append=kind==XR_LIR_OWNERSHIP_STRING_OPERATION;
    if (event->transfer_mode!=(append ? XR_LIR_OWNERSHIP_TRANSFER_BORROW : XR_LIR_OWNERSHIP_TRANSFER_NONE)) return false;
    if (append) {
        if (event->operation!=XR_LIR_STRING_OP_APPEND_CELL && event->operation!=XR_LIR_STRING_OP_APPEND_STATE &&
            event->operation!=XR_LIR_STRING_OP_APPEND_REF) return false;
    } else if (event->operation && event->operation!=(kind==XR_LIR_OWNERSHIP_BINDING_READ ? XR_LIR_STRING_OP_READ_REF :
        kind==XR_LIR_OWNERSHIP_BINDING_WRITE ? XR_LIR_STRING_OP_WRITE_REF : 0u)) return false;
    bool read=kind==XR_LIR_OWNERSHIP_BINDING_READ;
    bool copy=kind==XR_LIR_OWNERSHIP_BINDING_NEW || kind==XR_LIR_OWNERSHIP_BINDING_WRITE;
    if (append || copy) {
        const XrLirValueHome *input=rbc_value_home(code,target);
        if (append || (input && input->parameter)) {
            if (!rbc_value_string_workspace(code,access,event,receiver,target)) return false;
        } else if (access.window!=UINT32_MAX) return false;
    } else if (access.window) return false;
    if (!append && !copy && !read) return true;
    uint32_t argument=kind==XR_LIR_OWNERSHIP_BINDING_NEW ? 0u : 1u;
    for (size_t n=access.index;n<code->homes->event_count;++n) {
        const XrLirHomeEvent *operand=&code->homes->events[n];
        if (operand->block!=event->block || operand->instruction!=event->instruction) break;
        if (operand->source==target && !operand->target && !operand->edge && !operand->target_block &&
            operand->kind==(uint32_t)(read ? XR_LIR_OWNERSHIP_RESULT_OWNER : XR_LIR_OWNERSHIP_OPERAND_BORROW) &&
            operand->argument==(read ? 0u : argument) &&
            operand->transfer_mode==(read ? XR_LIR_OWNERSHIP_TRANSFER_NONE : XR_LIR_OWNERSHIP_TRANSFER_BORROW)) return true;
    }
    return false;
}
static bool rbc_value_binding_access_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess a) {
    if (a.first || a.count) return false;
    uint32_t value=a.operation==XR_RBC_VALUE_BINDING_NEW ? a.result : a.source;
    const XrLirHomeBinding *binding=rbc_value_binding(code,value);
    if (!rbc_value_binding_valid(module,code,function,value)) return false;
    bool string=rbc_value_kind(module,code,value)==XRT_VALUE_STRING;
    if (string) {
        uint32_t target=a.operation==XR_RBC_VALUE_BINDING_DROP ? 0 : a.operation==XR_RBC_VALUE_BINDING_NEW ? a.source :
            a.operation==XR_RBC_VALUE_BINDING_READ ? a.result : a.other;
        XrLirOwnershipFactKind kind=a.operation==XR_RBC_VALUE_BINDING_NEW ? XR_LIR_OWNERSHIP_BINDING_NEW :
            a.operation==XR_RBC_VALUE_BINDING_READ ? XR_LIR_OWNERSHIP_BINDING_READ :
            a.operation==XR_RBC_VALUE_BINDING_WRITE ? XR_LIR_OWNERSHIP_BINDING_WRITE : XR_LIR_OWNERSHIP_BINDING_RELEASE;
        if (!rbc_value_string_binding_event(module,code,a,value,target,kind)) return false;
    } else if (a.index || a.window) return false;
    if (a.operation==XR_RBC_VALUE_BINDING_DROP)
        return !a.result && !a.other && binding->kind==XR_LIR_HOME_BINDING_LOCAL;
    if (a.operation==XR_RBC_VALUE_BINDING_NEW)
        return !a.other && binding->kind==XR_LIR_HOME_BINDING_LOCAL &&
            (string ? rbc_value_string(module,code,a.source,false) : rbc_value_container(module,code,a.source,false)) &&
            binding->type==rbc_value_home(code,a.source)->type;
    if (a.operation==XR_RBC_VALUE_BINDING_READ)
        return !a.other && (string ? rbc_value_string(module,code,a.result,true) : rbc_value_container(module,code,a.result,true)) &&
            binding->type==rbc_value_home(code,a.result)->type;
    return !a.result && (string ? rbc_value_string(module,code,a.other,false) : rbc_value_container(module,code,a.other,false)) &&
        binding->type==rbc_value_home(code,a.other)->type;
}
static bool rbc_value_string_binding_append_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess access) {
    return !access.result && !access.first && !access.count &&
        rbc_value_kind(module,code,access.source)==XRT_VALUE_STRING &&
        rbc_value_binding_valid(module,code,function,access.source) && rbc_value_string(module,code,access.other,false) &&
        rbc_value_binding(code,access.source)->type==rbc_value_home(code,access.other)->type &&
        rbc_value_string_binding_event(module,code,access,access.source,access.other,XR_LIR_OWNERSHIP_STRING_OPERATION);
}
static bool rbc_value_array_item_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,uint32_t value,XrtValueKind kind) {
    return kind==XRT_VALUE_ARRAY_STRING ? rbc_value_string(module,code,value,false) : rbc_value_scalar(code,function,value,XR_RBC_BITS);
}
static bool rbc_value_array_access_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess a) {
    if (a.source || a.other || a.first>code->argument_count || a.count>code->argument_count-a.first ||
        (a.count && !code->arguments) || a.index<XR_LIR_ARRAY_OP_NEW || a.index>XR_LIR_ARRAY_OP_DISPLAY) return false;
    for (uint32_t p=0;p<a.count;++p) if (!rbc_value_id(code,code->arguments[a.first+p])) return false;
    const uint32_t *args=a.count ? code->arguments+a.first : NULL;
    bool create=a.index==XR_LIR_ARRAY_OP_NEW || a.index==XR_LIR_ARRAY_OP_REPEAT;
    XrtValueKind kind=rbc_value_kind(module,code,create ? a.result : a.count ? args[0] : 0);
    if (kind<XRT_VALUE_ARRAY_I64 || kind>XRT_VALUE_ARRAY_U64) return false;
    if (a.index==XR_LIR_ARRAY_OP_SET || a.index==XR_LIR_ARRAY_OP_PUSH) {
        if (a.result || a.count!=(a.index==XR_LIR_ARRAY_OP_SET ? 3u : 2u) ||
            !rbc_value_binding_valid(module,code,function,args[0]) || a.window>=code->homes->workspace_count) return false;
        const XrLirHomeBinding *binding=rbc_value_binding(code,args[0]);
        const XrLirHomeWorkspace *workspace=&code->homes->workspaces[a.window];
        return workspace->type==binding->type && workspace->first_owner<code->homes->frame.slot_count &&
            code->homes->frame.owned_offsets[workspace->first_owner]==workspace->offset &&
            (a.index!=XR_LIR_ARRAY_OP_SET || rbc_value_scalar(code,function,args[1],XR_RBC_BITS)) &&
            rbc_value_array_item_valid(module,code,function,args[a.count-1],kind);
    }
    if (a.window!=UINT32_MAX) return false;
    if (create) {
        if (!rbc_value_array(module,code,a.result,true)) return false;
        if (a.index==XR_LIR_ARRAY_OP_REPEAT) return a.count==2 && rbc_value_scalar(code,function,args[0],XR_RBC_BITS) &&
            rbc_value_array_item_valid(module,code,function,args[1],kind);
        for (uint32_t p=0;p<a.count;++p) if (!rbc_value_array_item_valid(module,code,function,args[p],kind)) return false;
        return true;
    }
    if (!a.count || !rbc_value_array(module,code,args[0],false)) return false;
    if (a.index==XR_LIR_ARRAY_OP_LEN) return a.count==1 && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    if (a.index==XR_LIR_ARRAY_OP_DISPLAY) return a.count==1 && rbc_value_string(module,code,a.result,true);
    return a.count==2 && rbc_value_scalar(code,function,args[1],XR_RBC_BITS) &&
        rbc_value_array_item_valid(module,code,function,a.result,kind) &&
        (kind!=XRT_VALUE_ARRAY_STRING || rbc_value_string(module,code,a.result,true));
}
