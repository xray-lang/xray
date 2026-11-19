/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_map_admission.inc.c - Closed Map operations over authentic homes
 */
static bool rbc_value_nullable_i64(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,uint32_t value) {
    const XrLirValueHome *home=rbc_value_home(code,value);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (!home || !image || !image->types || !home->type || home->type>image->type_count || home->owner_count ||
        !rbc_value_location_valid(function,code->locations[value]) ||
        code->locations[value].kind!=XR_RBC_VALUE_AGGREGATE) return false;
    const XrtValueImageType *type=&image->types[home->type-1];
    if (type->kind!=XRT_VALUE_NULLABLE || !type->nullable_element || type->nullable_element>image->type_count ||
        image->types[type->nullable_element-1].kind!=XRT_VALUE_I64 || type->size!=home->size ||
        type->nullable_tag>=home->size || type->nullable_payload>home->size || home->size-type->nullable_payload<8) return false;
    XrRbcAggregateValue aggregate=function->aggregate_values[code->locations[value].index];
    if (aggregate.type>=module->aggregate_type_count || !module->aggregate_types) return false;
    XrPodType packed=module->aggregate_types[aggregate.type];
    return packed.kind==XR_POD_NULLABLE && packed.element==XR_POD_I64 &&
        packed.physical.size==home->size && packed.physical.alignment==type->alignment;
}
static bool rbc_value_map_literal_valid(const XrRbcModule *module, const XrRbcValueCode *code,
    const XrRbcFunction *function, XrRbcValueAccess access) {
    if (!xr_rbc_module_code_owner(module) || !access.index || access.index > module->map_literal_count ||
        !module->map_literals || access.source || access.other || access.window != UINT32_MAX ||
        access.first > code->argument_count || access.count > code->argument_count - access.first ||
        (access.count && !code->arguments) || !rbc_value_map(module,code,access.result,true)) return false;
    const XrtMapLiteral *literal = &module->map_literals[access.index - 1];
    if ((literal->path_length != 0) != (literal->path != NULL) ||
        (literal->count != 0) != (literal->items != NULL) || literal->count > access.count) return false;
    uint32_t operand = 0;
    for (size_t n = 0; n < literal->count; ++n) {
        uint32_t kind = literal->items[n].kind;
        if (kind > 1 || operand >= access.count) return false;
        uint32_t value = code->arguments[access.first + operand++];
        if (!rbc_value_id(code,value)) return false;
        if (kind) {
            if (!rbc_value_map(module,code,value,false)) return false;
        } else {
            if (!rbc_value_string(module,code,value,false) || operand >= access.count) return false;
            value = code->arguments[access.first + operand++];
            if (!rbc_value_id(code,value) || !rbc_value_scalar(code,function,value,XR_RBC_BITS)) return false;
        }
    }
    return operand == access.count;
}
static bool rbc_value_map_access_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess a) {
    if (a.first>code->argument_count || a.count>code->argument_count-a.first ||
        (a.count && !code->arguments) || a.index<XR_LIR_MAP_OP_NEW || a.index>XR_LIR_MAP_OP_CLEAR) return false;
    if ((a.source || a.other) && (!xr_rbc_module_code_owner(module) ||
        !xr_lir_map_reads_key(a.index)))
        return false;
    for (uint32_t p=0;p<a.count;++p) if (!rbc_value_id(code,code->arguments[a.first+p])) return false;
    const uint32_t *args=a.count ? code->arguments+a.first : NULL;
    if (a.index==XR_LIR_MAP_OP_NEW) {
        if (a.window!=UINT32_MAX || (a.count&1u) || !rbc_value_map(module,code,a.result,true)) return false;
        for (uint32_t p=0;p<a.count;p+=2)
            if (!rbc_value_string(module,code,args[p],false) || !rbc_value_scalar(code,function,args[p+1],XR_RBC_BITS)) return false;
        return true;
    }
    bool mutate=a.index==XR_LIR_MAP_OP_SET || a.index==XR_LIR_MAP_OP_REMOVE || a.index==XR_LIR_MAP_OP_CLEAR;
    if (mutate) {
        uint32_t count=a.index==XR_LIR_MAP_OP_SET ? 3u : a.index==XR_LIR_MAP_OP_REMOVE ? 2u : 1u;
        if (a.count!=count || rbc_value_kind(module,code,args[0])!=XRT_VALUE_MAP_STRING_I64 ||
            !rbc_value_binding_valid(module,code,function,args[0]) || a.window>=code->homes->workspace_count ||
            (a.index==XR_LIR_MAP_OP_REMOVE ? !rbc_value_scalar(code,function,a.result,XR_RBC_BITS) : !!a.result)) return false;
        const XrLirHomeBinding *binding=rbc_value_binding(code,args[0]);
        const XrLirHomeWorkspace *workspace=&code->homes->workspaces[a.window];
        return workspace->type==binding->type && workspace->first_owner<code->homes->frame.slot_count &&
            code->homes->frame.owned_offsets && code->homes->frame.owned_offsets[workspace->first_owner]==workspace->offset &&
            (count==1 || rbc_value_string_borrow_operand(module,code,args[1])) &&
            (count!=3 || rbc_value_scalar(code,function,args[2],XR_RBC_BITS));
    }
    if (a.window!=UINT32_MAX || !a.count || !rbc_value_map(module,code,args[0],false)) return false;
    if (a.index==XR_LIR_MAP_OP_LEN) return a.count==1 && rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
    if (a.index==XR_LIR_MAP_OP_DISPLAY) return a.count==1 && rbc_value_string(module,code,a.result,true);
    XrLirType snapshot=xr_lir_map_snapshot_element(a.index);
    if (snapshot)
        return a.count==1 && rbc_value_container(module,code,a.result,true) &&
            rbc_value_kind(module,code,a.result)==(snapshot==XR_LIR_STRING ? XRT_VALUE_ARRAY_STRING : XRT_VALUE_ARRAY_I64);
    if (!xr_lir_map_reads_key(a.index) || a.count!=2 ||
        !rbc_value_string_borrow_operand(module,code,args[1]) ||
        a.source != rbc_local_borrow_slot(code->homes, args[0]) ||
        a.other != rbc_local_borrow_slot(code->homes, args[1])) return false;
    if (a.index==XR_LIR_MAP_OP_GET) return rbc_value_nullable_i64(module,code,function,a.result);
    return rbc_value_scalar(code,function,a.result,XR_RBC_BITS);
}
