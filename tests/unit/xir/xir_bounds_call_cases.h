/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_bounds_call_cases.h - Fault detail validation and owner-independent results
 *
 * KEY CONCEPT:
 *   Host-forged actions cannot publish detail outside the bounds-fault channel.
 */
#ifndef XIR_BOUNDS_CALL_CASES_H
#define XIR_BOUNDS_CALL_CASES_H

typedef struct BoundsWitness {
    XrXirAction action;
    XrXirCallStatus expected;
    uint32_t cleanups, outputs;
    uint64_t fault_allocations;
    XrXirCallAccounting *accounting;
} BoundsWitness;

static XrXirAction bounds_child(XrXirCallView *view) {
    BoundsWitness *witness = view->instance;
    witness->fault_allocations = witness->accounting->allocations;
    return witness->action;
}
static void bounds_cleanup(XrXirCallView *view, XrXirCallStatus reason) {
    BoundsWitness *witness = view->instance;
    CHECK(reason == witness->expected);
    CHECK(*(const uint32_t *) view->environment == 1u - witness->cleanups);
    ++witness->cleanups;
    witness->action.fault = (XrXirFaultDetail) {0};
}
static bool bounds_output(void *context, const XrXirOutputGroup *group) {
    (void) group;
    ++((BoundsWitness *) context)->outputs;
    return true;
}
static void bounds_fault_boundary(void) {
    const XrXirCallEntry entries[] = {
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, 0, fault_parent, bounds_cleanup, &identities[0]},
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, 0, bounds_child, bounds_cleanup, &identities[1]},
    };
    const int64_t indices[] = {INT64_MIN, -1, 3, INT64_MAX, 0};
    for (uint32_t variant = 0; variant < 21; ++variant) {
        BoundsWitness witness = {0};
        int64_t index = indices[variant < 5 ? variant : 0], length = variant == 4 ? 0 : 3;
        witness.action = xr_xir_call_bounds(index, length);
        witness.expected = variant < 5 ? XR_XIR_CALL_BOUNDS : XR_XIR_CALL_BAD_STATE;
        switch (variant) {
        case 5: witness.action.fault.length = -1; break;
        case 6: witness.action.fault.index = 1; break;
        case 7: witness.action.fault.code = 429; break;
        case 8: witness.action.fault.reserved = 1; break;
        case 9: witness.action.value.reserved = 1; break;
        case 10: witness.action.value.type = XR_XIR_BOOL; break;
        case 11: witness.action.value.payload = XR_XIR_CALL_OOM; break;
        case 12: witness.action.fault = (XrXirFaultDetail) {0}; break;
        case 13: witness.action.callee = 1; break;
        case 14: witness.action.arguments = &witness.action.value; break;
        case 15: witness.action.argument_count = 1; break;
        default:
            if (variant >= 16) {
                witness.action.kind = variant == 16 ? XR_XIR_ACTION_RETURN :
                    variant == 17 ? XR_XIR_ACTION_CONTINUE : variant == 18 ? XR_XIR_ACTION_CALL :
                    variant == 19 ? XR_XIR_ACTION_SUSPEND : XR_XIR_ACTION_OUTPUT;
                witness.action.value = (XrXirValue) {0};
                if (variant == 20) witness.action.callee = XR_XIR_OUTPUT_LINE;
            }
            break;
        }
        XrXirCallAccounting accounting = {0};
        witness.accounting = &accounting;
        XrXirCallConfig config = {entries, 2, &witness, 65536, 10, 2, &accounting,
            {bounds_output, &witness}, {0}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, NULL, 0, &call) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_call_poll(call);
        CHECK(result.status == witness.expected && result.value.type == XR_XIR_UNIT &&
            !result.value.reserved && !result.value.payload && !result.wake);
        CHECK(witness.cleanups == 2 && !witness.outputs && !accounting.depth);
        CHECK(accounting.allocations == witness.fault_allocations);
        if (variant < 5) CHECK(result.fault.code == 430 && !result.fault.reserved &&
            result.fault.index == index && result.fault.length == length);
        else CHECK(xr_xir_fault_empty(result.fault));
        XrXirCallResult repeated = xr_xir_call_poll(call);
        CHECK(!memcmp(&result.fault, &repeated.fault, sizeof(result.fault)));
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees);
        if (variant < 5) CHECK(result.fault.index == index && result.fault.length == length);
    }
}
#endif // XIR_BOUNDS_CALL_CASES_H
