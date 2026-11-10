/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_state_transfer.inc.c - Capability provenance through bytecode CFGs
 */
static bool rbc_state_payload(RbcStringAdmission *s, uint32_t slot, uint32_t kind) {
    return kind == XRT_MODULE_UNIT || (kind == XRT_MODULE_STRING ? rbc_string_source(s, slot) :
        rbc_register_kind(s->function, slot) == XR_RBC_BITS && s->state[slot] == RBC_BITS);
}

static bool rbc_state_transfer(RbcStringAdmission *s, XrRbcOperands v) {
    if (v.op == XR_RBC_MODULE_BEGIN || v.op == XR_RBC_MODULE_READY)
        return s->state[v.a] == RBC_STATE_BORROWED;
    if (v.op == XR_RBC_STATE_PUBLISH)
        return s->state[v.a] == RBC_STATE_REFERENCE && (s->origin[v.a] & 256u);
    if (v.op == XR_RBC_STATE_READ || v.op == XR_RBC_STATE_WRITE) {
        uint32_t reference = v.op == XR_RBC_STATE_READ ? v.b : v.a;
        if (s->state[reference] != RBC_STATE_REFERENCE || (s->origin[reference] & 255u) != v.c + 1u)
            return false;
        if (v.op == XR_RBC_STATE_WRITE)
            return (s->origin[reference] & 256u) && rbc_state_payload(s, v.b, v.c);
        if (v.c == XRT_MODULE_UNIT) return true;
        if (v.c == XRT_MODULE_STRING) {
            if (!rbc_string_destination(s, v.a)) return false;
            s->state[v.a] = RBC_STRING_OWNED;
        } else {
            if (!rbc_bits_destination(s, v.a)) return false;
            s->state[v.a] = RBC_BITS;
        }
        return true;
    }
    XrRbcStateAccess access = s->function->state_accesses[v.immediate];
    uint32_t shape = xr_rbc_state_shape(s->module, access.slot), kind = (shape & 255u) - 1u;
    if (v.op == XR_RBC_STATE_INITIALIZE)
        return s->state[v.a] == RBC_STATE_BORROWED && rbc_state_payload(s, access.value, kind);
    if (s->state[access.state] != RBC_STATE_BORROWED) return false;
    if (v.op == XR_RBC_STATE_CONSTRUCT && !rbc_state_payload(s, access.value, kind)) return false;
    s->state[v.a] = RBC_STATE_REFERENCE;
    s->origin[v.a] = shape;
    return true;
}
