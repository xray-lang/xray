/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_admission.inc.c - Authentic typed operations and bounded SSA places
 */
static const XrRbcValueCode *rbc_value_code(const XrRbcModule *module, const XrRbcFunction *function) {
    size_t index = (size_t)(function - module->functions);
    if (index >= module->function_count || index > UINT32_MAX) return NULL;
    return xr_rbc_module_value_code(module,(uint32_t)index);
}
static const XrLirValueHome *rbc_value_home(const XrRbcValueCode *code, uint32_t value) {
    return code && code->homes && code->homes->values && value && value <= code->value_count &&
        value <= code->homes->value_count ? &code->homes->values[value] : NULL;
}
static bool rbc_value_id(const XrRbcValueCode *code, uint32_t value) {
    return code && code->locations && value && value <= code->value_count;
}
static bool rbc_value_location_valid(const XrRbcFunction *f, XrRbcValueLocation location) {
    return location.kind == XR_RBC_VALUE_REGISTER ? location.index < f->register_count :
        location.kind == XR_RBC_VALUE_SPILL ? location.index >= f->reference_spill_count && location.index < f->spill_count :
        location.kind == XR_RBC_VALUE_AGGREGATE && location.index < f->aggregate_value_count;
}
static bool rbc_value_scalar(const XrRbcValueCode *code, const XrRbcFunction *f,
    uint32_t value, XrRbcRegisterKind kind) {
    if (!rbc_value_id(code,value)) return false;
    XrRbcValueLocation location = code->locations[value];
    const XrLirValueHome *home = rbc_value_home(code,value);
    if ((home && home->type) || !rbc_value_location_valid(f,location) ||
        location.kind == XR_RBC_VALUE_AGGREGATE) return false;
    return location.kind == XR_RBC_VALUE_SPILL ? kind == XR_RBC_BITS || kind == XR_RBC_STATE_REF :
        rbc_register_kind(f,location.index) == kind;
}
static bool rbc_value_string_shape(const XrRbcModule *module, const XrRbcValueCode *code,
    uint32_t value, bool destination) {
    const XrLirValueHome *home = rbc_value_home(code,value);
    const XrtValueImage *image = xr_rbc_module_value_image(module);
    return home && image && image->types && home->type && home->type <= image->type_count &&
        image->types[home->type - 1].kind == XRT_VALUE_STRING && home->owner_count == 1 &&
        home->size == sizeof(XrtManagedHeader *) && code->locations[value].kind == XR_RBC_VALUE_NOWHERE &&
        (!destination || (!home->parameter && home->offset != SIZE_MAX &&
            home->temporary_offset != SIZE_MAX && home->temporary_first_owner < code->homes->frame.slot_count));
}
static bool rbc_value_string(const XrRbcModule *module, const XrRbcValueCode *code,
    uint32_t value, bool destination) {
    return rbc_value_string_shape(module,code,value,destination) &&
        !rbc_value_home(code,value)->literal_cache_borrow;
}
static bool rbc_value_string_destination(const XrRbcModule *module,const XrRbcValueCode *code,
    uint32_t value,bool literal) {
    if (!xr_rbc_module_code_owner(module) || !rbc_value_string_shape(module,code,value,true)) return false;
    const XrLirValueHome *home=rbc_value_home(code,value);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    const XrtValueImageType *type=&image->types[home->type-1];
    const XrtManagedFrameSpec *frame=&code->homes->frame;
    if ((!literal && home->literal_cache_borrow) || type->size!=sizeof(XrtManagedHeader *) ||
        type->alignment!=_Alignof(XrtManagedHeader *) || type->owned_count!=1 ||
        type->first_owned>=image->owned_count || !image->owned_offsets || image->owned_offsets[type->first_owned] ||
        !code->homes->owned_frame_needed || !frame->owned_offsets || frame->payload_alignment<_Alignof(XrtManagedHeader *)) return false;
    size_t offsets[]={home->offset,home->temporary_offset,home->literal_cache_offset};
    size_t slots[]={home->first_owner,home->temporary_first_owner,home->literal_cache_owner};
    for (size_t n=0;n<(literal ? 3u : 2u);++n) {
        if (slots[n]>=frame->slot_count || offsets[n]%_Alignof(XrtManagedHeader *) ||
            offsets[n]>frame->payload_size || sizeof(XrtManagedHeader *)>frame->payload_size-offsets[n] ||
            frame->owned_offsets[slots[n]]!=offsets[n]) return false;
        for (size_t p=0;p<n;++p) if (slots[n]==slots[p] || offsets[n]==offsets[p]) return false;
    }
    return true;
}
static bool rbc_value_string_borrow_operand(const XrRbcModule *module, const XrRbcValueCode *code,
    uint32_t value) {
    const XrLirValueHome *home = rbc_value_home(code,value);
    return home && (home->literal_cache_borrow ? rbc_value_string_destination(module,code,value,true) :
        rbc_value_string(module,code,value,false));
}
static const XrLirHomeBinding *rbc_value_binding(const XrRbcValueCode *code, uint32_t value) {
    return rbc_value_id(code,value) && code->homes && code->homes->bindings &&
        value<=code->homes->value_count && code->homes->bindings[value].type ? &code->homes->bindings[value] : NULL;
}
static bool rbc_value_binding_state_cap(const XrRbcValueCode *code,uint32_t value) {
    const XrLirHomeBinding *binding=rbc_value_binding(code,value);
    return binding && binding->kind==XR_LIR_HOME_BINDING_STATE_BORROW && binding->source==value;
}
static XrtValueKind rbc_value_kind(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value) {
    const XrLirValueHome *home=rbc_value_home(code,value);
    const XrLirHomeBinding *binding=rbc_value_binding(code,value);
    uint32_t type=binding ? binding->type : home ? home->type : 0;
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    return type && image && image->types && type<=image->type_count ? image->types[type-1].kind : XRT_VALUE_UNIT;
}
static bool rbc_value_i64_ref(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value) {
    const XrLirHomeBinding *binding=rbc_value_binding(code,value);
    return binding && binding->kind==XR_LIR_HOME_BINDING_PARAMETER && !binding->source &&
        binding->parameter && binding->parameter<=code->parameter_count && code->parameters &&
        code->parameters[binding->parameter-1]==value && binding->offset==SIZE_MAX && binding->first_owner==SIZE_MAX &&
        code->locations[value].kind==XR_RBC_VALUE_NOWHERE && rbc_value_kind(module,code,value)==XRT_VALUE_I64;
}
static bool rbc_value_array(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,bool destination) {
    const XrLirValueHome *home=rbc_value_home(code,value); XrtValueKind kind=rbc_value_kind(module,code,value);
    return home && xrt_value_kind_is_array(kind) && home->owner_count==1 &&
        home->size==sizeof(XrtManagedHeader *) && code->locations[value].kind==XR_RBC_VALUE_NOWHERE &&
        (!destination || (!home->parameter && home->offset!=SIZE_MAX && home->temporary_offset!=SIZE_MAX &&
            home->temporary_first_owner<code->homes->frame.slot_count));
}
static bool rbc_value_map(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,bool destination) {
    const XrLirValueHome *home=rbc_value_home(code,value);
    return home && rbc_value_kind(module,code,value)==XRT_VALUE_MAP_STRING_I64 && home->owner_count==1 &&
        home->size==sizeof(XrtManagedHeader *) && code->locations[value].kind==XR_RBC_VALUE_NOWHERE &&
        (!destination || (!home->parameter && home->offset!=SIZE_MAX && home->temporary_offset!=SIZE_MAX &&
            home->temporary_first_owner<code->homes->frame.slot_count));
}
static bool rbc_value_set(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,bool destination) {
    const XrLirValueHome *home=rbc_value_home(code,value);
    return xr_rbc_module_code_owner(module) && home && rbc_value_kind(module,code,value)==XRT_VALUE_SET_I64 &&
        home->owner_count==1 && home->size==sizeof(XrtManagedHeader *) &&
        code->locations[value].kind==XR_RBC_VALUE_NOWHERE &&
        (!destination || (!home->parameter && home->offset!=SIZE_MAX && home->temporary_offset!=SIZE_MAX &&
            home->temporary_first_owner<code->homes->frame.slot_count));
}
static bool rbc_value_container(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,bool destination) {
    return rbc_value_array(module,code,value,destination) || rbc_value_map(module,code,value,destination) || rbc_value_set(module,code,value,destination);
}
static bool rbc_value_function(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,bool destination) {
    const XrLirValueHome *home=rbc_value_home(code,value);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (!home || !image || !home->type || home->type>image->type_count ||
        code->locations[value].kind!=XR_RBC_VALUE_NOWHERE) return false;
    const XrtValueImageType *type=&image->types[home->type-1];
    if (type->kind!=XRT_VALUE_FUNCTION || type->size!=sizeof(XrtFunction) ||
        type->alignment!=_Alignof(XrtFunction) || type->owned_count!=1 ||
        image->owned_offsets[type->first_owned]!=offsetof(XrtFunction,owner) || home->size!=type->size) return false;
    bool owned=home->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED;
    if ((!owned && home->transport!=XR_LIR_CALLABLE_TRANSPORT_FRAME &&
            home->transport!=XR_LIR_CALLABLE_TRANSPORT_BORROW) || home->owner_count!=(owned ? 1u : 0u) ||
        (!owned && (home->first_owner!=SIZE_MAX || home->temporary_first_owner!=SIZE_MAX))) return false;
    return !destination || (!home->parameter && home->offset!=SIZE_MAX &&
        (!owned || (home->temporary_offset!=SIZE_MAX && home->temporary_first_owner<code->homes->frame.slot_count)));
}
static bool rbc_value_cell(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,bool destination) {
    if (!xr_rbc_module_code_owner(module)) return false;
    const XrLirValueHome *home=rbc_value_home(code,value);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (!home || !home->type || !image || !image->types || home->type>image->type_count ||
        code->locations[value].kind!=XR_RBC_VALUE_NOWHERE || home->size!=sizeof(XrtManagedHeader *) ||
        home->transport!=XR_LIR_CALLABLE_TRANSPORT_NONE || home->borrow_source || home->environment_type || home->view_workspace)
        return false;
    const XrtValueImageType *type=&image->types[home->type-1];
    if (type->kind!=XRT_VALUE_CELL || type->size!=sizeof(XrtManagedHeader *) || type->alignment!=_Alignof(XrtManagedHeader *) ||
        type->owned_count!=1 || !image->owned_offsets || type->first_owned>=image->owned_count ||
        image->owned_offsets[type->first_owned] || !type->element || type->element!=home->cell_payload_type ||
        type->element>image->type_count) return false;
    XrtValueKind payload=image->types[type->element-1].kind;
    if (payload!=XRT_VALUE_I64 && payload!=XRT_VALUE_MAP_STRING_I64) return false;
    if (home->parameter)
        return !destination && home->cell_transport==XR_LIR_CELL_TRANSPORT_BORROW && !home->owner_count &&
            home->offset==SIZE_MAX && home->temporary_offset==SIZE_MAX && home->first_owner==SIZE_MAX &&
            home->temporary_first_owner==SIZE_MAX && home->parameter<=code->parameter_count &&
            code->parameters && code->parameters[home->parameter-1]==value && home->byte_count==1 &&
            code->homes->bytes && home->first_bytes<code->homes->byte_count &&
            !code->homes->bytes[home->first_bytes].offset && code->homes->bytes[home->first_bytes].size==home->size;
    const XrtManagedFrameSpec *frame=&code->homes->frame;
    if (home->cell_transport!=XR_LIR_CELL_TRANSPORT_OWNED || home->owner_count!=1 || home->byte_count ||
        !code->homes->owned_frame_needed || !frame->owned_offsets || frame->payload_alignment<type->alignment ||
        home->first_owner==home->temporary_first_owner || home->offset==home->temporary_offset) return false;
    size_t slots[]={home->first_owner,home->temporary_first_owner},offsets[]={home->offset,home->temporary_offset};
    for (size_t n=0;n<2;++n)
        if (slots[n]>=frame->slot_count || offsets[n]%type->alignment || offsets[n]>frame->payload_size ||
            home->size>frame->payload_size-offsets[n] || frame->owned_offsets[slots[n]]!=offsets[n]) return false;
    return true;
}
static bool rbc_value_inline(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,bool destination) {
    const XrLirValueHome *home=rbc_value_home(code,value);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (!home || !home->type || !image || !image->types || home->type>image->type_count ||
        code->locations[value].kind!=XR_RBC_VALUE_NOWHERE) return false;
    const XrtValueImageType *type=&image->types[home->type-1];
    if (type->kind==XRT_VALUE_FUNCTION) return rbc_value_function(module,code,value,destination);
    if (type->kind==XRT_VALUE_CELL) return rbc_value_cell(module,code,value,destination);
    return (xrt_value_kind_has_fields(type->kind) || type->kind==XRT_VALUE_NULLABLE ||
        type->kind==XRT_VALUE_CLASS || type->kind==XRT_VALUE_SUM) && home->size==type->size &&
        home->owner_count==type->owned_count && (!destination || (!home->parameter && home->offset!=SIZE_MAX &&
            (!home->owner_count || (home->temporary_offset!=SIZE_MAX && home->temporary_first_owner<code->homes->frame.slot_count))));
}
static bool rbc_value_owner(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,bool destination) {
    return rbc_value_string(module,code,value,destination) || rbc_value_container(module,code,value,destination) ||
        rbc_value_inline(module,code,value,destination);
}
static bool rbc_value_exact_home(const XrRbcModule *module, const XrRbcValueCode *code,
    uint32_t value, uint32_t type, bool destination) {
    const XrLirValueHome *home = rbc_value_home(code, value);
    return home && home->type == type && (rbc_value_array(module, code, value, destination) ||
        rbc_value_inline(module, code, value, destination));
}
static const XrtValueImageType *rbc_value_host_nullable_element(const XrRbcModule *module,
    const XrRbcValueCode *code, uint32_t value) {
    if (!rbc_value_inline(module, code, value, true)) return NULL;
    const XrtValueImage *image = xr_rbc_module_value_image(module);
    const XrLirValueHome *home = rbc_value_home(code, value);
    const XrtValueImageType *type = &image->types[home->type - 1];
    const XrtManagedFrameSpec *frame = &code->homes->frame;
    const size_t owner = offsetof(XrtHostNullableOwner, owner);
    if (type->kind != XRT_VALUE_NULLABLE || !type->nullable_element || type->nullable_element >= home->type ||
        type->size != sizeof(XrtHostNullableOwner) || type->alignment != _Alignof(XrtHostNullableOwner) ||
        type->nullable_tag != offsetof(XrtHostNullableOwner, present) || type->nullable_payload != owner ||
        type->owned_count != 1 || !image->owned_offsets || type->first_owned >= image->owned_count ||
        image->owned_offsets[type->first_owned] != owner || !frame->owned_offsets ||
        frame->payload_alignment < type->alignment || home->first_owner == home->temporary_first_owner ||
        home->first_owner >= frame->slot_count || home->temporary_first_owner >= frame->slot_count) return NULL;
    size_t offsets[] = {home->offset, home->temporary_offset};
    size_t slots[] = {home->first_owner, home->temporary_first_owner};
    for (size_t i = 0; i < 2; ++i) {
        size_t base = offsets[i];
        if (base % type->alignment || base > frame->payload_size || type->size > frame->payload_size - base ||
            frame->owned_offsets[slots[i]] != base + owner) return NULL;
    }
    if (offsets[0] < offsets[1] + type->size && offsets[1] < offsets[0] + type->size) return NULL;
    return &image->types[type->nullable_element - 1];
}
static bool rbc_value_file_storage(const XrRbcModule *module, const XrRbcValueCode *code, uint32_t value, bool optional) {
    const XrtValueImageType *type;
    if (optional) type = rbc_value_host_nullable_element(module, code, value);
    else {
        if (!rbc_value_inline(module, code, value, false)) return false;
        const XrtValueImage *image = xr_rbc_module_value_image(module);
        type = &image->types[rbc_value_home(code, value)->type - 1];
    }
    return type && type->kind == XRT_VALUE_CLASS && type->class_storage == XR_NATIVE_DECLARATION_FILE_STORAGE;
}
static bool rbc_value_host_nullable_backing(const XrRbcModule *module, const XrRbcValueCode *code,
    uint32_t value, XrtValueKind kind) {
    const XrtValueImageType *type = rbc_value_host_nullable_element(module, code, value);
    const XrtValueImage *image = xr_rbc_module_value_image(module);
    return type && type->kind == kind && type->size == sizeof(XrtManagedHeader *) &&
        type->alignment == _Alignof(XrtManagedHeader *) && type->owned_count == 1 &&
        type->first_owned < image->owned_count && !image->owned_offsets[type->first_owned];
}
/* An owner-free Tuple (possibly wrapped in Nullable) can leave the outer
 * activation as natural bytes and its original image type. */
