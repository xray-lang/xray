/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_callable_state.inc.c - Retained State snapshots before cold code checks
 */
static XrtManagedStatus rbc_value_function_receive(const XrRbcModule *module,
    const XrtFunction *binding,uint32_t expected) {
    const uint32_t *types=xr_rbc_module_type_callables(module);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    const XrRbcCodeOwner *code=xr_rbc_code_owner_borrow((XrtManagedLease){module->code_lease.owner,
        module->code_lease.retain,module->code_lease.release});
    if (!code || !types || !image || !expected || expected>image->type_count ||
        image->types[expected-1].kind!=XRT_VALUE_FUNCTION) return XRT_MANAGED_ARGUMENT;
    RbcOwnedFunction received={0};
    XrtManagedStatus status=rbc_owned_function_borrow(binding,&received);
    if (status!=XRT_MANAGED_OK) return status;
    return received.code==code && rbc_callable_accepts(module,types[expected-1],
        code->functions[received.function].callable) ? XRT_MANAGED_OK : XRT_MANAGED_ARGUMENT;
}
static XrtManagedStatus rbc_value_function_state(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access) {
    bool read=access.operation==XR_RBC_VALUE_STATE_READ;
    uint32_t value=read ? access.result : access.other;
    const XrLirValueHome *home=rbc_value_home(activation->code,value);
    XrRbcRegister *operand=rbc_value_register(stack,activation,access.source);
    if (!home || !operand) return XRT_MANAGED_ARGUMENT;
    XrtFunction binding={0};
    XrtManagedStatus status;
    if (read) {
        if (home->transport!=XR_LIR_CALLABLE_TRANSPORT_OWNED || home->owner_count!=1)
            return XRT_MANAGED_ARGUMENT;
        const void *borrow=NULL;
        status=xrt_module_borrow(operand->state_ref->reference,&borrow);
        if (status!=XRT_MANAGED_OK) return status;
        memcpy(&binding,borrow,sizeof(binding));
        XrtExternalOwner external;
        XrtPlace temporary;
        status=xrt_module_external_owner(operand->state_ref->reference,0,&external);
        if (status==XRT_MANAGED_OK) status=rbc_value_place((RbcValueOrigin){activation,value},true,0,&temporary);
        /* No allocator or user callback intervenes between the checked borrow
         * and retaining its true owner leaf into the already empty temp. */
        if (status==XRT_MANAGED_OK) status=xrt_place_copy_replace(temporary,
            (XrtPlace){.external=&external,.nulls=XRT_PLACE_NONNULL},NULL);
        if (status!=XRT_MANAGED_OK) return status;
        const XrtManagedHeader *retained=NULL;
        status=xrt_place_borrow(temporary,&retained);
        if (status==XRT_MANAGED_OK && retained!=binding.owner) status=XRT_MANAGED_ARGUMENT;
        if (status==XRT_MANAGED_OK) status=rbc_value_function_receive(module,&binding,home->type);
        if (status==XRT_MANAGED_OK) status=rbc_value_function_publish(module,stack,activation,value,&binding);
        if (status!=XRT_MANAGED_OK) {
            XrtManagedStatus cleanup=xrt_managed_slot_drop(activation->frame,home->temporary_first_owner);
            XR_CHECK(cleanup==XRT_MANAGED_OK,"a rejected State snapshot retires only its retained temporary");
        }
        return status;
    }
    status=rbc_value_function_read(module,stack,activation,value,&binding);
    if (status==XRT_MANAGED_OK) status=rbc_value_function_receive(module,&binding,home->type);
    if (status!=XRT_MANAGED_OK) return status;
    if (access.operation==XR_RBC_VALUE_STATE_WRITE)
        return xrt_module_replace(operand->state_ref->reference,&binding);
    if (access.operation==XR_RBC_VALUE_STATE_INITIALIZE)
        return xrt_module_initialize(operand->state,access.index,&binding);
    XrtModuleRef *reference=NULL;
    status=xrt_module_construct(operand->state,access.index,&binding,&reference);
    if (status==XRT_MANAGED_OK) {
        XrtModuleAccess *cache=&stack->module_accesses[(size_t)access.index*2];
        status=xrt_module_reference_cache(reference,cache);
        XrRbcRegister *result=rbc_value_register(stack,activation,access.result);
        if (status==XRT_MANAGED_OK && !result) status=XRT_MANAGED_ARGUMENT;
        if (status==XRT_MANAGED_OK) result->state_ref=cache;
    }
    return status;
}
