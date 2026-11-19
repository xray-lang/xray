/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_cell_execute.inc.c - Stable Cell identities under the shared Core
 */
static XrtManagedStatus rbc_value_cell_step(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,uint32_t source) {
    const XrLirHomeEvent *event=&activation->code->homes->events[access.index-1];
    if (event->kind==XR_LIR_OWNERSHIP_RELEASE) {
        const XrLirValueHome *home=rbc_value_home(activation->code,access.source);
        return xrt_managed_slot_drop(activation->frame,home->first_owner);
    }
    if (event->kind==XR_LIR_OWNERSHIP_CELL_VALUE || event->kind==XR_LIR_OWNERSHIP_CELL_SCOPE_EXIT) {
        if (access.source) {
            XrtManagedStatus status=rbc_value_replace(module,stack,activation,(RbcValueOrigin){activation,access.result},
                rbc_value_origin(activation,access.source),event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY);
            if (status!=XRT_MANAGED_OK) return status;
        }
        if (access.other) {
            XrRbcStatus status=xr_rbc_closure_scope_step(module,stack,&module->functions[activation->function],
                (XrRbcAggregateAccess){.operation=event->kind==XR_LIR_OWNERSHIP_CELL_SCOPE_EXIT ?
                    XR_RBC_FUNCTION_SCOPE_LEAVE : XR_RBC_FUNCTION_BIND,.destination=access.other},NULL,source);
            if (status!=XR_RBC_OK) return XRT_MANAGED_ARGUMENT;
        }
        return XRT_MANAGED_OK;
    }
    if (event->cell_operation==XR_LIR_CELL_OP_BORROW_MAP) return XRT_MANAGED_OK;
    bool create=event->cell_operation==XR_LIR_CELL_OP_NEW;
    uint32_t value=create ? access.result : access.source;
    const XrLirValueHome *home=rbc_value_home(activation->code,value);
    bool integer=activation->image->types[home->cell_payload_type-1].kind==XRT_VALUE_I64;
    XrtManagedHeader *cell=NULL;
    XrtManagedStatus status=create ? rbc_value_environments(module,stack) :
        rbc_value_cell_borrow(module,stack,activation,value,&cell);
    if (status!=XRT_MANAGED_OK) return status;
    bool read=event->cell_operation==XR_LIR_CELL_OP_READ;
    uint32_t payload=create ? access.source : read ? access.result : access.other;
    if (integer) {
        XrRbcRegister *scalar=rbc_value_register(stack,activation,payload);
        if (!scalar) return XRT_MANAGED_ARGUMENT;
        int64_t bits=0;
        if (!read) memcpy(&bits,&scalar->bits,sizeof(bits));
        if (create) status=xrt_i64_cell_create(activation->frame,home->temporary_first_owner,
            stack->values->environments[home->type-1],bits);
        else if (read) {
            status=xrt_i64_cell_read((const XrtI64Cell *)cell,&bits);
            if (status==XRT_MANAGED_OK) memcpy(&scalar->bits,&bits,sizeof(bits));
        } else status=xrt_i64_cell_write_exclusive((XrtI64Cell *)cell,bits);
    } else if (read) {
        const XrLirValueHome *result=rbc_value_home(activation->code,payload);
        status=xrt_map_cell_read_copy(activation->frame,result->temporary_first_owner,(const XrtMapCell *)cell);
    } else {
        XrtPlace place; const XrtManagedHeader *map=NULL;
        status=rbc_value_place(rbc_value_origin(activation,payload),false,0,&place);
        if (status==XRT_MANAGED_OK) status=xrt_place_borrow(place,&map);
        if (status==XRT_MANAGED_OK) status=create ? xrt_map_cell_create(activation->frame,home->temporary_first_owner,
            stack->values->environments[home->type-1],(const XrtMapBacking *)map) :
            xrt_map_cell_write_exclusive((XrtMapCell *)cell,(const XrtMapBacking *)map);
    }
    if (status==XRT_MANAGED_OK && (create || (!integer && read)))
        status=rbc_value_publish_leaf(activation,create ? access.result : payload);
    return status;
}