static bool rbc_inline_tuple_result(const XrtValueImage *image, uint32_t type) {
    if (!image || !image->types || !type || type > image->type_count) return false;
    for (;;) {
        const XrtValueImageType *row = &image->types[type - 1];
        if (row->owned_count || !row->size) return false;
        if (row->kind == XRT_VALUE_TUPLE) return true;
        if (row->kind != XRT_VALUE_NULLABLE || !row->nullable_element || row->nullable_element >= type) return false;
        type = row->nullable_element;
    }
}
static bool rbc_value_state_group(const XrRbcModule *module, const XrRbcValueCode *code,
    const XrRbcFunction *function, XrRbcValueAccess access) {
    const XrtModuleLayoutSpec *state = module->state_spec;
    const XrtValueImage *image = xr_rbc_module_value_image(module);
    if (access.result || !rbc_value_scalar(code, function, access.source, XR_RBC_STATE) ||
        !rbc_value_inline(module, code, access.other, false) || !state || !state->slots || !state->values ||
        !state->values->types || access.index >= state->slot_count || !image->slots) return false;
    const XrtValueImageType *type = &image->types[rbc_value_home(code, access.other)->type - 1];
    if (type->kind != XRT_VALUE_TUPLE || !type->field_count || type->owned_count ||
        type->field_count > state->slot_count - access.index || type->first_field > image->slot_count ||
        type->field_count > image->slot_count - type->first_field || type->size % sizeof(int64_t) ||
        type->size / sizeof(int64_t) != type->field_count) return false;
    uint32_t owner = state->slots[access.index].module;
    for (size_t n = 0; n < type->field_count; ++n) {
        XrtModuleSlotSpec slot = state->slots[access.index + n];
        XrtValueImageSlot field = image->slots[type->first_field + n];
        if (slot.module != owner || !slot.type || slot.type > state->values->type_count || field.type != slot.type ||
            slot.type > image->type_count || field.name_length || field.offset != n * sizeof(int64_t) ||
            image->types[field.type - 1].kind != XRT_VALUE_I64 ||
            state->values->types[slot.type - 1].kind != XRT_VALUE_I64) return false;
    }
    return true;
}
static bool rbc_value_pod(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,uint32_t value,uint32_t result) {
    const XrLirValueHome *home=rbc_value_home(code,value);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (!home || !home->type || home->owner_count || !image || !image->types || home->type>image->type_count ||
        !result || result>module->aggregate_type_count || !module->aggregate_types ||
        !rbc_value_location_valid(function,code->locations[value]) ||
        code->locations[value].kind!=XR_RBC_VALUE_AGGREGATE) return false;
    XrtValueKind kind=image->types[home->type-1].kind;
    XrRbcAggregateValue aggregate=function->aggregate_values[code->locations[value].index];
    return aggregate.type==result-1 && (kind==XRT_VALUE_NULLABLE || kind==XRT_VALUE_FIXED_INT) &&
        home->size==module->aggregate_types[result-1].physical.size;
}
static bool rbc_value_inline_operand(const XrRbcModule *, const XrRbcValueCode *,
    const XrRbcFunction *, uint32_t value, uint32_t type, bool destination);
