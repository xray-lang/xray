/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_boxed_transfer.inc.c - Canonical ownership across actual bytecode edges
 */
static bool rbc_boxed_define(RbcStringAdmission *s, XrRbcArrayLocation result) {
    if (!rbc_array_destination(s, result)) return false;
    rbc_array_define(s, result, rbc_array_location_kind(s->function, result) == XR_RBC_BITS ? RBC_BITS : RBC_STRING_OWNED, 0);
    return true;
}
static bool rbc_boxed_transfer(RbcStringAdmission *s, XrRbcAggregateAccess access) {
    const XrRbcArrayLocation *args = access.count ? s->function->aggregate_operands + access.first : NULL;
    if (access.operation == XR_RBC_BOXED_TAKE) {
        if (s->state[access.source] != RBC_STRING_OWNED ||
            (rbc_register_kind(s->function, access.source) == XR_RBC_CALL_WINDOW && s->origin[access.source] != access.slot) ||
            !rbc_boxed_define(s, access.result)) return false;
        s->state[access.source] = RBC_EMPTY;
        rbc_string_forget_borrows(s, access.source);
        return true;
    }
    if (access.operation >= XR_RBC_BOXED_STATE_READ) {
        bool read = access.operation == XR_RBC_BOXED_STATE_READ;
        bool construct = access.operation == XR_RBC_BOXED_STATE_CONSTRUCT;
        bool initialize = access.operation == XR_RBC_BOXED_STATE_INITIALIZE;
        uint8_t state = rbc_array_state(s, args[0]);
        if (!read && !rbc_array_source(s, args[1])) return false;
        if (construct || initialize) {
            if (state != RBC_STATE_BORROWED) return false;
            if (construct) {
                s->state[access.result.index] = RBC_STATE_REFERENCE;
                s->origin[access.result.index] = xr_rbc_state_shape(s->module, access.destination);
            }
            return true;
        }
        uint32_t origin = s->origin[args[0].index];
        if (state != RBC_STATE_REFERENCE || !origin || !rbc_boxed_state_type(s->module, (origin >> 1) - 1, access.slot)) return false;
        return read ? rbc_boxed_define(s, access.result) : (origin & 1u) != 0;
    }
    for (uint32_t p = 0; p < access.count; ++p)
        if (rbc_array_location_kind(s->function, args[p]) == XR_RBC_BITS ? rbc_array_state(s, args[p]) != RBC_BITS :
            !rbc_array_source(s, args[p])) return false;
    if (access.operation == XR_RBC_BOXED_DROP) {
        if (rbc_array_state(s, args[0]) != RBC_STRING_OWNED) return false;
        rbc_array_define(s, args[0], RBC_EMPTY, 0);
        if (!args[0].spill) rbc_string_forget_borrows(s, args[0].index);
        return true;
    }
    return rbc_boxed_define(s, access.result);
}
