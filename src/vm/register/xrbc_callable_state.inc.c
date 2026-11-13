/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_callable_state.inc.c - Persistent canonical values below scoped tokens
 */
static XrRbcStatus xr_rbc_callable_state_step(const XrRbcModule *module, XrRbcStack *stack,
    const XrRbcFunction *function, XrRbcAggregateAccess access, uint32_t source) {
    bool read = access.operation == XR_RBC_FUNCTION_STATE_READ;
    (void)function;
    bool construct = access.operation == XR_RBC_FUNCTION_STATE_CONSTRUCT;
    bool initialize = access.operation == XR_RBC_FUNCTION_STATE_INITIALIZE;
    XrRbcFrame *frame = &stack->frames[stack->depth - 1];
    XrtModuleAccess *reference = NULL;
    if (!construct && !initialize) {
        reference = stack->slots[frame->base + access.source].state_ref;
        xr_rbc_state_ready(reference, source);
    }
    if (read) {
        if (!xr_rbc_closure_scope_enter(stack) || !xr_rbc_closure_owner_reserve(stack))
            return XR_RBC_OUT_OF_MEMORY;
        XirObject *object = NULL;
        memcpy(&object, reference->payload, sizeof(object));
        if (!object) return XR_RBC_INVALID;
        XrXirValue value = {.type=(uint32_t)object->type, .payload=(int64_t)(intptr_t)object};
        const XrXirFunctionBinding *binding = xr_xir_function_binding(&value);
        const XrXirValue *captures;
        uint32_t count;
        if (!binding || binding->entry >= module->closure_entry_count ||
            !rbc_callable_accepts(module, access.slot,
                module->functions[module->closure_targets[binding->entry]].callable) ||
            xrt_callable_borrow(stack->closure_code, &value, binding->entry, &captures, &count) != XR_XIR_VALUE_OK)
            return XR_RBC_INVALID;
        XrXirValueStatus status = xr_xir_value_copy(&value, &stack->closure_pending);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
        stack->slots[frame->base + access.result.index].bits =
            xr_rbc_closure_owner_commit(stack, stack->closure_scope_count - 1, UINT32_MAX);
        return XR_RBC_OK;
    }
    uint64_t token = stack->slots[frame->base + access.window].bits;
    uint32_t callee;
    size_t environment;
    const XrXirValue *captures;
    if (!xr_rbc_callable_target(module, stack, token, &callee, &environment, &captures) ||
        environment != SIZE_MAX || !rbc_callable_accepts(module, access.slot, module->functions[callee].callable))
        return XR_RBC_INVALID;
    XirObject *object = NULL;
    bool temporary = !(token & (UINT64_C(1) << 63));
    if (temporary) {
        uint32_t entry = 0;
        while (entry < module->closure_entry_count && module->closure_targets[entry] != callee) ++entry;
        if (entry == module->closure_entry_count) return XR_RBC_INVALID;
        if (!stack->closure_domain) {
            XrXirValueStatus status = xr_xir_domain_new(UINT64_MAX, &stack->closure_domain);
            if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
        }
        XrXirValueStatus status = xrt_callable_new(stack->closure_code, stack->closure_domain,
            entry, NULL, 0, &stack->closure_pending);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
        object = (XirObject *)(intptr_t)stack->closure_pending.payload;
    } else {
        size_t index;
        if (!xr_rbc_closure_slot(stack, token, &index)) return XR_RBC_INVALID;
        object = (XirObject *)(intptr_t)stack->closures[index].payload;
    }
    XrXirValueStatus status;
    if (construct || initialize) {
        XrtModuleState *state = stack->slots[frame->base + access.source].state;
        XrtModuleRef *actual = NULL;
        status = initialize ? xrt_module_initialize(state, access.destination, &object) :
            xrt_module_construct(state, access.destination, &object, &actual);
        if (status == XR_XIR_VALUE_OK && construct) {
            XrtModuleAccess *cache = &stack->module_accesses[(size_t)access.destination * 2];
            status = xrt_module_reference_cache(actual, cache);
            if (status == XR_XIR_VALUE_OK) stack->slots[frame->base + access.result.index].state_ref = cache;
        }
    } else status = xrt_module_replace(reference->reference, &object);
    if (temporary) xr_xir_value_drop(&stack->closure_pending);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    return XR_RBC_OK;
}