#include "xrbc_value_array_admission.inc.c"
#include "xrbc_value_map_admission.inc.c"
#include "xrbc_value_set_admission.inc.c"
#include "xrbc_value_slice_admission.inc.c"
static const XrLirHomeWorkspace *rbc_value_cell_stage(const XrRbcValueCode *code,XrLirHomeTransfer transfer) {
    const XrLirValueHomes *homes=code->homes;
    if (!homes || !homes->workspaces || transfer.borrowed_owner_workspace>=homes->workspace_count) return NULL;
    const XrLirHomeWorkspace *workspace=&homes->workspaces[transfer.borrowed_owner_workspace];
    const XrLirValueHome *source=rbc_value_home(code,transfer.source),*target=rbc_value_home(code,transfer.target);
    if (!source || !target || !source->parameter || workspace->kind!=XR_LIR_HOME_WORKSPACE_CELL_BORROW_STAGE ||
        workspace->type!=source->type || workspace->value!=transfer.source || !workspace->block || !workspace->instruction ||
        workspace->temporary_offset!=SIZE_MAX || workspace->temporary_first_owner!=SIZE_MAX ||
        !homes->frame.owned_offsets || workspace->first_owner>=homes->frame.slot_count ||
        workspace->first_owner==target->first_owner || workspace->first_owner==target->temporary_first_owner ||
        workspace->offset%_Alignof(XrtManagedHeader *) || workspace->offset>homes->frame.payload_size ||
        sizeof(XrtManagedHeader *)>homes->frame.payload_size-workspace->offset ||
        homes->frame.owned_offsets[workspace->first_owner]!=workspace->offset) return NULL;
    return workspace;
}
static const XrLirHomeWorkspace *rbc_value_header_stage(const XrRbcValueCode *code,XrLirHomeTransfer transfer) {
    const XrLirValueHomes *homes=code ? code->homes : NULL;
    if (!homes || !homes->workspaces || transfer.borrowed_owner_workspace>=homes->workspace_count) return NULL;
    const XrLirHomeWorkspace *workspace=&homes->workspaces[transfer.borrowed_owner_workspace];
    const XrLirValueHome *source=rbc_value_home(code,transfer.source),*target=rbc_value_home(code,transfer.target);
    if (!source || !target || !source->parameter || source->cell_transport || source->transport || source->view_storage ||
        source->owner_count!=1 || source->offset!=SIZE_MAX || source->first_owner!=SIZE_MAX ||
        source->temporary_offset!=SIZE_MAX || source->temporary_first_owner!=SIZE_MAX ||
        target->parameter || target->cell_transport || target->transport || target->view_storage || target->owner_count!=1 ||
        source->type!=target->type || target->offset==SIZE_MAX || target->temporary_offset==SIZE_MAX ||
        transfer.transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_COPY ||
        transfer.hold_transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE ||
        transfer.source_storage!=XR_LIR_SLICE_STORAGE_NONE || transfer.source_workspace || transfer.target_workspace ||
        workspace->kind!=XR_LIR_HOME_WORKSPACE_HEADER_BORROW_STAGE || workspace->type!=source->type ||
        workspace->value!=transfer.source || !workspace->block || !workspace->instruction ||
        workspace->temporary_offset!=SIZE_MAX || workspace->temporary_first_owner!=SIZE_MAX ||
        !homes->frame.owned_offsets || workspace->first_owner>=homes->frame.slot_count ||
        target->first_owner>=homes->frame.slot_count || target->temporary_first_owner>=homes->frame.slot_count ||
        workspace->first_owner==target->first_owner || workspace->first_owner==target->temporary_first_owner ||
        workspace->offset%_Alignof(XrtManagedHeader *) || workspace->offset>homes->frame.payload_size ||
        sizeof(XrtManagedHeader *)>homes->frame.payload_size-workspace->offset ||
        homes->frame.owned_offsets[workspace->first_owner]!=workspace->offset) return NULL;
    return workspace;
}
static bool rbc_value_edge_valid(const XrRbcModule *module,const XrRbcValueCode *code, XrRbcValueAccess access) {
    if (access.first > code->argument_count || access.count > (code->argument_count - access.first) / 2 ||
        (access.count && !code->arguments)) return false;
    const XrLirHomeEdge *edge = NULL;
    if (access.index != UINT32_MAX) {
        if (!code->homes || !code->homes->edges || access.index >= code->homes->edge_count) return false;
        edge = &code->homes->edges[access.index];
        if (edge->first > code->homes->transfer_count || edge->count > code->homes->transfer_count - edge->first ||
            (edge->count && !code->homes->transfers)) return false;
    }
    size_t typed = 0;
    for (uint32_t p = 0; p < access.count; ++p) {
        uint32_t source = code->arguments[access.first + (size_t)p * 2];
        uint32_t target = code->arguments[access.first + (size_t)p * 2 + 1];
        if (!rbc_value_id(code,source) || !rbc_value_id(code,target)) return false;
        const XrLirValueHome *from = rbc_value_home(code,source), *to = rbc_value_home(code,target);
        if (to && to->type) {
            if (!from || from->type != to->type || !edge || typed >= edge->count) return false;
            XrLirHomeTransfer transfer = code->homes->transfers[edge->first + typed++];
            if (transfer.source != source || transfer.target != target || to->parameter ||
                to->temporary_offset == SIZE_MAX) return false;
            if (from->parameter && rbc_value_set(module,code,target,true)) {
                if (!rbc_value_set(module,code,source,false)) return false;
                const XrLirHomeWorkspace *stage=rbc_value_header_stage(code,transfer);
                if (!stage || stage->block!=edge->block) return false;
                continue;
            }
            if (rbc_value_cell(module,code,target,true)) {
                if (!rbc_value_cell(module,code,source,false) || transfer.transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_COPY ||
                    transfer.source_storage!=XR_LIR_SLICE_STORAGE_NONE || transfer.hold_transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE || transfer.source_workspace || transfer.target_workspace)
                    return false;
                if (from->parameter) {
                    const XrLirHomeWorkspace *stage=rbc_value_cell_stage(code,transfer);
                    if (!stage || stage->block!=edge->block) return false;
                } else if (transfer.borrowed_owner_workspace!=SIZE_MAX) return false;
                continue;
            }
            if (rbc_value_slice(module,code,target)) {
                if (!rbc_value_slice(module,code,source) || transfer.transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_BORROW ||
                    transfer.hold_transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_COPY ||
                    transfer.source_workspace!=from->view_workspace || transfer.target_workspace!=to->view_workspace ||
                    transfer.source_storage!=from->view_storage) return false;
                if (from->view_storage==XR_LIR_SLICE_STORAGE_BORROWED_FORMAL) {
                    const XrLirHomeWorkspace *stage=rbc_value_slice_stage(code,transfer);
                    if (!stage || stage->block!=edge->block) return false;
                } else if (transfer.borrowed_owner_workspace!=SIZE_MAX) return false;
            } else if (transfer.borrowed_owner_workspace!=SIZE_MAX) return false;
            else if (transfer.source_storage!=XR_LIR_SLICE_STORAGE_NONE || transfer.hold_transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE ||
                transfer.source_workspace || transfer.target_workspace) return false;
            else if (to->transport!=XR_LIR_CALLABLE_TRANSPORT_NONE) {
                if (to->borrow_source || transfer.transfer_mode!=(from->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED ?
                        XR_LIR_OWNERSHIP_TRANSFER_COPY : XR_LIR_OWNERSHIP_TRANSFER_BORROW)) return false;
            } else if (transfer.transfer_mode!=(to->owner_count ?
                    XR_LIR_OWNERSHIP_TRANSFER_MOVE : XR_LIR_OWNERSHIP_TRANSFER_NONE)) return false;
        } else if (from && from->type) return false;
    }
    return typed == (edge ? edge->count : 0);
}
static bool rbc_value_array_fold_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    XrRbcValueAccess access) {
    if (!access.other) return true;
    const XrLirArrayRegionBindings *bindings=code->array_regions;
    if (!xr_rbc_module_code_owner(module) || !bindings || access.index>=bindings->edge_count ||
        !code->array_read_folds || access.other>bindings->region_count ||
        !code->homes || access.index>=code->homes->edge_count) return false;
    XrRbcArrayReadFold fold=code->array_read_folds[access.other-1];
    XrLirArrayRegionEdge edge=bindings->edges[access.index];
    if (!fold.accumulator || access.other!=edge.region) return false;
    const XrLirArrayRegionBinding *row=&bindings->regions[access.other-1];
    const XrLirArraySetRegionRange *range=&row->region.range;
    if (row->region.primary_kind!=XR_LIR_ARRAY_REGION_PRIMARY_GET ||
        row->owner_home.kind!=XR_LIR_ARRAY_SET_OWNER_HOME_READ_ONLY_WINDOW ||
        (range->kind!=XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN &&
            range->kind!=XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT) ||
        (range->kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN && (fold.constant || fold.limit)) ||
        (range->kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT &&
            (fold.constant ? fold.limit!=0 : !rbc_value_id(code,fold.limit)))) return false;
    if (edge.kind==XR_LIR_ARRAY_REGION_EDGE_ENTRY) return true;
    if (edge.kind!=XR_LIR_ARRAY_REGION_EDGE_LATCH || access.count<3 ||
        !code->homes->edges[access.index].direct_header_move) return false;
    bool accumulator=false, induction=false;
    for (uint32_t p=0;p<access.count;++p) {
        const uint32_t *pair=code->arguments+access.first+(size_t)p*2;
        const XrLirValueHome *target=rbc_value_home(code,pair[1]);
        if (!target || target->type) continue;
        accumulator|=pair[0]==fold.next_accumulator && pair[1]==fold.accumulator;
        induction|=pair[0]==range->latch_iv && pair[1]==range->iv;
    }
    return accumulator && induction && fold.accumulator!=range->iv;
}
static bool rbc_value_array_push_edge_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess access) {
    if (!access.window) return true;
    const XrLirArrayRegionBindings *bindings=code->array_push_windows;
    if (!xr_rbc_module_code_owner(module) || access.other || !bindings || access.index>=bindings->edge_count ||
        access.window>bindings->region_count || !code->array_push_limits) return false;
    XrLirArrayRegionEdge edge=bindings->edges[access.index];
    if (edge.region!=access.window ||
        (edge.kind!=XR_LIR_ARRAY_REGION_EDGE_ENTRY && edge.kind!=XR_LIR_ARRAY_REGION_EDGE_LATCH)) return false;
    const XrLirArrayRegionBinding *row=&bindings->regions[edge.region-1];
    const XrLirArrayPushCapacity *capacity=&row->region.push_capacity;
    const XrRbcArrayPushLimit *limit=&code->array_push_limits[edge.region-1];
    return row->slot_count && row->region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_PUSH &&
        capacity->kind==XR_LIR_ARRAY_PUSH_CAPACITY_UNIT_CURRENT && limit->available &&
        rbc_value_scalar(code,function,capacity->iv,XR_RBC_BITS) &&
        (!limit->value || (limit->value==capacity->end && rbc_value_scalar(code,function,limit->value,XR_RBC_BITS)));
}
static bool rbc_value_display_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess access) {
    if (access.other || !rbc_value_id(code,access.source) || !rbc_value_string(module,code,access.result,true)) return false;
    const XrLirValueHome *home=rbc_value_home(code,access.source);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (home && image && image->types && home->type && home->type <= image->type_count) {
        const XrtValueImageType *row = &image->types[home->type - 1];
        if (row->kind == XRT_VALUE_TUPLE)
            return !access.index && (row->flags & XRT_VALUE_INLINE_DISPLAY) &&
                rbc_value_inline(module, code, access.source, false);
    }
    if (access.index>=code->display_count || !code->displays) return false;
    const XrtPodDisplay *display=&code->displays[access.index];
    XrPodLayout expected={0};
    if (!xr_pod_scalar_layout(display->scalar,&expected)) return false;
    if (!display->nullable_depth)
        return (display->scalar==XR_POD_BOOL || display->scalar==XR_POD_U8 || display->scalar==XR_POD_U64 ||
                display->scalar==XR_POD_F32) &&
            rbc_value_scalar(code,function,access.source,XR_RBC_BITS) &&
            display->physical.size==expected.size && display->physical.alignment==expected.alignment &&
            display->physical.scalar_count==expected.scalar_count;
    if (!home || !home->type || home->owner_count || !image || !image->types || home->type>image->type_count ||
        display->nullable_depth>image->type_count || !rbc_value_location_valid(function,code->locations[access.source]) ||
        code->locations[access.source].kind!=XR_RBC_VALUE_AGGREGATE) return false;
    uint32_t type=home->type;
    for (uint32_t n=0;n<display->nullable_depth;++n) {
        const XrtValueImageType *row=&image->types[type-1];
        if (row->kind!=XRT_VALUE_NULLABLE || row->owned_count || !row->nullable_element || row->nullable_element>=type ||
            row->nullable_tag || row->nullable_payload!=image->types[row->nullable_element-1].alignment ||
            !xr_pod_nullable_layout(expected,&expected)) return false;
        type=row->nullable_element;
    }
    XrtValueKind leaf;
    if (!xr_pod_value_kind(display->scalar,&leaf) || image->types[type-1].kind!=leaf ||
        home->size!=expected.size || display->physical.size!=expected.size ||
        display->physical.alignment!=expected.alignment || display->physical.scalar_count!=expected.scalar_count) return false;
    XrRbcAggregateValue aggregate=function->aggregate_values[code->locations[access.source].index];
    uint32_t ordinal=aggregate.type;
    for (uint32_t n=0;n<display->nullable_depth;++n) {
        if (ordinal>=module->aggregate_type_count || !module->aggregate_types) return false;
        XrPodType packed=module->aggregate_types[ordinal];
        if (packed.kind!=XR_POD_NULLABLE) return false;
        if (n+1==display->nullable_depth) return packed.element==(uint32_t)display->scalar;
        if (packed.element<XR_POD_SCALAR_COUNT || packed.element-XR_POD_SCALAR_COUNT>=ordinal) return false;
        ordinal=packed.element-XR_POD_SCALAR_COUNT;
    }
    return false;
}
static bool rbc_value_try_parse_valid(const XrRbcModule *module, const XrRbcValueCode *code,
    const XrRbcFunction *function, XrRbcValueAccess access, XrtValueKind kind, XrPodScalar pod) {
    if (!xr_rbc_module_code_owner(module) || access.other || access.index ||
        !rbc_value_string_borrow_operand(module, code, access.source)) return false;
    const XrLirValueHome *home = rbc_value_home(code, access.result);
    const XrtValueImage *image = xr_rbc_module_value_image(module);
    if (!home || !home->type || home->parameter || home->owner_count ||
        !image || !image->types || home->type > image->type_count) return false;
    XrRbcValueLocation location = code->locations[access.result];
    if (location.kind != XR_RBC_VALUE_AGGREGATE || !rbc_value_location_valid(function, location) ||
        !function->aggregate_values || !module->aggregate_types) return false;
    const XrtValueImageType *result = &image->types[home->type - 1];
    if (result->kind != XRT_VALUE_NULLABLE || result->owned_count ||
        !result->nullable_element || result->nullable_element >= home->type || result->nullable_tag) return false;
    const XrtValueImageType *element = &image->types[result->nullable_element - 1];
    uint32_t ordinal = function->aggregate_values[location.index].type;
    if (ordinal >= module->aggregate_type_count || element->kind != kind) return false;
    XrPodType packed = module->aggregate_types[ordinal];
    XrPodLayout scalar, expected;
    return xr_pod_scalar_layout(pod, &scalar) && xr_pod_nullable_layout(scalar, &expected) &&
        packed.kind == XR_POD_NULLABLE && packed.element == pod &&
        packed.physical.size == expected.size && packed.physical.alignment == expected.alignment &&
        packed.physical.scalar_count == expected.scalar_count && home->size == expected.size &&
        result->size == expected.size && result->alignment == expected.alignment &&
        result->nullable_payload == scalar.alignment;
}
#include "xrbc_value_inline_admission.inc.c"
#include "xrbc_value_callable_admission.inc.c"
#include "xrbc_value_cell_admission.inc.c"
static bool rbc_value_string_append_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    XrRbcValueAccess access) {
    const XrLirHomeEvent *transfer=rbc_value_event(code,access.index,XR_LIR_OWNERSHIP_APPEND_TRANSFER);
    if (!xr_rbc_module_code_owner(module) || !transfer ||
        !transfer->block || !transfer->instruction ||
        transfer->source!=access.source || transfer->argument || transfer->target || transfer->edge || transfer->target_block ||
        transfer->transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_MOVE ||
        !rbc_value_string(module,code,access.source,false) ||
        !rbc_value_string_borrow_operand(module,code,access.other) ||
        !rbc_value_string(module,code,access.result,true)) return false;
    const XrLirValueHome *source=rbc_value_home(code,access.source),*result=rbc_value_home(code,access.result);
    size_t slots[]={source->first_owner,result->first_owner,result->temporary_first_owner};
    if (source->parameter || source->offset==SIZE_MAX || !code->homes->frame.owned_offsets) return false;
    for (size_t n=0;n<3;++n) {
        if (slots[n]>=code->homes->frame.slot_count) return false;
        for (size_t p=0;p<n;++p) if (slots[n]==slots[p]) return false;
    }
    if (code->homes->frame.owned_offsets[slots[0]]!=source->offset ||
        code->homes->frame.owned_offsets[slots[1]]!=result->offset ||
        code->homes->frame.owned_offsets[slots[2]]!=result->temporary_offset) return false;
    bool borrowed=false,owned=false;
    for (size_t n=0;n<code->homes->event_count;++n) {
        const XrLirHomeEvent *event=&code->homes->events[n];
        if (event->block!=transfer->block || event->instruction!=transfer->instruction) continue;
        if (event->target || event->edge || event->target_block) continue;
        if (event->kind==XR_LIR_OWNERSHIP_OPERAND_BORROW && event->source==access.other && event->argument==1 &&
            event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW) borrowed=true;
        if (event->kind==XR_LIR_OWNERSHIP_RESULT_OWNER && event->source==access.result && !event->argument &&
            event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE) owned=true;
    }
    return borrowed && owned;
}
static bool rbc_value_typed_error_op(XrRbcValueOperation operation) {
    return operation==XR_RBC_VALUE_INVOKE || operation==XR_RBC_VALUE_INVOKE_RESULT ||
        operation==XR_RBC_VALUE_INVOKE_ERROR || operation==XR_RBC_VALUE_RETURN_ERROR ||
        operation==XR_RBC_VALUE_PANIC_ERROR || operation==XR_RBC_VALUE_UNCAUGHT_ERROR;
}

