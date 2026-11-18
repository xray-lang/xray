/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_borrowed_values.inc.c - Complete receiving checks for owner-free raw Fn homes
 */
typedef struct RbcBorrowedRange { size_t begin,end; } RbcBorrowedRange;
static int rbc_borrowed_range_compare(const void *left,const void *right) {
    const RbcBorrowedRange *a=left,*b=right;
    return (a->begin>b->begin)-(a->begin<b->begin);
}
static bool rbc_borrowed_home_ranges(const XrRbcModule *module,const XrRbcValueCode *code,
    RbcBorrowedRange *ranges,uint8_t *colors) {
    const XrLirValueHomes *homes=code->homes;
    size_t count=0;
    for (uint32_t v=1;v<=code->value_count;++v) {
        const XrLirValueHome *home=&homes->values[v];
        if (home->owner_count || (homes->bindings && homes->bindings[v].type)) return false;
        if (!home->type) {
            if (home->borrow_source || home->environment_type || home->transport!=XR_LIR_CALLABLE_TRANSPORT_NONE) return false;
            continue;
        }
        if (!rbc_value_function(module,code,v,false) || home->environment_type ||
            home->first_bytes>=homes->byte_count || home->byte_count!=1 || !homes->bytes ||
            homes->bytes[home->first_bytes].offset || homes->bytes[home->first_bytes].size!=sizeof(XrtFunction)) return false;
        if (home->parameter) {
            if (home->parameter>code->parameter_count || code->parameters[home->parameter-1]!=v ||
                home->offset!=SIZE_MAX || home->temporary_offset!=SIZE_MAX || home->borrow_source) return false;
        } else if (home->offset==SIZE_MAX) return false;
        size_t offsets[]={home->offset,home->temporary_offset};
        for (size_t n=0;n<2;++n) {
            size_t offset=offsets[n]; if (offset==SIZE_MAX) continue;
            if (offset%_Alignof(XrtFunction) || offset>homes->frame.payload_size ||
                sizeof(XrtFunction)>homes->frame.payload_size-offset) return false;
            ranges[count++]=(RbcBorrowedRange){offset,offset+sizeof(XrtFunction)};
        }
        if (home->borrow_source && (!rbc_value_function(module,code,home->borrow_source,false) ||
            !rbc_callable_accepts(module,rbc_value_signature(module,code,v),rbc_value_signature(module,code,home->borrow_source)))) return false;
    }
    qsort(ranges,count,sizeof(*ranges),rbc_borrowed_range_compare);
    for (size_t n=1;n<count;++n) if (ranges[n].begin<ranges[n-1].end) return false;
    for (uint32_t v=1;v<=code->value_count;++v) {
        uint32_t current=v;
        while (current && !colors[current]) { colors[current]=1; current=homes->values[current].borrow_source; }
        if (current && colors[current]==1) return false;
        current=v;
        while (current && colors[current]==1) { colors[current]=2; current=homes->values[current].borrow_source; }
    }
    return true;
}
static XrRbcStatus rbc_borrowed_home_admit(const XrRbcModule *module,const XrRbcFunction *function,
    const XrRbcValueCode *code) {
    if (code->string_binding_setup || code->value_count==UINT32_MAX || !code->locations || code->parameter_count!=function->parameter_count ||
        (code->parameter_count && !code->parameters) || code->access_count>65536 ||
        (code->access_count && !code->accesses) || (code->argument_count && !code->arguments) ||
        code->scope_count!=rbc_function_scopes(module,function).count ||
        (code->scope_count && !code->scope_values)) return XR_RBC_INVALID;
    for (uint32_t n=0;n<code->scope_count;++n)
        if (!rbc_value_id(code,code->scope_values[n]) ||
            (n && code->scope_values[n-1]>=code->scope_values[n])) return XR_RBC_INVALID;
    for (uint32_t p=0;p<code->parameter_count;++p) {
        uint32_t value=code->parameters[p];
        if (!rbc_value_id(code,value)) return XR_RBC_INVALID;
        const XrLirValueHome *home=rbc_value_home(code,value);
        if (home && home->type) continue;
        if (code->locations[value].kind!=XR_RBC_VALUE_REGISTER || code->locations[value].index!=p) return XR_RBC_INVALID;
    }
    const XrLirValueHomes *homes=code->homes;
    if (!homes) return XR_RBC_OK;
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    if (homes->result_type && (!image || homes->result_type>image->type_count)) return XR_RBC_INVALID;
    if (homes->result_type && image->types[homes->result_type-1].kind==XRT_VALUE_FUNCTION) return XR_RBC_INVALID;
    if (homes->value_count!=code->value_count || !homes->values || homes->frame.slot_count ||
        homes->owned_frame_needed || homes->workspace_count ||
        (homes->frame.payload_size && homes->frame.payload_alignment!=_Alignof(XrtFunction)) ||
        (homes->event_count && !homes->events) || (homes->byte_count && !homes->bytes) ||
        (homes->edge_count && !homes->edges) || (homes->transfer_count && !homes->transfers)) return XR_RBC_INVALID;
    size_t count=(size_t)code->value_count+1;
    if (count>SIZE_MAX/(2*sizeof(RbcBorrowedRange))) return XR_RBC_INVALID;
    RbcBorrowedRange *ranges=xr_malloc(count*2*sizeof(*ranges));
    uint8_t *colors=xr_calloc(count,1);
    if (!ranges || !colors) { xr_free(ranges); xr_free(colors); return XR_RBC_OUT_OF_MEMORY; }
    bool valid=rbc_borrowed_home_ranges(module,code,ranges,colors);
    xr_free(ranges); xr_free(colors);
    for (size_t n=0;valid && n<code->access_count;++n) {
        XrRbcValueAccess access=code->accesses[n];
        if (access.operation>=XR_RBC_VALUE_STATE_READ && access.operation<=XR_RBC_VALUE_STATE_INITIALIZE &&
            rbc_value_function(module,code,access.operation==XR_RBC_VALUE_STATE_READ ? access.result : access.other,false)) valid=false;
    }
    for (size_t n=0;valid && n<homes->event_count;++n) {
        const XrLirHomeEvent *event=&homes->events[n];
        if (event->forwarding_mode || event->forwarding_drop) valid=false;
        if (event->callable_materialize || event->callable_transport==XR_LIR_CALLABLE_TRANSPORT_OWNED ||
            event->callable_transport==XR_LIR_CALLABLE_TRANSPORT_MIXED ||
            event->kind==XR_LIR_OWNERSHIP_CALLABLE_MATERIALIZE || event->kind==XR_LIR_OWNERSHIP_RESULT_OWNER ||
            event->kind==XR_LIR_OWNERSHIP_BLOCK_OWNER || event->kind==XR_LIR_OWNERSHIP_RELEASE) valid=false;
        if (event->kind==XR_LIR_OWNERSHIP_CALLABLE_VALUE) {
            bool alias=event->operation==XR_LIR_FUNCTION_BIND || event->operation==XR_LIR_FUNCTION_WEAKEN ||
                event->operation==XR_LIR_FUNCTION_SCOPE_LEAVE;
            if (event->transfer_mode!=(alias ? XR_LIR_OWNERSHIP_TRANSFER_BORROW : XR_LIR_OWNERSHIP_TRANSFER_NONE)) valid=false;
        }
        if (event->kind==XR_LIR_OWNERSHIP_CALLABLE_SCOPE_EXIT && event->source &&
            event->transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_BORROW) valid=false;
    }
    for (size_t n=0;valid && n<homes->transfer_count;++n)
        valid=homes->transfers[n].transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW;
    return valid ? XR_RBC_OK : XR_RBC_INVALID;
}
static XrRbcStatus rbc_borrowed_values_admit(const XrRbcModule *module) {
    if (xr_rbc_module_code_owner(module)) return XR_RBC_OK;
    if (!module->borrowed_value_codes)
        return module->borrowed_value_code_count || module->borrowed_value_image ||
            module->borrowed_callable_entries || module->borrowed_type_callables ? XR_RBC_INVALID : XR_RBC_OK;
    if (module->borrowed_value_code_count!=module->function_count) return XR_RBC_INVALID;
    const XrtValueImage *image=module->borrowed_value_image;
    if (image) {
        if (image->type_count && !image->types) return XR_RBC_INVALID;
        for (uint32_t n=0;n<image->type_count;++n) if (image->types[n].kind==XRT_VALUE_ENVIRONMENT) return XR_RBC_INVALID;
        XrtBillingOwner *billing=NULL; XrtValueImagePlans *plans=NULL;
        XrtManagedStatus status=xrt_billing_owner_new(NULL,SIZE_MAX,&billing);
        if (status==XRT_MANAGED_OK) status=xrt_value_image_bind(billing,image,(XrtManagedLease){0},&plans);
        xrt_value_image_drop(&plans); xrt_billing_owner_drop(billing);
        if (status!=XRT_MANAGED_OK) return status==XRT_MANAGED_OOM ? XR_RBC_OUT_OF_MEMORY : XR_RBC_INVALID;
    }
    const XrRbcCallableEntry *entries=module->borrowed_callable_entries;
    for (size_t n=0;n<module->function_count;++n) {
        if (entries && (entries[n].kind!=XR_RBC_CALLABLE_ENTRY_CODE || entries[n].physical_function!=n || (entries[n].signature_type &&
            (!image || entries[n].signature_type>image->type_count ||
             image->types[entries[n].signature_type-1].kind!=XRT_VALUE_FUNCTION)) || entries[n].environment_type)) return XR_RBC_INVALID;
        const XrRbcValueCode *code=module->borrowed_value_codes[n];
        if (!code) continue;
        XrRbcStatus status=rbc_borrowed_home_admit(module,&module->functions[n],code);
        if (status!=XR_RBC_OK) return status;
    }
    return XR_RBC_OK;
}
