/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static bool xr_rbc_captured_cell_value(const XrRbcModule *module, const XrRbcStack *stack,
    XrRbcRegisterKind kind, uint64_t bits, XrXirType type, XrXirValue *output) {
    if (kind == XR_RBC_CAPTURED_I64_CELL) {
        size_t index;
        if (!xr_rbc_closure_slot(stack, bits, &index)) return false;
        *output = stack->closures[index];
        return xr_xir_value_argument(output, module->closure_types, type);
    }
    return kind == XR_RBC_CAPTURED_I64_BORROW &&
        xr_xir_value_from_payload(module->closure_types, type, (int64_t)bits, output);
}
static XrRbcStatus xr_rbc_captured_cell_step(const XrRbcModule *module, XrRbcStack *stack,
    const XrRbcFunction *function, XrRbcAggregateAccess access, uint32_t source) {
    XrRbcFrame *frame = &stack->frames[stack->depth - 1];
    XrXirValue value = {0};
    if (access.operation != XR_RBC_CAPTURED_CELL_NEW &&
        !xr_rbc_captured_cell_value(module, stack, rbc_register_kind(function, access.source),
            stack->slots[frame->base + access.source].bits, (XrXirType)access.slot, &value)) return XR_RBC_INVALID;
    if (access.operation == XR_RBC_CAPTURED_CELL_READ) {
        XrXirValue result = {0};
        XrXirValueStatus status = xr_xir_cell_read(&value, &result);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
        memcpy(&stack->slots[frame->base + access.result.index].bits, &result.payload, sizeof(result.payload));
        return XR_RBC_OK;
    }
    if (access.operation == XR_RBC_CAPTURED_CELL_WRITE) {
        XrXirValue replacement = {XR_XIR_I64, 0, 0};
        memcpy(&replacement.payload, &stack->slots[frame->base + access.window].bits, sizeof(replacement.payload));
        XrXirValueStatus status = xrt_callable_cell_write(stack->closure_code, stack->closure_domain,
            (XrXirType)access.slot, &value, &replacement);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
        return XR_RBC_OK;
    }
    if (!xr_rbc_closure_scope_enter(stack) || !xr_rbc_closure_owner_reserve(stack)) return XR_RBC_OUT_OF_MEMORY;
    size_t scope = stack->closure_scope_count - 1;
    size_t existing = 0;
    uint32_t binding = UINT32_MAX;
    if (access.operation == XR_RBC_CAPTURED_CELL_BIND) {
        size_t selected = stack->closure_scope_count;
        while (selected && (stack->closure_scopes[selected - 1].depth != stack->depth ||
                stack->closure_scopes[selected - 1].id != access.destination)) --selected;
        if (!selected) return XR_RBC_INVALID;
        scope = selected - 1;
        binding = access.window;
        existing = stack->closure_scopes[scope].first;
        while (existing && stack->closure_owners[existing - 1].binding != binding)
            existing = stack->closure_owners[existing - 1].next;
        if (existing && stack->closure_owners[existing - 1].generation == UINT32_C(0x3fffffff))
            xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
    }
    XrXirValueStatus status;
    if (access.operation == XR_RBC_CAPTURED_CELL_NEW) {
        if (!stack->closure_domain) {
            status = xr_xir_domain_new(UINT64_MAX, &stack->closure_domain);
            if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
        }
        XrXirValue initial = {XR_XIR_I64, 0, 0};
        memcpy(&initial.payload, &stack->slots[frame->base + access.source].bits, sizeof(initial.payload));
        status = xrt_callable_cell_new(stack->closure_code, stack->closure_domain,
            (XrXirType)access.slot, &initial, &stack->closure_pending);
    } else status = xr_xir_value_copy(&value, &stack->closure_pending);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    uint64_t token;
    if (existing) {
        size_t index = existing - 1;
        XrXirValue previous = stack->closures[index];
        stack->closures[index] = stack->closure_pending;
        stack->closure_pending = (XrXirValue){0};
        uint32_t generation = ++stack->closure_owners[index].generation;
        token = (UINT64_C(1) << 63) | ((uint64_t)generation << 33) | ((uint64_t)existing << 1);
        xr_xir_value_drop(&previous);
    } else token = xr_rbc_closure_owner_commit(stack, scope, binding);
    stack->slots[frame->base + access.result.index].bits = token;
    return XR_RBC_OK;
}