static bool rbc_error_home(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,
    uint32_t type,bool output) {
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    const XrLirValueHome *home=rbc_value_home(code,value);
    return image && image->types && type && type<=image->type_count && home && home->type==type &&
        image->types[type-1].kind==XRT_VALUE_SUM && !image->types[type-1].owned_count &&
        rbc_value_inline(module,code,value,output);
}

static bool rbc_invoke_result_home(const XrRbcModule *module,const XrRbcFunction *caller,
    const XrRbcValueCode *code,const XrRbcFunction *target,const XrRbcValueCode *callee,uint32_t result) {
    uint32_t aggregate=target->aggregate_result_type;
    if (target->result_kind==XR_RBC_STRING)
        return !callee->error_unit && !aggregate && rbc_value_string_destination(module,code,result,false) &&
            rbc_value_home(code,result)->type==rbc_result_value_type(callee);
    XrtValueKind kind=rbc_value_kind(module,code,result);
    if (xrt_value_kind_is_array(kind)) {
        uint32_t family=xr_rbc_array_family(target->result_kind);
        return !callee->error_unit && !aggregate && rbc_value_array(module,code,result,true) &&
            rbc_value_home(code,result)->type==rbc_result_value_type(callee) &&
            (family < XR_RBC_ARRAY_FAMILY_COUNT ? kind==(XrtValueKind)(XRT_VALUE_ARRAY_I64+family) :
                target->result_kind==XR_RBC_BITS && kind==XRT_VALUE_ARRAY_INLINE);
    }
    if (target->result_kind!=XR_RBC_BITS) return false;
    if (callee->error_unit) return !aggregate && !result;
    if (!aggregate) return rbc_value_scalar(code,caller,result,XR_RBC_BITS);
    if (!rbc_value_pod(module,code,caller,result,aggregate)) return false;
    const XrLirValueHome *home=rbc_value_home(code,result);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    const XrtValueImageType *type=&image->types[home->type-1];
    return !home->parameter && home->type==rbc_result_value_type(callee) &&
        type->kind==XRT_VALUE_NULLABLE && !type->owned_count;
}

