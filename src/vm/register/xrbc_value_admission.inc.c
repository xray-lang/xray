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
static bool rbc_value_edge_valid(const XrRbcValueCode *code, XrRbcValueAccess access) {
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
        } else if (from && from->type) return false;
    }
    return typed == (edge ? edge->count : 0);
}
static bool xr_rbc_value_instruction_valid(const XrRbcModule *module,
    const XrRbcFunction *function, uint32_t word) {
    if ((word & 255) != XR_RBC_VALUE_ACCESS || ((word >> 8) & 255)) return false;
    const XrRbcValueCode *code = rbc_value_code(module,function);
    size_t index = word >> 16;
    if (!code || !code->accesses || index >= code->access_count || code->access_count > 65536 ||
        !code->locations || (code->homes && code->homes->value_count != code->value_count)) return false;
    XrRbcValueAccess a = code->accesses[index];
    if (a.operation!=XR_RBC_VALUE_CALL && a.operation!=XR_RBC_VALUE_EDGE && (a.first || a.count || a.window)) return false;
    if (a.operation!=XR_RBC_VALUE_STRING_LITERAL && a.operation!=XR_RBC_VALUE_STATE_CONSTRUCT &&
        a.operation!=XR_RBC_VALUE_STATE_INITIALIZE && a.operation!=XR_RBC_VALUE_CALL &&
        a.operation!=XR_RBC_VALUE_EDGE && a.index) return false;
    switch (a.operation) {
    case XR_RBC_VALUE_STRING_LITERAL:
        return !a.source && !a.other && a.index < module->literal_count && rbc_value_string(module,code,a.result,true);
    case XR_RBC_VALUE_STRING_I64: case XR_RBC_VALUE_STRING_F64:
        return !a.other && rbc_value_string(module,code,a.result,true) && rbc_value_scalar(code,function,a.source,XR_RBC_BITS);
    case XR_RBC_VALUE_STRING_CONCAT:
        return rbc_value_string(module,code,a.result,true) && rbc_value_string(module,code,a.source,false) &&
            rbc_value_string(module,code,a.other,false);
    case XR_RBC_VALUE_COPY: case XR_RBC_VALUE_MOVE:
        return !a.other && rbc_value_string(module,code,a.result,true) && rbc_value_string(module,code,a.source,false) &&
            (a.operation == XR_RBC_VALUE_COPY || !rbc_value_home(code,a.source)->parameter);
    case XR_RBC_VALUE_DROP:
        return !a.result && !a.other && rbc_value_string(module,code,a.source,false) &&
            !rbc_value_home(code,a.source)->parameter;
    case XR_RBC_VALUE_STRING_PRINT:
        return !a.result && !a.other && rbc_value_string(module,code,a.source,false);
    case XR_RBC_VALUE_STRING_LENGTH:
        return !a.other && rbc_value_string(module,code,a.source,false) && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    case XR_RBC_VALUE_STATE_READ:
        return !a.other && rbc_value_scalar(code,function,a.source,XR_RBC_STATE_REF) &&
            rbc_value_string(module,code,a.result,true);
    case XR_RBC_VALUE_STATE_WRITE:
        return !a.result && rbc_value_scalar(code,function,a.source,XR_RBC_STATE_REF) &&
            rbc_value_string(module,code,a.other,false);
    case XR_RBC_VALUE_STATE_CONSTRUCT: case XR_RBC_VALUE_STATE_INITIALIZE: {
        const XrtModuleLayoutSpec *spec = module->state_spec;
        if (!spec || !spec->values || !spec->values->types || !spec->slots || a.index >= spec->slot_count ||
            !rbc_value_scalar(code,function,a.source,XR_RBC_STATE) || !rbc_value_string(module,code,a.other,false)) return false;
        XrtModuleSlotSpec slot = spec->slots[a.index];
        if (!slot.type || slot.type > spec->values->type_count || spec->values->types[slot.type - 1].kind != XRT_VALUE_STRING)
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
            if (formal && formal->type) {
                if (!from || from->type!=formal->type || formal->parameter!=p+1) return false;
                if (formal->owner_count && (!rbc_value_string(module,code,source,false) ||
                    !rbc_value_string(module,callee,callee->parameters[p],false))) return false;
            } else {
                if (from && from->type) return false;
                XrRbcRegisterKind kind=location.kind==XR_RBC_VALUE_REGISTER ? rbc_register_kind(target,location.index) : XR_RBC_BITS;
                if (!rbc_value_scalar(code,function,source,kind)) return false;
            }
        }
        return target->result_kind==XR_RBC_STRING ? callee && rbc_value_string(module,code,a.result,true) :
            target->result_kind==XR_RBC_BITS && (!a.result || rbc_value_scalar(code,function,a.result,XR_RBC_BITS));
    }
    case XR_RBC_VALUE_RETURN:
        return !a.result && !a.other && (!a.source || rbc_value_string(module,code,a.source,false) ||
            rbc_value_scalar(code,function,a.source,XR_RBC_BITS));
    case XR_RBC_VALUE_EDGE:
        return !a.result && !a.source && !a.other && rbc_value_edge_valid(code,a);
    default: return false;
    }
}
static bool xr_rbc_value_return_word(const XrRbcModule *module, const XrRbcFunction *function, uint32_t word) {
    const XrRbcValueCode *code = rbc_value_code(module,function);
    return (word & 255) == XR_RBC_VALUE_ACCESS && !((word>>8)&255) && code && code->accesses && (word >> 16) < code->access_count &&
        code->accesses[word >> 16].operation == XR_RBC_VALUE_RETURN;
}
