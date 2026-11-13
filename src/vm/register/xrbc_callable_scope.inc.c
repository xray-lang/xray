/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_callable_scope.inc.c - Indexed owners and their actual scope membership
 */
static bool xr_rbc_closure_scope_reserve(XrRbcStack *stack) {
    if (stack->closure_scope_count == SIZE_MAX) return false;
    void *storage = stack->closure_scopes;
    if (!xr_rbc_grow(&storage, &stack->closure_scope_capacity,
            stack->closure_scope_count + 1, sizeof(*stack->closure_scopes))) return false;
    stack->closure_scopes = storage;
    return true;
}
static bool xr_rbc_closure_scope_enter(XrRbcStack *stack) {
    if (stack->closure_scope_count && stack->closure_scopes[stack->closure_scope_count - 1].depth == stack->depth)
        return true;
    if (!xr_rbc_closure_scope_reserve(stack)) return false;
    stack->closure_scopes[stack->closure_scope_count++] = (XrRbcClosureScope){stack->depth, 0, 0};
    return true;
}
static bool xr_rbc_closure_owner_reserve(XrRbcStack *stack) {
    if (stack->closure_free) return true;
    if (stack->closure_count >= UINT32_MAX) return false;
    size_t count = stack->closure_count + 1;
    void *storage = stack->closures;
    if (!xr_rbc_grow(&storage, &stack->closure_capacity, count, sizeof(*stack->closures))) return false;
    stack->closures = storage;
    storage = stack->closure_owners;
    if (!xr_rbc_grow(&storage, &stack->closure_owner_capacity, count, sizeof(*stack->closure_owners))) return false;
    stack->closure_owners = storage;
    return true;
}
static uint64_t xr_rbc_closure_owner_commit(XrRbcStack *stack, size_t scope, uint32_t binding) {
    size_t index;
    if (stack->closure_free) {
        index = stack->closure_free - 1;
        stack->closure_free = stack->closure_owners[index].next;
    } else {
        index = stack->closure_count++;
        stack->closure_owners[index].generation = 1;
    }
    XrRbcClosureOwner *owner = &stack->closure_owners[index];
    owner->binding = binding;
    size_t previous = 0, next = stack->closure_scopes[scope].first;
    while (next && stack->closure_owners[next - 1].binding > binding) {
        previous = next;
        next = stack->closure_owners[next - 1].next;
    }
    owner->next = next;
    if (previous) stack->closure_owners[previous - 1].next = index + 1;
    else stack->closure_scopes[scope].first = index + 1;
    stack->closures[index] = stack->closure_pending;
    stack->closure_pending = (XrXirValue){0};
    return (UINT64_C(1) << 63) | ((uint64_t)owner->generation << 33) | ((uint64_t)(index + 1) << 1);
}
static void xr_rbc_closure_scope_drop(XrRbcStack *stack, size_t scope) {
    size_t next = stack->closure_scopes[scope].first;
    stack->closure_scopes[scope].first = 0;
    while (next) {
        size_t index = next - 1;
        XrRbcClosureOwner *owner = &stack->closure_owners[index];
        next = owner->next;
        xr_xir_value_drop(&stack->closures[index]);
        if (owner->generation < UINT32_C(0x3fffffff)) {
            ++owner->generation;
            owner->next = stack->closure_free;
            stack->closure_free = index + 1;
        }
    }
}
static XrRbcStatus xr_rbc_closure_scope_step(const XrRbcModule *module, XrRbcStack *stack,
    const XrRbcFunction *function, XrRbcAggregateAccess access, uint64_t *token, uint32_t source) {
    XrRbcFunctionScopes shape = rbc_function_scopes(module, function);
    size_t count = stack->closure_scope_count;
    uint32_t current = count && stack->closure_scopes[count - 1].depth == stack->depth ?
        stack->closure_scopes[count - 1].id : 0;
    if (access.operation == XR_RBC_FUNCTION_SCOPE_ENTER) {
        if (access.source != current) return XR_RBC_INVALID;
        if (!xr_rbc_closure_scope_reserve(stack)) return XR_RBC_OUT_OF_MEMORY;
        stack->closure_scopes[stack->closure_scope_count++] = (XrRbcClosureScope){stack->depth, 0, access.destination};
        return XR_RBC_OK;
    }
    bool owned = token && (*token & (UINT64_C(1) << 63));
    size_t input = SIZE_MAX;
    if (token) {
        uint32_t callee;
        size_t environment;
        const XrXirValue *captures;
        if (!xr_rbc_callable_target(module, stack, *token, &callee, &environment, &captures) ||
            !rbc_callable_accepts(module, access.slot, module->functions[callee].callable) ||
            (owned && !xr_rbc_closure_slot(stack, *token, &input))) return XR_RBC_INVALID;
    }
    if (access.operation == XR_RBC_FUNCTION_BIND) {
        size_t scope = count;
        while (scope && (stack->closure_scopes[scope - 1].depth != stack->depth ||
                stack->closure_scopes[scope - 1].id != access.destination)) --scope;
        if (!scope) return XR_RBC_INVALID;
        --scope;
        size_t existing = stack->closure_scopes[scope].first;
        while (existing && stack->closure_owners[existing - 1].binding != access.window)
            existing = stack->closure_owners[existing - 1].next;
        if (existing && stack->closure_owners[existing - 1].generation == UINT32_C(0x3fffffff))
            xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
        if (!owned) {
            if (existing) {
                ++stack->closure_owners[existing - 1].generation;
                xr_xir_value_drop(&stack->closures[existing - 1]);
            }
            return XR_RBC_OK;
        }
        if (!existing && !xr_rbc_closure_owner_reserve(stack)) return XR_RBC_OUT_OF_MEMORY;
        XR_DCHECK(!stack->closure_pending.type, "one binding owns its pending replacement");
        XrXirValueStatus status = xr_xir_value_copy(&stack->closures[input], &stack->closure_pending);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
        if (existing) {
            size_t index = existing - 1;
            XrXirValue previous = stack->closures[index];
            stack->closures[index] = stack->closure_pending;
            stack->closure_pending = (XrXirValue){0};
            uint32_t generation = ++stack->closure_owners[index].generation;
            *token = (UINT64_C(1) << 63) | ((uint64_t)generation << 33) | ((uint64_t)existing << 1);
            xr_xir_value_drop(&previous);
        } else *token = xr_rbc_closure_owner_commit(stack, scope, access.window);
        return XR_RBC_OK;
    }
    if (!count || current != access.destination) return XR_RBC_INVALID;
    uint32_t parent = shape.parents[current - 1];
    bool implicit = count == 1 || stack->closure_scopes[count - 2].depth != stack->depth;
    if (!implicit && stack->closure_scopes[count - 2].id != parent) return XR_RBC_INVALID;
    if (implicit && parent) return XR_RBC_INVALID;
    if (owned) {
        if (!xr_rbc_closure_scope_reserve(stack) || !xr_rbc_closure_owner_reserve(stack))
            return XR_RBC_OUT_OF_MEMORY;
        XR_DCHECK(!stack->closure_pending.type, "one scope exit owns its protected result");
        XrXirValueStatus status = xr_xir_value_copy(&stack->closures[input], &stack->closure_pending);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    }
    xr_rbc_closure_scope_drop(stack, count - 1);
    --stack->closure_scope_count;
    if (owned) {
        if (implicit) stack->closure_scopes[stack->closure_scope_count++] = (XrRbcClosureScope){stack->depth, 0, 0};
        *token = xr_rbc_closure_owner_commit(stack, stack->closure_scope_count - 1, UINT32_MAX);
    }
    return XR_RBC_OK;
}
static XrRbcStatus xr_rbc_closure_scope_return(const XrRbcModule *module, XrRbcStack *stack,
    uint64_t *token, uint32_t signature, uint32_t source) {
    bool owned = token && (*token & (UINT64_C(1) << 63));
    if (token) {
        uint32_t callee;
        size_t environment;
        const XrXirValue *captures;
        if (!signature || (*token & 1u) || !xr_rbc_callable_target(module, stack, *token,
                &callee, &environment, &captures) ||
            !rbc_callable_accepts(module, signature, module->functions[callee].callable)) return XR_RBC_INVALID;
    }
    if (owned) {
        if (stack->depth < 2 || !xr_rbc_closure_scope_reserve(stack) || !xr_rbc_closure_owner_reserve(stack))
            return XR_RBC_OUT_OF_MEMORY;
        size_t index;
        if (!xr_rbc_closure_slot(stack, *token, &index)) return XR_RBC_INVALID;
        XR_DCHECK(!stack->closure_pending.type, "one return owns its independently retained pending value");
        XrXirValueStatus status = xr_xir_value_copy(&stack->closures[index], &stack->closure_pending);
        if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
    }
    while (stack->closure_scope_count && stack->closure_scopes[stack->closure_scope_count - 1].depth == stack->depth) {
        xr_rbc_closure_scope_drop(stack, stack->closure_scope_count - 1);
        --stack->closure_scope_count;
    }
    if (owned) {
        size_t parent = stack->depth - 1;
        if (!stack->closure_scope_count || stack->closure_scopes[stack->closure_scope_count - 1].depth != parent)
            stack->closure_scopes[stack->closure_scope_count++] = (XrRbcClosureScope){parent, 0, 0};
        *token = xr_rbc_closure_owner_commit(stack, stack->closure_scope_count - 1, UINT32_MAX);
    }
    return XR_RBC_OK;
}