static bool rbc_invoke_receivers(const XrRbcFunction *function,const XrRbcValueCode *code,size_t pc,
    uint32_t ordinal,XrRbcValueAccess access) {
    uint32_t stride=2+access.source;
    if (access.source>1 || function->code_count-pc<3+2*stride ||
        function->code[pc+1] != (XR_RBC_EQI | (access.window<<8)) ||
        function->code[pc+2] != (XR_RBC_JMP | (stride<<8))) return false;
    for (uint32_t n=0;n<2;++n) {
        size_t site=pc+3+stride*n;
        uint32_t word=function->code[site];
        if ((word&65535u)!=XR_RBC_VALUE_ACCESS || (word>>16)>=code->access_count ||
            (function->code[site+stride-1]&255u)!=XR_RBC_JMP) return false;
        XrRbcValueAccess receiver=code->accesses[word>>16];
        if (receiver.operation!=(n ? XR_RBC_VALUE_INVOKE_RESULT : XR_RBC_VALUE_INVOKE_ERROR) ||
            receiver.index!=ordinal || receiver.result!=(n ? access.result : access.other)) return false;
        if (access.source) {
            uint32_t transfer=function->code[site+1];
            if ((transfer&65535u)!=XR_RBC_VALUE_ACCESS || (transfer>>16)>=code->access_count ||
                code->accesses[transfer>>16].operation!=XR_RBC_VALUE_EDGE) return false;
        }
    }
    return true;
}

