/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_callable_factory.inc.c - Real Env factories and borrowed Frame caches
 */
static XrtManagedStatus rbc_value_function_publish(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,uint32_t value,const XrtFunction *binding) {
    RbcValueOrigin destination={activation,value};
    RbcValuePayload payload;
    XrtManagedStatus status=rbc_value_payload(module,stack,destination,&payload);
    if (status!=XRT_MANAGED_OK) return status;
    activation->publication_count=1;
    activation->publications[0]=(RbcValuePublication){.destination=payload,.snapshot=(const uint8_t *)binding};
    XrtPlace to,temporary;
    status=rbc_value_place(destination,false,0,&to);
    if (status==XRT_MANAGED_OK) status=rbc_value_place(destination,true,0,&temporary);
    const XrtPlacePublication publication={rbc_value_publication,activation};
    return status==XRT_MANAGED_OK ? xrt_place_move_replace(to,temporary,&publication) : status;
}
static XrtManagedStatus rbc_value_function_new(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access) {
    const XrLirValueHome *home=rbc_value_home(activation->code,access.result);
    const XrRbcCallableEntry *entries=xr_rbc_module_callable_entries(module);
    const XrRbcFunction *callee=&module->functions[access.index];
    XrtFunction binding={&entries[access.index],SIZE_MAX,NULL};
    if (home->transport==XR_LIR_CALLABLE_TRANSPORT_FRAME) {
        size_t bytes=0;
        for (uint32_t p=0;p<access.count;++p) {
            uint32_t value=activation->code->arguments[access.first+p];
            RbcValuePayload input;
            XrtManagedStatus status=rbc_value_payload(module,stack,rbc_value_origin(activation,value),&input);
            if (status!=XRT_MANAGED_OK || input.size>activation->snapshot_capacity-bytes) return XRT_MANAGED_ARGUMENT;
            if (p<callee->capture_count && callee->captures[p].kind==XR_RBC_CALLABLE_FUNCTION) {
                XrtFunction captured; memcpy(&captured,input.address,sizeof(captured)); RbcFunctionCall checked;
                status=rbc_value_function_target(module,stack,&captured,callee->captures[p].type+1,&checked);
                if (status!=XRT_MANAGED_OK) return status;
            } else if (p<callee->capture_count) {
                uint64_t bits=0;
                if (input.size!=sizeof(bits)) return XRT_MANAGED_ARGUMENT;
                memcpy(&bits,input.address,sizeof(bits));
                if (callee->captures[p].kind==XR_RBC_CALLABLE_CELL ? !bits : callee->captures[p].kind==XR_RBC_CALLABLE_BINDING ?
                    !xr_rbc_cell(stack,module,bits,XR_RBC_BITS) :
                    !xr_rbc_callable_scalar_valid(callee->captures[p],bits)) return XRT_MANAGED_ARGUMENT;
            }
            memcpy(activation->snapshots+bytes,input.address,input.size); bytes+=input.size;
        }
        if (access.window) {
            const XrRbcFrame *frame=&stack->frames[activation->depth-1];
            const XrRbcFunction *function=&module->functions[activation->function];
            size_t offset,total;
            if (!frame->aggregate_base || !xr_rbc_activation_size(function,&offset,&total)) return XRT_MANAGED_ARGUMENT;
            XrRbcEnvironment environment=function->environments[access.window-1];
            size_t first=frame->aggregate_base-1+(offset+environment.offset)/8;
            binding.environment=first+XR_RBC_FUNCTION_LANES;
            memcpy(stack->spills+binding.environment,activation->snapshots,bytes);
            memcpy(stack->spills+first,&binding,sizeof(binding));
        }
        return rbc_value_inline_replace(module,stack,(RbcValueOrigin){activation,access.result},&binding);
    }
    XrtManagedStatus status=rbc_value_environments(module,stack);
    if (status!=XRT_MANAGED_OK) return status;
    const XrtValueImageType *environment=&activation->image->types[home->environment_type-1];
    if (environment->size>activation->snapshot_capacity) return XRT_MANAGED_ARGUMENT;
    memset(activation->snapshots,0,environment->size);
    for (uint32_t p=0;p<access.count;++p) {
        XrtValueImageSlot slot=activation->image->slots[environment->first_capture+p];
        status=rbc_value_inline_pack(module,stack,activation,activation->code->arguments[access.first+p],
            slot.type,activation->snapshots+slot.offset);
        if (status!=XRT_MANAGED_OK) return status;
        if (activation->image->types[slot.type-1].kind==XRT_VALUE_FUNCTION) {
            XrtFunction captured; memcpy(&captured,activation->snapshots+slot.offset,sizeof(captured));
            RbcFunctionCall checked;
            const uint32_t *types=xr_rbc_module_type_callables(module);
            if (!captured.owner) return XRT_MANAGED_ARGUMENT;
            status=rbc_value_function_target(module,stack,&captured,types[slot.type-1],&checked);
            if (status!=XRT_MANAGED_OK) return status;
        }
    }
    status=xrt_environment_create(activation->frame,home->temporary_first_owner,
        stack->values->environments[home->environment_type-1],environment->size ? activation->snapshots : NULL);
    if (status!=XRT_MANAGED_OK) return status;
    XrtManagedHeader *owner=NULL;
    status=xrt_managed_slot_borrow(activation->frame,home->temporary_first_owner,&owner);
    if (status==XRT_MANAGED_OK && !owner) status=XRT_MANAGED_ARGUMENT;
    if (status==XRT_MANAGED_OK) {
        binding.owner=owner; binding.environment=(uintptr_t)(owner+1);
        status=rbc_value_function_publish(module,stack,activation,access.result,&binding);
    }
    if (status!=XRT_MANAGED_OK) {
        XrtManagedStatus cleanup=xrt_managed_slot_drop(activation->frame,home->temporary_first_owner);
        XR_CHECK(cleanup==XRT_MANAGED_OK,"a failed callable publication retires only its prepared temporary");
    }
    return status;
}
