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
    bool inline_array = kind == XRT_VALUE_ARRAY_INLINE;
    bool owned_shape = inline_array || kind == XRT_VALUE_STRING || kind == XRT_VALUE_MAP_STRING_I64 ||
        kind == XRT_VALUE_SET_I64;
    if (!binding || (!owned_shape && !xrt_value_kind_is_scalar_array(kind))) return false;
    if (owned_shape) {
        const XrRbcCodeOwner *owner = xr_rbc_module_code_owner(module);
        if (!owner) return false;
        const XrtValueImage *image = xr_rbc_module_value_image(module);
        const XrtValueImageType *type = &image->types[binding->type - 1];
        if (type->size != sizeof(XrtManagedHeader *) || type->alignment != _Alignof(XrtManagedHeader *) ||
            type->owned_count != 1 || type->first_owned >= image->owned_count || !image->owned_offsets ||
            image->owned_offsets[type->first_owned]) return false;
        if (inline_array) {
            size_t ordinal = (size_t)(function - module->functions);
            if (binding->kind != XR_LIR_HOME_BINDING_LOCAL || !type->element || type->element >= binding->type ||
                ordinal >= owner->function_count || !owner->value_homes || !owner->value_codes ||
                owner->value_homes[ordinal] != code->homes || owner->value_codes[ordinal] != code ||
                owner->values != image) return false;
            const XrtValueImageType *element = &image->types[type->element - 1];
            if (element->kind != XRT_VALUE_CLASS || element->class_storage) return false;
        }
    }
    bool state_cap=binding->kind==XR_LIR_HOME_BINDING_STATE_BORROW && binding->source==value;
    if (state_cap) {
        if ((kind!=XRT_VALUE_STRING && kind!=XRT_VALUE_MAP_STRING_I64 && kind!=XRT_VALUE_SET_I64) ||
            !rbc_value_scalar(code,function,value,XR_RBC_STATE_REF)) return false;
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
static const XrLirHomeWorkspace *rbc_value_binding_workspace(const XrRbcValueCode *code,
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
static bool rbc_value_binding_event(const XrRbcModule *module,const XrRbcValueCode *code,
    XrRbcValueAccess access,uint32_t receiver,uint32_t target,XrLirOwnershipFactKind kind) {
    uint32_t value_kind=rbc_value_kind(module,code,receiver);
    bool string=value_kind==XRT_VALUE_STRING;
    bool can_forward=value_kind==XRT_VALUE_MAP_STRING_I64 &&
        (kind==XR_LIR_OWNERSHIP_BINDING_NEW || kind==XR_LIR_OWNERSHIP_BINDING_READ);
    if (!xr_rbc_module_code_owner(module) || (string && !code->string_binding_setup) || !code->homes->events ||
        !access.index || access.index>code->homes->event_count) return false;
    const XrLirHomeEvent *event=&code->homes->events[access.index-1];
    if (event->kind!=(uint32_t)kind || event->source!=receiver || event->target!=target || !event->block || !event->instruction ||
        event->argument || event->edge || event->target_block || event->callable_transport || event->cell_transport ||
        event->view_operation || event->hold_transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE ||
        (!can_forward && (event->forwarding_mode || event->forwarding_drop))) return false;
    bool append=kind==XR_LIR_OWNERSHIP_STRING_OPERATION;
    if (event->transfer_mode!=(append ? XR_LIR_OWNERSHIP_TRANSFER_BORROW : XR_LIR_OWNERSHIP_TRANSFER_NONE)) return false;
    if (append) {
        if (event->operation!=XR_LIR_STRING_OP_APPEND_CELL && event->operation!=XR_LIR_STRING_OP_APPEND_STATE &&
            event->operation!=XR_LIR_STRING_OP_APPEND_REF) return false;
    } else if (!string) {
        if (event->operation) return false;
    } else if (event->operation && event->operation!=(kind==XR_LIR_OWNERSHIP_BINDING_READ ? XR_LIR_STRING_OP_READ_REF :
        kind==XR_LIR_OWNERSHIP_BINDING_WRITE ? XR_LIR_STRING_OP_WRITE_REF : 0u)) return false;
    bool read=kind==XR_LIR_OWNERSHIP_BINDING_READ;
    bool copy=kind==XR_LIR_OWNERSHIP_BINDING_NEW || kind==XR_LIR_OWNERSHIP_BINDING_WRITE;
    if (append || copy) {
        const XrLirValueHome *input=rbc_value_home(code,target);
        if (append || (input && input->parameter)) {
            if (!rbc_value_binding_workspace(code,access,event,receiver,target)) return false;
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
static bool rbc_value_binding_forwarding_event_plain(const XrLirHomeEvent *event) {
    return !event->argument && !event->edge && !event->target_block && !event->operation &&
        !event->callable_function && !event->callable_scope && !event->callable_transport && !event->callable_materialize &&
        !event->transfer_mode && !event->view_operation && !event->view_workspace && !event->view_cleanup &&
        !event->hold_transfer_mode && !event->cell_operation && !event->cell_type && !event->cell_payload_type &&
        !event->cell_transport && !event->cell_scope && !event->view_storage && event->view_parameter==UINT32_MAX;
}
/* An event index is only a projection. The receiving code owner, exact homes,
 * original operand/result event and adjacent logical release grant this use. */
static const XrLirHomeEvent *rbc_value_binding_forwarding_event(const XrRbcModule *module,
    const XrRbcValueCode *code,const XrRbcFunction *function,XrRbcValueAccess access) {
    bool create=access.operation==XR_RBC_VALUE_BINDING_NEW;
    const XrLirValueHomes *homes=code->homes;
    const XrRbcCodeOwner *owner=xr_rbc_module_code_owner(module);
    size_t ordinal=(size_t)(function-module->functions);
    if ((!create && access.operation!=XR_RBC_VALUE_BINDING_READ) || !owner || ordinal>=owner->function_count ||
        !owner->value_homes || !owner->value_codes || owner->value_homes[ordinal]!=homes || owner->value_codes[ordinal]!=code ||
        !homes || !homes->events || !access.index || access.index>homes->event_count ||
        access.other || access.first || access.count || !homes->owned_frame_needed ||
        !homes->frame.owned_offsets || homes->frame.payload_alignment<_Alignof(XrtManagedHeader *)) return NULL;
    uint32_t receiver=create ? access.result : access.source, value=create ? access.source : access.result;
    const XrLirHomeBinding *binding=rbc_value_binding(code,receiver);
    const XrLirValueHome *home=rbc_value_home(code,value);
    uint32_t kind=rbc_value_kind(module,code,receiver);
    bool map=kind==XRT_VALUE_MAP_STRING_I64;
    if (!binding || binding->kind!=XR_LIR_HOME_BINDING_LOCAL || binding->parameter || binding->source ||
        !home || home->type!=binding->type || home->parameter || home->borrow_source ||
        home->owner_count!=1 || home->size!=sizeof(XrtManagedHeader *) || home->byte_count ||
        home->transport || home->cell_transport || home->view_workspace || home->view_storage ||
        code->locations[value].kind!=XR_RBC_VALUE_NOWHERE ||
        (!map && !xrt_value_kind_is_array(kind)) || access.window!=(map && create ? UINT32_MAX : 0)) return NULL;
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    const XrtValueImageType *type=&image->types[binding->type-1];
    if (type->size!=sizeof(XrtManagedHeader *) || type->alignment!=_Alignof(XrtManagedHeader *) ||
        type->owned_count!=1 || type->first_owned>=image->owned_count || !image->owned_offsets ||
        image->owned_offsets[type->first_owned]) return NULL;
    size_t slots[]={binding->first_owner,home->first_owner}, offsets[]={binding->offset,home->offset};
    for (size_t n=0;n<2;++n)
        if (slots[n]>=homes->frame.slot_count || offsets[n]%_Alignof(XrtManagedHeader *) ||
            offsets[n]>homes->frame.payload_size || sizeof(XrtManagedHeader *)>homes->frame.payload_size-offsets[n] ||
            homes->frame.owned_offsets[slots[n]]!=offsets[n]) return NULL;
    if (slots[0]==slots[1] || offsets[0]==offsets[1]) return NULL;
    const XrLirHomeEvent *event=&homes->events[access.index-1];
    if (event->kind!=(uint32_t)(create ? XR_LIR_OWNERSHIP_BINDING_NEW : XR_LIR_OWNERSHIP_BINDING_READ) ||
        event->source!=receiver || event->target!=value || !event->block || !event->instruction ||
        event->instruction==UINT32_MAX || event->forwarding_mode!=XR_LIR_OWNERSHIP_TRANSFER_MOVE ||
        event->forwarding_drop!=event->instruction+1 || !rbc_value_binding_forwarding_event_plain(event)) return NULL;
    bool operand=false, release=false;
    for (size_t n=access.index;n<homes->event_count;++n) {
        const XrLirHomeEvent *next=&homes->events[n];
        if (next->block!=event->block || next->instruction>event->forwarding_drop) break;
        if (next->instruction==event->instruction && next->source==value && !next->target && !next->argument &&
            !next->edge && !next->target_block && next->kind==(uint32_t)(create ?
                XR_LIR_OWNERSHIP_OPERAND_BORROW : XR_LIR_OWNERSHIP_RESULT_OWNER) &&
            next->transfer_mode==(create ? XR_LIR_OWNERSHIP_TRANSFER_BORROW : XR_LIR_OWNERSHIP_TRANSFER_NONE)) operand=true;
        if (next->instruction==event->forwarding_drop && next->source==access.source && !next->target &&
            next->kind==(uint32_t)(create ? XR_LIR_OWNERSHIP_RELEASE : XR_LIR_OWNERSHIP_BINDING_RELEASE) &&
            !next->forwarding_mode && !next->forwarding_drop && rbc_value_binding_forwarding_event_plain(next)) release=true;
    }
    return operand && release ? event : NULL;
}
/* Admission proves the actual next word as well as the logical release.
 * The owned activation can subsequently use the shared MOVE fact. */
static bool rbc_value_binding_forwarding_words(const XrtValueImage *image,const XrRbcFunction *function,
    const XrRbcValueCode *code,uint32_t index,size_t pc) {
    if (index>=code->access_count || index>UINT16_MAX || pc>=function->code_count || function->code_count-pc<=2 ||
        function->code[pc]!=(XR_RBC_VALUE_ACCESS | (index<<16))) return false;
    uint32_t word=function->code[pc+1], drop_index=word>>16;
    if (drop_index>=code->access_count || (word&65535u)!=rbc_value_access_opcode(image,code,drop_index)) return false;
    XrRbcValueAccess access=code->accesses[index], drop=code->accesses[drop_index];
    bool create=access.operation==XR_RBC_VALUE_BINDING_NEW;
    const XrLirValueHomes *homes=code->homes;
    if ((!create && access.operation!=XR_RBC_VALUE_BINDING_READ) ||
        drop.operation!=(create ? XR_RBC_VALUE_DROP : XR_RBC_VALUE_BINDING_DROP) || drop.source!=access.source ||
        drop.result || drop.other || drop.first || drop.count || drop.window) return false;
    if (!drop.index) return true;
    if (create || !access.index || access.index>homes->event_count || drop.index>homes->event_count) return false;
    const XrLirHomeEvent *forward=&homes->events[access.index-1], *release=&homes->events[drop.index-1];
    return release->kind==XR_LIR_OWNERSHIP_BINDING_RELEASE && release->source==drop.source &&
        release->block==forward->block && release->instruction==forward->forwarding_drop;
}
static bool rbc_value_binding_access_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess a) {
    if (a.first || a.count) return false;
    uint32_t value=a.operation==XR_RBC_VALUE_BINDING_NEW ? a.result : a.source;
    const XrLirHomeBinding *binding=rbc_value_binding(code,value);
    if (rbc_value_i64_ref(module,code,value)) {
        if (a.index || a.window) return false;
        if (a.operation==XR_RBC_VALUE_BINDING_READ)
            return !a.other && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
        return a.operation==XR_RBC_VALUE_BINDING_WRITE && !a.result &&
            rbc_value_scalar(code,function,a.other,XR_RBC_BITS);
    }
    if (!rbc_value_binding_valid(module,code,function,value)) return false;
    bool string=rbc_value_kind(module,code,value)==XRT_VALUE_STRING;
    if (string || rbc_value_kind(module,code,value)==XRT_VALUE_MAP_STRING_I64 || rbc_value_kind(module,code,value)==XRT_VALUE_SET_I64) {
        uint32_t target=a.operation==XR_RBC_VALUE_BINDING_DROP ? 0 : a.operation==XR_RBC_VALUE_BINDING_NEW ? a.source :
            a.operation==XR_RBC_VALUE_BINDING_READ ? a.result : a.other;
        XrLirOwnershipFactKind kind=a.operation==XR_RBC_VALUE_BINDING_NEW ? XR_LIR_OWNERSHIP_BINDING_NEW :
            a.operation==XR_RBC_VALUE_BINDING_READ ? XR_LIR_OWNERSHIP_BINDING_READ :
            a.operation==XR_RBC_VALUE_BINDING_WRITE ? XR_LIR_OWNERSHIP_BINDING_WRITE : XR_LIR_OWNERSHIP_BINDING_RELEASE;
        if (!rbc_value_binding_event(module,code,a,value,target,kind)) return false;
        const XrLirHomeEvent *event=&code->homes->events[a.index-1];
        if ((event->forwarding_mode || event->forwarding_drop) &&
            !rbc_value_binding_forwarding_event(module,code,function,a)) return false;
    } else if (a.window || (a.index && !rbc_value_binding_forwarding_event(module,code,function,a))) return false;
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
        rbc_value_binding_event(module,code,access,access.source,access.other,XR_LIR_OWNERSHIP_STRING_OPERATION);
}
static bool rbc_value_array_item_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,uint32_t value,XrtValueKind kind) {
    return kind==XRT_VALUE_ARRAY_STRING ? rbc_value_string(module,code,value,false) : rbc_value_scalar(code,function,value,XR_RBC_BITS);
}
static bool rbc_value_array_local_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess access,XrtValueKind kind) {
    const XrRbcCodeOwner *owner=xr_rbc_module_code_owner(module);
    const XrLirValueHomes *homes=code->homes;
    size_t ordinal=(size_t)(function-module->functions);
    if (!owner || ordinal>=owner->function_count || !owner->value_homes || !owner->value_codes ||
        owner->value_homes[ordinal]!=homes || owner->value_codes[ordinal]!=code ||
        !homes || !homes->events || !access.other || access.other>homes->event_count ||
        !homes->owned_frame_needed || !homes->workspaces || access.window>=homes->workspace_count) return false;
    const uint32_t *args=code->arguments+access.first;
    const XrLirHomeBinding *binding=rbc_value_binding(code,args[0]);
    const XrLirHomeWorkspace *workspace=&homes->workspaces[access.window];
    const XrLirHomeEvent *event=&homes->events[access.other-1];
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (!binding || binding->kind!=XR_LIR_HOME_BINDING_LOCAL || binding->parameter || binding->source ||
        !image || owner->values!=image || !image->types || !binding->type || binding->type>image->type_count ||
        code->locations[args[0]].kind!=XR_RBC_VALUE_NOWHERE) return false;
    const XrtValueImageType *type=&image->types[binding->type-1];
    if (type->kind!=kind || type->size!=sizeof(XrtManagedHeader *) ||
        type->alignment!=_Alignof(XrtManagedHeader *) || type->owned_count!=1 ||
        type->first_owned>=image->owned_count || !image->owned_offsets || image->owned_offsets[type->first_owned]) return false;
    const XrtManagedFrameSpec *frame=&homes->frame;
    if (!frame->owned_offsets || frame->payload_alignment<_Alignof(XrtManagedHeader *) ||
        (frame->payload_alignment & (frame->payload_alignment-1)) ||
        workspace->kind!=XR_LIR_HOME_WORKSPACE_VALUE || workspace->value || workspace->temporary_offset ||
        workspace->temporary_first_owner || workspace->type!=binding->type ||
        workspace->block!=event->block || workspace->instruction!=event->instruction) return false;
    size_t slots[]={binding->first_owner,workspace->first_owner},offsets[]={binding->offset,workspace->offset};
    for (size_t n=0;n<2;++n)
        if (slots[n]>=frame->slot_count || offsets[n]%_Alignof(XrtManagedHeader *) ||
            offsets[n]>frame->payload_size || sizeof(XrtManagedHeader *)>frame->payload_size-offsets[n] ||
            frame->owned_offsets[slots[n]]!=offsets[n]) return false;
    if (slots[0]==slots[1] || offsets[0]==offsets[1]) return false;
    XrLirHomeEvent plain=*event; plain.operation=0;
    return event->kind==XR_LIR_OWNERSHIP_ARRAY_OPERATION && event->source==args[0] && !event->target &&
        event->block && event->instruction && event->operation==access.index &&
        !event->forwarding_mode && !event->forwarding_drop && rbc_value_binding_forwarding_event_plain(&plain);
}

static bool rbc_value_inline_class_push_valid(const XrRbcModule *module, const XrRbcValueCode *code,
    const XrRbcFunction *function, XrRbcValueAccess access, const uint32_t *args) {
    if (access.result || access.count != 2 ||
        !rbc_value_binding_valid(module, code, function, args[0]) ||
        !rbc_value_array_local_valid(module, code, function, access, XRT_VALUE_ARRAY_INLINE)) return false;
    const XrLirHomeBinding *binding = rbc_value_binding(code, args[0]);
    const XrtValueImage *image = xr_rbc_module_value_image(module);
    uint32_t element = image->types[binding->type - 1].element;
    if (!rbc_value_exact_home(module, code, args[1], element, false)) return false;
    const XrLirValueHomes *homes = code->homes;
    const XrLirValueHome *item = rbc_value_home(code, args[1]);
    const XrLirHomeWorkspace *workspace = &homes->workspaces[access.window];
    if (workspace->first_owner == item->first_owner || workspace->first_owner == item->temporary_first_owner)
        return false;

    const XrLirHomeEvent *operation = &homes->events[access.other - 1];
    /* Common operand facts precede the Array operation at this instruction. */
    for (size_t n = access.other - 1; n; --n) {
        const XrLirHomeEvent *operand = &homes->events[n - 1];
        if (operand->block != operation->block || operand->instruction != operation->instruction) break;
        if (operand->kind == XR_LIR_OWNERSHIP_OPERAND_BORROW && operand->argument == 1 &&
            operand->source == args[1] && !operand->target && !operand->edge && !operand->target_block &&
            operand->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_BORROW) return true;
    }
    return false;
}

static bool rbc_value_array_access_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess a) {
    if (a.source || a.first>code->argument_count || a.count>code->argument_count-a.first ||
        (a.count && !code->arguments) || a.index<XR_LIR_ARRAY_OP_NEW || a.index>XR_LIR_ARRAY_OP_TIMING_EQUAL) return false;
    for (uint32_t p=0;p<a.count;++p) if (!rbc_value_id(code,code->arguments[a.first+p])) return false;
    const uint32_t *args=a.count ? code->arguments+a.first : NULL;
    bool create=a.index==XR_LIR_ARRAY_OP_NEW || a.index==XR_LIR_ARRAY_OP_REPEAT ||
        a.index==XR_LIR_ARRAY_OP_COPY_STRING_BYTES || a.index==XR_LIR_ARRAY_OP_WITH_CAPACITY;
    uint32_t array = create ? a.result : a.count ? args[0] : 0;
    XrtValueKind kind=rbc_value_kind(module,code,array);
    if (!xrt_value_kind_is_array(kind)) return false;
    if (kind == XRT_VALUE_ARRAY_INLINE) {
        if (a.index == XR_LIR_ARRAY_OP_PUSH) return rbc_value_inline_class_push_valid(module, code, function, a, args);
        if (a.other || a.window != UINT32_MAX || a.index > XR_LIR_ARRAY_OP_GET ||
            !rbc_value_array(module,code,array,create)) return false;
        const XrtValueImage *image = xr_rbc_module_value_image(module);
        if (a.index != XR_LIR_ARRAY_OP_LEN && !xr_rbc_module_code_owner(module) &&
            (!module->state_spec || module->state_spec->values != image)) return false;
        const XrLirValueHome *home = rbc_value_home(code,array);
        if (!image || !image->types || !home->type || home->type > image->type_count ||
            image->types[home->type - 1].kind != XRT_VALUE_ARRAY_INLINE) return false;
        uint32_t element = image->types[home->type - 1].element;
        if (!element || element >= home->type) return false;
        const XrtValueImageType *row = &image->types[element - 1];
        if (!xrt_value_inline_array_element_supported(row->kind, row->class_storage)) return false;
        if (create) {
            for (uint32_t p = 0; p < a.count; ++p)
                if (!rbc_value_inline_operand(module,code,function,args[p],element,false)) return false;
            return true;
        }
        if (a.index == XR_LIR_ARRAY_OP_LEN) return a.count == 1 && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
        return a.count == 2 && rbc_value_scalar(code,function,args[1],XR_RBC_BITS) &&
            rbc_value_inline_operand(module,code,function,a.result,element,true);
    }
    if (a.index==XR_LIR_ARRAY_OP_RANDOM_BYTES) {
        if (a.result || a.count!=1 || kind!=XRT_VALUE_ARRAY_U8 ||
            !rbc_value_binding_valid(module,code,function,args[0]) ||
            rbc_value_binding(code,args[0])->kind==XR_LIR_HOME_BINDING_LOCAL) return false;
        const XrRbcCodeOwner *owner=xr_rbc_module_code_owner(module);
        size_t ordinal=(size_t)(function-module->functions);
        if (!owner || ordinal>=owner->function_count || !owner->value_codes || owner->value_codes[ordinal]!=code ||
            !owner->value_homes || owner->value_homes[ordinal]!=code->homes ||
            !a.other || a.other>code->homes->event_count || !code->homes->events) return false;
        const XrLirHomeEvent *event=&code->homes->events[a.other-1];
        return event->kind==XR_LIR_OWNERSHIP_ARRAY_OPERATION && event->operation==XR_LIR_ARRAY_OP_RANDOM_BYTES &&
            event->source==args[0] && rbc_value_binding_workspace(code,a,event,args[0],0);
    }
    if (a.index==XR_LIR_ARRAY_OP_SET || a.index==XR_LIR_ARRAY_OP_PUSH) {
        if (a.result || a.count!=(a.index==XR_LIR_ARRAY_OP_SET ? 3u : 2u) ||
            !rbc_value_binding_valid(module,code,function,args[0]) || a.window>=code->homes->workspace_count) return false;
        const XrLirHomeBinding *binding=rbc_value_binding(code,args[0]);
        const XrLirHomeWorkspace *workspace=&code->homes->workspaces[a.window];
        bool local=kind==XRT_VALUE_ARRAY_I64 && binding->kind==XR_LIR_HOME_BINDING_LOCAL;
        if (a.other && (!local || !rbc_value_array_local_valid(module,code,function,a,XRT_VALUE_ARRAY_I64))) return false;
        return workspace->type==binding->type && workspace->first_owner<code->homes->frame.slot_count &&
            code->homes->frame.owned_offsets[workspace->first_owner]==workspace->offset &&
            (a.index!=XR_LIR_ARRAY_OP_SET || rbc_value_scalar(code,function,args[1],XR_RBC_BITS)) &&
            rbc_value_array_item_valid(module,code,function,args[a.count-1],kind);
    }
    if (a.other || a.window!=UINT32_MAX) return false;
    if (create) {
        if (!rbc_value_array(module,code,a.result,true)) return false;
        if (a.index==XR_LIR_ARRAY_OP_COPY_STRING_BYTES)
            return kind==XRT_VALUE_ARRAY_U8 && a.count==1 && rbc_value_string(module,code,args[0],false);
        if (a.index==XR_LIR_ARRAY_OP_WITH_CAPACITY)
            return a.count==1 && rbc_value_scalar(code,function,args[0],XR_RBC_BITS);
        if (a.index==XR_LIR_ARRAY_OP_REPEAT) return a.count==2 && rbc_value_scalar(code,function,args[0],XR_RBC_BITS) &&
            rbc_value_array_item_valid(module,code,function,args[1],kind);
        for (uint32_t p=0;p<a.count;++p) if (!rbc_value_array_item_valid(module,code,function,args[p],kind)) return false;
        return true;
    }
    if (!a.count || !rbc_value_array(module,code,args[0],false)) return false;
    if (a.index==XR_LIR_ARRAY_OP_TIMING_EQUAL)
        return a.count==2 && kind==XRT_VALUE_ARRAY_U8 &&
            rbc_value_kind(module,code,args[1])==XRT_VALUE_ARRAY_U8 &&
            rbc_value_array(module,code,args[1],false) &&
            rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    if (a.index==XR_LIR_ARRAY_OP_LEN || a.index==XR_LIR_ARRAY_OP_CAPACITY)
        return a.count==1 && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    if (a.index==XR_LIR_ARRAY_OP_DISPLAY) return a.count==1 && rbc_value_string(module,code,a.result,true);
    if (a.index==XR_LIR_ARRAY_OP_RESERVE) return a.count==2 && rbc_value_scalar(code,function,args[1],XR_RBC_BITS) &&
        rbc_value_kind(module,code,a.result)==kind && rbc_value_array(module,code,a.result,true);
    return a.count==2 && rbc_value_scalar(code,function,args[1],XR_RBC_BITS) &&
        rbc_value_array_item_valid(module,code,function,a.result,kind) &&
        (kind!=XRT_VALUE_ARRAY_STRING || rbc_value_string(module,code,a.result,true));
}
