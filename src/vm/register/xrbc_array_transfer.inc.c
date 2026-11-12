/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_array_transfer.inc.c - Closed Array ownership through real storage banks
 */
static uint8_t rbc_array_state(const RbcStringAdmission *s, XrRbcArrayLocation location) {
    return location.spill ? s->spills[location.index].state : s->state[location.index];
}
static void rbc_array_define(RbcStringAdmission *s, XrRbcArrayLocation location, uint8_t state, uint32_t origin) {
    if (location.spill) s->spills[location.index] = (RbcSpillState){origin, state};
    else { s->state[location.index] = state; s->origin[location.index] = origin; }
}
static bool rbc_array_destination(const RbcStringAdmission *s, XrRbcArrayLocation location) {
    XrRbcRegisterKind kind = rbc_array_location_kind(s->function, location);
    uint8_t state = rbc_array_state(s, location);
    return kind == XR_RBC_BITS ? state == RBC_BITS || state == RBC_EMPTY : state == RBC_EMPTY;
}
static bool rbc_array_source(const RbcStringAdmission *s, XrRbcArrayLocation location) {
    return rbc_string_state(rbc_array_state(s, location));
}
#include "xrbc_array_ref_transfer.inc.c"
static bool rbc_array_transfer(RbcStringAdmission *s, uint32_t index) {
    XrRbcArrayAccess access = s->function->array_accesses[index];
    const XrRbcArrayLocation *operands = access.count ? s->function->array_operands + access.first : NULL;
    uint8_t source = access.count ? rbc_array_state(s, operands[0]) : RBC_EMPTY;
    if (access.operation == XR_RBC_ARRAY_BORROW || access.operation == XR_RBC_ARRAY_REF_READ ||
        access.operation == XR_RBC_ARRAY_REF_WRITE || access.operation == XR_RBC_ARRAY_REF_SET ||
        access.operation == XR_RBC_ARRAY_REF_PUSH) return rbc_array_ref_transfer(s, access, operands);
    if (access.operation == XR_RBC_ARRAY_NEW) {
        if (!rbc_array_destination(s, access.result)) return false;
        for (uint32_t p = 0; p < access.count; ++p) {
            if (access.kind != XR_RBC_ARRAY_STRING ? rbc_array_state(s, operands[p]) != RBC_BITS :
                !rbc_array_source(s, operands[p])) return false;
        }
        rbc_array_define(s, access.result, RBC_STRING_OWNED, 0);
        return true;
    }
    if (access.operation == XR_RBC_ARRAY_REPEAT) {
        if (!rbc_array_destination(s, access.result) || source != RBC_BITS) return false;
        if (access.kind != XR_RBC_ARRAY_STRING ? rbc_array_state(s, operands[1]) != RBC_BITS :
            !rbc_array_source(s, operands[1])) return false;
        rbc_array_define(s, access.result, RBC_STRING_OWNED, 0);
        return true;
    }
    bool cell = access.operation == XR_RBC_ARRAY_SET || access.operation == XR_RBC_ARRAY_PUSH ||
        access.operation == XR_RBC_ARRAY_CELL_READ || access.operation == XR_RBC_ARRAY_CELL_WRITE ||
        access.operation == XR_RBC_ARRAY_CELL_DROP;
    if (cell ? !rbc_cell_state(source) : !rbc_array_source(s, operands[0])) return false;
    if (access.operation == XR_RBC_ARRAY_CELL_NEW) {
        if (!rbc_array_destination(s, access.result)) return false;
        for (uint32_t r=0;r<s->function->register_count;++r)
            if (rbc_cell_state(s->state[r]) && s->origin[r]==access.cell+1) return false;
        rbc_array_define(s, access.result, RBC_CELL_OWNED, access.cell + 1);
        return true;
    }
    if (access.operation == XR_RBC_ARRAY_DROP || access.operation == XR_RBC_ARRAY_CELL_DROP ||
        access.operation == XR_RBC_ARRAY_TAKE) {
        if (source != (cell ? RBC_CELL_OWNED : RBC_STRING_OWNED)) return false;
        if (access.operation == XR_RBC_ARRAY_TAKE) {
            if (!rbc_array_destination(s, access.result)) return false;
            if (!operands[0].spill && rbc_register_kind(s->function, operands[0].index) == XR_RBC_CALL_WINDOW &&
                s->origin[operands[0].index] != (uint32_t)access.kind) return false;
            rbc_array_define(s, access.result, RBC_STRING_OWNED, 0);
        }
        uint32_t origin = operands[0].spill ? 0 : s->origin[operands[0].index];
        rbc_array_define(s, operands[0], RBC_EMPTY, 0);
        if (cell) {
            for (uint32_t r = 0; r < s->function->register_count; ++r)
                if ((rbc_cell_state(s->state[r]) || s->state[r] == RBC_ARRAY_REF_CELL) &&
                    s->origin[r] == origin) s->state[r] = RBC_EMPTY;
        } else if (!operands[0].spill) rbc_string_forget_borrows(s, operands[0].index);
        return true;
    }
    if (access.operation == XR_RBC_ARRAY_SET || access.operation == XR_RBC_ARRAY_GET)
        if (rbc_array_state(s, operands[1]) != RBC_BITS) return false;
    if (access.operation == XR_RBC_ARRAY_SET || access.operation == XR_RBC_ARRAY_PUSH) {
        XrRbcArrayLocation value = operands[access.operation == XR_RBC_ARRAY_SET ? 2 : 1];
        return access.kind != XR_RBC_ARRAY_STRING ? rbc_array_state(s, value) == RBC_BITS : rbc_array_source(s, value);
    }
    if (access.operation == XR_RBC_ARRAY_CELL_WRITE) return rbc_array_source(s, operands[1]);
    if (!rbc_array_destination(s, access.result)) return false;
    rbc_array_define(s, access.result, access.operation == XR_RBC_ARRAY_LEN ||
        (access.operation == XR_RBC_ARRAY_GET && access.kind != XR_RBC_ARRAY_STRING) ? RBC_BITS : RBC_STRING_OWNED, 0);
    return true;
}

static bool rbc_call_parameter_matches(const RbcStringAdmission *s, uint32_t actual, XrRbcRegisterKind formal) {
    uint8_t state = s->state[actual];
    uint32_t reference = xr_rbc_array_ref_family(formal);
    if (reference < XR_RBC_ARRAY_FAMILY_COUNT)
        return rbc_array_ref_state(state) &&
            rbc_array_ref_payload(s, state, s->origin[actual]) == xr_rbc_array_kind_at(reference);
    if (rbc_managed_kind(formal)) {
        uint32_t source = s->origin[actual];
        return state == RBC_STRING_BORROWED && source && source <= s->function->register_count &&
            rbc_register_kind(s->function, source - 1) == formal;
    }
    if (formal == XR_RBC_STRING_CELL || rbc_array_cell_kind(formal)) {
        if (!rbc_cell_state(state) || !s->origin[actual]) return false;
        uint32_t origin = s->origin[actual];
        if (origin >= 65537) return origin - 65537 < s->function->parameter_count &&
            rbc_register_kind(s->function, origin - 65537) == formal;
        XrRbcRegisterKind payload = formal == XR_RBC_STRING_CELL ? XR_RBC_STRING : rbc_array_cell_payload(formal);
        return origin <= s->function->cell_count && s->function->cell_kinds &&
            s->function->cell_kinds[origin - 1] == payload;
    }
    return formal == XR_RBC_STATE ? state == RBC_STATE_BORROWED : state == RBC_BITS;
}
