/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_callable_scope_transfer.inc.c - Scope geometry in the existing CFG rows
 */
static size_t rbc_flow_state_bytes(const XrRbcModule *module, const XrRbcFunction *function) {
    size_t homes = function->aggregate_value_count + function->environment_count;
    if (homes < function->aggregate_value_count) return SIZE_MAX;
    if (!rbc_function_scopes(module, function).count) return homes;
    size_t scopes = ((size_t)function->register_count + 1) * sizeof(uint32_t);
    if (homes > SIZE_MAX - 3u || ((homes + 3u) & ~(size_t)3u) > SIZE_MAX - scopes) return SIZE_MAX;
    return ((homes + 3u) & ~(size_t)3u) + scopes;
}
static uint32_t *rbc_scope_state(const RbcStringAdmission *state) {
    if (!rbc_function_scopes(state->module, state->function).count) return NULL;
    size_t homes = state->function->aggregate_value_count + state->function->environment_count;
    return (uint32_t *)(state->aggregates + ((homes + 3u) & ~(size_t)3u));
}
static uint32_t rbc_scope_current(const RbcStringAdmission *state) {
    const uint32_t *scopes = rbc_scope_state(state);
    return scopes ? scopes[0] : 0;
}
static bool rbc_scope_captured_cells(const XrRbcFunction *function) {
    for (uint32_t r = 0; r < function->register_count; ++r) {
        XrRbcRegisterKind kind = rbc_register_kind(function, r);
        if (kind == XR_RBC_CAPTURED_CELL || kind == XR_RBC_CAPTURED_CELL_BORROW) return true;
    }
    return false;
}
static void rbc_scope_define(RbcStringAdmission *state, uint32_t reg, uint32_t scope) {
    uint32_t *scopes = rbc_scope_state(state);
    if (scopes) scopes[reg + 1] = scope;
}
static bool rbc_scope_contains(XrRbcFunctionScopes scopes, uint32_t parent, uint32_t child) {
    for (uint64_t steps = 0; steps <= scopes.count && child <= scopes.count; ++steps) {
        if (parent == child) return true;
        if (!child) break;
        child = scopes.parents[child - 1];
    }
    return false;
}
static int rbc_scope_binding_compare(const void *left, const void *right) {
    const uint32_t *a = left, *b = right;
    if (a[0] != b[0]) return a[0] < b[0] ? -1 : 1;
    return (a[1] > b[1]) - (a[1] < b[1]);
}
static bool rbc_callable_scope_transfer(RbcStringAdmission *state, XrRbcAggregateAccess access) {
    XrRbcFunctionScopes shape = rbc_function_scopes(state->module, state->function);
    uint32_t *scopes = rbc_scope_state(state);
    if (!scopes) return false;
    uint32_t current = scopes[0];
    if (access.operation == XR_RBC_FUNCTION_SCOPE_ENTER) {
        if (access.source != current) return false;
        scopes[0] = access.destination;
        return true;
    }
    uint8_t kind = RBC_EMPTY;
    uint32_t signature = 0;
    if (access.count) {
        kind = state->state[access.source];
        signature = state->origin[access.source];
        if (!rbc_function_state(kind) || signature != access.slot) return false;
    }
    uint32_t receiving = access.destination;
    uint32_t borrowed = access.count ? scopes[access.source + 1] : 0;
    bool borrowed_cells = kind == RBC_FUNCTION && rbc_scope_captured_cells(state->function);
    if (access.operation == XR_RBC_FUNCTION_SCOPE_LEAVE && access.destination == current)
        receiving = shape.parents[current - 1];
    if (borrowed_cells && !rbc_scope_contains(shape, borrowed, receiving)) return false;
    if (access.operation == XR_RBC_FUNCTION_SCOPE_LEAVE) {
        if (access.destination != current) return false;
        receiving = shape.parents[current - 1];
        for (uint32_t r = 0; r < state->function->register_count; ++r)
            if ((rbc_function_state(state->state[r]) || state->state[r] == RBC_CAPTURED_CELL ||
                    state->state[r] == RBC_ARRAY_REF_CAPTURED) &&
                scopes[r + 1] == current) state->state[r] = RBC_EMPTY;
        scopes[0] = receiving;
    } else if (!rbc_scope_contains(shape, access.destination, current)) return false;
    if (access.count) {
        uint32_t r = access.result.index;
        if (state->state[r] != RBC_EMPTY && !rbc_function_state(state->state[r])) return false;
        state->state[r] = kind;
        state->origin[r] = signature;
        scopes[r + 1] = borrowed_cells ? borrowed : receiving;
    }
    return true;
}
