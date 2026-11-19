/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_callable_execute.inc.c - Full bindings in stable common Fn homes
 */
typedef struct RbcFunctionCall {
    XrtFunction binding;
    const XrRbcCallableEntry *entry;
    const uint8_t *captures;
    size_t frame_captures;
} RbcFunctionCall;
static XrtManagedStatus rbc_value_function_read(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,uint32_t value,XrtFunction *output) {
    RbcValuePayload payload;
    XrtManagedStatus status=rbc_value_payload(module,stack,rbc_value_origin(activation,value),&payload);
    if (status!=XRT_MANAGED_OK) return status;
    if (payload.size!=sizeof(*output) || payload.lanes) return XRT_MANAGED_ARGUMENT;
    memcpy(output,payload.address,sizeof(*output));
    return XRT_MANAGED_OK;
}
static XrtManagedStatus rbc_value_function_target(const XrRbcModule *module,XrRbcStack *stack,
    const XrtFunction *binding,uint32_t signature,RbcFunctionCall *output) {
    const XrRbcCallableEntry *entries=xr_rbc_module_callable_entries(module);
    size_t count=xr_rbc_module_callable_entry_count(module);
    if (!entries || !binding->entry || count>SIZE_MAX/sizeof(*entries)) return XRT_MANAGED_ARGUMENT;
    uintptr_t first=(uintptr_t)entries,address=(uintptr_t)binding->entry;
    if (address<first || address-first>=count*sizeof(*entries) ||
        (address-first)%sizeof(*entries)) return XRT_MANAGED_ARGUMENT;
    size_t index=(address-first)/sizeof(*entries);
    const XrRbcCallableEntry *entry=&entries[index];
    bool assertion=entry->kind==XR_RBC_CALLABLE_ENTRY_ASSERT_CONDITION;
    const XrRbcCodeOwner *producer=xr_rbc_module_code_owner(module);
    if (assertion) {
        if (!producer || entry->producer!=producer || entry->physical_function!=UINT32_MAX ||
            !rbc_callable_accepts(module,signature,entry->callable)) return XRT_MANAGED_ARGUMENT;
    } else if (entry->kind!=XR_RBC_CALLABLE_ENTRY_CODE || entry->physical_function!=index ||
        index>=module->function_count || !rbc_callable_accepts(module,signature,module->functions[index].callable))
        return XRT_MANAGED_ARGUMENT;
    *output=(RbcFunctionCall){.binding=*binding,.entry=entry,.frame_captures=SIZE_MAX};
    if (binding->owner) {
        if (!producer || entry->producer!=producer || !entry->environment_type)
            return XRT_MANAGED_ARGUMENT;
        /* Introductions receive and validate the genuine Env once. Invocation
         * borrows that admitted value; it does not repeat plan/capture walks. */
        output->captures=(const uint8_t *)binding->environment;
        return output->captures ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
    }
    if (assertion) return binding->environment==SIZE_MAX ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
    const XrRbcFunction *target=&module->functions[index];
    if (!target->capture_count && !target->state_capture)
        return binding->environment==SIZE_MAX ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
    if (binding->environment<XR_RBC_FUNCTION_LANES) return XRT_MANAGED_ARGUMENT;
    size_t prefix=binding->environment-XR_RBC_FUNCTION_LANES;
    for (size_t n=stack->depth;n--;) {
        const XrRbcFrame *frame=&stack->frames[n];
        const XrRbcFunction *function=&module->functions[frame->function];
        if (!frame->aggregate_base) continue;
        size_t offset,total;
        if (!xr_rbc_activation_size(function,&offset,&total)) return XRT_MANAGED_ARGUMENT;
        size_t base=frame->aggregate_base-1+offset/8;
        for (uint32_t e=0;e<function->environment_count;++e) {
            XrRbcEnvironment home=function->environments[e];
            if (home.producer!=UINT32_MAX || home.callee!=index || base+home.offset/8!=prefix) continue;
            XrtFunction canonical; memcpy(&canonical,stack->spills+prefix,sizeof(canonical));
            if (canonical.entry!=binding->entry || canonical.owner || canonical.environment!=binding->environment)
                return XRT_MANAGED_ARGUMENT;
            output->frame_captures=binding->environment;
            return XRT_MANAGED_OK;
        }
    }
    return XRT_MANAGED_ARGUMENT;
}
static XrtManagedStatus rbc_value_function_alias(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,uint32_t source) {
    bool leave=access.operation==XR_RBC_VALUE_FUNCTION_SCOPE_EXIT;
    const XrLirHomeEvent *event=rbc_value_event(activation->code,access.index,leave ?
        XR_LIR_OWNERSHIP_CALLABLE_SCOPE_EXIT : XR_LIR_OWNERSHIP_CALLABLE_VALUE);
    if (!event) return XRT_MANAGED_ARGUMENT;
    if (access.source) {
        RbcValueOrigin from=rbc_value_origin(activation,access.source),to={activation,access.result};
        XrtManagedStatus status;
        if (event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE) status=rbc_value_replace(module,stack,activation,to,from,false);
        else if (event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY || event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_BORROW) {
            RbcValuePayload payload;
            status=rbc_value_payload(module,stack,from,&payload);
            if (status==XRT_MANAGED_OK) status=rbc_value_inline_replace(module,stack,to,payload.address);
        } else return XRT_MANAGED_ARGUMENT;
        if (status!=XRT_MANAGED_OK) return status;
    }
    if (access.other) {
        XrRbcStatus status=xr_rbc_closure_scope_step(module,stack,&module->functions[activation->function],
            (XrRbcAggregateAccess){.operation=leave ? XR_RBC_FUNCTION_SCOPE_LEAVE : XR_RBC_FUNCTION_BIND,
                .destination=access.other},NULL,source);
        if (status!=XR_RBC_OK) return XRT_MANAGED_ARGUMENT;
    }
    return XRT_MANAGED_OK;
}
static XrtManagedStatus rbc_value_function_captures(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *child,const RbcFunctionCall *call) {
    const XrRbcFunction *target=&module->functions[call->entry->physical_function];
    const XrtValueImageType *environment=call->binding.owner ?
        &child->image->types[call->entry->environment_type-1] : NULL;
    size_t offset=0;
    for (uint32_t p=0;p<target->capture_count;++p) {
        uint32_t value=child->code->parameters[p];
        XrRbcCallableValue capture=target->captures[p];
        const uint8_t *bytes;
        XrtValueKind kind=XRT_VALUE_UNIT;
        if (environment) {
            XrtValueImageSlot slot=child->image->slots[environment->first_capture+p];
            bytes=call->captures+slot.offset;
            kind=child->image->types[slot.type-1].kind;
        } else bytes=(const uint8_t *)(stack->spills+call->frame_captures)+offset;
        if (capture.kind==XR_RBC_CALLABLE_FUNCTION) {
            XrtFunction binding; memcpy(&binding,bytes,sizeof(binding));
            RbcFunctionCall checked;
            XrtManagedStatus status=rbc_value_function_target(module,stack,&binding,capture.type+1,&checked);
            if (status!=XRT_MANAGED_OK || (environment && !binding.owner)) return XRT_MANAGED_ARGUMENT;
            child->parameter_storage[p].function=binding;
            child->borrowed_parameters[p]=(const uint8_t *)&child->parameter_storage[p].function;
            offset+=sizeof(binding);
        } else if (capture.kind==XR_RBC_CALLABLE_CELL) {
            if (environment && kind!=XRT_VALUE_CELL) return XRT_MANAGED_ARGUMENT;
            XrtManagedHeader *cell=NULL; memcpy(&cell,bytes,sizeof(cell));
            if (!cell) return XRT_MANAGED_ARGUMENT;
            /* The physical Frame cache can move with the spill bank. Keep only
             * these borrowed natural bytes in the stable callee activation. */
            child->parameter_storage[p].cell=cell;
            child->borrowed_parameters[p]=(const uint8_t *)&child->parameter_storage[p].cell;
            offset+=sizeof(cell);
        } else if (capture.kind==XR_RBC_CALLABLE_REFERENCE) {
            const XrLirValueHome *home=rbc_value_home(child->code,value);
            if (!environment || capture.type!=XR_RBC_MAP_STRING_I64 || kind!=XRT_VALUE_MAP_STRING_I64 ||
                !rbc_value_map(module,child->code,value,false) || home->parameter!=p+1 ||
                home->offset!=SIZE_MAX || home->first_owner!=SIZE_MAX ||
                home->temporary_offset!=SIZE_MAX || home->temporary_first_owner!=SIZE_MAX) return XRT_MANAGED_ARGUMENT;
            const XrtManagedHeader *map=NULL; memcpy(&map,bytes,sizeof(map));
            if (!map) return XRT_MANAGED_ARGUMENT;
            /* The invocation's real Fn keeper retains this immutable Env across
             * callee allocation and return. Its Map field is a borrowed formal,
             * not a register owner or a new mutable binding. */
            child->borrowed_parameters[p]=bytes;
            child->parameter_storage[p].map=(RbcMapParameter){.address=bytes};
            offset+=sizeof(map);
        } else {
            XrRbcRegister *output=rbc_value_register(stack,child,value);
            uint64_t bits=0;
            if (!output || (environment ? !rbc_state_scalar_load(kind,bytes,&bits) : false)) return XRT_MANAGED_ARGUMENT;
            if (!environment) memcpy(&bits,bytes,8);
            if (capture.kind==XR_RBC_CALLABLE_BINDING ? !xr_rbc_cell(stack,module,bits,XR_RBC_BITS) :
                !xr_rbc_callable_scalar_valid(capture,bits)) return XRT_MANAGED_ARGUMENT;
            output->bits=bits; offset+=8;
        }
    }
    if (target->state_capture) {
        if (environment || !target->parameter_count) return XRT_MANAGED_ARGUMENT;
        uint32_t value=child->code->parameters[target->parameter_count-1];
        XrRbcRegister *output=rbc_value_register(stack,child,value);
        if (!output) return XRT_MANAGED_ARGUMENT;
        memcpy(&output->state,(const uint8_t *)(stack->spills+call->frame_captures)+offset,sizeof(output->state));
        if (!output->state) return XRT_MANAGED_ARGUMENT;
    }
    return XRT_MANAGED_OK;
}
static XrtManagedStatus rbc_value_function_return(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access) {
    const XrLirHomeEvent *event=rbc_value_event(activation->code,access.index,XR_LIR_OWNERSHIP_RETURN_TRANSFER);
    if (!event) return XRT_MANAGED_ARGUMENT;
    RbcValueOrigin from=rbc_value_origin(activation,access.source);
    RbcValuePayload payload;
    XrtManagedStatus status=rbc_value_payload(module,stack,from,&payload);
    if (status!=XRT_MANAGED_OK) return status;
    if (stack->depth>1) {
        RbcValueActivation *caller=activation->previous;
        if (!activation->receiving_call || !caller || caller->depth+1!=activation->depth) return XRT_MANAGED_ARGUMENT;
        uint32_t target=stack->frames[stack->depth-1].destination;
        if (event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY)
            return rbc_value_inline_replace(module,stack,(RbcValueOrigin){caller,target},payload.address);
        if (event->transfer_mode!=XR_LIR_OWNERSHIP_TRANSFER_MOVE) return XRT_MANAGED_ARGUMENT;
        return rbc_value_replace(module,stack,activation,(RbcValueOrigin){caller,target},from,false);
    }
    XrtFunction binding; memcpy(&binding,payload.address,sizeof(binding));
    if (!stack->function_result || stack->function_result->entry || stack->function_result->owner ||
        !binding.owner || !module->code_lease.owner) return XRT_MANAGED_ARGUMENT;
    if (event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_COPY) {
        if (!xrt_managed_retain(binding.owner)) return XRT_MANAGED_REFCOUNT;
    } else if (event->transfer_mode==XR_LIR_OWNERSHIP_TRANSFER_MOVE) {
        const XrLirValueHome *home=rbc_value_home(from.activation->code,from.value);
        if (from.activation!=activation || !home || home->parameter || home->owner_count!=1) return XRT_MANAGED_ARGUMENT;
        XrtManagedHeader *taken=NULL;
        status=xrt_managed_slot_take(activation->frame,home->first_owner,&taken);
        if (status!=XRT_MANAGED_OK) return status;
        XR_CHECK(taken==binding.owner,"a callable return transfers its actual stable owner leaf");
        XrtFunction empty={0}; memcpy(payload.address,&empty,offsetof(XrtFunction,owner));
    } else return XRT_MANAGED_ARGUMENT;
    *stack->function_result=binding;
    stack->slots[0]=(XrRbcRegister){0};
    return XRT_MANAGED_OK;
}