XR_FUNC bool xr_rbc_value_instruction_valid(const XrRbcModule *module,
    const XrRbcFunction *function, size_t pc) {
    if (!module || !function || !function->code || pc>=function->code_count) return false;
    uint32_t word=function->code[pc];
    XrRbcOp op = (XrRbcOp)(word & 255u);
    if ((word >> 8) & 255) return false;
    const XrRbcValueCode *code = rbc_value_code(module,function);
    size_t index = word >> 16;
    if (!code || !code->accesses || index >= code->access_count || code->access_count > 65536 ||
        !code->locations || (code->homes && code->homes->value_count != code->value_count)) return false;
    XrRbcValueAccess a = code->accesses[index];
    if ((code->error_type || rbc_value_typed_error_op(a.operation)) && !xr_rbc_module_code_owner(module))
        return false;
    XrRbcOp access_op = rbc_value_access_opcode(code, a);
    if (op == XR_RBC_STRING_LITERAL_MAP_READ) {
        if (access_op != XR_RBC_STRING_LITERAL_BORROW || function->code_count - pc < 2 ||
            !rbc_literal_map_pair(code, word, function->code[pc + 1])) return false;
    } else if (op != access_op) return false;
    if (op != XR_RBC_VALUE_ACCESS && (!xr_rbc_module_code_owner(module) ||
        (code->array_region_accesses && code->array_region_accesses[index].region))) return false;
    if (a.operation!=XR_RBC_VALUE_CALL && a.operation!=XR_RBC_VALUE_INVOKE && a.operation!=XR_RBC_VALUE_EDGE && a.operation!=XR_RBC_VALUE_ARRAY &&
        a.operation!=XR_RBC_VALUE_MAP && a.operation!=XR_RBC_VALUE_SET && a.operation!=XR_RBC_VALUE_MAP_LITERAL && a.operation!=XR_RBC_VALUE_RECORD_NEW && a.operation!=XR_RBC_VALUE_CLASS_NEW &&
        a.operation!=XR_RBC_VALUE_BINDING_NEW && a.operation!=XR_RBC_VALUE_BINDING_WRITE &&
        a.operation!=XR_RBC_VALUE_STRING_APPEND_BINDING &&
        a.operation!=XR_RBC_VALUE_FUNCTION_NEW && a.operation!=XR_RBC_VALUE_FUNCTION_CALL &&
        a.operation!=XR_RBC_VALUE_FUNCTION_ALIAS && a.operation!=XR_RBC_VALUE_SLICE &&
        a.operation!=XR_RBC_VALUE_SUM_NEW && a.operation!=XR_RBC_VALUE_HOST_FILE_WRITE &&
        a.operation!=XR_RBC_VALUE_CLASS_ARRAY_PUSH && a.operation!=XR_RBC_VALUE_STRING_REPLACE &&
        (a.first || a.count || a.window)) return false;
    if (a.operation!=XR_RBC_VALUE_STRING_LITERAL && a.operation!=XR_RBC_VALUE_STATE_CONSTRUCT &&
        a.operation!=XR_RBC_VALUE_STATE_INITIALIZE && a.operation!=XR_RBC_VALUE_STATE_GROUP_INITIALIZE &&
        a.operation!=XR_RBC_VALUE_CALL &&
        a.operation!=XR_RBC_VALUE_EDGE && a.operation!=XR_RBC_VALUE_ARRAY && a.operation!=XR_RBC_VALUE_MAP &&
        a.operation!=XR_RBC_VALUE_DISPLAY && a.operation!=XR_RBC_VALUE_RECORD_FIELD && a.operation!=XR_RBC_VALUE_CLASS_FIELD &&
        a.operation!=XR_RBC_VALUE_CLASS_IS &&
        a.operation!=XR_RBC_VALUE_SUM_NEW && a.operation!=XR_RBC_VALUE_SUM_GET &&
        a.operation!=XR_RBC_VALUE_RETURN &&
        !(a.operation>=XR_RBC_VALUE_BINDING_NEW && a.operation<=XR_RBC_VALUE_BINDING_DROP) &&
        (a.operation<XR_RBC_VALUE_FUNCTION_NEW && a.operation!=XR_RBC_VALUE_STRING_REPLACE) && a.index) return false;
    switch (a.operation) {
    case XR_RBC_VALUE_CELL: return rbc_value_cell_access_valid(module,code,function,a);
    case XR_RBC_VALUE_STRING_APPEND_OWNED: return rbc_value_string_append_valid(module,code,a);
    case XR_RBC_VALUE_STRING_APPEND_BINDING: return rbc_value_string_binding_append_valid(module,code,function,a);
    case XR_RBC_VALUE_SLICE: return rbc_value_slice_access_valid(module,code,function,a);
    case XR_RBC_VALUE_FUNCTION_NEW: case XR_RBC_VALUE_FUNCTION_CALL:
    case XR_RBC_VALUE_FUNCTION_ALIAS: case XR_RBC_VALUE_FUNCTION_SCOPE_EXIT:
        return rbc_value_function_access_valid(module,code,function,a);
    case XR_RBC_VALUE_STRING_LITERAL:
        return !a.source && !a.other && a.index < module->literal_count && rbc_value_string_destination(module,code,a.result,true);
    case XR_RBC_VALUE_STRING_I64: case XR_RBC_VALUE_STRING_F64:
        return !a.other && rbc_value_string(module,code,a.result,true) && rbc_value_scalar(code,function,a.source,XR_RBC_BITS);
    case XR_RBC_VALUE_STRING_CONCAT:
        return rbc_value_string(module,code,a.result,true) && rbc_value_string(module,code,a.source,false) &&
            rbc_value_string(module,code,a.other,false);
    case XR_RBC_VALUE_STRING_REPLACE:
        return a.first <= 1 && !a.count && !a.window && rbc_value_string(module,code,a.result,true) &&
            rbc_value_string(module,code,a.source,false) && rbc_value_string(module,code,a.other,false) &&
            rbc_value_string(module,code,a.index,false);
    case XR_RBC_VALUE_STRING_SLICE:
        return !a.first && !a.count && !a.window && rbc_value_string(module,code,a.result,true) &&
            rbc_value_string(module,code,a.source,false) && rbc_value_scalar(code,function,a.other,XR_RBC_BITS) &&
            rbc_value_scalar(code,function,a.index,XR_RBC_BITS);
    case XR_RBC_VALUE_COPY: case XR_RBC_VALUE_MOVE:
        return !a.other && !rbc_value_function(module,code,a.result,false) && !rbc_value_cell(module,code,a.result,false) &&
            rbc_value_owner(module,code,a.result,true) && rbc_value_owner(module,code,a.source,false) &&
            rbc_value_home(code,a.result)->type==rbc_value_home(code,a.source)->type && (a.operation == XR_RBC_VALUE_COPY || !rbc_value_home(code,a.source)->parameter);
    case XR_RBC_VALUE_CLASS_ADOPT: {
        const XrtValueImage *image=xr_rbc_module_value_image(module);
        const XrLirValueHome *destination=rbc_value_home(code,a.result),*from=rbc_value_home(code,a.source);
        return !a.other && !a.index && image && image->types && destination && from &&
            destination->type && from->type && destination->type<=image->type_count && from->type<=image->type_count &&
            !image->types[destination->type-1].class_storage && !image->types[from->type-1].class_storage &&
            image->types[destination->type-1].kind==XRT_VALUE_CLASS && image->types[from->type-1].kind==XRT_VALUE_CLASS &&
            rbc_value_inline(module,code,a.result,true) && rbc_value_inline(module,code,a.source,false);
    }
    case XR_RBC_VALUE_CLASS_IS: {
        const XrtValueImage *image=xr_rbc_module_value_image(module);
        const XrLirValueHome *from=rbc_value_home(code,a.source);
        return !a.other && image && image->types && from && from->type && from->type<=image->type_count &&
            image->types[from->type-1].kind==XRT_VALUE_CLASS && a.index && a.index<=image->type_count &&
            image->types[a.index-1].kind==XRT_VALUE_CLASS &&
            rbc_value_inline(module,code,a.source,false) && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    }
    case XR_RBC_VALUE_DROP:
        return !a.result && !a.other && !rbc_value_cell(module,code,a.source,false) && rbc_value_owner(module,code,a.source,false) &&
            !rbc_value_home(code,a.source)->parameter &&
            (!rbc_value_function(module,code,a.source,false) || rbc_value_home(code,a.source)->owner_count==1);
    case XR_RBC_VALUE_STRING_PRINT:
        return !a.result && !a.other && rbc_value_string(module,code,a.source,false);
    case XR_RBC_VALUE_ASSERT_CONDITION:
        return !a.result && !a.index && rbc_value_scalar(code,function,a.source,XR_RBC_BITS) &&
            rbc_value_string(module,code,a.other,false);
    case XR_RBC_VALUE_STRING_EQUAL: case XR_RBC_VALUE_STRING_NOT_EQUAL:
        return !a.index && rbc_value_string(module,code,a.source,false) &&
            rbc_value_string(module,code,a.other,false) &&
            rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    case XR_RBC_VALUE_STRING_LENGTH:
        return !a.other && rbc_value_string(module,code,a.source,false) && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    case XR_RBC_VALUE_STRING_BYTE_LENGTH:
        return !a.other && !a.index && rbc_value_string_borrow_operand(module,code,a.source) &&
            rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    case XR_RBC_VALUE_STRING_INDEX_OF:
        return !a.first && !a.count && !a.window && rbc_value_string(module,code,a.source,false) &&
            rbc_value_string(module,code,a.other,false) && rbc_value_scalar(code,function,a.index,XR_RBC_BITS) &&
            rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    case XR_RBC_VALUE_STRING_ORDER:
        return a.index >= XR_LIR_LT && a.index <= XR_LIR_GE && !a.first && !a.count && !a.window &&
            rbc_value_string(module,code,a.source,false) && rbc_value_string(module,code,a.other,false) &&
            rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    case XR_RBC_VALUE_STRING_LAST_INDEX_OF:
        return !a.index && !a.first && !a.count && !a.window &&
            rbc_value_string(module,code,a.source,false) && rbc_value_string(module,code,a.other,false) &&
            rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    case XR_RBC_VALUE_STRING_TEST:
        return a.index <= 2 && !a.first && !a.count && !a.window &&
            rbc_value_string(module,code,a.source,false) && rbc_value_string(module,code,a.other,false) &&
            rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    case XR_RBC_VALUE_HOST_PATH_BOOL: {
        uint32_t paths = xr_native_blocking_path_count(a.index);
        return paths == (a.other ? 2u : 1u) && rbc_value_string(module,code,a.source,false) &&
            (!a.other || rbc_value_string(module,code,a.other,false)) &&
            rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    }
    case XR_RBC_VALUE_HOST_FILE_OPEN_WRITE:
        return xr_native_host_shape(a.index) == XR_NATIVE_HOST_FILE_OPEN_WRITE &&
            rbc_value_string(module,code,a.source,false) && rbc_value_scalar(code,function,a.other,XR_RBC_BITS) &&
            rbc_value_file_storage(module,code,a.result,true);
    case XR_RBC_VALUE_HOST_PATH_CLASS: {
        uint32_t storage = xr_native_host_path_result_class(a.index);
        const XrtValueImageType *type = rbc_value_host_nullable_element(module, code, a.result);
        return !a.other && storage && type && type->kind == XRT_VALUE_CLASS && type->class_storage == storage &&
            rbc_value_string(module, code, a.source, false);
    }
    case XR_RBC_VALUE_HOST_FILE_READ:
        return xr_native_host_shape(a.index) == XR_NATIVE_HOST_FILE_READ &&
            rbc_value_file_storage(module, code, a.source, false) && rbc_value_scalar(code, function, a.other, XR_RBC_BITS) &&
            rbc_value_host_nullable_backing(module, code, a.result, XRT_VALUE_ARRAY_U8);
    case XR_RBC_VALUE_HOST_ENV_GET:
        return !a.other && xr_native_host_shape(a.index) == XR_NATIVE_HOST_ENV_GET &&
            rbc_value_string(module, code, a.source, false) &&
            rbc_value_host_nullable_backing(module, code, a.result, XRT_VALUE_STRING);
    case XR_RBC_VALUE_HOST_TEXT_RESULT:
        return !a.other && xr_native_host_shape(a.index) == XR_NATIVE_HOST_TEXT_RESULT &&
            rbc_value_string(module, code, a.source, false) &&
            rbc_value_host_nullable_backing(module, code, a.result, XRT_VALUE_STRING);
    case XR_RBC_VALUE_HOST_DIRECTORY:
        if (a.other || xr_native_host_shape(a.index) != XR_NATIVE_HOST_DIRECTORY ||
            !xr_rbc_module_code_owner(module)) return false;
        switch (a.index) {
        case XR_NATIVE_CALLABLE_IO_DIR_OPEN:
            return rbc_value_string(module, code, a.source, false) &&
                rbc_value_scalar(code, function, a.result, XR_RBC_BITS);
        case XR_NATIVE_CALLABLE_IO_DIR_NEXT:
            return rbc_value_scalar(code, function, a.source, XR_RBC_BITS) &&
                rbc_value_host_nullable_backing(module, code, a.result, XRT_VALUE_STRING);
        case XR_NATIVE_CALLABLE_IO_DIR_CLOSE:
            return rbc_value_scalar(code, function, a.source, XR_RBC_BITS) &&
                rbc_value_scalar(code, function, a.result, XR_RBC_BITS);
        default: return false;
        }
    case XR_RBC_VALUE_HOST_DIRECTORY_CLEANUP_CLOSE:
    case XR_RBC_VALUE_HOST_DIRECTORY_CLEANUP_POLL:
        return xr_rbc_module_code_owner(module) && function->source_ids &&
            function->source_id_count == function->code_count && function->source_ids[pc] &&
            !a.result && !a.other && !a.index &&
            (a.operation == XR_RBC_VALUE_HOST_DIRECTORY_CLEANUP_CLOSE ?
                rbc_value_scalar(code, function, a.source, XR_RBC_BITS) : !a.source);
    case XR_RBC_VALUE_HOST_FILE_CLOSE:
        return !a.other && xr_native_host_shape(a.index) == XR_NATIVE_HOST_FILE_CLOSE &&
            rbc_value_file_storage(module,code,a.source,false) && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    case XR_RBC_VALUE_HOST_FILE_WRITE: {
        if (a.count != 1 || a.window || a.first >= code->argument_count || !code->arguments ||
            !rbc_value_file_storage(module, code, a.source, false) ||
            !rbc_value_scalar(code, function, code->arguments[a.first], XR_RBC_BITS) ||
            !rbc_value_scalar(code, function, a.result, XR_RBC_BITS)) return false;
        XrNativeHostShape shape = xr_native_host_shape(a.index);
        return (shape == XR_NATIVE_HOST_FILE_WRITE_STRING && rbc_value_string_borrow_operand(module, code, a.other)) ||
            (shape == XR_NATIVE_HOST_FILE_WRITE_BYTES && rbc_value_array(module, code, a.other, false) &&
                rbc_value_kind(module, code, a.other) == XRT_VALUE_ARRAY_U8);
    }
    case XR_RBC_VALUE_BOOL_TRY_PARSE:
        return rbc_value_try_parse_valid(module,code,function,a,XRT_VALUE_BOOL,XR_POD_BOOL);
    case XR_RBC_VALUE_I64_TRY_PARSE:
        return rbc_value_try_parse_valid(module,code,function,a,XRT_VALUE_I64,XR_POD_I64);
    case XR_RBC_VALUE_F64_TRY_PARSE:
        return rbc_value_try_parse_valid(module,code,function,a,XRT_VALUE_F64,XR_POD_F64);
    case XR_RBC_VALUE_STATE_READ:
        return !a.other && rbc_value_scalar(code,function,a.source,XR_RBC_STATE_REF) &&
            rbc_value_owner(module,code,a.result,true);
    case XR_RBC_VALUE_STATE_WRITE:
        return !a.result && rbc_value_scalar(code,function,a.source,XR_RBC_STATE_REF) &&
            rbc_value_owner(module,code,a.other,false);
    case XR_RBC_VALUE_STATE_GROUP_INITIALIZE:
        return rbc_value_state_group(module,code,function,a);
    case XR_RBC_VALUE_STATE_CONSTRUCT: case XR_RBC_VALUE_STATE_INITIALIZE: {
        const XrtModuleLayoutSpec *spec = module->state_spec;
        if (!spec || !spec->values || !spec->values->types || !spec->slots || a.index >= spec->slot_count ||
            !rbc_value_scalar(code,function,a.source,XR_RBC_STATE) || !rbc_value_owner(module,code,a.other,false)) return false;
        XrtModuleSlotSpec slot = spec->slots[a.index];
        if (!slot.type || slot.type > spec->values->type_count || slot.type != rbc_value_home(code,a.other)->type)
            return false;
        return a.operation == XR_RBC_VALUE_STATE_INITIALIZE ? !a.result && !(slot.flags & XRT_MODULE_SLOT_MUTABLE) :
            (slot.flags & XRT_MODULE_SLOT_MUTABLE) && rbc_value_scalar(code,function,a.result,XR_RBC_STATE_REF);
    }
    case XR_RBC_VALUE_CALL: case XR_RBC_VALUE_INVOKE: {
        if (a.index >= module->function_count || a.first > code->argument_count ||
            a.count > code->argument_count - a.first || (a.count && !code->arguments) || a.window >= function->register_count)
            return false;
        for (uint32_t p = 0; p < a.count; ++p) if (!rbc_value_id(code,code->arguments[a.first + p])) return false;
        const XrRbcValueCode *callee = xr_rbc_module_value_code(module,a.index);
        const XrRbcFunction *target=&module->functions[a.index];
        bool invoke=a.operation==XR_RBC_VALUE_INVOKE;
        if (invoke != (callee && callee->error_type!=0)) return false;
        if (target->parameter_count>function->register_count-a.window-1 ||
            (callee && (callee->parameter_count!=a.count || (a.count && !callee->parameters))) ||
            (!callee && target->parameter_count!=a.count)) return false;
        for (uint32_t p=0;p<a.count;++p) {
            uint32_t source=code->arguments[a.first+p];
            const XrLirValueHome *from=rbc_value_home(code,source),*formal=NULL;
            XrRbcValueLocation location={XR_RBC_VALUE_REGISTER,p};
            if (callee) {
                if (!rbc_value_id(callee,callee->parameters[p])) return false;
                formal=rbc_value_home(callee,callee->parameters[p]); location=callee->locations[callee->parameters[p]];
            }
            const XrLirHomeBinding *binding=rbc_value_binding(callee,callee ? callee->parameters[p] : 0);
            if (rbc_value_i64_ref(module,callee,callee ? callee->parameters[p] : 0)) {
                if (binding->parameter != p+1 || (!rbc_value_i64_ref(module,code,source) &&
                    !rbc_value_scalar(code,function,source,XR_RBC_I64_BINDING) &&
                    !rbc_value_scalar(code,function,source,XR_RBC_STATE_REF))) return false;
            } else if (binding && !rbc_value_binding_state_cap(callee,callee->parameters[p])) {
                const XrLirHomeBinding *actual=rbc_value_binding(code,source);
                if (!actual || binding->kind!=XR_LIR_HOME_BINDING_PARAMETER || binding->parameter!=p+1 ||
                    binding->type!=actual->type || !rbc_value_binding_valid(module,code,function,source)) return false;
            } else if (formal && formal->type) {
                if (!rbc_value_formal_accepts(module,callee,callee->parameters[p],code,source) ||
                    formal->parameter!=p+1) return false;
                if (formal->owner_count && (!rbc_value_owner(module,code,source,false) ||
                    !rbc_value_owner(module,callee,callee->parameters[p],false))) return false;
            } else {
                if (from && from->type) return false;
                XrRbcRegisterKind kind=location.kind==XR_RBC_VALUE_REGISTER ? rbc_register_kind(target,location.index) : XR_RBC_BITS;
                if (!rbc_value_scalar(code,function,source,kind)) return false;
            }
        }
        if (invoke)
            return a.result!=a.other && rbc_register_kind(function,a.window)==XR_RBC_CALL_WINDOW &&
                rbc_error_home(module,code,a.other,callee->error_type,true) &&
                rbc_invoke_result_home(module,function,code,target,callee,a.result) &&
                rbc_invoke_receivers(function,code,pc,(uint32_t)index,a);
        uint32_t result_type = rbc_result_value_type(callee);
        if (rbc_callable_home_type(xr_rbc_module_value_image(module), result_type))
            return !a.other && rbc_value_exact_home(module, code, a.result, result_type, true);
        if (callee && rbc_value_slice(module,code,a.result))
            return rbc_value_slice_call(module,code,callee,a)!=NULL;
        if (a.other) return false;
        if (callee && rbc_value_inline(module,code,a.result,true))
            return result_type==rbc_value_home(code,a.result)->type;
        if (target->aggregate_result_type)
            return callee && rbc_value_pod(module,code,function,a.result,target->aggregate_result_type) &&
                callee->homes->result_type==rbc_value_home(code,a.result)->type;
        return rbc_managed_kind(target->result_kind) ? callee && rbc_value_owner(module,code,a.result,true) &&
            callee->homes->result_type==rbc_value_home(code,a.result)->type :
            target->result_kind==XR_RBC_BITS && (!a.result || rbc_value_scalar(code,function,a.result,XR_RBC_BITS));
    }
    case XR_RBC_VALUE_INVOKE_RESULT: case XR_RBC_VALUE_INVOKE_ERROR: {
        bool failed=a.operation==XR_RBC_VALUE_INVOKE_ERROR;
        if (a.source || a.other || a.index>=code->access_count) return false;
        XrRbcValueAccess origin=code->accesses[a.index];
        if (origin.operation!=XR_RBC_VALUE_INVOKE || origin.source>1) return false;
        size_t distance=failed ? 3u : 5u+origin.source;
        if (pc<distance || function->code[pc-distance]!=(XR_RBC_VALUE_ACCESS | (a.index<<16))) return false;
        return a.result==(failed ? origin.other : origin.result) &&
            rbc_invoke_receivers(function,code,pc-distance,a.index,origin);
    }
    case XR_RBC_VALUE_RETURN_ERROR:
        return !a.result && !a.other && !a.index && rbc_error_home(module,code,a.source,code->error_type,false);
    case XR_RBC_VALUE_PANIC_ERROR: case XR_RBC_VALUE_UNCAUGHT_ERROR: {
        if (a.operation==XR_RBC_VALUE_UNCAUGHT_ERROR) {
            const XrRbcCodeOwner *owner=xr_rbc_module_code_owner(module);
            if (!owner || !owner->startup_function || owner->startup_function>module->function_count ||
                function!=module->functions+owner->startup_function-1 || code->error_type) return false;
        }
        const XrLirValueHome *home=rbc_value_home(code,a.source);
        const XrtValueImage *image=xr_rbc_module_value_image(module);
        if (a.result || !home || !rbc_error_home(module,code,a.source,home->type,false) ||
            !a.other || a.index>=module->literal_count || !module->literals || !image) return false;
        const XrRbcLiteral *metadata=&module->literals[a.index];
        return metadata->bytes && metadata->length>=12 &&
            xr_error_metadata_u32((const uint8_t *)metadata->bytes)==XR_ERROR_METADATA_VERSION &&
            xr_error_metadata_u32((const uint8_t *)metadata->bytes+4)==a.other &&
            xr_error_metadata_u32((const uint8_t *)metadata->bytes+8)==image->types[home->type-1].size;
    }
    case XR_RBC_VALUE_RECORD_NEW: case XR_RBC_VALUE_RECORD_FIELD: case XR_RBC_VALUE_CLASS_NEW: case XR_RBC_VALUE_CLASS_FIELD:
    case XR_RBC_VALUE_CLASS_SCALAR_WRITE: case XR_RBC_VALUE_CLASS_STRING_WRITE:
    case XR_RBC_VALUE_CLASS_ARRAY_WRITE: case XR_RBC_VALUE_CLASS_ARRAY_PUSH:
    case XR_RBC_VALUE_NULLABLE_NONE: case XR_RBC_VALUE_NULLABLE_SOME:
    case XR_RBC_VALUE_NULLABLE_IS_SOME: case XR_RBC_VALUE_NULLABLE_UNWRAP:
    case XR_RBC_VALUE_SUM_NEW: case XR_RBC_VALUE_SUM_TAG: case XR_RBC_VALUE_SUM_GET:
        return rbc_value_inline_access_valid(module,code,function,a);
    case XR_RBC_VALUE_RETURN: {
        if (a.result || a.other) return false;
        uint32_t result_type = rbc_result_value_type(code);
        const XrtValueImage *image = xr_rbc_module_value_image(module);
        if (rbc_callable_home_type(image, result_type))
            return !a.index && rbc_value_exact_home(module, code, a.source, result_type, false);
        if (rbc_value_slice(module,code,a.source)) return rbc_value_slice_return_valid(module,code,a);
        if (rbc_value_function(module,code,a.source,false) || rbc_value_cell(module,code,a.source,false)) {
            const XrLirHomeEvent *event=rbc_value_event(code,a.index,XR_LIR_OWNERSHIP_RETURN_TRANSFER);
            return event && event->source==a.source && code->homes->result_type==rbc_value_home(code,a.source)->type &&
                (!rbc_value_cell(module,code,a.source,false) || rbc_value_cell_event(module,code,event,a.source)) &&
                (event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY || event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE);
        }
        if (a.index) return false;
        if (function->aggregate_result_type)
            return rbc_value_pod(module,code,function,a.source,function->aggregate_result_type) &&
                result_type==rbc_value_home(code,a.source)->type;
        if (rbc_value_inline(module,code,a.source,false))
            return result_type == rbc_value_home(code,a.source)->type;
        if (rbc_inline_tuple_result(image, result_type)) return false;
        return !a.source || rbc_value_owner(module,code,a.source,false) || rbc_value_scalar(code,function,a.source,XR_RBC_BITS);
    }
    case XR_RBC_VALUE_ARRAY: return rbc_value_array_access_valid(module,code,function,a);
    case XR_RBC_VALUE_MAP: return rbc_value_map_access_valid(module,code,function,a);
    case XR_RBC_VALUE_SET: return rbc_value_set_access_valid(module,code,function,a);
    case XR_RBC_VALUE_MAP_LITERAL: return rbc_value_map_literal_valid(module,code,function,a);
    case XR_RBC_VALUE_DISPLAY: return rbc_value_display_valid(module,code,function,a);
    case XR_RBC_VALUE_BINDING_NEW: case XR_RBC_VALUE_BINDING_READ:
    case XR_RBC_VALUE_BINDING_WRITE: case XR_RBC_VALUE_BINDING_DROP:
        return rbc_value_binding_access_valid(module,code,function,a) &&
            (!a.index || !code->homes->events[a.index-1].forwarding_mode ||
             rbc_value_binding_forwarding_words(function,code,(uint32_t)index,pc));
    case XR_RBC_VALUE_EDGE:
        return !a.result && !a.source && rbc_value_edge_valid(module,code,a) &&
            rbc_value_array_fold_valid(module,code,a) && rbc_value_array_push_edge_valid(module,code,function,a);
    default: return false;
    }
}
static bool xr_rbc_value_return_word(const XrRbcModule *module, const XrRbcFunction *function, uint32_t word) {
    const XrRbcValueCode *code = rbc_value_code(module,function);
    if ((word&65535u)!=XR_RBC_VALUE_ACCESS || !code || !code->accesses || (word>>16)>=code->access_count) return false;
    XrRbcValueOperation op=code->accesses[word>>16].operation;
    return op==XR_RBC_VALUE_RETURN || op==XR_RBC_VALUE_RETURN_ERROR || op==XR_RBC_VALUE_PANIC_ERROR ||
        op==XR_RBC_VALUE_UNCAUGHT_ERROR;
}
