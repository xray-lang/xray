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
    witness->action.panic.detail = (XrXirFaultDetail) {0};
}
static XrXirOutputStatus bounds_output(void *context, const XrXirOutputGroup *group) {
    (void) group;
    ++((BoundsWitness *) context)->outputs;
    return XR_XIR_OUTPUT_OK;
}
static void bounds_fault_boundary(void) {
    const XrXirCallEntry entries[] = {
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, 0, fault_parent, bounds_cleanup, &identities[0], 0, 0},
        {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, 0, bounds_child, bounds_cleanup, &identities[1], 0, 0},
    };
    const int64_t indices[] = {INT64_MIN, -1, 3, INT64_MAX, 0};
    for (uint32_t variant = 0; variant < 21; ++variant) {
        BoundsWitness witness = {0};
        int64_t index = indices[variant < 5 ? variant : 0], length = variant == 4 ? 0 : 3;
        witness.action = xr_xir_call_bounds(index, length);
        witness.expected = variant < 5 ? XR_XIR_CALL_BOUNDS : XR_XIR_CALL_BAD_STATE;
        switch (variant) {
        case 5: witness.action.panic.detail.length = -1; break;
        case 6: witness.action.panic.detail.index = 1; break;
        case 7: witness.action.panic.detail.code = 429; break;
        case 8: witness.action.panic.detail.reserved = 1; break;
        case 9: witness.action.value.reserved = 1; break;
        case 10: witness.action.value.type = XR_XIR_BOOL; break;
        case 11: witness.action.value.payload = XR_XIR_CALL_OOM; break;
        case 12: witness.action.panic.detail = (XrXirFaultDetail) {0}; break;
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
        XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = entries; config.entry_count = 2; config.instance = &witness; config.byte_limit = 65536; config.poll_limit = 10; config.depth_limit = 2; config.accounting = &accounting; config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, bounds_output, &witness}; config.admission = (XrXirValueAdmission) {0};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, NULL, 0, &call) == XR_XIR_CALL_READY);
        XrXirCallResult result = xr_xir_call_poll_bounded(call, UINT64_MAX);
        CHECK(result.status == witness.expected && result.value.type == XR_XIR_UNIT &&
            !result.value.reserved && !result.value.payload && !result.wake);
        CHECK(witness.cleanups == 2 && !witness.outputs && !accounting.depth);
        CHECK(accounting.allocations == witness.fault_allocations);
        if (variant < 5) CHECK(result.panic.detail.code == 430 && !result.panic.detail.reserved &&
            result.panic.detail.index == index && result.panic.detail.length == length);
        else CHECK(xr_xir_fault_empty(result.panic.detail));
        XrXirCallResult repeated = xr_xir_call_poll_bounded(call, UINT64_MAX);
        CHECK(!memcmp(&result.panic.detail, &repeated.panic.detail, sizeof(result.panic.detail)));
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(accounting.live_bytes == 0 && accounting.allocations == accounting.frees);
        if (variant < 5) CHECK(result.panic.detail.index == index && result.panic.detail.length == length);
    }
}
static void match_fault_boundary(void) {
    const XrXirCallEntry entries[] = {
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,0,fault_parent,bounds_cleanup,&identities[0], 0, 0},
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,0,bounds_child,bounds_cleanup,&identities[1], 0, 0},
    };
    for (unsigned variant=0;variant<12;++variant) {
        BoundsWitness witness={0}; witness.action=xr_xir_call_match_failure();
        witness.expected=variant ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_MATCH_FAILURE;
        switch (variant) {
        case 1: witness.action.panic.detail.code=430; break;
        case 2: witness.action.panic.detail.reserved=1; break;
        case 3: witness.action.panic.detail.index=-1; break;
        case 4: witness.action.panic.detail.length=1; break;
        case 5: witness.action.value.payload=XR_XIR_CALL_BOUNDS; break;
        case 6: witness.action.value.reserved=1; break;
        case 7: witness.action.value.type=XR_XIR_BOOL; break;
        case 8: witness.action.kind=XR_XIR_ACTION_THROW; break;
        case 9: witness.action.callee=1; break;
        case 10: witness.action.arguments=&witness.action.value; break;
        case 11: witness.action.argument_count=1; break;
        default: break;
        }
        XrXirCallAccounting accounting={0}; witness.accounting=&accounting;
        XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = entries; config.entry_count = 2; config.instance = &witness; config.byte_limit = 65536; config.poll_limit = 10; config.depth_limit = 2; config.accounting = &accounting; config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, bounds_output, &witness}; config.admission = (XrXirValueAdmission) {0};
        XrXirCall *call=NULL;
        CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY);
        XrXirCallResult result=xr_xir_call_poll_bounded(call, UINT64_MAX);
        CHECK(result.status==witness.expected && result.value.type==XR_XIR_UNIT && !result.value.payload && !result.wake);
        CHECK(witness.cleanups==2 && !witness.outputs && !accounting.depth && accounting.allocations==witness.fault_allocations);
        CHECK(variant ? xr_xir_fault_empty(result.panic.detail) : xr_xir_fault_match_valid(result.panic.detail));
        XrXirCallResult repeated=xr_xir_call_poll_bounded(call, UINT64_MAX);
        CHECK(repeated.status==result.status && !memcmp(&result.panic.detail,&repeated.panic.detail,sizeof(result.panic.detail)));
        CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
        CHECK(!accounting.live_bytes && accounting.allocations==accounting.frees);
        CHECK(variant ? xr_xir_fault_empty(result.panic.detail) : xr_xir_fault_match_valid(result.panic.detail));
    }
}
#endif // XIR_BOUNDS_CALL_CASES_H
