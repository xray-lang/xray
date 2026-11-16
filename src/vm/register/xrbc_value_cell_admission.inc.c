/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_cell_admission.inc.c - Authentic identity homes and exact Cell hooks
 */
static bool rbc_value_cell_event(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrLirHomeEvent *event,uint32_t value) {
    const XrLirValueHome *home=rbc_value_home(code,value);
    return event && event->block && event->instruction && !event->edge && !event->target_block &&
        event->operation==(uint32_t)event->cell_operation && !event->callable_function && !event->callable_scope && !event->callable_materialize &&
        event->callable_transport==XR_LIR_CALLABLE_TRANSPORT_NONE && !event->view_operation && !event->view_workspace &&
        event->hold_transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE && rbc_value_cell(module,code,value,false) &&
        event->cell_type==home->type && event->cell_payload_type==home->cell_payload_type &&
        event->cell_transport==home->cell_transport;
}
static bool rbc_value_cell_access_valid(const XrRbcModule *module,const XrRbcValueCode *code,
    const XrRbcFunction *function,XrRbcValueAccess access) {
    if (!xr_rbc_module_code_owner(module) || access.first || access.count || access.window || !code->homes ||
        !code->homes->events || !access.index || access.index>code->homes->event_count) return false;
    const XrLirHomeEvent *event=&code->homes->events[access.index-1];
    if (event->kind==XR_LIR_OWNERSHIP_CELL_SCOPE_EXIT) {
        if (!event->cell_scope || !rbc_value_scope(module,code,function,access.other,event->cell_scope) ||
            event->source!=access.source || event->target!=access.result) return false;
        if (!access.source) return !access.result && !event->cell_type && !event->cell_payload_type &&
            event->cell_transport==XR_LIR_CELL_TRANSPORT_NONE && event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE;
        if (!rbc_value_cell_event(module,code,event,access.source) || event->cell_operation!=XR_LIR_CELL_OP_KEEP ||
            !rbc_value_cell(module,code,access.result,true) ||
            rbc_value_home(code,access.result)->type!=event->cell_type) return false;
        bool keep=false;
        for (size_t n=0;n<code->homes->event_count;++n) {
            const XrLirHomeEvent *row=&code->homes->events[n];
            if (row->block==event->block && row->instruction==event->instruction &&
                row->kind==XR_LIR_OWNERSHIP_CELL_VALUE && row->source==access.result && row->target==access.source &&
                row->cell_operation==XR_LIR_CELL_OP_KEEP && row->transfer_mode==event->transfer_mode &&
                rbc_value_cell_event(module,code,row,access.result)) keep=true;
        }
        return keep && event->transfer_mode==(rbc_value_home(code,access.source)->cell_transport==XR_LIR_CELL_TRANSPORT_BORROW ?
            XR_LIR_OWNERSHIP_TRANSFER_COPY : XR_LIR_OWNERSHIP_TRANSFER_MOVE);
    }
    uint32_t cell=event->kind==XR_LIR_OWNERSHIP_CELL_VALUE || event->cell_operation==XR_LIR_CELL_OP_NEW ?
        access.result : access.source;
    if (!rbc_value_cell_event(module,code,event,cell) || event->source!=cell || event->argument) return false;
    const XrLirValueHome *home=rbc_value_home(code,cell);
    if (event->kind==XR_LIR_OWNERSHIP_RELEASE)
        return !access.result && !access.other && !event->target && !home->parameter &&
            event->cell_operation==XR_LIR_CELL_OP_NONE && event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_NONE;
    if (event->kind==XR_LIR_OWNERSHIP_CELL_VALUE) {
        if (event->target!=access.source || !rbc_value_cell(module,code,access.result,true) ||
            !rbc_value_cell(module,code,access.source,false) ||
            rbc_value_home(code,access.source)->type!=home->type || event->transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_COPY) return false;
        return event->cell_operation==XR_LIR_CELL_OP_COPY ? !access.other :
            event->cell_operation==XR_LIR_CELL_OP_BIND && rbc_value_scope(module,code,function,access.other,event->cell_scope);
    }
    if (event->kind!=XR_LIR_OWNERSHIP_CELL_OPERATION || event->transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_NONE) return false;
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    bool integer=image->types[home->cell_payload_type-1].kind==XRT_VALUE_I64;
    uint32_t payload=event->cell_operation==XR_LIR_CELL_OP_NEW ? access.source :
        event->cell_operation==XR_LIR_CELL_OP_WRITE ? access.other : access.result;
    if (event->target!=payload) return false;
    if (event->cell_operation==XR_LIR_CELL_OP_BORROW_MAP) {
        const XrLirHomeBinding *binding=rbc_value_binding(code,access.result);
        return !integer && !access.other && binding && binding->kind==XR_LIR_HOME_BINDING_CELL_PAYLOAD_BORROW &&
            binding->source==access.source && binding->type==home->cell_payload_type &&
            binding->offset==SIZE_MAX && binding->first_owner==SIZE_MAX;
    }
    if (event->cell_operation!=XR_LIR_CELL_OP_NEW && event->cell_operation!=XR_LIR_CELL_OP_READ &&
        event->cell_operation!=XR_LIR_CELL_OP_WRITE) return false;
    bool read=event->cell_operation==XR_LIR_CELL_OP_READ;
    if (event->cell_operation==XR_LIR_CELL_OP_NEW ? access.other || !rbc_value_cell(module,code,access.result,true) :
        read ? !!access.other : !!access.result) return false;
    return integer ? rbc_value_scalar(code,function,payload,XR_RBC_BITS) :
        rbc_value_map(module,code,payload,read) && rbc_value_home(code,payload)->type==home->cell_payload_type;
}
