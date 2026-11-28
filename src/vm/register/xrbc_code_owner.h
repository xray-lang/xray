/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_code_owner.h - Actual executable ownership without compiler linkage
 *
 * KEY CONCEPT:
 *   The producer embeds this owner in its one immutable code allocation. Queries
 *   borrow its real assets; no registry, runtime Layout or validation flag exists.
 */
#ifndef XRBC_CODE_OWNER_H
#define XRBC_CODE_OWNER_H
#include "xrbc_value_code.h"
#include <stdatomic.h>

typedef struct XrRbcCodeOwner XrRbcCodeOwner;
typedef enum XrRbcCallableEntryKind {
    XR_RBC_CALLABLE_ENTRY_CODE, XR_RBC_CALLABLE_ENTRY_ASSERT_CONDITION
} XrRbcCallableEntryKind;
typedef struct XrRbcCallableEntry {
    const XrRbcCodeOwner *producer; /* Borrowed enclosing code allocation. */
    uint32_t physical_function, source_function, signature_type, environment_type;
    XrRbcCallableEntryKind kind;
    uint32_t callable;
} XrRbcCallableEntry;
struct XrRbcCodeOwner {
    _Atomic(uint32_t) references;
    const XrRbcFunction *functions;
    size_t function_count;
    const bool *uncaught_error_roots; /* Indexed by physical function ordinal. */
    XrLirValueHomes *const *value_homes;
    XrRbcValueCode *const *value_codes;
    const XrtValueImage *values;
    const XrRbcModule *module;
    XrtLocationOwner *locations;
    /* Ordinary entries form the physical prefix; intrinsic entries have no code body. */
    const XrRbcCallableEntry *entries;
    size_t entry_count;
    const uint32_t *type_callables; /* Image type -> one-based RBC signature, zero for non-Fn. */
    /* The enclosing allocation owns every borrowed asset above. Retire runs
     * exactly once at zero actual references and must return normally. The
     * producer keeps its callback code live through this final retirement. */
    void (*retire)(XrRbcCodeOwner *);
};

/* Only the receiving producer initializes this owner, before publishing any
 * lease. Fields other than the real reference count are immutable thereafter.
 * These operations neither infer nor authorize a new owning or borrowing fact. */
XR_FUNC bool xr_rbc_code_owner_retain(void *);
XR_FUNC void xr_rbc_code_owner_release(void *);
/* A genuine producer lease protects its own raw snapshot, never a Program
 * shell or the caller's current module. The returned owner is only borrowed. */
XR_FUNC const XrRbcCodeOwner *xr_rbc_code_owner_borrow(XrtManagedLease);
XR_FUNC const XrRbcCodeOwner *xr_rbc_module_code_owner_compared(const XrRbcModule *);
/* The sealed description an execution runs on authenticates by identity, which
 * keeps every lookup on its hot paths cheap. Any other descriptor must equal
 * the owner's raw snapshot field for field. */
static inline const XrRbcCodeOwner *xr_rbc_module_code_owner(const XrRbcModule *module) {
    if (module && module->code_lease.owner && module->code_lease.retain == xr_rbc_code_owner_retain &&
        module->code_lease.release == xr_rbc_code_owner_release &&
        ((const XrRbcCodeOwner *)module->code_lease.owner)->module == module)
        return module->code_lease.owner;
    return xr_rbc_module_code_owner_compared(module);
}
/* Typed descriptions borrow the actual code lease, or the raw caller's
 * explicit immutable storage when no owner is attached. Raw bytecode cannot
 * acquire typed owning permissions from forged pointers, and scalar-only code
 * without a typed operation has no descriptor. Every typed operation asks
 * these, so they stay inline with the identity check above. */
static inline const XrtValueImage *xr_rbc_module_value_image(const XrRbcModule *module) {
    const XrRbcCodeOwner *owner = xr_rbc_module_code_owner(module);
    return owner ? owner->values : module ? module->borrowed_value_image : NULL;
}
static inline const XrRbcValueCode *xr_rbc_module_value_code(const XrRbcModule *module, uint32_t function) {
    const XrRbcCodeOwner *owner = xr_rbc_module_code_owner(module);
    if (owner) return function < owner->function_count && owner->value_codes ? owner->value_codes[function] : NULL;
    return module && module->functions && module->borrowed_value_codes &&
        module->borrowed_value_code_count == module->function_count && function < module->function_count ?
        module->borrowed_value_codes[function] : NULL;
}
/* The word admission expects for a typed access of this module. A direct
 * call is selected with its callee's description. */
static inline XrRbcOp xr_rbc_module_value_opcode(const XrRbcModule *module, const XrRbcValueCode *code,
    uint32_t index) {
    if (!code || !code->accesses || index >= code->access_count) return XR_RBC_VALUE_ACCESS;
    XrRbcValueAccess access = code->accesses[index];
    const XrRbcValueCode *callee = access.operation == XR_RBC_VALUE_CALL && access.index < module->function_count ?
        xr_rbc_module_value_code(module, access.index) : NULL;
    return rbc_value_access_opcode(xr_rbc_module_value_image(module), code, index, callee);
}
XR_FUNC const XrRbcCallableEntry *xr_rbc_module_callable_entries(const XrRbcModule *);
XR_FUNC size_t xr_rbc_module_callable_entry_count(const XrRbcModule *);
XR_FUNC const uint32_t *xr_rbc_module_type_callables(const XrRbcModule *);
#endif // XRBC_CODE_OWNER_H
