/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_map_transfer.inc.c - Map ownership through the existing CFG banks
 *
 * KEY CONCEPT:
 *   The existing CFG state transports Map owners and synchronous references.
 *   A reference preserves its actual Cell or State origin across each edge.
 */
static bool rbc_map_transfer(RbcStringAdmission *s, XrRbcArrayAccess a) {
    const XrRbcArrayLocation *args = a.count ? s->function->array_operands + a.first : NULL;
    uint8_t source = a.count ? rbc_array_state(s, args[0]) : RBC_EMPTY;
    if (a.operation == XR_RBC_MAP_STATE_READ || a.operation == XR_RBC_MAP_STATE_WRITE) {
        uint32_t origin = args[0].spill ? s->spills[args[0].index].origin : s->origin[args[0].index];
        if (source != RBC_STATE_REFERENCE || xr_rbc_state_kind(s->module, origin) != XRT_MODULE_MAP_STRING_I64)
            return false;
        if (a.operation == XR_RBC_MAP_STATE_WRITE) return (origin & 1u) && rbc_array_source(s, args[1]);
        if (!rbc_array_destination(s, a.result)) return false;
        rbc_array_define(s, a.result, RBC_STRING_OWNED, 0);
        return true;
    }
    bool reference = a.operation == XR_RBC_ARRAY_REF_READ || a.operation == XR_RBC_ARRAY_REF_WRITE ||
        a.operation == XR_RBC_ARRAY_REF_SET || a.operation == XR_RBC_MAP_REF_REMOVE || a.operation == XR_RBC_MAP_REF_CLEAR;
    if (a.operation == XR_RBC_ARRAY_BORROW || reference) {
        uint32_t origin = args[0].spill ? s->spills[args[0].index].origin : s->origin[args[0].index];
        if (a.operation == XR_RBC_ARRAY_BORROW) {
            if (s->state[a.result.index] != RBC_EMPTY && !rbc_array_ref_state(s->state[a.result.index])) return false;
            if (a.cell) {
                if (source != RBC_CAPTURED_CELL || origin != a.cell) return false;
                uint32_t *scopes = rbc_scope_state(s);
                if (scopes) scopes[a.result.index + 1] = scopes[args[0].index + 1];
                rbc_array_define(s, a.result, RBC_ARRAY_REF_CAPTURED, origin);
                return true;
            }
            source = rbc_cell_state(source) ? RBC_ARRAY_REF_CELL :
                source == RBC_STATE_REFERENCE ? RBC_ARRAY_REF_STATE : RBC_EMPTY;
            if (rbc_array_ref_payload(s, source, origin) != XR_RBC_MAP_STRING_I64) return false;
            rbc_array_define(s, a.result, source, origin);
            return true;
        }
        if (!rbc_array_ref_state(source) || rbc_array_ref_payload(s, source, origin) != XR_RBC_MAP_STRING_I64)
            return false;
    } else if (a.operation == XR_RBC_ARRAY_NEW) {
        if (!rbc_array_destination(s, a.result)) return false;
        for (uint32_t p = 0; p < a.count; ++p)
            if (p % 2 ? rbc_array_state(s, args[p]) != RBC_BITS : !rbc_array_source(s, args[p])) return false;
        rbc_array_define(s, a.result, RBC_STRING_OWNED, 0);
        return true;
    } else {
        bool cell = a.operation == XR_RBC_ARRAY_CELL_READ || a.operation == XR_RBC_ARRAY_CELL_WRITE ||
            a.operation == XR_RBC_ARRAY_CELL_DROP || a.operation == XR_RBC_ARRAY_SET ||
            a.operation == XR_RBC_MAP_REMOVE || a.operation == XR_RBC_MAP_CLEAR;
        if (cell ? !rbc_cell_state(source) : !rbc_string_state(source)) return false;
    }
    if (a.operation == XR_RBC_ARRAY_CELL_NEW) {
        if (!rbc_array_destination(s, a.result)) return false;
        for (uint32_t r = 0; r < s->function->register_count; ++r)
            if (rbc_cell_state(s->state[r]) && s->origin[r] == a.cell + 1) return false;
        rbc_array_define(s, a.result, RBC_CELL_OWNED, a.cell + 1);
        return true;
    }
    if (a.operation == XR_RBC_ARRAY_DROP || a.operation == XR_RBC_ARRAY_CELL_DROP || a.operation == XR_RBC_ARRAY_TAKE) {
        bool cell = a.operation == XR_RBC_ARRAY_CELL_DROP;
        if (source != (cell ? RBC_CELL_OWNED : RBC_STRING_OWNED)) return false;
        if (a.operation == XR_RBC_ARRAY_TAKE) {
            if (!rbc_array_destination(s, a.result)) return false;
            if (!args[0].spill && rbc_register_kind(s->function, args[0].index) == XR_RBC_CALL_WINDOW &&
                s->origin[args[0].index] != XR_RBC_MAP_STRING_I64) return false;
            rbc_array_define(s, a.result, RBC_STRING_OWNED, 0);
        }
        uint32_t origin = args[0].spill ? 0 : s->origin[args[0].index];
        rbc_array_define(s, args[0], RBC_EMPTY, 0);
        if (cell) {
            for (uint32_t r = 0; r < s->function->register_count; ++r)
                if ((rbc_cell_state(s->state[r]) || s->state[r] == RBC_ARRAY_REF_CELL) && s->origin[r] == origin)
                    s->state[r] = RBC_EMPTY;
        } else if (!args[0].spill) rbc_string_forget_borrows(s, args[0].index);
        return true;
    }
    if (a.operation == XR_RBC_ARRAY_CELL_WRITE || a.operation == XR_RBC_ARRAY_REF_WRITE)
        return rbc_array_source(s, args[1]);
    if (a.count > 1 && !rbc_array_source(s, args[1])) return false;
    if ((a.operation == XR_RBC_ARRAY_SET || a.operation == XR_RBC_ARRAY_REF_SET) &&
        rbc_array_state(s, args[2]) != RBC_BITS) return false;
    if (a.operation == XR_RBC_ARRAY_GET) { s->aggregates[a.result.index] = 1; return true; }
    if (a.operation == XR_RBC_ARRAY_SET || a.operation == XR_RBC_ARRAY_REF_SET ||
        a.operation == XR_RBC_MAP_CLEAR || a.operation == XR_RBC_MAP_REF_CLEAR) return true;
    if (!rbc_array_destination(s, a.result)) return false;
    rbc_array_define(s, a.result, a.operation == XR_RBC_MAP_INDEX || a.operation == XR_RBC_ARRAY_LEN || a.operation == XR_RBC_MAP_CONTAINS ||
        a.operation == XR_RBC_MAP_REMOVE || a.operation == XR_RBC_MAP_REF_REMOVE ?
        RBC_BITS : RBC_STRING_OWNED, 0);
    return true;
}
