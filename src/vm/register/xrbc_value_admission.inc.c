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
static bool rbc_value_string(const XrRbcModule *module, const XrRbcValueCode *code,
    uint32_t value, bool destination) {
    const XrLirValueHome *home = rbc_value_home(code,value);
    const XrtValueImage *image = xr_rbc_module_value_image(module);
    return home && image && image->types && home->type && home->type <= image->type_count &&
        image->types[home->type - 1].kind == XRT_VALUE_STRING && home->owner_count == 1 &&
        home->size == sizeof(XrtManagedHeader *) && code->locations[value].kind == XR_RBC_VALUE_NOWHERE &&
        (!destination || (!home->parameter && home->offset != SIZE_MAX &&
            home->temporary_offset != SIZE_MAX && home->temporary_first_owner < code->homes->frame.slot_count));
}
static const XrLirHomeBinding *rbc_value_binding(const XrRbcValueCode *code, uint32_t value) {
    return rbc_value_id(code,value) && code->homes && code->homes->bindings &&
        value<=code->homes->value_count && code->homes->bindings[value].type ? &code->homes->bindings[value] : NULL;
}
static XrtValueKind rbc_value_kind(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value) {
    const XrLirValueHome *home=rbc_value_home(code,value);
    const XrLirHomeBinding *binding=rbc_value_binding(code,value);
    uint32_t type=binding ? binding->type : home ? home->type : 0;
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    return type && image && image->types && type<=image->type_count ? image->types[type-1].kind : XRT_VALUE_UNIT;
}
static bool rbc_value_array(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,bool destination) {
    const XrLirValueHome *home=rbc_value_home(code,value); XrtValueKind kind=rbc_value_kind(module,code,value);
    return home && kind>=XRT_VALUE_ARRAY_I64 && kind<=XRT_VALUE_ARRAY_U64 && home->owner_count==1 &&
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
static bool rbc_value_container(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,bool destination) {
    return rbc_value_array(module,code,value,destination) || rbc_value_map(module,code,value,destination);
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
static bool rbc_value_inline(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,bool destination) {
    const XrLirValueHome *home=rbc_value_home(code,value);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (!home || !home->type || !image || !image->types || home->type>image->type_count ||
        code->locations[value].kind!=XR_RBC_VALUE_NOWHERE) return false;
    const XrtValueImageType *type=&image->types[home->type-1];
    if (type->kind==XRT_VALUE_FUNCTION) return rbc_value_function(module,code,value,destination);
    return (type->kind==XRT_VALUE_RECORD || type->kind==XRT_VALUE_NULLABLE) && home->size==type->size &&
        home->owner_count==type->owned_count && (!destination || (!home->parameter && home->offset!=SIZE_MAX &&
            (!home->owner_count || (home->temporary_offset!=SIZE_MAX && home->temporary_first_owner<code->homes->frame.slot_count))));
}
static bool rbc_value_owner(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value,bool destination) {
    return rbc_value_string(module,code,value,destination) || rbc_value_container(module,code,value,destination) ||
        rbc_value_inline(module,code,value,destination);
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
    return aggregate.type==result-1 && (kind==XRT_VALUE_NULLABLE || kind==XRT_VALUE_FIXED_U8 || kind==XRT_VALUE_TUPLE_I64) &&
        home->size==module->aggregate_types[result-1].physical.size;
}
#include "xrbc_value_array_admission.inc.c"
#include "xrbc_value_map_admission.inc.c"
#include "xrbc_value_slice_admission.inc.c"
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
            if (rbc_value_slice(module,code,target)) {
                if (!rbc_value_slice(module,code,source) || transfer.transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_BORROW ||
                    transfer.hold_transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_COPY ||
                    transfer.source_workspace!=from->view_workspace || transfer.target_workspace!=to->view_workspace) return false;
            } else if (transfer.hold_transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE || transfer.source_workspace || transfer.target_workspace) return false;
            else if (to->transport!=XR_LIR_CALLABLE_TRANSPORT_NONE) {
                if (to->borrow_source || transfer.transfer_mode!=(from->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED ?
                        XR_LIR_OWNERSHIP_TRANSFER_COPY : XR_LIR_OWNERSHIP_TRANSFER_BORROW)) return false;
            } else if (transfer.transfer_mode!=(to->owner_count ?
                    XR_LIR_OWNERSHIP_TRANSFER_MOVE : XR_LIR_OWNERSHIP_TRANSFER_NONE)) return false;
        } else if (from && from->type) return false;
    }
    return typed == (edge ? edge->count : 0);
}
static bool rbc_value_display_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess access) {
    if (access.other || !rbc_value_id(code,access.source) || !rbc_value_string(module,code,access.result,true) ||
        access.index>=code->display_count || !code->displays) return false;
    const XrtPodDisplay *display=&code->displays[access.index];
    const XrLirValueHome *home=rbc_value_home(code,access.source);
    XrPodLayout expected={0};
    if (!xr_pod_scalar_layout(display->scalar,&expected)) return false;
    if (!display->nullable_depth)
        return (display->scalar==XR_POD_BOOL || display->scalar==XR_POD_U8 || display->scalar==XR_POD_U64) &&
            rbc_value_scalar(code,function,access.source,XR_RBC_BITS) &&
            display->physical.size==expected.size && display->physical.alignment==expected.alignment &&
            display->physical.scalar_count==expected.scalar_count;
    const XrtValueImage *image=xr_rbc_module_value_image(module);
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
#include "xrbc_value_inline_admission.inc.c"
#include "xrbc_value_callable_admission.inc.c"
static bool xr_rbc_value_instruction_valid(const XrRbcModule *module,
    const XrRbcFunction *function, uint32_t word) {
    if ((word & 255) != XR_RBC_VALUE_ACCESS || ((word >> 8) & 255)) return false;
    const XrRbcValueCode *code = rbc_value_code(module,function);
    size_t index = word >> 16;
    if (!code || !code->accesses || index >= code->access_count || code->access_count > 65536 ||
        !code->locations || (code->homes && code->homes->value_count != code->value_count)) return false;
    XrRbcValueAccess a = code->accesses[index];
    if (a.operation!=XR_RBC_VALUE_CALL && a.operation!=XR_RBC_VALUE_EDGE && a.operation!=XR_RBC_VALUE_ARRAY &&
        a.operation!=XR_RBC_VALUE_MAP && a.operation!=XR_RBC_VALUE_RECORD_NEW &&
        a.operation!=XR_RBC_VALUE_FUNCTION_NEW && a.operation!=XR_RBC_VALUE_FUNCTION_CALL &&
        a.operation!=XR_RBC_VALUE_FUNCTION_ALIAS && a.operation!=XR_RBC_VALUE_SLICE && (a.first || a.count || a.window)) return false;
    if (a.operation!=XR_RBC_VALUE_STRING_LITERAL && a.operation!=XR_RBC_VALUE_STATE_CONSTRUCT &&
        a.operation!=XR_RBC_VALUE_STATE_INITIALIZE && a.operation!=XR_RBC_VALUE_CALL &&
        a.operation!=XR_RBC_VALUE_EDGE && a.operation!=XR_RBC_VALUE_ARRAY && a.operation!=XR_RBC_VALUE_MAP &&
        a.operation!=XR_RBC_VALUE_DISPLAY && a.operation!=XR_RBC_VALUE_RECORD_FIELD && a.operation!=XR_RBC_VALUE_RETURN &&
        a.operation<XR_RBC_VALUE_FUNCTION_NEW && a.index) return false;
    switch (a.operation) {
    case XR_RBC_VALUE_SLICE: return rbc_value_slice_access_valid(module,code,function,a);
    case XR_RBC_VALUE_FUNCTION_NEW: case XR_RBC_VALUE_FUNCTION_CALL:
    case XR_RBC_VALUE_FUNCTION_ALIAS: case XR_RBC_VALUE_FUNCTION_SCOPE_EXIT:
        return rbc_value_function_access_valid(module,code,function,a);
    case XR_RBC_VALUE_STRING_LITERAL:
        return !a.source && !a.other && a.index < module->literal_count && rbc_value_string(module,code,a.result,true);
    case XR_RBC_VALUE_STRING_I64: case XR_RBC_VALUE_STRING_F64:
        return !a.other && rbc_value_string(module,code,a.result,true) && rbc_value_scalar(code,function,a.source,XR_RBC_BITS);
    case XR_RBC_VALUE_STRING_CONCAT:
        return rbc_value_string(module,code,a.result,true) && rbc_value_string(module,code,a.source,false) &&
            rbc_value_string(module,code,a.other,false);
    case XR_RBC_VALUE_COPY: case XR_RBC_VALUE_MOVE:
        return !a.other && !rbc_value_function(module,code,a.result,false) &&
            rbc_value_owner(module,code,a.result,true) && rbc_value_owner(module,code,a.source,false) &&
            rbc_value_home(code,a.result)->type==rbc_value_home(code,a.source)->type && (a.operation == XR_RBC_VALUE_COPY || !rbc_value_home(code,a.source)->parameter);
    case XR_RBC_VALUE_DROP:
        return !a.result && !a.other && rbc_value_owner(module,code,a.source,false) &&
            !rbc_value_home(code,a.source)->parameter &&
            (!rbc_value_function(module,code,a.source,false) || rbc_value_home(code,a.source)->owner_count==1);
    case XR_RBC_VALUE_STRING_PRINT:
        return !a.result && !a.other && rbc_value_string(module,code,a.source,false);
    case XR_RBC_VALUE_STRING_LENGTH:
        return !a.other && rbc_value_string(module,code,a.source,false) && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    case XR_RBC_VALUE_STATE_READ:
        return !a.other && rbc_value_scalar(code,function,a.source,XR_RBC_STATE_REF) &&
            rbc_value_owner(module,code,a.result,true);
    case XR_RBC_VALUE_STATE_WRITE:
        return !a.result && rbc_value_scalar(code,function,a.source,XR_RBC_STATE_REF) &&
            rbc_value_owner(module,code,a.other,false);
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
    case XR_RBC_VALUE_CALL: {
        if (a.index >= module->function_count || a.first > code->argument_count ||
            a.count > code->argument_count - a.first || (a.count && !code->arguments) || a.window >= function->register_count)
            return false;
        for (uint32_t p = 0; p < a.count; ++p) if (!rbc_value_id(code,code->arguments[a.first + p])) return false;
        const XrRbcValueCode *callee = xr_rbc_module_value_code(module,a.index);
        const XrRbcFunction *target=&module->functions[a.index];
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
            if (binding) {
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
        if (callee && rbc_value_inline(module,code,a.result,true))
            return callee->homes->result_type==rbc_value_home(code,a.result)->type;
        if (target->aggregate_result_type)
            return callee && rbc_value_pod(module,code,function,a.result,target->aggregate_result_type) &&
                callee->homes->result_type==rbc_value_home(code,a.result)->type;
        return rbc_managed_kind(target->result_kind) ? callee && rbc_value_owner(module,code,a.result,true) &&
            callee->homes->result_type==rbc_value_home(code,a.result)->type :
            target->result_kind==XR_RBC_BITS && (!a.result || rbc_value_scalar(code,function,a.result,XR_RBC_BITS));
    }
    case XR_RBC_VALUE_RECORD_NEW: case XR_RBC_VALUE_RECORD_FIELD:
    case XR_RBC_VALUE_NULLABLE_NONE: case XR_RBC_VALUE_NULLABLE_SOME:
    case XR_RBC_VALUE_NULLABLE_IS_SOME: case XR_RBC_VALUE_NULLABLE_UNWRAP:
        return rbc_value_inline_access_valid(module,code,function,a);
    case XR_RBC_VALUE_RETURN:
        if (a.result || a.other) return false;
        if (rbc_value_function(module,code,a.source,false)) {
            const XrLirHomeEvent *event=rbc_value_event(code,a.index,XR_LIR_OWNERSHIP_RETURN_TRANSFER);
            return event && event->source==a.source && code->homes->result_type==rbc_value_home(code,a.source)->type &&
                (event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY || event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE);
        }
        if (a.index) return false;
        if (function->aggregate_result_type)
            return rbc_value_pod(module,code,function,a.source,function->aggregate_result_type) &&
                code->homes->result_type==rbc_value_home(code,a.source)->type;
        return !a.source || rbc_value_owner(module,code,a.source,false) || rbc_value_scalar(code,function,a.source,XR_RBC_BITS);
    case XR_RBC_VALUE_ARRAY: return rbc_value_array_access_valid(module,code,function,a);
    case XR_RBC_VALUE_MAP: return rbc_value_map_access_valid(module,code,function,a);
    case XR_RBC_VALUE_DISPLAY: return rbc_value_display_valid(module,code,function,a);
    case XR_RBC_VALUE_BINDING_NEW: case XR_RBC_VALUE_BINDING_READ:
    case XR_RBC_VALUE_BINDING_WRITE: case XR_RBC_VALUE_BINDING_DROP:
        return rbc_value_binding_access_valid(module,code,function,a);
    case XR_RBC_VALUE_EDGE:
        return !a.result && !a.source && !a.other && rbc_value_edge_valid(module,code,a);
    default: return false;
    }
}
static bool xr_rbc_value_return_word(const XrRbcModule *module, const XrRbcFunction *function, uint32_t word) {
    const XrRbcValueCode *code = rbc_value_code(module,function);
    return (word & 255) == XR_RBC_VALUE_ACCESS && !((word>>8)&255) && code && code->accesses && (word >> 16) < code->access_count &&
        code->accesses[word >> 16].operation == XR_RBC_VALUE_RETURN;
}
