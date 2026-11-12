/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_string_library_transfer.inc.c - String binding provenance in the existing CFG
 */
static bool rbc_string_library_transfer(RbcStringAdmission *s, XrRbcAggregateAccess access) {
    XrRbcArrayLocation receiver = s->function->aggregate_operands[access.first];
    uint8_t state = rbc_array_state(s, receiver);
    uint32_t origin = receiver.spill ? s->spills[receiver.index].origin : s->origin[receiver.index];
    if (access.count == 2 && !rbc_array_source(s, s->function->aggregate_operands[access.first + 1])) return false;
    if (access.slot == XR_LIR_STRING_OP_BORROW_CELL || access.slot == XR_LIR_STRING_OP_BORROW_STATE) {
        uint8_t next = rbc_cell_state(state) ? RBC_ARRAY_REF_CELL :
            state == RBC_STATE_REFERENCE ? RBC_ARRAY_REF_STATE : RBC_EMPTY;
        if (next == RBC_EMPTY || rbc_array_ref_payload(s, next, origin) != XR_RBC_STRING ||
            (rbc_array_state(s, access.result) != RBC_EMPTY && !rbc_array_ref_state(rbc_array_state(s, access.result))))
            return false;
        rbc_array_define(s, access.result, next, origin);
        return true;
    }
    if (access.slot >= XR_LIR_STRING_OP_READ_REF) {
        if (!rbc_array_ref_state(state) || rbc_array_ref_payload(s, state, origin) != XR_RBC_STRING) return false;
        if (access.slot != XR_LIR_STRING_OP_READ_REF) return true;
    } else if (access.slot == XR_LIR_STRING_OP_APPEND_CELL) return rbc_cell_state(state);
    else if (access.slot == XR_LIR_STRING_OP_APPEND_STATE)
        return state == RBC_STATE_REFERENCE && (origin & 1u) &&
            xr_rbc_state_kind(s->module, origin) == XRT_MODULE_STRING;
    else {
        if (state != RBC_STRING_OWNED) return false;
        if (!receiver.spill) rbc_string_forget_borrows(s, receiver.index);
        rbc_array_define(s, receiver, RBC_EMPTY, 0);
    }
    if (!rbc_array_destination(s, access.result)) return false;
    rbc_array_define(s, access.result, RBC_STRING_OWNED, 0);
    return true;
}
