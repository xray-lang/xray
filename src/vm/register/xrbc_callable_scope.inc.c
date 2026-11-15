/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_callable_scope.inc.c - Actual lexical scopes for borrowed callables
 *
 * KEY CONCEPT:
 *   Scope geometry remains real even when values carry no heap owners. Static
 *   code tokens may escape, while active-frame environments remain borrowed.
 */
static bool xr_rbc_closure_scope_reserve(XrRbcStack *stack) {
    if (stack->closure_scope_count == SIZE_MAX) return false;
    void *storage = stack->closure_scopes;
    if (!xr_rbc_grow(&storage,&stack->closure_scope_capacity,
            stack->closure_scope_count + 1,sizeof(*stack->closure_scopes))) return false;
    stack->closure_scopes = storage;
    return true;
}
static XrRbcStatus xr_rbc_closure_scope_step(const XrRbcModule *module, XrRbcStack *stack,
    const XrRbcFunction *function, XrRbcAggregateAccess access, uint64_t *token, uint32_t source) {
    (void)source;
    XrRbcFunctionScopes shape = rbc_function_scopes(module,function);
    size_t count = stack->closure_scope_count;
    uint32_t current = count && stack->closure_scopes[count - 1].depth == stack->depth ?
        stack->closure_scopes[count - 1].id : 0;
    if (!access.destination || access.destination > shape.count) return XR_RBC_INVALID;
    if (access.operation == XR_RBC_FUNCTION_SCOPE_ENTER) {
        if (access.source != current || shape.parents[access.destination - 1] != current)
            return XR_RBC_INVALID;
        if (!xr_rbc_closure_scope_reserve(stack)) return XR_RBC_OUT_OF_MEMORY;
        stack->closure_scopes[stack->closure_scope_count++] = (XrRbcClosureScope){stack->depth,access.destination};
        return XR_RBC_OK;
    }
    if (token) {
        uint32_t callee; size_t environment;
        if (!xr_rbc_callable_target(module,stack,*token,&callee,&environment) ||
            !rbc_callable_accepts(module,access.slot,module->functions[callee].callable)) return XR_RBC_INVALID;
    }
    if (access.operation == XR_RBC_FUNCTION_BIND) {
        size_t scope = count;
        while (scope && (stack->closure_scopes[scope - 1].depth != stack->depth ||
                stack->closure_scopes[scope - 1].id != access.destination)) --scope;
        return scope ? XR_RBC_OK : XR_RBC_INVALID;
    }
    if (access.operation != XR_RBC_FUNCTION_SCOPE_LEAVE || !count || current != access.destination)
        return XR_RBC_INVALID;
    uint32_t parent = shape.parents[current - 1];
    bool implicit = count == 1 || stack->closure_scopes[count - 2].depth != stack->depth;
    if ((!implicit && stack->closure_scopes[count - 2].id != parent) || (implicit && parent))
        return XR_RBC_INVALID;
    --stack->closure_scope_count;
    return XR_RBC_OK;
}
static XrRbcStatus xr_rbc_closure_scope_return(const XrRbcModule *module, XrRbcStack *stack,
    uint64_t *token, uint32_t signature, uint32_t source) {
    (void)source;
    if (token) {
        uint32_t callee; size_t environment;
        if (!signature || (*token & 1u) || !xr_rbc_callable_target(module,stack,*token,&callee,&environment) ||
            !rbc_callable_accepts(module,signature,module->functions[callee].callable)) return XR_RBC_INVALID;
    }
    while (stack->closure_scope_count && stack->closure_scopes[stack->closure_scope_count - 1].depth == stack->depth)
        --stack->closure_scope_count;
    return XR_RBC_OK;
}
