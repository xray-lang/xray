/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_array_ref_transfer.inc.c - Borrowed Array references retain true provenance
 */
static bool rbc_array_ref_state(uint8_t state) {
    return state == RBC_ARRAY_REF_CELL || state == RBC_ARRAY_REF_STATE || state == RBC_ARRAY_REF_FORMAL;
}
static XrRbcRegisterKind rbc_array_ref_payload(const RbcStringAdmission *s, uint8_t state, uint32_t origin) {
    if (state == RBC_ARRAY_REF_STATE) {
        uint32_t kind = xr_rbc_state_kind(s->module, origin);
        if ((origin & 1u) && kind == XRT_MODULE_STRING) return XR_RBC_STRING;
        return (origin & 1u) && xr_rbc_state_array(kind) ?
            xr_rbc_array_kind_at(kind - XRT_MODULE_ARRAY_I64) : XR_RBC_BITS;
    }
    if (origin >= 65537) {
        uint32_t parameter = origin - 65537;
        if (parameter >= s->function->parameter_count) return XR_RBC_BITS;
        XrRbcRegisterKind kind = rbc_register_kind(s->function, parameter);
        if (kind == XR_RBC_STRING_REF || kind == XR_RBC_STRING_CELL) return XR_RBC_STRING;
        return state == RBC_ARRAY_REF_FORMAL ? xr_rbc_array_kind_at(xr_rbc_array_ref_family(kind)) :
            rbc_array_cell_payload(kind);
    }
    return state == RBC_ARRAY_REF_CELL && origin && origin <= s->function->cell_count && s->function->cell_kinds ?
        s->function->cell_kinds[origin - 1] : XR_RBC_BITS;
}
static bool rbc_array_ref_transfer(RbcStringAdmission *s, XrRbcArrayAccess access,
    const XrRbcArrayLocation *operands) {
    XrRbcArrayLocation first = operands[0];
    uint8_t state = rbc_array_state(s, first);
    uint32_t origin = first.spill ? s->spills[first.index].origin : s->origin[first.index];
    if (access.operation == XR_RBC_ARRAY_BORROW) {
        if (access.result.spill) return false;
        uint8_t destination = s->state[access.result.index];
        if (destination != RBC_EMPTY && !rbc_array_ref_state(destination)) return false;
        if (rbc_cell_state(state)) state = RBC_ARRAY_REF_CELL;
        else if (state == RBC_STATE_REFERENCE) state = RBC_ARRAY_REF_STATE;
        else return false;
        if (rbc_array_ref_payload(s, state, origin) != access.kind) return false;
        rbc_array_define(s, access.result, state, origin);
        return true;
    }
    if (!rbc_array_ref_state(state) || rbc_array_ref_payload(s, state, origin) != access.kind) return false;
    if (access.operation == XR_RBC_ARRAY_REF_WRITE) return rbc_array_source(s, operands[1]);
    if (access.operation == XR_RBC_ARRAY_REF_SET || access.operation == XR_RBC_ARRAY_REF_PUSH) {
        bool set = access.operation == XR_RBC_ARRAY_REF_SET;
        if (set && rbc_array_state(s, operands[1]) != RBC_BITS) return false;
        return access.kind == XR_RBC_ARRAY_STRING ? rbc_array_source(s, operands[set ? 2 : 1]) :
            rbc_array_state(s, operands[set ? 2 : 1]) == RBC_BITS;
    }
    if (!rbc_array_destination(s, access.result)) return false;
    rbc_array_define(s, access.result, RBC_STRING_OWNED, 0);
    return true;
}
