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
    if (!binding || kind<XRT_VALUE_ARRAY_I64 || kind>XRT_VALUE_ARRAY_U64 ||
        code->locations[value].kind!=XR_RBC_VALUE_NOWHERE) return false;
    if (binding->kind==XR_LIR_HOME_BINDING_LOCAL)
        return binding->first_owner<code->homes->frame.slot_count && code->homes->frame.owned_offsets &&
            code->homes->frame.owned_offsets[binding->first_owner]==binding->offset;
    if (binding->kind==XR_LIR_HOME_BINDING_PARAMETER)
        return binding->parameter && binding->parameter<=code->parameter_count &&
            code->parameters[binding->parameter-1]==value;
    if (binding->kind==XR_LIR_HOME_BINDING_STATE_BORROW)
        return rbc_value_scalar(code,function,binding->source,XR_RBC_STATE_REF);
    const XrLirHomeBinding *source=rbc_value_binding(code,binding->source);
    return binding->kind==XR_LIR_HOME_BINDING_CELL_BORROW && source && source->type==binding->type &&
        (source->kind==XR_LIR_HOME_BINDING_LOCAL || source->kind==XR_LIR_HOME_BINDING_PARAMETER);
}
static bool rbc_value_binding_access_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess a) {
    uint32_t value=a.operation==XR_RBC_VALUE_BINDING_NEW ? a.result : a.source;
    const XrLirHomeBinding *binding=rbc_value_binding(code,value);
    if (!rbc_value_binding_valid(module,code,function,value)) return false;
    if (a.operation==XR_RBC_VALUE_BINDING_DROP)
        return !a.result && !a.other && binding->kind==XR_LIR_HOME_BINDING_LOCAL;
    if (a.operation==XR_RBC_VALUE_BINDING_NEW)
        return !a.other && binding->kind==XR_LIR_HOME_BINDING_LOCAL && rbc_value_array(module,code,a.source,false) &&
            binding->type==rbc_value_home(code,a.source)->type;
    if (a.operation==XR_RBC_VALUE_BINDING_READ)
        return !a.other && rbc_value_array(module,code,a.result,true) && binding->type==rbc_value_home(code,a.result)->type;
    return !a.result && rbc_value_array(module,code,a.other,false) && binding->type==rbc_value_home(code,a.other)->type;
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
