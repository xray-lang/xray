/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_view_transfer.inc.c - Capabilities participate in the existing CFG owner
 */
static uint8_t rbc_view_state(XrRbcRegisterKind kind) {
    return kind == XR_RBC_VIEW_FRAME ? RBC_VIEW_FRAME :
        kind == XR_RBC_VIEW_CELL ? RBC_VIEW_CELL : RBC_VIEW_SLICE;
}
static bool rbc_view_transfer(RbcStringAdmission *s, uint32_t index) {
    const XrRbcViewAccess *row = &s->function->view_accesses[index];
    const RbcViewSignature *shape = &rbc_view_signatures[row->operation];
    if (row->operation == XR_RBC_VIEW_FRAME_ENTER) {
        if (rbc_array_state(s, row->result) != RBC_EMPTY) return false;
        rbc_array_define(s, row->result, RBC_VIEW_FRAME, row->result.index + 1);
        return true;
    }
    for (uint32_t p = 0; p < row->count; ++p) {
        uint8_t state = rbc_array_state(s, row->operands[p]);
        XrRbcRegisterKind kind = shape->parameters[p];
        if (xr_rbc_view_kind(kind) ? state != rbc_view_state(kind) :
            rbc_managed_kind(kind) ? !rbc_string_state(state) :
            kind == XR_RBC_STATE ? state != RBC_STATE_BORROWED :
            kind == XR_RBC_STATE_REF ? state != RBC_STATE_REFERENCE : state != RBC_BITS) return false;
    }
    if (row->operation == XR_RBC_VIEW_CELL_IMPORT) {
        XrRbcArrayLocation reference = row->operands[2];
        uint32_t origin = reference.spill ? s->spills[reference.index].origin : s->origin[reference.index];
        if (xr_rbc_state_kind(s->module, origin) != XRT_MODULE_CELL_ARRAY_I64 || !(origin & 1u)) return false;
    }
    if (row->operation == XR_RBC_VIEW_FRAME_LEAVE) {
        for (uint32_t r = 0; r < s->function->register_count; ++r)
            if (s->state[r] == RBC_VIEW_FRAME || s->state[r] == RBC_VIEW_CELL || s->state[r] == RBC_VIEW_SLICE)
                s->state[r] = RBC_EMPTY;
        return true;
    }
    if (!shape->produces) return true;
    uint8_t destination = rbc_array_state(s, row->result);
    if (destination == RBC_STRING_OWNED || destination == RBC_CELL_OWNED) return false;
    uint8_t state = xr_rbc_view_kind(shape->result) ? rbc_view_state(shape->result) :
        rbc_managed_kind(shape->result) ? RBC_STRING_OWNED :
        shape->result == XR_RBC_STATE_REF ? RBC_STATE_REFERENCE : RBC_BITS;
    uint32_t origin = 0;
    if (row->operation == XR_RBC_VIEW_RETURN) state = RBC_VIEW_RETURNED;
    if (row->operation == XR_RBC_VIEW_CELL_CONSTRUCT) origin = xr_rbc_state_shape(s->module, (uint32_t)row->bits);
    rbc_array_define(s, row->result, state, origin);
    return true;
}
