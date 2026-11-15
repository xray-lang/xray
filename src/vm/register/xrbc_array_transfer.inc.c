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
static bool rbc_call_parameter_matches(const RbcStringAdmission *s, uint32_t actual, XrRbcRegisterKind formal) {
    uint8_t state = s->state[actual];
    if (formal == XR_RBC_MAP_STRING_I64_REF || formal == XR_RBC_STRING_REF ||
        formal == XR_RBC_VIEW_CELL || formal == XR_RBC_SLICE_I64 ||
        xr_rbc_array_ref_family(formal) < XR_RBC_ARRAY_FAMILY_COUNT) return false;
    if (rbc_managed_kind(formal)) {
        uint32_t source = s->origin[actual];
        return state == RBC_STRING_BORROWED && source && source <= s->function->register_count &&
            rbc_register_kind(s->function, source - 1) == formal;
    }
    if (formal == XR_RBC_STRING_CELL || formal == XR_RBC_I64_BINDING || rbc_array_cell_kind(formal)) {
        if (!rbc_cell_state(state) || !s->origin[actual]) return false;
        uint32_t origin = s->origin[actual];
        if (formal == XR_RBC_I64_BINDING && origin == UINT32_MAX) return state == RBC_CELL_BORROWED;
        if (origin >= 65537) return origin - 65537 < s->function->parameter_count &&
            rbc_register_kind(s->function, origin - 65537) == formal;
        XrRbcRegisterKind payload = formal == XR_RBC_STRING_CELL ? XR_RBC_STRING :
            formal == XR_RBC_I64_BINDING ? XR_RBC_BITS : rbc_array_cell_payload(formal);
        return origin <= s->function->cell_count && s->function->cell_kinds &&
            s->function->cell_kinds[origin - 1] == payload;
    }
    return formal == XR_RBC_STATE ? state == RBC_STATE_BORROWED : state == RBC_BITS;
}
