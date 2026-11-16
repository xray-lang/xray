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
XR_FUNC const XrRbcCodeOwner *xr_rbc_code_owner_borrow(XrtManagedLease lease) {
    if (!lease.context || lease.retain != xr_rbc_code_owner_retain ||
        lease.drop != xr_rbc_code_owner_release) return NULL;
    const XrRbcCodeOwner *owner=lease.context;
    return owner->module && owner->locations && owner->entries &&
        owner->module->functions==owner->functions && owner->module->function_count==owner->function_count ? owner : NULL;
}
XR_FUNC const XrRbcCodeOwner *xr_rbc_module_code_owner(const XrRbcModule *module) {
    if (!module || !module->code_lease.owner ||
        module->code_lease.retain != xr_rbc_code_owner_retain ||
        module->code_lease.release != xr_rbc_code_owner_release) return NULL;
    const XrRbcCodeOwner *owner = module->code_lease.owner;
    const XrRbcModule *raw=owner->module;
    if (!raw || owner->functions != module->functions || owner->function_count != module->function_count ||
        raw->natives!=module->natives || raw->native_count!=module->native_count ||
        raw->literals!=module->literals || raw->literal_count!=module->literal_count ||
        raw->aggregate_types!=module->aggregate_types || raw->aggregate_type_count!=module->aggregate_type_count ||
        raw->callable_types!=module->callable_types || raw->callable_type_count!=module->callable_type_count ||
        raw->callable_results!=module->callable_results || raw->callable_result_count!=module->callable_result_count ||
        raw->function_scopes!=module->function_scopes || raw->function_scope_count!=module->function_scope_count ||
        raw->function_callable_parameters!=module->function_callable_parameters ||
        raw->function_callable_parameter_count!=module->function_callable_parameter_count ||
        raw->state_spec!=module->state_spec || raw->state_entry!=module->state_entry ||
        raw->borrowed_value_codes!=module->borrowed_value_codes ||
        raw->borrowed_value_code_count!=module->borrowed_value_code_count || raw->borrowed_value_image!=module->borrowed_value_image ||
        raw->borrowed_callable_entries!=module->borrowed_callable_entries || raw->borrowed_type_callables!=module->borrowed_type_callables)
        return NULL;
    return owner;
}
XR_FUNC const XrRbcCallableEntry *xr_rbc_module_callable_entries(const XrRbcModule *module) {
    const XrRbcCodeOwner *owner=xr_rbc_module_code_owner(module);
    return owner ? owner->entries : module ? module->borrowed_callable_entries : NULL;
}
XR_FUNC const uint32_t *xr_rbc_module_type_callables(const XrRbcModule *module) {
    const XrRbcCodeOwner *owner=xr_rbc_module_code_owner(module);
    return owner ? owner->type_callables : module ? module->borrowed_type_callables : NULL;
}
XR_FUNC const XrLirValueHomes *xr_rbc_module_value_homes(const XrRbcModule *module, uint32_t function) {
    const XrRbcCodeOwner *owner = xr_rbc_module_code_owner(module);
    if (owner) return function < owner->function_count && owner->value_homes ? owner->value_homes[function] : NULL;
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,function);
    return code ? code->homes : NULL;
}
XR_FUNC const XrtValueImage *xr_rbc_module_value_image(const XrRbcModule *module) {
    const XrRbcCodeOwner *owner = xr_rbc_module_code_owner(module);
    return owner ? owner->values : module ? module->borrowed_value_image : NULL;
}
XR_FUNC const XrRbcValueCode *xr_rbc_module_value_code(const XrRbcModule *module, uint32_t function) {
    const XrRbcCodeOwner *owner = xr_rbc_module_code_owner(module);
    if (owner) return function < owner->function_count && owner->value_codes ? owner->value_codes[function] : NULL;
    return module && module->functions && module->borrowed_value_codes &&
        module->borrowed_value_code_count==module->function_count && function<module->function_count ?
        module->borrowed_value_codes[function] : NULL;
}
