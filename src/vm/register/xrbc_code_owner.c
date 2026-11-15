/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_code_owner.c - Runtime-side leases and immutable physical descriptions
 */
#include "xrbc_code_owner.h"
#include "base/xchecks.h"

XR_FUNC bool xr_rbc_code_owner_retain(void *context) {
    if (!context) return false;
    XrRbcCodeOwner *owner = context;
    uint32_t count = atomic_load_explicit(&owner->references, memory_order_relaxed);
    while (count && count != UINT32_MAX) {
        if (atomic_compare_exchange_weak_explicit(&owner->references, &count, count + 1,
                memory_order_relaxed, memory_order_relaxed)) return true;
    }
    return false;
}
XR_FUNC void xr_rbc_code_owner_release(void *context) {
    if (!context) return;
    XrRbcCodeOwner *owner = context;
    uint32_t count = atomic_fetch_sub_explicit(&owner->references, 1, memory_order_acq_rel);
    XR_CHECK(count, "only an actual code lease can retire its owner");
    if (count == 1) {
        XR_CHECK(owner->retire, "the enclosing code allocation owns its final retirement");
        owner->retire(owner);
    }
}
static const XrRbcCodeOwner *rbc_module_code_owner(const XrRbcModule *module) {
    if (!module || !module->code_lease.owner ||
        module->code_lease.retain != xr_rbc_code_owner_retain ||
        module->code_lease.release != xr_rbc_code_owner_release) return NULL;
    const XrRbcCodeOwner *owner = module->code_lease.owner;
    if (owner->functions != module->functions || owner->function_count != module->function_count)
        return NULL;
    return owner;
}
XR_FUNC const XrLirValueHomes *xr_rbc_module_value_homes(const XrRbcModule *module, uint32_t function) {
    const XrRbcCodeOwner *owner = rbc_module_code_owner(module);
    return owner && function < owner->function_count && owner->value_homes ? owner->value_homes[function] : NULL;
}
XR_FUNC const XrtValueImage *xr_rbc_module_value_image(const XrRbcModule *module) {
    const XrRbcCodeOwner *owner = rbc_module_code_owner(module);
    return owner ? owner->values : NULL;
}
XR_FUNC const XrRbcValueCode *xr_rbc_module_value_code(const XrRbcModule *module, uint32_t function) {
    const XrRbcCodeOwner *owner = rbc_module_code_owner(module);
    return owner && function < owner->function_count && owner->value_codes ? owner->value_codes[function] : NULL;
}
