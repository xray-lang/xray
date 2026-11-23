/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_callable_admission.inc.c - Exact Fn homes and common transfer events
 */
static uint32_t rbc_value_signature(const XrRbcModule *module,const XrRbcValueCode *code,uint32_t value) {
    const XrLirValueHome *home=rbc_value_home(code,value);
    const uint32_t *types=xr_rbc_module_type_callables(module);
    return home && home->type && types && rbc_value_function(module,code,value,false) ? types[home->type-1] : 0;
}
static bool rbc_value_formal_accepts(const XrRbcModule *module,const XrRbcValueCode *receiving,
    uint32_t formal,const XrRbcValueCode *sending,uint32_t source) {
    const XrLirValueHome *to=rbc_value_home(receiving,formal),*from=rbc_value_home(sending,source);
    if (!to || !from || !to->type || !from->type) return false;
    if (rbc_value_function(module,receiving,formal,false))
        return rbc_value_function(module,sending,source,false) && rbc_callable_accepts(module,
            rbc_value_signature(module,receiving,formal),rbc_value_signature(module,sending,source));
    if (rbc_value_slice(module,receiving,formal))
        return rbc_value_slice(module,sending,source) && to->type==from->type;
    return to->type==from->type;
}
static const XrLirHomeEvent *rbc_value_event(const XrRbcValueCode *code,uint32_t index,uint32_t kind) {
    return code && code->homes && code->homes->events && index && index<=code->homes->event_count &&
        code->homes->events[index-1].kind==kind ? &code->homes->events[index-1] : NULL;
}
static bool rbc_value_scope(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,uint32_t physical,uint32_t original) {
    return code->scope_values && code->scope_count==rbc_function_scopes(module,function).count &&
        physical && physical<=code->scope_count && code->scope_values[physical-1]==original;
}
static bool rbc_value_callable_argument(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,uint32_t value,XrRbcCallableValue expected) {
    if (expected.kind==XR_RBC_CALLABLE_CELL)
        return rbc_value_cell(module,code,value,false) && rbc_value_home(code,value)->type==expected.type;
    if (expected.kind==XR_RBC_CALLABLE_FUNCTION)
        return rbc_callable_accepts(module,expected.type+1,rbc_value_signature(module,code,value));
    if (expected.kind == XR_RBC_CALLABLE_HOME)
        return (rbc_value_array(module, code, value, false) || rbc_value_inline(module, code, value, false)) &&
            rbc_callable_image_value(module, rbc_value_home(code, value)->type, expected);
    if (expected.kind==XR_RBC_CALLABLE_REFERENCE) {
        XrtValueKind kind=rbc_value_kind(module,code,value);
        return rbc_value_owner(module,code,value,false) &&
            (expected.type==XR_RBC_STRING ? kind==XRT_VALUE_STRING : expected.type==XR_RBC_SET_I64 ?
             kind==XRT_VALUE_SET_I64 : expected.type==XR_RBC_MAP_STRING_I64 ?
             kind==XRT_VALUE_MAP_STRING_I64 : rbc_array_kind((XrRbcRegisterKind)expected.type) &&
             kind==(XrtValueKind)(XRT_VALUE_ARRAY_I64+xr_rbc_array_family((XrRbcRegisterKind)expected.type)));
    }
    return expected.kind==XR_RBC_CALLABLE_SCALAR && rbc_value_scalar(code,function,value,XR_RBC_BITS);
}
static bool rbc_value_function_access_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess access) {
    if (access.operation==XR_RBC_VALUE_FUNCTION_NEW) {
        const XrLirHomeEvent *event=rbc_value_event(code,access.other,XR_LIR_OWNERSHIP_CALLABLE_VALUE);
        const XrRbcCallableEntry *entries=xr_rbc_module_callable_entries(module);
        if (!event || !entries || access.source || access.index>=xr_rbc_module_callable_entry_count(module) ||
            !rbc_value_function(module,code,access.result,true) || event->source!=access.result ||
            event->operation!=XR_LIR_FUNCTION_REF || event->callable_function!=entries[access.index].source_function ||
            access.first>code->argument_count || access.count>code->argument_count-access.first ||
            (access.count && !code->arguments)) return false;
        const XrLirValueHome *home=rbc_value_home(code,access.result);
        const XrRbcCallableEntry *entry=&entries[access.index];
        if (entry->kind==XR_RBC_CALLABLE_ENTRY_ASSERT_CONDITION) {
            const XrRbcCodeOwner *owner=xr_rbc_module_code_owner(module);
            if (!owner || entry->producer!=owner || entry->physical_function!=UINT32_MAX ||
                access.count || access.window || home->borrow_source ||
                event->callable_transport!=home->transport ||
                !rbc_callable_accepts(module,rbc_value_signature(module,code,access.result),entry->callable)) return false;
            if (home->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED) {
                const XrtValueImage *image=xr_rbc_module_value_image(module);
                return image && home->environment_type && home->environment_type==entry->environment_type &&
                    home->environment_type<=image->type_count &&
                    image->types[home->environment_type-1].kind==XRT_VALUE_ENVIRONMENT;
            }
            return home->transport==XR_LIR_CALLABLE_TRANSPORT_FRAME && !home->environment_type && !event->callable_materialize;
        }
        if (entry->kind!=XR_RBC_CALLABLE_ENTRY_CODE || entry->physical_function!=access.index ||
            access.index>=module->function_count) return false;
        const XrRbcFunction *callee=&module->functions[access.index];
        const XrRbcValueCode *receiver=xr_rbc_module_value_code(module,access.index);
        if (!receiver || receiver->parameter_count!=callee->parameter_count) return false;
        if (home->borrow_source || event->callable_transport!=home->transport || access.count!=callee->capture_count+callee->state_capture ||
            !rbc_callable_accepts(module,rbc_value_signature(module,code,access.result),callee->callable)) return false;
        for (uint32_t p=0;p<access.count;++p) {
            uint32_t value=code->arguments[access.first+p];
            if (p==callee->capture_count) {
                if (home->transport!=XR_LIR_CALLABLE_TRANSPORT_FRAME ||
                    !rbc_value_scalar(code,function,value,XR_RBC_STATE)) return false;
                continue;
            }
            if (callee->captures[p].kind==XR_RBC_CALLABLE_BINDING) {
                if (home->transport!=XR_LIR_CALLABLE_TRANSPORT_FRAME ||
                    !rbc_value_scalar(code,function,value,XR_RBC_I64_BINDING)) return false;
            } else if ((callee->captures[p].kind==XR_RBC_CALLABLE_FUNCTION &&
                    home->transport!=XR_LIR_CALLABLE_TRANSPORT_OWNED) ||
                !rbc_value_callable_argument(module,code,function,value,callee->captures[p])) return false;
        }
        if (home->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED) {
            const XrtValueImage *image=xr_rbc_module_value_image(module);
            return module->code_lease.owner && !callee->state_capture && !access.window && home->environment_type &&
                home->environment_type==entries[access.index].environment_type &&
                home->environment_type<=image->type_count && image->types[home->environment_type-1].kind==XRT_VALUE_ENVIRONMENT;
        }
        if (event->callable_materialize || home->environment_type ||
            home->transport!=XR_LIR_CALLABLE_TRANSPORT_FRAME) return false;
        if (!access.count) return !access.window;
        return access.window && access.window<=function->environment_count &&
            function->environments[access.window-1].producer==UINT32_MAX &&
            function->environments[access.window-1].callee==access.index;
    }
    if (access.operation==XR_RBC_VALUE_FUNCTION_CALL) {
        if (access.other || !access.index || access.index>module->callable_type_count ||
            access.index!=rbc_value_signature(module,code,access.source) ||
            access.first>code->argument_count || access.count>code->argument_count-access.first ||
            (access.count && !code->arguments) || access.window>=function->register_count) return false;
        const XrRbcCallableType *signature=&module->callable_types[access.index-1];
        if (access.count!=signature->parameter_count) return false;
        for (uint32_t p=0;p<access.count;++p)
            if (!rbc_value_callable_argument(module,code,function,code->arguments[access.first+p],signature->parameters[p].value)) return false;
        XrRbcCallableValue result=signature->result;
        if (result.kind==XR_RBC_CALLABLE_FUNCTION)
            return rbc_callable_accepts(module,rbc_value_signature(module,code,access.result),result.type+1);
        return result.kind==XR_RBC_CALLABLE_SCALAR ? result.type==XR_POD_UNIT ? !access.result :
            rbc_value_scalar(code,function,access.result,XR_RBC_BITS) :
            (result.kind == XR_RBC_CALLABLE_REFERENCE || result.kind == XR_RBC_CALLABLE_HOME) &&
            rbc_value_callable_argument(module,code,function,access.result,result) &&
            rbc_value_owner(module,code,access.result,true);
    }
    if (access.operation==XR_RBC_VALUE_FUNCTION_ALIAS) {
        const XrLirHomeEvent *event=rbc_value_event(code,access.index,XR_LIR_OWNERSHIP_CALLABLE_VALUE);
        if (!event || access.first || access.count || event->source!=access.result ||
            event->target!=access.source || !rbc_value_function(module,code,access.result,true) ||
            !rbc_callable_accepts(module,rbc_value_signature(module,code,access.result),rbc_value_signature(module,code,access.source))) return false;
        const XrLirValueHome *home=rbc_value_home(code,access.result);
        if (home->borrow_source!=(home->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED ? 0 : access.source)) return false;
        if (event->operation==XR_LIR_FUNCTION_BIND)
            return access.window!=UINT32_MAX && rbc_value_scope(module,code,function,access.other,event->callable_scope) &&
                (event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY || event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW);
        return event->operation==XR_LIR_FUNCTION_WEAKEN && !access.other && !access.window &&
            (event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY || event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW);
    }
    const XrLirHomeEvent *event=rbc_value_event(code,access.index,XR_LIR_OWNERSHIP_CALLABLE_SCOPE_EXIT);
    if (!event || access.first || access.count || access.window || event->source!=access.source ||
        event->target!=access.result || !rbc_value_scope(module,code,function,access.other,event->callable_scope)) return false;
    if (!access.source) return !access.result;
    const XrLirValueHome *home=rbc_value_home(code,access.result);
    return rbc_value_function(module,code,access.source,false) && rbc_value_function(module,code,access.result,true) &&
        home->borrow_source==(home->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED ? 0 : access.source) &&
        (event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE || event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW);
}
